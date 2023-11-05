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
#include <system_error>
#include <filesystem>
#include <unistd.h>
#include <fcntl.h>
#include "fmt/format.h"

namespace rgw::pack {

  static constexpr mode_t def_mode =
    S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH;

  PositionalIO make_positional(std::string& archive_path)
  {
    PositionalIO pio;
    int rc = pio.open(archive_path);
    return pio;
  } /* make_positional */

  int PositionalIO::open(const std::string& archive_path)
  {
    fd = ::open(archive_path.c_str(), O_RDWR|O_CREAT, def_mode);
    if (fd > 0) {
      flags |= FLAG_OPEN;
    } else {
      throw std::system_error(-fd, std::system_category(),
			      fmt::format("open failed for {}",
					  archive_path));
    }
    return 0;
  } /* open */

  ssize_t PositionalIO::read(void* buf, size_t len, off64_t off)
  {
    return ::pread(fd, buf, len, off);
  } /* read */

  ssize_t PositionalIO::write(const void* buf, size_t len, off_t off)
  {
    return ::pwrite(fd, buf, len, off);
  } /* write */
  
  void PositionalIO::close()
  {
    ::close(fd);
    flags &= ~FLAG_OPEN;
  } /* close */

  PositionalIO::~PositionalIO() {
    if (flags & FLAG_OPEN) {
      close();
    }
  } /* ~PositionalIO */

  template<>
  Pack<PositionalIO> Pack<PositionalIO>::make_pack(PositionalIO& io)
  {
    Pack<PositionalIO> pack;
    pack.io = io;
    return pack;
  } /* make_pack */

} /* namespace rgw::pack */
