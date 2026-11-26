// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/* RADOS::pool_is_erasure(), pool_is_crimson() and
 * pool_allows_ec_overwrites():  a pool's type, Crimson flag and EC
 * overwrites flag, read from the client's OSDMap */

#include <coroutine>

#include <boost/system/system_error.hpp>

#include "include/neorados/RADOS.hpp"
#include "test/neorados/common_tests.h"
#include "gtest/gtest.h"

CORO_TEST_F(NeoRadosPoolType, Replicated, NeoRadosTest) {
  EXPECT_FALSE(rados().pool_is_erasure(pool().get_pool()));
  EXPECT_FALSE(rados().pool_is_crimson(pool().get_pool()));
  EXPECT_FALSE(rados().pool_allows_ec_overwrites(pool().get_pool()));
  co_return;
}

CORO_TEST_F(NeoRadosPoolTypeEC, Erasure, NeoRadosECTest) {
  EXPECT_TRUE(rados().pool_is_erasure(pool().get_pool()));
  EXPECT_FALSE(rados().pool_is_crimson(pool().get_pool()));  // a classic OSD's
  EXPECT_FALSE(rados().pool_allows_ec_overwrites(pool().get_pool()));
  co_return;
}

CORO_TEST_F(NeoRadosPoolTypeFastEC, ErasureWithOverwrites, NeoRadosFastECTest) {
  EXPECT_TRUE(rados().pool_is_erasure(pool().get_pool()));
  EXPECT_TRUE(rados().pool_allows_ec_overwrites(pool().get_pool()));
  co_return;
}

CORO_TEST_F(NeoRadosPoolType, UnknownPoolThrows, NeoRadosTest) {
  constexpr int64_t none = 999'999;
  EXPECT_THROW(rados().pool_is_erasure(none), boost::system::system_error);
  EXPECT_THROW(rados().pool_is_crimson(none), boost::system::system_error);
  EXPECT_THROW(rados().pool_allows_ec_overwrites(none),
	       boost::system::system_error);
  co_return;
}
