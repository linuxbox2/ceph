// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#ifndef BPLUS_IO_H
#define BPLUS_IO_H

#include "bplus_types.h"
#include "include/types.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include <boost/container/flat_map.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive_ptr.hpp>
#include "common/ceph_timer.h"
#include "common/likely.h"

#include "objclass/objclass.h"

namespace rgw::bplus::ondisk {

  class BTreeCache;

  using link_mode = bi::link_mode<bi::safe_link>; /* XXX normal */

  class BTreeIO
  {

    static constexpr uint32_t FLAG_NONE =   0x0000;
    static constexpr uint32_t FLAG_INAVL =  0x0001;

    object_t oid; // XXX sufficient?
    ondisk::Header header;
    mutable std::atomic<uint32_t> refcnt;
    bi::list_member_hook<link_mode> tree_cache_hook;
    BTreeCache* cache;
    uint32_t flags;
    cls_method_context_t hctx;

    typedef bi::list<BTreeIO,
		     bi::member_hook<
		       BTreeIO, bi::list_member_hook<link_mode>,
		       &BTreeIO::tree_cache_hook>,
		     bi::constant_time_size<true>> TreeQueue;

    using tree_hook_type = bi::avl_set_member_hook<link_mode>;

    PageLRU page_lru;
    PageCache page_cache;

  private:
    Page* get_page(const uint16_t page_no) {
      uint32_t offset = Layout::page_start + (page_no * Layout::page_size);
      PageCache::Latch lat;
      // 1. search the cache
      auto page = page_cache.find_latch(1, offset, lat, PageCache::FLAG_LOCK);
      // 2. if !found, load from disk
    }

    Page* get_page(const std::string& key) {
      /* XXXX */
      return get_page(0);
    }

  public:
    BTreeIO(const std::string& oid, BTreeCache* cache,
	    cls_method_context_t _hctx)
      : oid(oid), refcnt(1), cache(cache), flags(FLAG_NONE), hctx(_hctx),
	page_lru(1 /* lanes */, 23 /* hiwat */),
	page_cache(1 /* partitions */, 23 /* size */)
      {}

    void set_hctx(cls_method_context_t _hctx) {
      hctx = _hctx;
    }

    BTreeIO* ref() {
      intrusive_ptr_add_ref(this);
      return this;
    }

    inline void rele() {
      intrusive_ptr_release(this);
    }

    friend void intrusive_ptr_add_ref(const BTreeIO* tree) {
      tree->refcnt.fetch_add(1, std::memory_order_relaxed);
    }

    friend void intrusive_ptr_release(const BTreeIO* tree) {
      if (tree->refcnt.fetch_sub(1, std::memory_order_release) == 0) {
	std::atomic_thread_fence(std::memory_order_acquire);
	if (likely(tree->flags & FLAG_INAVL)) {
	  const_cast<BTreeIO*>(tree)->uncache_this();
	}
	delete tree;
      }
    } /* intrusive_ptr_release */

    void load();

    int insert(const std::string& key, const std::string &val);

    void uncache_this();

    friend class BTreeCache;
  }; /* BTreeIO */

  class BTreeCache
  {
  public:
    static constexpr uint16_t entries_hiwat = 12;
    static constexpr uint16_t max_idle_s = 120;
    static constexpr uint32_t SENTINEL_REFCNT = 1;

    using lock_guard = std::lock_guard<std::mutex>;
    using unique_lock = std::unique_lock<std::mutex>;

    BTreeIO* get_tree(const std::string& oid, cls_method_context_t hctx) {
      unique_lock guard(mtx);
      BTreeIO* t{nullptr};
      for (auto& elt : cache) {
	if (elt.oid == oid) {
	  cache.erase(BTreeIO::TreeQueue::s_iterator_to(elt));
	  cache.push_front(elt);
	  t = elt.ref();
	  t->set_hctx(hctx);
	  goto out;
	}
      }
      t = new BTreeIO(oid, this, hctx);
      t->flags |= BTreeIO::FLAG_INAVL;
      cache.push_front(*t);
    out:
      return t;
    } /* get_tree */

    void put_tree(BTreeIO* t) {
      unique_lock guard(mtx);
      /* return refcnt w/mtx LOCKED */
      t->rele();
      /* reclaim entries at LRU if that is idle, iff
       * cache is over target size */
      try_shrink_cache();
    } /* put_tree */

  private:
    std::mutex mtx;
    BTreeIO::TreeQueue cache;

    void try_shrink_cache() {
      /* assert mtx LOCKED */
    again:
      if (cache.size() > entries_hiwat) {
	/* potentially reclaim LRU */
	auto& elt = cache.back();
	if (elt.refcnt.load() == SENTINEL_REFCNT) {
	  elt.rele(); /* dequeues and frees elt if refcnt drops to 0 */
	  goto again;
	} /* node now idle */
      } /* cache too big */
    } /* try_shrink_cache */

    friend class BTreeIO;

  }; /* BTreeCache */

} /* namespace */

#endif /* BPLUS_IO_H */
