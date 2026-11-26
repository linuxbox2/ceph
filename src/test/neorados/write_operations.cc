// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2023 IBM
 *
 * See file COPYING for license information.
 *
 */

#include <coroutine>
#include <cstring>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <boost/system/error_code.hpp>
#include <boost/system/errc.hpp>

#include <fmt/format.h>

#include "include/neorados/RADOS.hpp"

#include "osd/error_code.h"

#include "test/neorados/common_tests.h"

#include "gtest/gtest.h"

#include "cls/hello/cls_hello_ops.h"

namespace sys = boost::system;

using namespace std::literals;

using neorados::cmp_op;
using neorados::ReadOp;
using neorados::WriteOp;

constexpr auto oid = "test"sv;

CORO_TEST_F(NeoRadosWriteOps, AssertExists, NeoRadosTest) {
  co_await expect_error_code(execute(oid, WriteOp{}.assert_exists()),
			     sys::errc::no_such_file_or_directory);
  co_await execute(oid, WriteOp{}.create(true));
  co_await execute(oid, WriteOp{}.assert_exists());
  co_return;
}

CORO_TEST_F(NeoRadosWriteOps, AssertVersion, NeoRadosTest) {
  co_await execute(oid, WriteOp{}.create(true));
  std::uint64_t v;
  // Write to the object a second time to guarantee that its
  // version number is greater than 0
  co_await execute(oid, WriteOp{}.write_full(to_buffer_list("hi")), &v);

  co_await expect_error_code(execute(oid, WriteOp{}.assert_version(v + 1)),
			     sys::errc::value_too_large);
  co_await expect_error_code(execute(oid, WriteOp{}.assert_version(v - 1)),
			     sys::errc::result_out_of_range);
  co_await execute(oid, WriteOp{}.assert_version(v));
  co_return;
}

CORO_TEST_F(NeoRadosWriteOps, Xattrs, NeoRadosTest) {
  // Hey, the compiler won't check that I don't have typos in my strings…
  static constexpr auto key = "key"sv;
  const auto value = to_buffer_list("value");
  // Create an object with an xattr
  co_await execute(oid, WriteOp{}
		   .create(true)
		   .setxattr(key, value));
  // Check that xattr exists, if it does, delete it.
  co_await execute(oid, WriteOp{}
		   .cmpxattr("key", cmp_op::eq, value)
		   .rmxattr(key));

  // Check the xattr exits, if it does, add it again (will fail)
  co_await expect_error_code(execute(oid, WriteOp{}
				     .cmpxattr(key, cmp_op::eq, value)
				     .setxattr(key, value)),
			     sys::errc::operation_canceled);

  co_return;
}

CORO_TEST_F(NeoRadosWriteOps, Write, NeoRadosTest) {
  // Create an object, write and write full to it
  {
    const auto value = to_buffer_list("hi");
    co_await execute(oid, WriteOp{}
		     .write(0, to_buffer_list("four"))
		     .write_full(value));
    auto bl = co_await read(oid);
    EXPECT_EQ(value, bl);
  }
  // Create write op with I/O hint
  {
    const auto value = to_buffer_list("ceph");
    co_await execute(oid, WriteOp{}
		     .write_full(value)
		     .set_fadvise_nocache());
    auto bl = co_await read(oid);
    EXPECT_EQ(value, bl);
  }
  // Truncate and append
  {
    co_await execute(oid, WriteOp{}
		     .truncate(1)
		     .append(to_buffer_list("hi")));
    auto bl = co_await read(oid);
    EXPECT_EQ(to_buffer_list("chi"), bl);
  }
  // Zero and remove
  {
    co_await execute(oid, WriteOp{}
		     .zero(0, 3)
		     .remove());
    co_await expect_error_code(execute(oid, ReadOp{}.read(0, 0, nullptr)),
			       sys::errc::no_such_file_or_directory);
  }

  co_return;
}

CORO_TEST_F(NeoRadosWriteOps, Exec, NeoRadosTest) {
  co_await execute(oid, WriteOp{}
		   .exec(::cls::hello::method::record_hello, to_buffer_list("test")));
  const auto bl = co_await read(oid);
  EXPECT_EQ(to_buffer_list("Hello, test!"), bl);
  co_return;
}

CORO_TEST_F(NeoRadosWriteOps, WriteSame, NeoRadosTest) {
  co_await execute(oid, WriteOp{}
		   .writesame(0, 4 * 4, // Total bytes, not total copies
			      to_buffer_list("four")));
  const auto bl = co_await read(oid);
  EXPECT_EQ(to_buffer_list("fourfourfourfour"), bl);
  co_return;
}

