// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <iostream>
#include <string>
#include <vector>
#include <utility>
#include "rgw/rgw_b64.h"

#include "gtest/gtest.h"
#include "common/ceph_argparse.h"
#include "common/debug.h"
#include "global/global_init.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_rgw

namespace {

  using namespace rgw;

  using std::vector;
  using std::string;
  using std::pair;

  vector<pair<string,string>> test_vector =
  { make_pair("", ""),
    make_pair("V", "Vg=="),
    make_pair("Va", "VmE="),
    make_pair("Van", "VmFu"),
    make_pair("Vanity", "VmFuaXR5"),
    make_pair("Vanity was", "VmFuaXR5IHdhcw=="),
    make_pair("Vanity was the", "VmFuaXR5IHdhcyB0aGU="),
    make_pair("Vanity was the beginning and",
	      "VmFuaXR5IHdhcyB0aGUgYmVnaW5uaW5nIGFuZA=="),
    make_pair("Vanity was the beginning and the end of Sir Walter Elliot's character",
    "VmFuaXR5IHdhcyB0aGUgYmVnaW5uaW5nIGFuZCB0aGUgZW5kIG9mIFNpciBXYWx0ZXIgRWxsaW90J3MgY2hhcmFjdGVy")
  };

} /* namespace */

TEST(B64, ENCODE) {
  for (auto& elt : test_vector) {
    ASSERT_EQ(elt.second, to_base64(elt.first));
  }
}

TEST(B64, ENCODE_BREAK_AT_76) {
  std::string enc_s = "VmFuaXR5IHdhcyB0aGUgYmVnaW5uaW5nIGFuZCB0aGUgZW5kIG9mIFNpciBXYWx0ZXIgRWxsaW90\nJ3MgY2hhcmFjdGVy";
  ASSERT_EQ(to_base64<76>(test_vector.back().first), enc_s);
}

TEST(B64, DECODE) {
  for (auto& elt : test_vector) {
    ASSERT_EQ(elt.first, from_base64(elt.second));
  }
}

int main(int argc, char *argv[])
{
  vector<const char*> args;

  argv_to_vec(argc, const_cast<const char**>(argv), args);
  env_to_vec(args);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
