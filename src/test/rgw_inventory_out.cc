// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
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
#include "fmt/format.h"
#include "include/function2.hpp"

#include "include/rados/librgw.h"
#include "include/rados/rgw_file.h"
#include "rgw/rgw_file_int.h"
#include "rgw/rgw_lib.h"
#include "rgw/rgw_sal.h"
#include "rgw/rgw_s3inventory.h"

#include "gtest/gtest.h"
#include "common/ceph_argparse.h"
#include "common/errno.h"
#include "common/debug.h"
#include "global/global_init.h"
#include "include/ceph_assert.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

using namespace std;

namespace {

  using namespace rgw;

  string uid("testuser");
  string access_key("");
  string secret_key("");

  librgw_t rgw_h{nullptr};
  struct rgw_fs* fs{nullptr};
  RGWLibFS* rlfs{nullptr};

  uint32_t owner_uid = 867;
  uint32_t owner_gid = 5309;
  uint32_t create_mask = RGW_SETATTR_UID | RGW_SETATTR_GID | RGW_SETATTR_MODE;
  uint32_t nobjects = 1010;

  string bucket_name{"inventory1"};

  struct rgw_file_handle* bucket_fh{nullptr};
  struct rgw_file_handle* object_fh{nullptr};

  DoutPrefixProvider* dpp{nullptr};

  rgw::sal::Driver* driver;
  rgw::sal::User* user;
  std::unique_ptr<rgw::sal::Bucket> bucket;

  bool do_stat = false;
  bool do_create = false;
  bool do_delete = false;
  bool verbose = false;

  struct {
    int argc;
    char **argv;
  } saved_args;

void put_object(const std::string& name, const std::string data) {
  
  int ret{0};
  size_t nbytes;

  ret = rgw_lookup(fs, bucket_fh, name.c_str(), &object_fh,
		   nullptr, 0, RGW_LOOKUP_FLAG_CREATE);
  ASSERT_EQ(ret, 0);
  ret = rgw_open(fs, object_fh, 0 /* posix flags */, 0 /* flags */);
  ASSERT_EQ(ret, 0);
  ret = rgw_write(fs, object_fh, 0, data.length(), &nbytes,
			(void*) data.c_str(), RGW_WRITE_FLAG_NONE);
  ASSERT_EQ(ret, 0);
  ASSERT_EQ(nbytes, data.length());
  /* commit write transaction */
  ret = rgw_close(fs, object_fh, 0 /* flags */);
  ASSERT_EQ(ret, 0);
}

void list_bucket(const fu2::unique_function<void(const rgw_bucket_dir_entry&) const>& func)
{
  int ret{0};
  rgw::sal::Bucket::ListParams list_params{};
  rgw::sal::Bucket::ListResults list_results;
  std::vector<rgw_bucket_dir_entry>::iterator obj_iter;

  bool first = true;
  list_params.allow_unordered = false;

next_page:
  /* XXX entering bucket->list() with list_results.is_truncated ==
   * true causes a non-terminating loop (at least w/dbstore?)--seems broken */
  ret = bucket->list(dpp, list_params, 1000, list_results, null_yield);
  ASSERT_EQ(ret, 0);

  obj_iter = list_results.objs.begin();
  while (obj_iter != list_results.objs.end()) {
    /* XXX without this check, re-entering bucket->list() with the
     * last key as marker presents that key again.  I thought that
     * searching from K returned a result list starting at K+1? */
    if ((! first) &&
	(obj_iter == list_results.objs.begin())) {
      ++obj_iter;
    }
    auto& obj = *obj_iter;
    list_params.marker = obj.key;
    func(obj);
    ++obj_iter;
  }
  if (list_results.is_truncated) {
    first = false;
    /* XXX entering bucket->list() with list_results.is_truncated ==
     * true causes a non-terminating loop--seems broken */
    list_results.is_truncated = false;
    goto next_page;
  }
} /* list_bucket */

} /* namespace */

