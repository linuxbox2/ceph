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
#include "common/ceph_timer.h"

namespace rgw::bplus::ondisk {

  class BTreeIO
  {
    object_t oid; // XXX sufficient?
    ondisk::Header header;
    // XXX refcnt
    typedef bi::link_mode<bi::safe_link> link_mode; /* XXX normal */
    typedef bi::avl_set_member_hook<link_mode> tree_hook_type;

  public:
    BTreeIO(std::string oid)
      : oid(oid) {}

    BTreeIO* ref() {
      // XXX do it
      return this;
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
	if (elt->oid == oid) {
	  return elt->ref();
	}
      }
      auto t = new BTreeIO(oid);
#if 0
      // TODO: implement
#endif
      return t;
    }
  private:
    std::mutex mtx;
    std::vector<BTreeIO*> cache;

  }; /* BTreeCache */

} /* namespace */

#endif /* BPLUS_IO_H */