CORO_TEST_F(NeoRadosWriteOps, CmpExt, NeoRadosTest) {
  static const auto four = to_buffer_list("four");
  static const auto five = to_buffer_list("five");
  static const auto six = to_buffer_list("six");

  // Create an object, write to it
  {
    co_await execute(oid, WriteOp{}
		     .create(true)
		     .write_full(four));
    const auto bl = co_await read(oid);
    EXPECT_EQ(four, bl);
  }
  // Compare and overwrite on (expected) match
  {
    uint64_t unmatch = 0;
    co_await execute(oid, WriteOp{}
		     .cmpext(0, four, &unmatch)
		     .write(0, five));
    const auto bl = co_await read(oid);
    EXPECT_EQ(five, bl);
    EXPECT_EQ(-1, unmatch);
  }
  // check offset return error value
  {
    uint64_t unmatch = -2;
    co_await expect_error_code(execute(oid, WriteOp()
				       .cmpext(0, four, &unmatch)
				       .write(0, six)
				       .returnvec()),
			       osd_errc::cmpext_mismatch);
    // 'four' mistmatches 'five' on character 1.
    EXPECT_EQ(1, unmatch);
  }
  // Compare and bail before write due to mismatch. Do it a thousand
  // times to make sure we are hitting some socket injection
  for (auto i = 0; i < 1000; ++i) {
    uint64_t unmatch = -2;
    co_await expect_error_code(execute(fmt::format("test_{}", i), WriteOp()
				       .cmpext(0, four, &unmatch)
				       .write(0, six)
				       .returnvec()),
			       osd_errc::cmpext_mismatch);
    EXPECT_EQ(0, unmatch);
    EXPECT_EQ(0, unmatch);
  }
  co_return;
}

CORO_TEST_F(NeoRadosWriteOps, Cancel, NeoRadosTest) {
  using namespace boost::asio::experimental::awaitable_operators;
  auto bl = filled_buffer_list(0x33, 4 * 1 << 20);
  co_await (execute(oid, WriteOp{}.write_full(std::move(bl))) || wait_for(1us));
  co_return;
}

/* CEPH_OSD_OP_CLONE_TO:  the destination is a sibling named <oid>~<id>, and
 * it is reached with oid as its locator key */
static std::string sibling_name(std::string_view anchor, std::uint64_t counter)
{
  return fmt::format("{}~{:016x}{:016x}", anchor, counter,
		     std::uint64_t{0xfeedfacecafebeef});
}

static neorados::IOContext keyed(const neorados::IOContext& pool,
				 std::string_view key)
{
  auto ioc = pool;
  ioc.set_key(std::string{key});
  return ioc;
}

/* CLONE_TO on every pool kind:  replicated, and erasure-coded on each
 * backend, the legacy one with and without overwrites (without them its
 * hash info keeps per-chunk hashes) */
enum class CloneToPool {
  replicated,
  ec_legacy,
  ec_legacy_overwrites,
  ec_optimized,
};

