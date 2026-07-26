// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 Red Hat, Inc.
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
#include <vector>
#include <map>
#include <random>
#include "xxhash.h"

#include "include/rados/librgw.h"
#include "include/rados/rgw_file.h"

#include "gtest/gtest.h"
#include "common/ceph_argparse.h"
#include "common/debug.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

using namespace std;

namespace {
  librgw_t rgw = nullptr;
  string userid("testuser");
  string access_key("");
  string secret_key("");
  struct rgw_fs *fs = nullptr;

  uint32_t owner_uid = 867;
  uint32_t owner_gid = 5309;
  uint32_t create_mask = RGW_SETATTR_UID | RGW_SETATTR_GID | RGW_SETATTR_MODE;

  bool do_create = false;
  bool do_delete = false;
  bool do_large = false;
  bool do_verify = false;
  bool do_hexdump = false;

  string bucket_name = "sorrydave";
  string object_name = "jocaml";

  std::string lorem =
    "Lorem ipsum dolor sit amet";

  std::string dolor =
    R"(Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.)";

  struct rgw_file_handle* bucket_fh = nullptr;
  struct rgw_file_handle* object_fh = nullptr;

  typedef std::tuple<string,uint64_t, struct rgw_file_handle*> fid_type;
  std::vector<fid_type> fids;

  std::uniform_int_distribution<uint8_t> uint_dist;
  std::mt19937 rng;

  struct {
    int argc;
    char **argv;
  } saved_args;
}

TEST(LibRGW, INIT) {
  int ret = librgw_create(&rgw, saved_args.argc, saved_args.argv);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(rgw, nullptr);
}

TEST(LibRGW, MOUNT) {
  int ret = rgw_mount2(rgw, userid.c_str(), access_key.c_str(),
                       secret_key.c_str(), "/", &fs, RGW_MOUNT_FLAG_NONE);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(fs, nullptr);
}

TEST(LibRGW, CREATE_BUCKET) {
  if (do_create) {
    struct stat st;
    struct rgw_file_handle *fh;

    st.st_uid = owner_uid;
    st.st_gid = owner_gid;
    st.st_mode = 755;

    int ret = rgw_mkdir(fs, fs->root_fh, bucket_name.c_str(), &st, create_mask,
			&fh, RGW_MKDIR_FLAG_NONE);
    ASSERT_EQ(ret, 0);
  }
}

TEST(LibRGW, LOOKUP_BUCKET) {
  int ret = rgw_lookup(fs, fs->root_fh, bucket_name.c_str(), &bucket_fh,
		       nullptr, 0, RGW_LOOKUP_FLAG_NONE);
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, LOOKUP_OBJECT) {
  int ret = rgw_lookup(fs, bucket_fh, object_name.c_str(), &object_fh,
		       nullptr, 0, RGW_LOOKUP_FLAG_CREATE);
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, OPEN1) {
  int ret = rgw_open(fs, object_fh, 0 /* posix flags */, RGW_OPEN_FLAG_NONE);
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, PUT_OBJECT) {
  size_t nbytes;
  string data = "hi mom"; // fix this
  int ret = rgw_write(fs, object_fh, 0, data.length(), &nbytes,
                      (void*) data.c_str(), RGW_WRITE_FLAG_NONE);
  ASSERT_EQ(ret, 0);
  ASSERT_EQ(nbytes, data.length());
  /* commit write transaction */
  ret = rgw_close(fs, object_fh, 0 /* flags */);
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, CLOSE1) {
  int ret = rgw_close(fs, object_fh, RGW_CLOSE_FLAG_NONE);
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, OPEN2)
{
  int ret = rgw_open2(fs, object_fh, 0 /* posix flags */,
                     RGW_OPEN_FLAG_NONE);
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, GET_OBJECT) {
  // XXXX do it
}

TEST(LibRGW, CLOSE2) {
  int ret = rgw_close(fs, object_fh, RGW_CLOSE_FLAG_NONE);
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, STAT_OBJECT) {
  struct stat st;
  int ret = rgw_getattr(fs, object_fh, &st, RGW_GETATTR_FLAG_NONE);
  ASSERT_EQ(ret, 0);
  dout(15) << "rgw_getattr on " << object_name << " size = "
	   << st.st_size << dendl;
}

TEST(LibRGW, DELETE_OBJECT) {
  if (do_delete) {
    int ret = rgw_unlink(fs, bucket_fh, object_name.c_str(),
			 RGW_UNLINK_FLAG_NONE);
    ASSERT_EQ(ret, 0);
  }
}

TEST(LibRGW, DELETE_BUCKET) {
  if (do_delete) {
    int ret = rgw_unlink(fs, fs->root_fh, bucket_name.c_str(),
			 RGW_UNLINK_FLAG_NONE);
    ASSERT_EQ(ret, 0);
  }
}

TEST(LibRGW, CLEANUP) {
  int ret;
  if (object_fh) {
    ret = rgw_fh_rele(fs, object_fh, RGW_FH_RELE_FLAG_NONE);
    ASSERT_EQ(ret, 0);
  }
  ret = rgw_fh_rele(fs, bucket_fh, 0 /* flags */);
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, UMOUNT) {
  if (! fs)
    return;

  int ret = rgw_umount(fs, RGW_UMOUNT_FLAG_NONE);
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, SHUTDOWN) {
  librgw_shutdown(rgw);
}

int main(int argc, char *argv[])
{
  auto args = argv_to_vec(argc, argv);
  env_to_vec(args);

  char* v = getenv("AWS_ACCESS_KEY_ID");
  if (v) {
    access_key = v;
  }

  v = getenv("AWS_SECRET_ACCESS_KEY");
  if (v) {
    secret_key = v;
  }

  string val;

  for (auto arg_iter = args.begin(); arg_iter != args.end();) {
    if (ceph_argparse_witharg(args, arg_iter, &val, "--access",
			      (char*) nullptr)) {
      access_key = val;
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--secret",
				     (char*) nullptr)) {
      secret_key = val;
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--userid",
				     (char*) nullptr)) {
      userid = val;
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--bn",
				     (char*) nullptr)) {
      bucket_name = val;
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--uid",
				     (char*) nullptr)) {
      owner_uid = std::stoi(val);
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--gid",
				     (char*) nullptr)) {
      owner_gid = std::stoi(val);
    } else if (ceph_argparse_flag(args, arg_iter, "--verify",
					    (char*) nullptr)) {
      do_verify = true;
    } else if (ceph_argparse_flag(args, arg_iter, "--create",
					    (char*) nullptr)) {
      do_create = true;
    } else if (ceph_argparse_flag(args, arg_iter, "--delete",
					    (char*) nullptr)) {
      do_delete = true;
    } else if (ceph_argparse_flag(args, arg_iter, "--large",
					    (char*) nullptr)) {
      do_large = true;
    } else if (ceph_argparse_flag(args, arg_iter, "--hexdump",
					    (char*) nullptr)) {
      do_hexdump = true;
    } else {
      ++arg_iter;
    }
  }

  /* don't accidentally run as anonymous */
  if ((access_key == "") ||
      (secret_key == "")) {
    std::cout << argv[0] << " no AWS credentials, exiting" << std::endl;
    return EPERM;
  }

  saved_args.argc = argc;
  saved_args.argv = argv;

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
