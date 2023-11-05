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

#include "positional_io.h"
#include <fcntl.h>

namespace rgw::pack {

  PositionalIO make_positional(std::string& archive_path)
  {
    PositionalIO pio;

    return pio;
  } /* make_positional */

  int PositionalIO::open(std::string& archive_path)
  {
    fd = ::open(archive_path.c_str(), O_RDWR|O_CREAT);
    return 0;
  }

  void PositionalIO::close()
  {
  }

  PositionalIO::~PositionalIO() {
    if (flags & FLAG_OPEN) {
      close();
    }
  }

  template<>
  Pack<PositionalIO> Pack<PositionalIO>::make_pack(PositionalIO& io)
  {
    Pack<PositionalIO> pack;
    pack.io = io;
    return pack;
  } /* make_pack */

} /* namespace rgw::pack */
