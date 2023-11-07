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

#include "rgw_pack.h"
#include <iostream>
#include "zpp_bits.h" // XXX maybe we do want this in rgw_pack.h
#include "fmt/format.h"

namespace rgw::pack {

  template<typename IO>
  Pack<IO>::Pack(IO& io) : io(io)
  {
    /* assert: no-header */
    if (io.get_flags() & FLAG_CREATE) {
      // TODO: implement
    }
  }

  template<typename IO>
  void Pack<IO>::AddObj::add_bytes(off64_t off, const void* buf, size_t len, uint8_t flags)
  {
    std::cout <<
      fmt::format("Pack<IO>::AddObj::add_bytes off={} buf={} len={} flags={}",
		  off, uint64_t(buf), len, flags) << std::endl;
  } /* add_bytes*/

  template<typename IO>
  int Pack<IO>::add_object(
    const std::string_view name, Pack<IO>::add_obj_cb_t cb)
  {
    std::cout  << "Pack<IO>::add_object name=\""
	       << name << "\""
	       << std::endl;


    // TODO: finish
    return 0; // do it
  }

  /* explicit instantiations (may need #define adjustments (e.g., CLS) */
  template class Pack<PositionalIO>;
} /* namespace rgw::pack */
