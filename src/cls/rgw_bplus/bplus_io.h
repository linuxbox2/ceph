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

#include "common/ceph_timer.h"


namespace rgw::bplus::ondisk {

  class BTreeIO
  {
    object_t oid; // XXX sufficient?
    ondisk::Header header;

    typedef bi::link_mode<bi::safe_link> link_mode; /* XXX normal */
    typedef bi::avl_set_member_hook<link_mode> tree_hook_type;

  }; /* BTreeIO */

  class BTreeCache
  {
  }; /* BTreeCache */

} /* namespace */

#endif /* BPLUS_IO_H */
