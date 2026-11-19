// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* fsio_replace_head on an OSD, in librados compound ops built as RGW's
 * head writes are (FSIO_RADOS_DESIGN.md §3):  the leaf clone, the
 * recreated head, the ordering rule, the publish. */

#include <fmt/format.h>

#include "cls/rgw/cls_rgw_client.h"
#include "cls/rgw/cls_rgw_fsio_ops.h"
#include "gtest/gtest.h"
#include "include/rados/librados.hpp"
#include "test/librados/test_cxx.h"

namespace fsio = cls_rgw_fsio;
using namespace librados;

namespace {

constexpr cls_rgw_fsio_handle GW1{1, 1}, GW2{2, 1};
const std::string MARKER = "zone.4125.1";
const std::string HDR = "user.rgw.fsio.hdr";

class FsioHead : public ::testing::Test {
protected:
  static Rados rados;
  static std::string pool;
  IoCtx ioctx;
  static uint64_t next;

  static void SetUpTestSuite() {
    pool = get_temp_pool_name();
    ASSERT_EQ("", create_one_pool_pp(pool, rados));
  }
  static void TearDownTestSuite() {
    ASSERT_EQ(0, destroy_one_pool_pp(pool, rados));
  }
  void SetUp() override {
    ASSERT_EQ(0, rados.ioctx_create(pool.c_str(), ioctx));
  }

  /* a fresh RGW head:  <marker>_<key>, data, ID tag and manifest */
  std::string make_head(std::string& key) {
    key = fmt::format("key{}", next++);
    const auto head = MARKER + "_" + key;
    ObjectWriteOperation op;
    op.create(true);
    bufferlist d, t, m;
    d.append("old");
    t.append("t0");
    m.append("C0");
    op.write_full(d);
    op.setxattr("user.rgw.idtag", t);
    op.setxattr("user.rgw.manifest", m);
    EXPECT_EQ(0, ioctx.operate(head, &op));
    return head;
  }

  template <typename Method, typename In, typename Ret>
  int call(const std::string& oid, const Method& method, const In& in,
	   Ret* ret) {
    ObjectWriteOperation op;
    bufferlist inbl, out;
    encode(in, inbl);
    int rval = 0;
    op.exec(method, inbl, &out, &rval);
    int r = ioctx.operate(oid, &op, OPERATION_RETURNVEC);
    if (r == 0 && ret) {
      auto it = out.cbegin();
      decode(*ret, it);
    }
    return r;
  }

  cls_rgw_fsio_join_ret join(const std::string& head, uint64_t seq,
			     bool writer, cls_rgw_fsio_handle caller = GW1) {
    cls_rgw_fsio_join_op op;
    op.caller = caller;
    op.seq = seq;
    op.writer = writer;
    op.nonce = 1;
    op.counter_seed = 10;
    op.marker = MARKER;
    cls_rgw_fsio_join_ret ret;
    EXPECT_EQ(0, call(head, cls::rgw::method::fsio_join, op, &ret));
    return ret;
  }

  cls_rgw_fsio_replace_op replace(fsio::replace_mode mode, uint64_t r) {
    cls_rgw_fsio_replace_op op;
    op.mode = static_cast<uint8_t>(mode);
    op.r = cls_rgw_fsio_id{r, 7};
    op.keep_attr_prefixes = {"user.rgw.olh."};
    op.marker = MARKER;
    return op;
  }