class NeoRadosCloneTo : public NeoRadosTestBase,
			public ::testing::WithParamInterface<CloneToPool> {
protected:
  /* whether a write may land inside the object, not only append */
  bool overwrites() const {
    return GetParam() != CloneToPool::ec_legacy;
  }

private:
  /* the flags each EC value stands for:  {overwrites, optimizations} */
  static std::pair<bool, bool> ec_flags(CloneToPool p) {
    switch (p) {
    case CloneToPool::ec_legacy:            return {false, false};
    case CloneToPool::ec_legacy_overwrites: return {true, false};
    case CloneToPool::ec_optimized:         return {true, true};
    default:                                return {false, false};
    }
  }

  boost::asio::awaitable<bool> pool_flag(std::string var) {
    // Workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=110913
    std::vector<std::string> get = {
      fmt::format(
	R"({{"prefix": "osd pool get", "pool": "{}", "var": "{}", "format": "json"}})",
	pool_name(), var)
    };
    ceph::buffer::list out;
    co_await rados().mon_command(std::move(get), {}, nullptr, &out,
				 boost::asio::use_awaitable);
    co_return out.to_str().find(fmt::format(R"("{}":true)", var)) !=
      std::string::npos;
  }

  boost::asio::awaitable<uint64_t> create_pool() override {
    namespace asio = boost::asio;
    switch (GetParam()) {
    case CloneToPool::replicated:
      co_return co_await ::create_pool(rados(), pool_name(),
				       asio::use_awaitable);
    case CloneToPool::ec_optimized: {
      const auto id = co_await ::create_fast_ec_pool(rados(), pool_name(),
						     asio::use_awaitable);
      co_await expect_ec_flags();
      co_return id;
    }
    default:
      break;
    }
    /* as NeoRadosECTest::create_pool(), with the names
     * delete_fast_ec_pool() removes */
    // Workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=110913
    std::vector<std::string> profile_set = {
      fmt::format(
	R"({{"prefix": "osd erasure-code-profile set", "name": "testprofile-{}", )"
	R"( "profile": [ "k=2", "m=1", "crush-failure-domain=osd"]}})",
	pool_name())
    };
    co_await rados().mon_command(std::move(profile_set), {}, nullptr, nullptr,
				 asio::use_awaitable);
    std::vector<std::string> pool_create = {
      fmt::format(
	R"({{"prefix": "osd pool create", "pool": "{}", "pool_type":"erasure", )"
	R"("pg_num":8, "pgp_num":8, "erasure_code_profile":"testprofile-{}"}})",
	pool_name(), pool_name())
    };
    co_await rados().mon_command(std::move(pool_create), {}, nullptr, nullptr,
				 asio::use_awaitable);
    if (GetParam() == CloneToPool::ec_legacy_overwrites) {
      std::vector<std::string> set_overwrites = {
	fmt::format(
	  R"({{"prefix": "osd pool set", "pool": "{}", "var": "allow_ec_overwrites", "val": "true"}})",
	  pool_name())
      };
      co_await rados().mon_command(std::move(set_overwrites), {}, nullptr,
				   nullptr, asio::use_awaitable);
    }
    co_await expect_ec_flags();
    co_await rados().wait_for_latest_osd_map(asio::use_awaitable);
    co_return co_await rados().lookup_pool(pool_name(), asio::use_awaitable);
  }

  /* the pool is what its value says:  the legacy pools' optimizations
   * come from the mon's default (osd_pool_default_flag_ec_optimizations) */
  boost::asio::awaitable<void> expect_ec_flags() {
    const auto [overwrites, optimizations] = ec_flags(GetParam());
    EXPECT_EQ(overwrites, co_await pool_flag("allow_ec_overwrites"))
      << pool_name() << ":  allow_ec_overwrites";
    EXPECT_EQ(optimizations, co_await pool_flag("allow_ec_optimizations"))
      << pool_name() << ":  allow_ec_optimizations";
  }

  boost::asio::awaitable<void> clean_pool() override {
    namespace asio = boost::asio;
    if (GetParam() == CloneToPool::replicated) {
      co_await rados().delete_pool(pool().get_pool(), asio::use_awaitable);
    } else {
      co_await ::delete_fast_ec_pool(rados(), pool().get_pool(), pool_name(),
				     asio::use_awaitable);
    }
  }
};

INSTANTIATE_TEST_SUITE_P(
  , NeoRadosCloneTo,
  ::testing::Values(CloneToPool::replicated, CloneToPool::ec_legacy,
		    CloneToPool::ec_legacy_overwrites,
		    CloneToPool::ec_optimized),
  [](const ::testing::TestParamInfo<CloneToPool>& info) -> std::string {
    switch (info.param) {
    case CloneToPool::replicated:           return "Replicated";
    case CloneToPool::ec_legacy:            return "ECLegacy";
    case CloneToPool::ec_legacy_overwrites: return "ECLegacyOverwrites";
    case CloneToPool::ec_optimized:         return "ECOptimized";
    }
    return "Unknown";
  });

CORO_TEST_P(NeoRadosCloneTo, CloneTo) {
  const auto dest = sibling_name(oid, 1);
  co_await execute(oid, WriteOp{}
		   .create(true)
		   .write_full(to_buffer_list("hello, sibling"))
		   .setxattr("keep", to_buffer_list("k"))
		   .setxattr("fsio.open", to_buffer_list("x")));
  co_await execute(oid, WriteOp{}.clone_to(dest, {"fsio."}));

  const auto dioc = keyed(pool(), oid);
  ceph::buffer::list got;
  co_await execute(dest, ReadOp{}.read(0, 0, &got), dioc);
  EXPECT_EQ("hello, sibling", got.to_str());

  /* a kept attribute follows the clone;  a dropped one does not */
  ceph::buffer::list kv;
  co_await execute(dest, ReadOp{}.get_xattr("keep", &kv), dioc);
  EXPECT_EQ("k", kv.to_str());
  kv.clear();
  co_await expect_error_code(
    execute(dest, ReadOp{}.get_xattr("fsio.open", &kv), dioc),
    sys::errc::no_message_available);

  /* the clone is independent of its source */
  co_await execute(oid, WriteOp{}.write_full(to_buffer_list("changed")));
  got.clear();
  co_await execute(dest, ReadOp{}.read(0, 0, &got), dioc);
  EXPECT_EQ("hello, sibling", got.to_str());
  co_return;
}

