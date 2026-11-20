// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/* FSIO's head methods on an OSD (FSIO_RADOS_DESIGN.md §2.2, §2.7, §8):
 * the records each writes and removes, its reply, and the clone it
 * makes;  the transitions themselves are unittest_cls_rgw_fsio's.  And
 * the FSIO open hints on a hint shard (§2.7):  the key each is stored
 * under, listing in pages and by filter, removal.  And the shadow's size
 * hint and truncate (§3.3):  their replies and the stored record. */

#include <coroutine>
#include <string>

#include <boost/container/flat_set.hpp>
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
/* the test's gateway:  its own RADOS client, an incarnation being the
 * op's origin (§3, Z28);  set by FsioTest for each test */
cls_rgw_fsio_handle GW1;
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
std::string M1;                      // GW1's FSIO-open record

class FsioTest : public NeoRadosTest {
protected:
  boost::asio::awaitable<void> CoSetUp() override {
    co_await NeoRadosTest::CoSetUp();
    GW1 = cls_rgw_fsio_handle{rados().instance_id(), 1};
    M1 = fmt::format("user.rgw.fsio.m.{:016x}", GW1.incarnation);
    co_return;
  }
};

} // anonymous namespace

CORO_TEST_F(NeoRadosClsRgwFsio, AbsentHeadIsEnoent, FsioTest) {
  co_await expect_error_code(
    execute(head_of(oid), WriteOp{}.exec(f::join(join_op(1, false)))),
    sys::errc::no_such_file_or_directory);
  co_return;
}