  std::string read(const std::string& oid, const IoCtx& io) {
    bufferlist bl;
    EXPECT_LE(0, const_cast<IoCtx&>(io).read(oid, bl, 0, 0));
    return bl.to_str();
  }
  int getxattr(const std::string& oid, const std::string& name,
	       std::string* v = nullptr) {
    bufferlist bl;
    int r = ioctx.getxattr(oid, name.c_str(), bl);
    if (r >= 0 && v) {
      *v = bl.to_str();
    }
    return r;
  }
  /* a clone's io context:  the head's name as its locator */
  IoCtx keyed(const std::string& head) {
    IoCtx io;
    io.dup(ioctx);
    io.locator_set_key(head);
    return io;
  }
  /* GW1's write open on head, its shadow readied and its last leave
   * made:  the shadow publishing */
  cls_rgw_fsio_id write_and_close(const std::string& head) {
    const auto j = join(head, 11, true);
    EXPECT_TRUE(j.construct);
    cls_rgw_fsio_runs_op rop;
    rop.caller = GW1;
    rop.cs = j.bound;
    rop.runs = {{0, 0}};
    EXPECT_EQ(0, call(head, cls::rgw::method::fsio_ready, rop,
		      (cls_rgw_fsio_flags_ret*)nullptr));
    cls_rgw_fsio_leave_op lop;
    lop.caller = GW1;
    lop.seq = 12;
    lop.cs = j.bound;
    lop.writer = true;
    cls_rgw_fsio_leave_ret lret;
    EXPECT_EQ(0, call(head, cls::rgw::method::fsio_leave, lop, &lret));
    EXPECT_TRUE(lret.publishing);
    return j.bound;
  }
  /* the publish in the head write's op, RGW's writes after it */
  int publish(const std::string& head, const cls_rgw_fsio_id& s, uint64_t r,
	      cls_rgw_fsio_replace_ret& ret) {
    ObjectWriteOperation op;
    bufferlist out, t, m;
    int rval = 0;
    auto in = replace(fsio::replace_mode::PUBLISH, r);
    in.caller = GW1;
    in.shadow = s;
    cls_rgw_fsio_replace_head(op, in, &out, &rval);
    t.append("t1");
    m.append("S");
    op.setxattr("user.rgw.idtag", t);
    op.setxattr("user.rgw.manifest", m);
    int rc = ioctx.operate(head, &op, OPERATION_RETURNVEC);
    if (rc == 0) {
      auto it = out.cbegin();
      decode(ret, it);
    }
    return rc;
  }
  std::string leaf(const std::string& key, uint64_t r) {
    return fmt::format("{}~fsio_G{}_{}", MARKER,
		       cls_rgw_fsio_id{r, 7}.to_str(), key);
  }
};
Rados FsioHead::rados;
std::string FsioHead::pool;
uint64_t FsioHead::next = 0;

} // anonymous namespace

/* an overwrite with a reader open:  the leaf clone keeps the old
 * content for the reader, the head takes the new, its FSIO state kept */
TEST_F(FsioHead, SupersedeWithReaderKeepsTheOldContent)
{
  std::string key;
  const auto head = make_head(key);
  join(head, 1, false);
  ObjectWriteOperation op;
  bufferlist out, d, t, m;
  int rval = 0;
  cls_rgw_fsio_replace_head(op, replace(fsio::replace_mode::SUPERSEDE, 1000),
			    &out, &rval);
  d.append("new");
  t.append("t1");
  m.append("C1");
  op.write_full(d);
  op.setxattr("user.rgw.idtag", t);
  op.setxattr("user.rgw.manifest", m);
  ASSERT_EQ(0, ioctx.operate(head, &op, OPERATION_RETURNVEC));
  cls_rgw_fsio_replace_ret ret;
  auto it = out.cbegin();
  decode(ret, it);
  EXPECT_TRUE(ret.keep_tail);
  EXPECT_EQ("new", read(head, ioctx));
  std::string v;
  ASSERT_LE(0, getxattr(head, "user.rgw.idtag", &v));
  EXPECT_EQ("t1", v);
  EXPECT_LE(0, getxattr(head, HDR));
  auto io = keyed(head);
  EXPECT_EQ("old", read(leaf(key, 1000), io));
  bufferlist g;
  EXPECT_LE(0, io.getxattr(leaf(key, 1000), "user.rgw.fsio.gen", g));
}

/* a DELETE with a reader open:  a tracking-only head, no ID tag or
 * manifest, its FSIO state kept (§2.2) */
TEST_F(FsioHead, DeleteWithReaderLeavesTrackingOnly)
{
  std::string key;
  const auto head = make_head(key);
  join(head, 1, false);
  ObjectWriteOperation op;
  bufferlist out;
  int rval = 0;
  auto in = replace(fsio::replace_mode::DELETE, 1001);
  in.expected_id_tag = "t0";
  cls_rgw_fsio_replace_head(op, in, &out, &rval);
  ASSERT_EQ(0, ioctx.operate(head, &op, OPERATION_RETURNVEC));
  EXPECT_EQ(-ENODATA, getxattr(head, "user.rgw.idtag"));
  EXPECT_EQ(-ENODATA, getxattr(head, "user.rgw.manifest"));
  EXPECT_LE(0, getxattr(head, HDR));
}