TEST(InvOut, INIT) {
  int ret = librgw_create(&rgw_h, saved_args.argc, saved_args.argv);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(rgw_h, nullptr);

  ret = rgw_mount(rgw_h, uid.c_str(), access_key.c_str(),
		  secret_key.c_str(), &fs, RGW_MOUNT_FLAG_NONE);
  ASSERT_EQ(ret, 0);
  ASSERT_NE(fs, nullptr);

  /* alias g_rgwlib for use later */
  dpp = g_rgwlib;
  ASSERT_NE(dpp, nullptr);

  ret = rgw_lookup(fs, fs->root_fh, bucket_name.c_str(), &bucket_fh,
		   nullptr, 0, RGW_LOOKUP_FLAG_NONE);

  if ((! bucket_fh) && do_create) {
    struct stat st;

    st.st_uid = owner_uid;
    st.st_gid = owner_gid;
    st.st_mode = 755;
    
    int ret = rgw_mkdir(fs, fs->root_fh, bucket_name.c_str(), &st, create_mask,
			&bucket_fh, RGW_MKDIR_FLAG_NONE);
    ASSERT_EQ(ret, 0);
  }

  if (do_create) {
    /* upload some objects */
    for (auto ix = 0; ix < nobjects; ++ix) {
      std::string oname = fmt::format("obj_{}", ix);
      std::string oval = fmt::format("value for obj_{}", ix);
      (void) put_object(oname, oval);
    }
  }

  /* set up sal handles from active fs mount, for use with rgw
   * internal apis */
  driver = g_rgwlib->get_driver();
  rlfs = static_cast<RGWLibFS*>(fs->fs_private);
  user = rlfs->get_user();
  ret = driver->get_bucket(dpp, user, user->get_tenant(), bucket_name, &bucket, null_yield);
} /* Init */

TEST(InvOut, Listdirs)
{
  uint32_t count{0};
  list_bucket([&](const rgw_bucket_dir_entry& obj) -> void {
    if (verbose) {
      std::cout << "Listdirs: obj: "
		<< "name: " << obj.key.name << " instance: " << obj.key.instance
		<< std::endl;
    }
    ++count;
  });
  ASSERT_EQ(count, nobjects);
} /* Listdirs */

namespace ri = rgw::inventory;

TEST(InvOut, GEN1)
{
  int ret{0};
  ri::Engine eng{dpp};
  ret = eng.generate(bucket.get(), ri::output_format::parquet);
  ASSERT_EQ(ret, 0);
}

/* TODO: finish */

TEST(InvOut, CLEANUP) {
  int ret = 0;
  if (object_fh) {
    ret = rgw_fh_rele(fs, object_fh, 0 /* flags */);
    ASSERT_EQ(ret, 0);
  }
  if (bucket_fh) {
    ret = rgw_fh_rele(fs, bucket_fh, 0 /* flags */);
  }
  ASSERT_EQ(ret, 0);

  if (! fs)
    return;

  ret = rgw_umount(fs, RGW_UMOUNT_FLAG_NONE);
  ASSERT_EQ(ret, 0);

  librgw_shutdown(rgw_h);
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
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--uid",
				     (char*) nullptr)) {
      uid = val;
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--bn",
				     (char*) nullptr)) {
      bucket_name = val;
    } else if (ceph_argparse_flag(args, arg_iter, "--stat",
					    (char*) nullptr)) {
      do_stat = true;
    } else if (ceph_argparse_flag(args, arg_iter, "--create",
					    (char*) nullptr)) {
      do_create = true;
    } else if (ceph_argparse_flag(args, arg_iter, "--delete",
					    (char*) nullptr)) {
      do_delete = true;
    } else if (ceph_argparse_flag(args, arg_iter, "--verbose",
					    (char*) nullptr)) {
      verbose = true;
    } else {
      ++arg_iter;
    }
  }

  /* dont accidentally run as anonymous */
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
