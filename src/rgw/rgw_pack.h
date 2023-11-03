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
#include <stdint.h>
#include <xxhash.h>

namespace rgw::pack {

  class Header
  {
  public:
    uint16_t struct_v;
    uint32_t flags;
    // TODO: implement
  }; /* Header */

  class ObjEntry
  {
  public:
    uint16_t struct_v;
    std::string name;
    std::string cksum; // XXX use mine?
    uint32_t flags;
    uint32_t size;
  }; /* ObjEntry */

  /*
  class PositionalIO
  {
  private:
    PositionalIO() {}
  public:
    ssize_t read(void* buf, size_t size, off_t off);
    ssize_t write(void* buf, size_t size, off_t off);
    void flush();
  }; // PositionalIO
  */
  
  /* type erasing i/o types */
  template <typename IO>
  class Pack
  {
    
  }; /* Pack */

} /* namespace rgw::pack */
