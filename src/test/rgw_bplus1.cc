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

#include "bplus_ondisk.h"

#define dout_subsys ceph_subsys_rgw

namespace {

  using namespace rgw::bplus;
  using std::get;
  using std::string;

  bool verbose = false;
  static constexpr uint64_t seed = 8675309;

  /* test classes */

  class Bitmap1 : public ::testing::Test {
  public:
  };



} /* namespace */

TEST_F(Bitmap1, test1) {
  ASSERT_EQ(0, 0);
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
