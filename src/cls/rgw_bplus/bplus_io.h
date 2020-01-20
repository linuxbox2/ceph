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

namespace rgw::bplus::ondisk {

  class BTreeIO
  {
    Header header;

  }; /* BTreeIO */

  class BTreeCache
  {
  }; /* BTreeCache */

} /* namespace */

#endif /* BPLUS_IO_H */
