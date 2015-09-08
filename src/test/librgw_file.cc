// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <stdint.h>
#include <tuple>
#include <iostream>

#include "include/rados/librgw.h"
#include "include/rados/rgw_file.h"

#include "gtest/gtest.h"
#include "common/ceph_argparse.h"
#include "common/debug.h"
#include "global/global_init.h"

namespace {
  librgw_t rgw = nullptr;
  string uid("testuser");
  string access_key("C4B4D3E4H355VTDTQXRF");
  string secret_key("NRBkhM2rUZNUbydD86HpNJ110VpQjVroumCOHJXw");
  struct rgw_fs *fs = nullptr;
  typedef std::tuple<string,uint64_t> fid_type; //in c++2014 can alias...
  std::vector<fid_type> fids1;
}

TEST(LibRGW, INIT) {
  int ret = librgw_create(&rgw, nullptr, 0, nullptr);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(rgw, nullptr);
}

TEST(LibRGW, MOUNT) {
  int ret = rgw_mount(uid.c_str(), access_key.c_str(), secret_key.c_str(),
		      &fs);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(fs, nullptr);
}

extern "C" {
  static bool r1_cb(const char* name, void *arg, uint64_t offset) {
    // don't need arg--it would point to fids1
    fids1.push_back(fid_type(name, offset));
    return true; /* XXX ? */
  }
}

TEST(LibRGW, LIST_BUCKETS) {
  /* list buckets via readdir in fs root */
  using std::get;

  bool eof = false;
  int ret = rgw_readdir(fs, &fs->root_fh, 0 /* offset */, r1_cb, &fids1, &eof);
  for (auto& fid : fids1) {
    std::cout << "fname: " << get<0>(fid) << " fid: " << get<1>(fid)
	      << std::endl;
  }
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, UMOUNT) {
  int ret = rgw_umount(fs);
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, SHUTDOWN) {
  librgw_shutdown(rgw);
}

int main(int argc, char *argv[])
{
  string val;
  vector<const char*> args;

  argv_to_vec(argc, const_cast<const char**>(argv), args);
  env_to_vec(args);

  for (auto arg_iter = args.begin(); arg_iter != args.end();) {
    if (ceph_argparse_witharg(args, arg_iter, &val, "--access",
			      (char*) NULL)) {
      access_key = val;
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--secret",
				     (char*) NULL)) {
      secret_key = val;
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--uid",
				     (char*) NULL)) {
      uid = val;
    }
    else {
      ++arg_iter;
    }
  }

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
