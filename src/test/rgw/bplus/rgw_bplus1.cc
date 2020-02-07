// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

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

#include "gtest/gtest.h"

#include <errno.h>
#include <iostream>
#include <string>
#include <vector>
#include <optional>
#include <boost/program_options.hpp>
#include "xxhash.h"

#include "mock_cls.h" /* include before any header including objclass.h */
#include "bplus_types.h"
#include "bplus_io.h"


#define dout_subsys ceph_subsys_rgw

namespace {

  using namespace rgw::bplus;
  using namespace rgw::bplus::ondisk;

  using std::get;
  using std::string;

  bool verbose = false;
  static constexpr uint64_t seed = 8675309;

  /* test classes */

  class Bitmap1 : public ::testing::Test {
  public:
  };

  class NameGen1 {
  public:
    char alpha{'a'};
    uint32_t ix1{0};
    uint32_t ix2{0};
    std::string next() {
      std::string name;
      name += alpha;
      name += "_lvl" + std::to_string(ix1) + "_file" + std::to_string(ix2);
      if (++ix1 > 100) {
	ix1 = 0;
      }
      if (++ix2 > 10) {
	ix2 = 0;
	if (++(++alpha) > 'z')
	  alpha = 'a';
      }
      return name;
    }
  };

  class TreeOps1 : public ::testing::Test {
  public:
    string oid{"tree1"};
    MockHctx mh{oid};
    cls_method_context_t hctx = &mh;
  };

} /* namespace */

TEST_F(Bitmap1, test1) {
  using namespace rgw::bplus::ondisk;
  Bitmap<500> bmap;
  std::cout << "bmap size: " << bmap.data.size() << std::endl;
  ASSERT_EQ(bmap.test(13), false);
  bmap.set(13);
  std::cout << "1 chunk0: " << bmap.data[0] << std::endl;
  ASSERT_EQ(bmap.test(13), true);
  ASSERT_EQ(bmap.data[0], uint64_t(1) << 13);
  bmap.set(63);
  std::cout << "2 chunk0: " << bmap.data[0] << std::endl;
  ASSERT_EQ(bmap.test(63), true);
  bmap.clear(13);
  ASSERT_EQ(bmap.data[0], uint64_t(1) << 63);
}

TEST_F(TreeOps1, names1) {
  NameGen1 ng;
  for (int ix = 0; ix < 512; ++ix) {
    std::cout << "name: " << ix << " "
	      << ng.next() << std::endl;
  }
}

TEST_F(TreeOps1, test1) {
  BTreeIO* bt = btree_cache.get_tree(oid, hctx);
  ASSERT_NE(bt, nullptr);
}

int main(int argc, char **argv)
{
  int code = 0;
  namespace po = boost::program_options;

  po::options_description opts("program options");
  po::variables_map vm;

  try {

    opts.add_options()
      ("verbose", "be verbose about things")
      ;

    po::variables_map::iterator vm_iter;
    po::store(po::parse_command_line(argc, argv, opts), vm);

    if (vm.count("verbose")) {
      verbose = true;
    }

    po::notify(vm);

    ::testing::InitGoogleTest(&argc, argv);
    code = RUN_ALL_TESTS();
  }

  catch(po::error& e) {
    std::cout << "Error parsing opts " << e.what() << std::endl;
  }

  catch(...) {
    std::cout << "Unhandled exception in main()" << std::endl;
  }

  return code;
}
