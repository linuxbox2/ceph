// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_cache.h"

#include <errno.h>

#define dout_subsys ceph_subsys_rgw

namespace rgw::cache {

ObjectCache::GetObjResult ObjectCache::get(
  std::string& name, ObjectCacheInfo& info, uint32_t mask,
  rgw_cache_entry_info* cache_info)
{
  shared_lock reader(shared_mtx);

  if (!enabled) {
    return GetObj_ENOENT;
  }

  /* XXX convertme */
  auto iter = cache_map.find(name);
  if (iter == cache_map.end() ||
      (expiry.count() &&
       (ceph::coarse_mono_clock::now() - iter->second.info.time_added)
       > expiry)) {
    ldout(cct, 10) << "cache get: name=" << name << " : miss" << dendl;
    if (perfcounter)
      perfcounter->inc(l_rgw_cache_miss);
    return GetObj_ENOENT;
  }

  ObjectCacheEntry *entry = &iter->second;

  if (lru_counter - entry->lru_promotion_ts > lru_window) {
    ldout(cct, 20) << "cache get: touching lru, lru_counter=" << lru_counter
                   << " promotion_ts=" << entry->lru_promotion_ts << dendl;

    /* promote to writer lock */
    reader.unlock();
    lock_guard writer(shared_mutex);

    /* need to redo this because entry might have dropped off the cache */
    iter = cache_map.find(name);
    if (iter == cache_map.end()) {
      ldout(cct, 10) << "lost race! cache get: name=" << name << " : miss"
		     << dendl;
      if(perfcounter) perfcounter->inc(l_rgw_cache_miss);
      return GetObj_ENOENT;
    }

    entry = &iter->second;
    /* check again, we might have lost a race here */
    if (lru_counter - entry->lru_promotion_ts > lru_window) {
      touch_lru(name, *entry, iter->second.lru_iter);
    }
  }

  ObjectCacheInfo& src = iter->second.info;
  if ((src.flags & mask) != mask) {
    ldout(cct, 10) << "cache get: name=" << name << " : type miss (requested=0x"
                   << std::hex << mask << ", cached=0x" << src.flags
                   << std::dec << ")" << dendl;
    if(perfcounter) perfcounter->inc(l_rgw_cache_miss);
    return GetObj_ENOENT;
  }
  ldout(cct, 10) << "cache get: name=" << name << " : hit (requested=0x"
                 << std::hex << mask << ", cached=0x" << src.flags
                 << std::dec << ")" << dendl;

  /* XXXX this is where we would return a ref to cached entry --
   * fixme!! */
  ObjectCache::GetObjResult result { &src, 0 };

  if (cache_info) {
    cache_info->cache_locator = name;
    cache_info->gen = entry->gen;
  }
  if(perfcounter) perfcounter->inc(l_rgw_cache_hit);

  return result;
}

bool ObjectCache::chain_cache_entry(
  std::initializer_list<rgw_cache_entry_info*> cache_info_entries,
  RGWChainedCache::Entry* chained_entry)
{
  lock_guard writer(shared_mtx);

  if (!enabled) {
    return false;
  }

  std::vector<ObjectCacheEntry*> entries;
  entries.reserve(cache_info_entries.size());
  /* first verify that all entries are still valid */
  for (auto cache_info : cache_info_entries) {
    ldout(cct, 10) << "chain_cache_entry: cache_locator="
		   << cache_info->cache_locator << dendl;
    auto iter = cache_map.find(cache_info->cache_locator);
    if (iter == cache_map.end()) {
      ldout(cct, 20) << "chain_cache_entry: couldn't find cache locator"
		     << dendl;
      return false;
    }

    auto entry = &iter->second;

    if (entry->gen != cache_info->gen) {
      ldout(cct, 20) << "chain_cache_entry: entry.gen (" << entry->gen
		     << ") != cache_info.gen (" << cache_info->gen << ")"
		     << dendl;
      return false;
    }
    entries.push_back(entry);
  }

  chained_entry->cache->chain_cb(chained_entry->key, chained_entry->data);

  for (auto entry : entries) {
    entry->chained_entries.push_back(
      make_pair(chained_entry->cache, chained_entry->key));
  }

  return true;
}

void ObjectCache::put(std::string& name, ObjectCacheInfo& info,
		      rgw_cache_entry_info* cache_info)
{
  lock_guard writer(shared_mtx);

  if (!enabled) {
    return;
  }

  ldout(cct, 10) << "cache put: name=" << name << " info.flags=0x"
                 << std::hex << info.flags << std::dec << dendl;
  auto iter = cache_map.find(name);
  if (iter == cache_map.end()) {
    ObjectCacheEntry entry;
    entry.lru_iter = lru.end();
#if 0 /* XXXX fixme!!!! */
    cache_map.insert(pair<string, ObjectCacheEntry>(name, entry));
#endif
    iter = cache_map.find(name);
  }
  ObjectCacheEntry& entry = iter->second;
  ObjectCacheInfo& target = entry.info;

  invalidate_lru(entry);

  entry.chained_entries.clear();
  entry.gen++;

  touch_lru(name, entry, entry.lru_iter);

  target.status = info.status;

  if (info.status < 0) {
    target.flags = 0;
    target.xattrs.clear();
    target.data.clear();
    return;
  }

  if (cache_info) {
    cache_info->cache_locator = name;
    cache_info->gen = entry.gen;
  }

  target.flags |= info.flags;

  if (info.flags & CACHE_FLAG_META)
    target.meta = info.meta;
  else if (!(info.flags & CACHE_FLAG_MODIFY_XATTRS))
    target.flags &= ~CACHE_FLAG_META; // non-meta change should reset meta

  if (info.flags & CACHE_FLAG_XATTRS) {
    target.xattrs = info.xattrs;
    /* XXX wrap loop in debug level condition */
    for (const auto& iter : target.xattrs) {
      ldout(cct, 10) << "updating xattr: name=" << iter.first
		     << " bl.length()=" << iter.second.length()
		     << dendl;
    }
  } else if (info.flags & CACHE_FLAG_MODIFY_XATTRS) {
    for (const auto& iter : info.rm_xattrs) {
      ldout(cct, 10) << "removing xattr: name=" << iter.first << dendl;
      target.xattrs.erase(iter.first);
    }
    for (const auto& iter : info.xattrs) {
      ldout(cct, 10) << "appending xattr: name=" << iter.first
		     << " bl.length()=" << iter.second.length() << dendl;
      target.xattrs[iter.first] = iter.second;
    }
  }

  if (info.flags & CACHE_FLAG_DATA)
    target.data = info.data;

  if (info.flags & CACHE_FLAG_OBJV)
    target.version = info.version;
}

void ObjectCache::remove(std::string& name)
{
  lock_guard writer(shared_mtx);

  if (!enabled) {
    return;
  }

  auto iter = cache_map.find(name);
  if (iter == cache_map.end())
    return;

  ldout(cct, 10) << "removing " << name << " from cache" << dendl;
  ObjectCacheEntry& entry = iter->second;

  for (auto& kv : entry.chained_entries) {
    kv.first->invalidate(kv.second);
  }

  remove_lru(name, iter->second.lru_iter);
  cache_map.erase(iter);
}

void ObjectCache::touch_lru(std::string& name, ObjectCacheEntry& entry,
			    std::list<std::string>::iterator& lru_iter)
{
  while (lru_size > cct->_conf->get_val<size_t>("rgw_cache_lru_size")) {
    auto iter = lru.begin();
    if ((*iter).compare(name) == 0) {
      /*
       * if the entry we're touching happens to be at the lru end,
       * don't remove it, lru shrinking can wait for next time
       */
      break;
    }
    auto map_iter = cache_map.find(*iter);
    ldout(cct, 10) << "removing entry: name=" << *iter << " from cache LRU"
		   << dendl;
    if (map_iter != cache_map.end()) {
      ObjectCacheEntry& entry = map_iter->second;
      invalidate_lru(entry);
      cache_map.erase(map_iter);
    }
    lru.pop_front();
    lru_size--;
  }

  if (lru_iter == lru.end()) {
    lru.push_back(name);
    lru_size++;
    lru_iter--;
    ldout(cct, 10) << "adding " << name << " to cache LRU end" << dendl;
  } else {
    ldout(cct, 10) << "moving " << name << " to cache LRU end" << dendl;
    lru.erase(lru_iter);
    lru.push_back(name);
    lru_iter = lru.end();
    --lru_iter;
  }

  lru_counter++;
  entry.lru_promotion_ts = lru_counter;
}

void ObjectCache::remove_lru(std::string& name,
			     std::list<std::string>::iterator& lru_iter)
{
  if (lru_iter == lru.end())
    return;

  lru.erase(lru_iter);
  lru_size--;
  lru_iter = lru.end();
}

void ObjectCache::invalidate_lru(ObjectCacheEntry& entry)
{
  for (auto iter = entry.chained_entries.begin();
       iter != entry.chained_entries.end(); ++iter) {
    RGWChainedCache *chained_cache = iter->first;
    chained_cache->invalidate(iter->second);
  }
}

void ObjectCache::set_enabled(bool status)
{
  lock_guard writer(shared_mtx);

  enabled = status;

  if (!enabled) {
    do_invalidate_all();
  }
}

void ObjectCache::invalidate_all()
{
  lock_guard writer(shared_mtx);

  do_invalidate_all();
}

void ObjectCache::do_invalidate_all()
{
  cache_map.clear();
  lru.clear();

  lru_size = 0;
  lru_counter = 0;
  lru_window = 0;

  for (auto& cache : chained_cache) {
    cache->invalidate_all();
  }
}

void ObjectCache::chain_cache(RGWChainedCache *cache) {
  lock_guard writer(shared_mtx);
  chained_cache.push_back(cache);
}

} /* namespace rgw::cache */
