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
#include <vector>
#include <map>
#include <random>
#include "xxhash.h"

#include "gtest/gtest.h"
#include "common/ceph_argparse.h"
#include "common/debug.h"
#include "global/global_init.h"

#define dout_subsys ceph_subsys_rgw

namespace {

  int n_items = 1000000;
  constexpr int seed = 8675309;

  std::uniform_int_distribution<uint64_t> uint_dist;
  std::mt19937 rng;

  struct in6_addr_type
  {
    uint8_t _s6_addr[16];
  };

  struct ipv6_addr
  {
    union {
      struct {
	uint64_t low;
	uint64_t hi;
      } u64;
      in6_addr_type uin6;
    };
    ipv6_addr(uint64_t low, uint64_t hi) {
      u64.low = low;
      u64.hi = hi;
    }
  };

  std::vector<ipv6_addr> addrs;

  struct addr_rec
  {
    uint64_t refs;
    addr_rec() : refs(1) {}
  };

  std::map<uint64_t,addr_rec> cmap;

} /* namespace */

TEST(XXHash, INIT) {
  for (int ix = 0; ix < n_items; ++ix) {
    addrs.push_back(ipv6_addr{uint_dist(rng), uint_dist(rng)});
  }
}

TEST(XXHash, HASH1) {
  uint64_t hk, sup = 0;
  for (auto& addr : addrs) {
    hk = XXH64(&addr, sizeof(ipv6_addr), seed);
    /* naively (i.e., with cost) force evaluation of hk (defeat optimizers) */
    if (hk > sup)
      sup = hk;
  }
  std::cout << "hs = " << sup << " sz = " << sizeof(ipv6_addr) << std::endl;
}

TEST(XXHash, COLLISION) {
  uint64_t hk, sup = 1;
  for (auto& addr : addrs) {
    hk = XXH64(&addr, sizeof(ipv6_addr), seed);
    auto crec = cmap.find(hk);
    if (crec != cmap.end()) {
      ++(crec->second.refs);
      // track most collided value
      if (crec->second.refs > sup)
	sup = crec->second.refs;
    }
    else
      cmap.insert(decltype(cmap)::value_type(hk, addr_rec()));
  }
  std::cout << "hs = " << sup << std::endl;
}

TEST(XXHash, SHUTDOWN) {
  // nothing
}

int main(int argc, char *argv[])
{
  string val;
  vector<const char*> args;

  argv_to_vec(argc, const_cast<const char**>(argv), args);
  env_to_vec(args);

  for (auto arg_iter = args.begin(); arg_iter != args.end();) {
    if (ceph_argparse_witharg(args, arg_iter, &val, "--count",
			      (char*) nullptr)) {
      n_items = std::stoi(val);
    } else {
      ++arg_iter;
    }
  }

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
