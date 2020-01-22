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

namespace rgw::bplus::ondisk {

  using link_mode = bi::link_mode<bi::safe_link>; /* XXX normal */

  class BTreeIO
  {
    object_t oid; // XXX sufficient?
    ondisk::Header header;
    mutable std::atomic<uint32_t> refcnt;
    bi::list_member_hook<link_mode> tree_cache_hook;
    BTreeCache* cache;

    typedef bi::list<BTreeIO,
		     bi::member_hook<
		       BTreeIO, bi::list_member_hook<link_mode>,
		       &BTreeIO::tree_cache_hook>,
		     bi::constant_time_size<true>> TreeQueue;

    using tree_hook_type = bi::avl_set_member_hook<link_mode>;

  public:
    BTreeIO(std::string oid, BTreeCache* cache)
      : oid(oid), refcnt(1), cache(cache) {}

    BTreeIO* ref() {
      intrusive_ptr_add_ref(this);
      return this;
    }

    inline void rele() {
    }

    friend void intrusive_ptr_add_ref(const BTreeIO* tree) {
      tree->refcnt.fetch_add(1, std::memory_order_relaxed);
    }

    friend void intrusive_ptr_release(const BTreeIO* tree) {
      if (tree->refcnt.fetch_sub(1, std::memory_order_release) == 0) {
	std::atomic_thread_fence(std::memory_order_acquire);
	delete tree;
      }
    }

    friend class BTreeCache;
  }; /* BTreeIO */

  class BTreeCache
  {
  public:
    static constexpr uint16_t entries_hiwat = 12;
    static constexpr uint16_t max_idle_s = 120;

    using lock_guard = std::lock_guard<std::mutex>;
    using unique_lock = std::unique_lock<std::mutex>;

    BTreeIO* get_tree(const std::string& oid) {
      lock_guard guard(mtx);
      for (auto& elt : cache) {
	if (elt.oid == oid) {
	  auto t = elt;
	  cache.erase(elt);
	  cache.push_front(elt);
	  return elt.ref();
	}
      }
      auto t = new BTreeIO(oid);
      cache.push_back(*t);
      return t;
    } /* get_tree */

    void put_tree(BTreeIO* t) {
      
      lock_guard guard(mtx);
      /* TODO: unref */
      
    } /* put_tree */

  private:
    std::mutex mtx;
    BTreeIO::TreeQueue cache;

  }; /* BTreeCache */

} /* namespace */

#endif /* BPLUS_IO_H */