/* set_attrs land on the destination after the drops, so one under a
 * dropped prefix is kept;  the source is untouched */
CORO_TEST_P(NeoRadosCloneTo, CloneToSetAttrs) {
  const auto dest = sibling_name(oid, 1);
  co_await execute(oid, WriteOp{}
		   .create(true)
		   .write_full(to_buffer_list("a"))
		   .setxattr("fsio.hdr", to_buffer_list("h")));
  co_await execute(oid, WriteOp{}.clone_to(
		     dest, {"fsio."},
		     {{"fsio.gen", to_buffer_list("g")},
		      {"other", to_buffer_list("o")}}));

  const auto dioc = keyed(pool(), oid);
  ceph::buffer::list v;
  co_await execute(dest, ReadOp{}.get_xattr("fsio.gen", &v), dioc);
  EXPECT_EQ("g", v.to_str());
  v.clear();
  co_await execute(dest, ReadOp{}.get_xattr("other", &v), dioc);
  EXPECT_EQ("o", v.to_str());
  v.clear();
  co_await expect_error_code(
    execute(dest, ReadOp{}.get_xattr("fsio.hdr", &v), dioc),
    sys::errc::no_message_available);

  v.clear();
  co_await execute(oid, ReadOp{}.get_xattr("fsio.hdr", &v));
  EXPECT_EQ("h", v.to_str());
  v.clear();
  co_await expect_error_code(
    execute(oid, ReadOp{}.get_xattr("fsio.gen", &v)),
    sys::errc::no_message_available);
  co_return;
}

CORO_TEST_F(NeoRadosWriteOps, CloneToSetAttrsNameTooLong, NeoRadosTest) {
  const auto dest = sibling_name(oid, 1);
  co_await execute(oid, WriteOp{}.create(true).write_full(to_buffer_list("a")));
  co_await expect_error_code(
    execute(oid, WriteOp{}.clone_to(
	      dest, {}, {{std::string(4096, 'n'), to_buffer_list("v")}})),
    sys::errc::filename_too_long);
  /* nothing was cloned */
  std::uint64_t size = 0;
  co_await expect_error_code(
    execute(dest, ReadOp{}.stat(&size, nullptr), keyed(pool(), oid)),
    sys::errc::no_such_file_or_directory);
  co_return;
}

/* a destination name over osd_max_object_name_len fails as do_op() fails
 * an op's own target name, and nothing is created */
CORO_TEST_F(NeoRadosWriteOps, CloneToDestNameTooLong, NeoRadosTest) {
  const auto maxlen = rados().cct()->_conf->osd_max_object_name_len;
  /* oid's name as a prefix, so the destination still sorts after it */
  const auto dest = std::string{oid} + std::string(maxlen + 1 - oid.size(), 'z');
  co_await execute(oid, WriteOp{}.create(true).write_full(to_buffer_list("a")));
  co_await expect_error_code(
    execute(oid, WriteOp{}.clone_to(dest, {}, {})),
    sys::errc::filename_too_long);
  co_return;
}

/* two clones to one destination in one op fail with EINVAL, not an OSD
 * assert */
CORO_TEST_F(NeoRadosWriteOps, CloneToTwiceInOneOp, NeoRadosTest) {
  const auto dest = sibling_name(oid, 1);
  co_await execute(oid, WriteOp{}.create(true).write_full(to_buffer_list("a")));
  co_await expect_error_code(
    execute(oid, WriteOp{}.clone_to(dest).clone_to(dest)),
    sys::errc::invalid_argument);
  std::uint64_t size = 0;
  co_await expect_error_code(
    execute(dest, ReadOp{}.stat(&size, nullptr), keyed(pool(), oid)),
    sys::errc::no_such_file_or_directory);
  co_return;
}

/* the destination is the source as it was before the op:  its data and
 * its object info agree, whatever earlier ops in the same op did */
