// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/* FSIO's head methods on an OSD (FSIO_RADOS_DESIGN.md §2.2, §2.7, §8):
 * the records each writes and removes, its reply, and the clone it
 * makes;  the transitions themselves are unittest_cls_rgw_fsio's. */

#include <coroutine>
#include <string>

#include <boost/system/errc.hpp>
#include <fmt/format.h>

#include "include/neorados/RADOS.hpp"
#include "neorados/cls/rgw_fsio.h"
#include "test/neorados/common_tests.h"
#include "gtest/gtest.h"

namespace sys = boost::system;
namespace f = neorados::cls::rgw_fsio;
using neorados::ReadOp;
using neorados::WriteOp;

namespace {

using namespace std::literals;

/* each test has its own pool, so one key serves */
constexpr auto oid = "key"sv;
constexpr cls_rgw_fsio_handle GW1{1, 1};
const std::string MARKER = "zone.4125.1";

/* an RGW head:  <marker>_<key>, with its ID tag and manifest */
std::string head_of(std::string_view oid)
{
  return fmt::format("{}_{}", MARKER, oid);
}

WriteOp make_head()
{
  return std::move(WriteOp{}
                   .create(true)
                   .write_full(to_buffer_list("data"))
                   .setxattr("user.rgw.idtag", to_buffer_list("t0"))
                   .setxattr("user.rgw.manifest", to_buffer_list("C0")));
}

cls_rgw_fsio_join_op join_op(uint64_t seq, bool writer,
                             std::string marker = MARKER)
{
  cls_rgw_fsio_join_op op;
  op.caller = GW1;
  op.seq = seq;
  op.writer = writer;
  op.nonce = 1;
  op.counter_seed = 10;
  op.marker = std::move(marker);
  return op;
}

const std::string HDR = "user.rgw.fsio.hdr";
const std::string M1 = fmt::format("user.rgw.fsio.m.{:016x}", GW1.incarnation);

} // anonymous namespace

CORO_TEST_F(NeoRadosClsRgwFsio, AbsentHeadIsEnoent, NeoRadosTest) {
  co_await expect_error_code(
    execute(head_of(oid), WriteOp{}.exec(f::join(join_op(1, false)))),
    sys::errc::no_such_file_or_directory);
  co_return;
}

CORO_TEST_F(NeoRadosClsRgwFsio, ReaderJoinAndLeave, NeoRadosTest) {
  const auto head = head_of(oid);
  co_await execute(head, make_head());
  cls_rgw_fsio_join_ret jret;
  co_await execute(head, WriteOp{}.exec(f::join(join_op(1, false), &jret)));
  EXPECT_FALSE(jret.construct);
  EXPECT_FALSE(jret.already_applied);
  ceph::buffer::list v;
  co_await execute(head, ReadOp{}.get_xattr(HDR, &v));
  EXPECT_GT(v.length(), 0u);
  v.clear();
  co_await execute(head, ReadOp{}.get_xattr(M1, &v));
  EXPECT_GT(v.length(), 0u);

  cls_rgw_fsio_leave_op lop;
  lop.caller = GW1;
  lop.seq = 2;
  lop.cs = jret.bound;
  cls_rgw_fsio_leave_ret lret;
  co_await execute(head, WriteOp{}.exec(f::leave(lop, &lret)));
  EXPECT_FALSE(lret.already_applied);
  /* its last state on the head:  its record and the header go */
  v.clear();
  co_await expect_error_code(execute(head, ReadOp{}.get_xattr(M1, &v)),
                             sys::errc::no_message_available);
  co_await expect_error_code(execute(head, ReadOp{}.get_xattr(HDR, &v)),
                             sys::errc::no_message_available);
  co_return;
}

CORO_TEST_F(NeoRadosClsRgwFsio, ResendIsAlreadyApplied, NeoRadosTest) {
  const auto head = head_of(oid);
  co_await execute(head, make_head());
  cls_rgw_fsio_join_ret first, again;
  co_await execute(head, WriteOp{}.exec(f::join(join_op(1, false), &first)));
  co_await execute(head, WriteOp{}.exec(f::join(join_op(1, false), &again)));
  EXPECT_FALSE(first.already_applied);
  EXPECT_TRUE(again.already_applied);
  co_return;
}

CORO_TEST_F(NeoRadosClsRgwFsio, WriterJoinClonesTheHead, NeoRadosTest) {
  const auto head = head_of(oid);
  co_await execute(head, make_head());
  cls_rgw_fsio_join_ret jret;
  co_await execute(head, WriteOp{}.exec(f::join(join_op(1, true), &jret)));
  EXPECT_TRUE(jret.construct);
  if (!jret.construct) {
    co_return;
  }
  /* §2.7, E2:  <marker>~fsio_a<id>_<key part>, after every head */
  const auto clone = fmt::format("{}~fsio_a{}_{}", MARKER, jret.bound.to_str(),
                                 oid);
  auto ioc = pool();
  ioc.set_key(head);
  ceph::buffer::list v;
  co_await execute(clone, ReadOp{}.get_xattr("user.rgw.fsio.pm", &v), ioc);
  EXPECT_EQ("C0", v.to_str());              // the head's manifest
  v.clear();
  co_await execute(clone, ReadOp{}.read(0, 0, &v), ioc);
  EXPECT_EQ("data", v.to_str());
  v.clear();
  co_await expect_error_code(                // FSIO's records not carried
    execute(clone, ReadOp{}.get_xattr(HDR, &v), ioc),
    sys::errc::no_message_available);
  co_return;
}

CORO_TEST_F(NeoRadosClsRgwFsio, WrongMarkerIsEinval, NeoRadosTest) {
  const auto head = head_of(oid);
  co_await execute(head, make_head());
  co_await expect_error_code(
    execute(head, WriteOp{}.exec(f::join(join_op(1, true, "other.1.1")))),
    sys::errc::invalid_argument);
  /* the op failed whole:  nothing written */
  ceph::buffer::list v;
  co_await expect_error_code(execute(head, ReadOp{}.get_xattr(HDR, &v)),
                             sys::errc::no_message_available);
  co_return;
}
