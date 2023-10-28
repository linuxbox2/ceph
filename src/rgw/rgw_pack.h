// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2023 IBM, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <string>
#include "include/function2.hpp"
#include "zpp_bits.h"
#include <python3.11/tupleobject.h>
#include <stdint.h>
#include <xxhash.h>

namespace rgw::pack {

  class Pack
  {
    class Header
    {
      uint16_t struct_v;
      uint32_t flags;
      // TODO: implement
    }; /* Header */

    class ObjEntry
    {
      uint16_t struct_v;
      std::string name;
      std::string cksum; // XXX use mine?
      uint32_t flags;
      uint32_t size;
    }; /* ObjEntry */

  }; /* Pack */

} /* namespace rgw::pack */