CORO_TEST_P(NeoRadosCloneTo, CloneToAfterWriteInOneOp) {
  const auto dest = sibling_name(oid, 1);
  co_await execute(oid, WriteOp{}.create(true).write_full(to_buffer_list("before")));
  co_await execute(oid, WriteOp{}
		   .write_full(to_buffer_list("after, and longer"))
		   .clone_to(dest));
  const auto dioc = keyed(pool(), oid);
  ceph::buffer::list got;
  co_await execute(dest, ReadOp{}.read(0, 0, &got), dioc);
  EXPECT_EQ("before", got.to_str());
  std::uint64_t size = 0;
  co_await execute(dest, ReadOp{}.stat(&size, nullptr), dioc);
  EXPECT_EQ(6u, size);
  co_return;
}

/* a write to the destination after the clone builds on what the clone
 * gave it:  its attr cache and, on the legacy backend, its hash info,
 * whose size that backend asserts against the object's;  both objects
 * then read back as written.  Writes are whole 8 KiB stripes (k=2, 4 KiB
 * chunks), as appends to a legacy pool without overwrites must be */
CORO_TEST_P(NeoRadosCloneTo, CloneToThenWrite) {
  constexpr std::size_t stripe = 8192;
  const auto dest = sibling_name(oid, 1);
  const auto dioc = keyed(pool(), oid);
  const std::string a(stripe, 'a'), b(stripe, 'b');
  co_await execute(oid, WriteOp{}
		   .create(true)
		   .write_full(to_buffer_list(a))
		   .setxattr("keep", to_buffer_list("k")));
  co_await execute(oid, WriteOp{}.clone_to(dest, {}, {{"set", to_buffer_list("s")}}));

  std::string want = a + b;
  co_await execute(dest, WriteOp{}.append(to_buffer_list(b)), dioc);
  if (overwrites()) {
    /* across the chunk boundary, inside the first stripe */
    const std::string c(200, 'c');
    co_await execute(dest, WriteOp{}.write(4000, to_buffer_list(c)), dioc);
    want.replace(4000, c.size(), c);
  }
  co_await execute(dest, WriteOp{}.setxattr("after", to_buffer_list("x")), dioc);

  ceph::buffer::list got;
  co_await execute(dest, ReadOp{}.read(0, 0, &got), dioc);
  EXPECT_EQ(want, got.to_str());
  std::uint64_t size = 0;
  co_await execute(dest, ReadOp{}.stat(&size, nullptr), dioc);
  EXPECT_EQ(want.size(), size);
  for (const auto& [name, val] : {std::pair{"keep", "k"}, {"set", "s"},
				  {"after", "x"}}) {
    ceph::buffer::list v;
    co_await execute(dest, ReadOp{}.get_xattr(name, &v), dioc);
    EXPECT_EQ(val, v.to_str()) << name;
  }

  /* the source's own hash info is its own:  an append to it lands */
  co_await execute(oid, WriteOp{}.append(to_buffer_list(b)));
  got.clear();
  co_await execute(oid, ReadOp{}.read(0, 0, &got));
  EXPECT_EQ(a + b, got.to_str());
  ceph::buffer::list v;
  co_await expect_error_code(execute(oid, ReadOp{}.get_xattr("set", &v)),
			     sys::errc::no_message_available);
  co_return;
}

CORO_TEST_F(NeoRadosWriteOps, CloneToExists, NeoRadosTest) {
  const auto dest = sibling_name(oid, 1);
  co_await execute(oid, WriteOp{}.create(true).write_full(to_buffer_list("a")));
  co_await execute(oid, WriteOp{}.clone_to(dest));
  co_await expect_error_code(execute(oid, WriteOp{}.clone_to(dest)),
			     sys::errc::file_exists);
  co_return;
}

/* a destination that sorts before its source could be left missing on a
 * backfill target, so the OSD returns EINVAL */
CORO_TEST_F(NeoRadosWriteOps, CloneToSortsFirst, NeoRadosTest) {
  co_await execute(oid, WriteOp{}.create(true).write_full(to_buffer_list("a")));
  const std::string before{oid.substr(0, oid.size() - 1)};
  co_await expect_error_code(execute(oid, WriteOp{}.clone_to(before)),
			     sys::errc::invalid_argument);
  co_return;
}

CORO_TEST_F(NeoRadosWriteOps, CloneToNoSource, NeoRadosTest) {
  co_await expect_error_code(
    execute(oid, WriteOp{}.clone_to(sibling_name(oid, 1))),
    sys::errc::no_such_file_or_directory);
  co_return;
}
