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

#include "bplus_io.h"

namespace rgw::bplus::ondisk {

  void BTreeIO::uncache_this() {
    /* mtx NOT HELD in rele() path */
    lock_guard guard(cache->mtx);
    cache->cache.erase(BTreeIO::TreeQueue::s_iterator_to(*this));
    flags &= ~FLAG_INAVL;
  } /* intrusive_ptr_release */

} /* namespace */
