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

#include "mock_cls.h"
#include <system_error>
#include <iostream>
#include <boost/filesystem.hpp>
#include <fstream>

namespace rgw::bplus::ondisk {

  namespace bf = boost::filesystem;
  
  void create_backing_file(std::string path, size_t size) {
    std::ofstream file(path);
    std::string s(size, '0');
    file << s;
  }

  MockHctx::MockHctx(std::string oid, size_t size, uint32_t flags)
  {
    bf::path path(oid);
    if ((flags & FLAG_CREATE) ||
	(! bf::exists(path))) {
      create_backing_file(oid, size);
    }
    std::error_code error;
    mmap = mio::make_mmap_sink(oid, 0, mio::map_entire_file, error);
    if (error) {
      std::cerr << "error mapping file: " << error.message() << std::endl;
      return;
    }
    if (flags & FLAG_ZERO) {
      memset(mmap.data(), 0, size);
      mmap.sync(error);
      if (error) {
	std::cerr << "error syncing file: " << error.message() << std::endl;
      }
    }
    
  } /* MockHctx(string, size_t, uint32_t) */

  int MockHctx::read2(int ofs, int len, ceph::buffer::list *bl,
		      uint32_t op_flags)
  {
    // assert ofs+len <= mmap.size()
    char* ptr = mmap.data() + ofs;
    bl->push_back(
      buffer::copy(ptr, len)); // XXX yes, we could share mapping here
    return len;
  } /* read2 */

  int MockHctx::write2(int ofs, int len, ceph::buffer::list *bl,
		      uint32_t op_flags)
  {
    // TODO:: implement
    return 0;
  } /* write2 */
} /* namespace */