CORO_TEST_F(NeoRadosClsRgwFsio, ReaderJoinAndLeave, FsioTest) {
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

CORO_TEST_F(NeoRadosClsRgwFsio, ResendIsAlreadyApplied, FsioTest) {
  const auto head = head_of(oid);
  co_await execute(head, make_head());
  cls_rgw_fsio_join_ret first, again;
  co_await execute(head, WriteOp{}.exec(f::join(join_op(1, false), &first)));
  co_await execute(head, WriteOp{}.exec(f::join(join_op(1, false), &again)));
  EXPECT_FALSE(first.already_applied);
  EXPECT_TRUE(again.already_applied);
  co_return;
}

CORO_TEST_F(NeoRadosClsRgwFsio, WriterJoinClonesTheHead, FsioTest) {
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

CORO_TEST_F(NeoRadosClsRgwFsio, WrongMarkerIsEinval, FsioTest) {
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

namespace {

constexpr auto shard = "fsio_open_hint.0000000000"sv;

rgw_bucket bucket_named(const std::string& name)
{
  rgw_bucket b;
  b.name = name;
  b.marker = MARKER;
  b.bucket_id = MARKER;
  return b;
}

cls_rgw_fsio_hint_op hint_op(uint64_t incarnation, uint64_t handle,
                             const std::string& bucket,
                             const std::string& name)
{
  cls_rgw_fsio_hint_op op;
  op.hint.kind = cls_rgw_fsio_hint_kind::PRESENCE;
  op.hint.target = cls_rgw_fsio_hint_target::HEAD;
  op.hint.bucket = bucket_named(bucket);
  op.hint.key = rgw_obj_key(name);
  op.hint.incarnation = incarnation;
  op.hint.handle = handle;
  op.oid = head_of(name);
  return op;
}

} // anonymous namespace

/* the key the class builds:  {incarnation:016x}_{handle:016x}_{kind}_
 * {target oid}[_{r}] */
CORO_TEST_F(NeoRadosClsRgwFsio, OpenHintKey, FsioTest) {
  auto presence = hint_op(0xa1, 2, "b1", "k1");
  auto publish = hint_op(0xa1, 2, "b1", "k1");
  publish.hint.kind = cls_rgw_fsio_hint_kind::PUBLISH;
  publish.hint.r = cls_rgw_fsio_id{5, 6};
  co_await execute(shard, WriteOp{}.exec(f::open_hint_add(presence)));
  co_await execute(shard, WriteOp{}.exec(f::open_hint_add(publish)));
  boost::container::flat_set<std::string> keys;
  bool truncated = true;
  co_await execute(shard, ReadOp{}.get_omap_keys(std::nullopt, 100, &keys,
                                                 &truncated));
  EXPECT_FALSE(truncated);
  const auto lead = fmt::format("{:016x}_{:016x}_", 0xa1, 2);
  EXPECT_EQ((boost::container::flat_set<std::string>{
              lead + "1_" + head_of("k1"),
              lead + "2_" + head_of("k1") + "_" +
                cls_rgw_fsio_id{5, 6}.to_str()}),
            keys);
  co_return;
}

/* the record comes back whole;  pages follow the marker;  a removal, and
 * a removal of an absent hint, succeed */
CORO_TEST_F(NeoRadosClsRgwFsio, OpenHintAddListRemove, FsioTest) {
  for (uint64_t h = 1; h <= 3; ++h) {
    co_await execute(shard, WriteOp{}.exec(
                       f::open_hint_add(hint_op(0xa1, h, "b1", "k1"))));
  }
  std::vector<cls_rgw_fsio_hint> all;
  cls_rgw_fsio_hint_list_op lop;
  lop.max = 2;
  for (;;) {
    cls_rgw_fsio_hint_list_ret ret;
    co_await execute(shard, ReadOp{}.exec(f::open_hint_list(lop, &ret)));
    all.insert(all.end(), ret.hints.begin(), ret.hints.end());
    if (!ret.truncated) {
      break;
    }
    EXPECT_EQ(2u, ret.hints.size());
    lop.marker = ret.marker;
  }
  EXPECT_EQ(3u, all.size());
  if (all.size() != 3u) {
    co_return;
  }
  for (uint64_t h = 1; h <= 3; ++h) {
    const auto& x = all[h - 1];
    EXPECT_EQ(h, x.handle);
    EXPECT_EQ(0xa1u, x.incarnation);
    EXPECT_EQ(cls_rgw_fsio_hint_kind::PRESENCE, x.kind);
    EXPECT_EQ(cls_rgw_fsio_hint_target::HEAD, x.target);
    EXPECT_EQ(bucket_named("b1"), x.bucket);
    EXPECT_EQ(rgw_obj_key("k1"), x.key);
    EXPECT_FALSE(x.r);
  }
  co_await execute(shard, WriteOp{}.exec(
                     f::open_hint_remove(hint_op(0xa1, 2, "b1", "k1"))));
  co_await execute(shard, WriteOp{}.exec(
                     f::open_hint_remove(hint_op(0xa1, 2, "b1", "k1"))));
  cls_rgw_fsio_hint_list_ret ret;
  co_await execute(shard, ReadOp{}.exec(
                     f::open_hint_list(cls_rgw_fsio_hint_list_op{}, &ret)));
  EXPECT_EQ(2u, ret.hints.size());
  if (ret.hints.size() != 2u) {
    co_return;
  }
  EXPECT_EQ(1u, ret.hints[0].handle);
  EXPECT_EQ(3u, ret.hints[1].handle);
  co_return;
}

/* by incarnation:  only its hints, the others' keys not read;  by
 * bucket:  only the hints naming it */
CORO_TEST_F(NeoRadosClsRgwFsio, OpenHintListFilters, FsioTest) {
  co_await execute(shard, WriteOp{}.exec(
                     f::open_hint_add(hint_op(0xa1, 1, "b1", "k1"))));
  co_await execute(shard, WriteOp{}.exec(
                     f::open_hint_add(hint_op(0xa2, 1, "b2", "k2"))));
  co_await execute(shard, WriteOp{}.exec(
                     f::open_hint_add(hint_op(0xa3, 1, "b1", "k3"))));
  {
    cls_rgw_fsio_hint_list_op lop;
    lop.incarnation = 0xa2;
    cls_rgw_fsio_hint_list_ret ret;
    co_await execute(shard, ReadOp{}.exec(f::open_hint_list(lop, &ret)));
    EXPECT_EQ(1u, ret.hints.size());
    if (ret.hints.size() != 1u) {
      co_return;
    }
    EXPECT_EQ(0xa2u, ret.hints[0].incarnation);
    EXPECT_FALSE(ret.truncated);
  }
  {
    cls_rgw_fsio_hint_list_op lop;
    lop.bucket = bucket_named("b1");
    cls_rgw_fsio_hint_list_ret ret;
    co_await execute(shard, ReadOp{}.exec(f::open_hint_list(lop, &ret)));
    EXPECT_EQ(2u, ret.hints.size());
    if (ret.hints.size() != 2u) {
      co_return;
    }
    EXPECT_EQ(0xa1u, ret.hints[0].incarnation);
    EXPECT_EQ(0xa3u, ret.hints[1].incarnation);
  }
  {
    cls_rgw_fsio_hint_list_op lop;
    lop.bucket = bucket_named("b3");
    cls_rgw_fsio_hint_list_ret ret;
    co_await execute(shard, ReadOp{}.exec(f::open_hint_list(lop, &ret)));
    EXPECT_TRUE(ret.hints.empty());
    EXPECT_FALSE(ret.truncated);
  }
  co_return;
}

/* §3.3:  a writer's hint merges by max in its epoch;  a truncate sets
 * the size exactly in a new epoch;  a hint from the old epoch merges
 * nothing and gets the current epoch and size */
CORO_TEST_F(NeoRadosClsRgwFsio, HintAndTruncate, FsioTest) {
  const auto head = head_of(oid);
  co_await execute(head, make_head());
  cls_rgw_fsio_join_ret jret;
  co_await execute(head, WriteOp{}.exec(f::join(join_op(1, true), &jret)));
  EXPECT_TRUE(jret.construct);
  if (!jret.construct) {
    co_return;
  }
  cls_rgw_fsio_runs_op rop;
  rop.caller = GW1;
  rop.cs = jret.bound;
  rop.runs = {{0, 0}};
  co_await execute(head, WriteOp{}.exec(f::ready(rop)));

  cls_rgw_fsio_hint_size_op hop;
  hop.caller = GW1;
  hop.cs = jret.bound;
  hop.trunc_epoch = 0;
  hop.size = 100;
  hop.mtime = ceph::real_time(std::chrono::seconds(5));
  cls_rgw_fsio_hint_size_ret hret;
  co_await execute(head, WriteOp{}.exec(f::hint(hop, &hret)));
  EXPECT_FALSE(hret.stale);
  EXPECT_FALSE(hret.others);
  EXPECT_EQ(100u, hret.size);

  cls_rgw_fsio_truncate_op top;
  top.caller = GW1;
  top.seq = 2;                   // a counted call, after the join's 1
  top.cs = jret.bound;
  top.size = 40;
  top.mtime = ceph::real_time(std::chrono::seconds(7));
  cls_rgw_fsio_truncate_ret tret;
  co_await execute(head, WriteOp{}.exec(f::truncate(top, &tret)));
  EXPECT_EQ(1u, tret.trunc_epoch);
  EXPECT_FALSE(tret.already_applied);
  /* resent, it changes nothing (A1) */
  co_await execute(head, WriteOp{}.exec(f::truncate(top, &tret)));
  EXPECT_TRUE(tret.already_applied);

  hop.size = 500;
  hop.mtime = ceph::real_time(std::chrono::seconds(8));
  co_await execute(head, WriteOp{}.exec(f::hint(hop, &hret)));
  EXPECT_TRUE(hret.stale);
  EXPECT_EQ(1u, hret.trunc_epoch);
  EXPECT_EQ(40u, hret.size);

  /* the stored record:  the truncate's size, its epoch and trunc_low */
  ceph::buffer::list v;
  co_await execute(head, ReadOp{}.get_xattr(HDR, &v));
  cls_rgw_fsio_hdr hdr;
  auto it = v.cbegin();
  decode(hdr, it);
  const auto* e = hdr.find(jret.bound);
  EXPECT_TRUE(e && e->shadow);
  if (!e || !e->shadow) {
    co_return;
  }
  EXPECT_EQ(40u, e->shadow->size_hint);
  EXPECT_EQ(ceph::real_time(std::chrono::seconds(7)), e->shadow->mtime_hint);
  EXPECT_EQ(1u, e->shadow->trunc_epoch);
  EXPECT_EQ(std::optional<uint64_t>(40), e->shadow->trunc_low);
  co_return;
}

/* A2, A3 (§2.2):  fsio_ready and fsio_publish_release are idempotent by
 * their effect:  a repeat finding the state reached succeeds and stores
 * nothing new */
CORO_TEST_F(NeoRadosClsRgwFsio, ReadyAndReleaseRepeat, FsioTest) {
  const auto head = head_of(oid);
  co_await execute(head, make_head());
  cls_rgw_fsio_join_ret jret;
  co_await execute(head, WriteOp{}.exec(f::join(join_op(1, true), &jret)));
  EXPECT_TRUE(jret.construct);
  if (!jret.construct) {
    co_return;
  }
  cls_rgw_fsio_runs_op rop;
  rop.caller = GW1;
  rop.cs = jret.bound;
  rop.runs = {{0, 0}};
  rop.size = 42;                       // A20:  the starting hint
  rop.mtime = ceph::real_time(std::chrono::seconds(9));
  co_await execute(head, WriteOp{}.exec(f::ready(rop)));
  co_await execute(head, WriteOp{}.exec(f::ready(rop)));   // a repeat:  0

  cls_rgw_fsio_leave_op lop;
  lop.caller = GW1;
  lop.seq = 2;
  lop.cs = jret.bound;
  lop.writer = true;
  cls_rgw_fsio_leave_ret lret;
  co_await execute(head, WriteOp{}.exec(f::leave(lop, &lret)));
  EXPECT_TRUE(lret.publishing);
  cls_rgw_fsio_cs_op cop;
  cop.caller = GW1;
  cop.cs = jret.bound;
  co_await execute(head, WriteOp{}.exec(f::publish_release(cop)));
  co_await execute(head, WriteOp{}.exec(f::publish_release(cop)));   // 0

  ceph::buffer::list v;
  co_await execute(head, ReadOp{}.get_xattr(HDR, &v));
  cls_rgw_fsio_hdr hdr;
  auto it = v.cbegin();
  decode(hdr, it);
  const auto* e = hdr.find(jret.bound);
  EXPECT_TRUE(e);
  if (!e) {
    co_return;
  }
  EXPECT_EQ(cls_rgw_fsio_cs_state::READY, e->state);
  EXPECT_FALSE(e->publishing);
  EXPECT_EQ(42u, e->shadow->size_hint);
  EXPECT_EQ(ceph::real_time(std::chrono::seconds(9)), e->shadow->mtime_hint);
  co_return;
}
