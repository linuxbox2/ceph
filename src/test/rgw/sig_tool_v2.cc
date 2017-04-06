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

#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctime>
#include <iostream>
#include <fstream>
#include <boost/filesystem.hpp>

#include "gtest/gtest.h"
#include "common/ceph_argparse.h"
#include "common/debug.h"
#include "global/global_init.h"
#include "common/ceph_crypto.h"
#include "rgw/rgw_auth_s3.h"

#define dout_subsys ceph_subsys_rgw

namespace {

  bool verbose{false};
  std::string secret_key("");
  CephContext *cct(nullptr);

  time_t start_ts = 0;
  uint32_t n_secs = 1;

  std::string timestamp_token = "SIGV2_TIMESTAMP_TOKEN";
  std::string auth_hdr_fn = "auth_hdr.txt";

  std::string get_auth_hdr(const std::string& fn) {
    string data;
    if (boost::filesystem::exists(fn)) {
      std::ifstream af(fn);
      af.seekg(0, ios::end);
      data.reserve(af.tellg());
      af.seekg(0, ios::beg);
      data.assign((std::istreambuf_iterator<char>(af)),
		  std::istreambuf_iterator<char>());
      data.pop_back(); // discard extra NL
    } else {
      std::cerr << "Failed to open auth_hdr file " << fn << std::endl;
    }
    if (verbose) {
      std::cout << "data=@@" << data << "@@" << std::endl;
    }
    return data;
  }

  std::string update_auth_hdr_ts(const std::string& auth_hdr, time_t t) {
    char timebuf[256];
    (void) std::strftime(timebuf, 100, "%a, %d %b %Y %H:%M:%S GMT",
			 std::gmtime(&t));
    std::string ts_string{timebuf};
    std::cout << "converted timestamp: " << ts_string << std::endl;
    std::string new_hdr{auth_hdr};
    size_t pos = new_hdr.find(timestamp_token);
    if (pos != std::string::npos) {
      new_hdr.replace(pos, timestamp_token.length(), ts_string);
    }
    return new_hdr;
  }
  
  struct {
    int argc;
    char **argv;
  } saved_args;
}

TEST(Sigv2, INIT) {
  ASSERT_NE(cct, nullptr);
  ceph::crypto::init(cct);
}

TEST(Sigv2, SIGNIT2) {
  if (start_ts > 0) {
    int ret;
    std::string digest;
    std::string auth_hdr = get_auth_hdr(auth_hdr_fn);
    ASSERT_NE(auth_hdr.size(), 0);
    std::cout << "\nresult:\n"
	      << "auth_hdr_fn=" << auth_hdr_fn
	      << std::endl;
    for (time_t ix = 0; ix < n_secs; ++ix) {
      time_t ts{start_ts + ix};
      std::string new_hdr = update_auth_hdr_ts(auth_hdr, ts);
      std::cout << "new_hdr=@@" << new_hdr << "@@" << std::endl;
      ret = rgw_get_s3_header_digest(new_hdr, secret_key, digest);
      ASSERT_EQ(ret, 0);
      std::cout << " timestamp=" << ts
		<< " calculated digest=" << digest << std::endl;
    }
  }
}

TEST(Sigv2, CLEANUP) {
  // do nothing
}

int main(int argc, char *argv[])
{
  char *v{nullptr};
  std::string val;
  vector<const char*> args;

  argv_to_vec(argc, const_cast<const char**>(argv), args);
  env_to_vec(args);

  v = getenv("AWS_SECRET_ACCESS_KEY");
  if (v) {
    secret_key = v;
  }

  for (auto arg_iter = args.begin(); arg_iter != args.end();) {
    if (ceph_argparse_witharg(args, arg_iter, &val, "--secret",
				     (char*) nullptr)) {
      secret_key = val;
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--auth_hdr",
				     (char*) nullptr)) {
      auth_hdr_fn = val;
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--start_ts",
				     (char*) nullptr)) {
      start_ts = atol(val.c_str());
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--n_secs",
				     (char*) nullptr)) {
      n_secs = atol(val.c_str());
    } else if (ceph_argparse_flag(args, arg_iter, "--verbose",
					    (char*) nullptr)) {
      verbose = true;
    } else {
      ++arg_iter;
    }
  }

  /* dont accidentally run as anonymous */
  if (secret_key == "") {
    std::cout << argv[0] << " no AWS credentials, exiting" << std::endl;
    return EPERM;
  }

  saved_args.argc = argc;
  saved_args.argv = argv;

  vector<const char *> def_args;
  int flags = CINIT_FLAG_UNPRIVILEGED_DAEMON_DEFAULTS;
  auto ip_cct = global_init(&def_args, args,
			    CEPH_ENTITY_TYPE_CLIENT,
			    CODE_ENVIRONMENT_DAEMON,
			    flags, saved_args.argv[0], true /* run preinit */);
  cct = ip_cct.get();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