/* a DELETE with no FSIO state:  the head goes, as cls_rgw_remove_obj */
TEST_F(FsioHead, DeleteWithoutFsioStateRemoves)
{
  std::string key;
  const auto head = make_head(key);
  ObjectWriteOperation op;
  bufferlist out;
  int rval = 0;
  auto in = replace(fsio::replace_mode::DELETE, 1002);
  in.expected_id_tag = "t0";
  cls_rgw_fsio_replace_head(op, in, &out, &rval);
  ASSERT_EQ(0, ioctx.operate(head, &op, OPERATION_RETURNVEC));
  uint64_t size;
  time_t mtime;
  EXPECT_EQ(-ENOENT, ioctx.stat(head, &size, &mtime));
}

/* §3, the ordering rule:  a write before the replacement is discarded,
 * one after it stays */
TEST_F(FsioHead, WritesBeforeTheReplacementAreDiscarded)
{
  std::string key;
  const auto head = make_head(key);
  join(head, 1, false);
  ObjectWriteOperation op;
  bufferlist out, b, a;
  int rval = 0;
  b.append("b");
  a.append("a");
  op.setxattr("user.rgw.before", b);
  cls_rgw_fsio_replace_head(op, replace(fsio::replace_mode::SUPERSEDE, 1003),
			    &out, &rval);
  op.setxattr("user.rgw.after", a);
  ASSERT_EQ(0, ioctx.operate(head, &op, OPERATION_RETURNVEC));
  EXPECT_EQ(-ENODATA, getxattr(head, "user.rgw.before"));
  EXPECT_LE(0, getxattr(head, "user.rgw.after"));
}

/* an unversioned publish with no open left:  the old content to its
 * leaf clone, and the header, idle, goes (§2.2, §3.5) */
TEST_F(FsioHead, UnversionedPublish)
{
  std::string key;
  const auto head = make_head(key);
  const auto s = write_and_close(head);
  cls_rgw_fsio_replace_ret ret;
  ASSERT_EQ(0, publish(head, s, 1004, ret));
  EXPECT_TRUE(ret.born_drained);       // no reader of the old content
  EXPECT_EQ(-ENODATA, getxattr(head, HDR));
  EXPECT_EQ(-ENODATA, getxattr(head, fmt::format("user.rgw.fsio.m.{:016x}",
						  GW1.incarnation)));
  std::string v;
  ASSERT_LE(0, getxattr(head, "user.rgw.idtag", &v));
  EXPECT_EQ("t1", v);                  // RGW's write after the call stands
  auto io = keyed(head);
  EXPECT_EQ("old", read(leaf(key, 1004), io));
}

/* an unversioned publish with a reader open:  the reader's chunk set
 * retired onto the leaf clone, the shadow published as the head's
 * content (§2.2, §3.5) */
TEST_F(FsioHead, UnversionedPublishWithReader)
{
  std::string key;
  const auto head = make_head(key);
  const auto rd = join(head, 1, false, GW2);
  const auto s = write_and_close(head);
  cls_rgw_fsio_replace_ret ret;
  ASSERT_EQ(0, publish(head, s, 1005, ret));
  EXPECT_FALSE(ret.born_drained);
  bufferlist hb;
  ASSERT_LE(0, ioctx.getxattr(head, HDR.c_str(), hb));
  cls_rgw_fsio_hdr hdr;
  auto hit = hb.cbegin();
  decode(hdr, hit);
  /* §2.2:  an entry only for a chunk set with bound opens, or retired;
   * nobody is bound to the published one, the head's content by
   * position */
  EXPECT_FALSE(hdr.find(s));
  const auto* old = hdr.find(rd.bound);
  ASSERT_TRUE(old);
  EXPECT_EQ(cls_rgw_fsio_cs_state::RETIRED, old->state);
  ASSERT_TRUE(old->clone);
  EXPECT_EQ((cls_rgw_fsio_id{1005, 7}), *old->clone);
  auto io = keyed(head);
  EXPECT_EQ("old", read(leaf(key, 1005), io));
}
