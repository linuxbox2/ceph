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
    buffer::list d, t, m;
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
    buffer::list inbl, out;
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
			     bool writer, cls_rgw_fsio_handle caller = GW1,
			     const std::string& instance = {}) {
    cls_rgw_fsio_join_op op;
    op.caller = caller;
    op.seq = seq;
    op.writer = writer;
    op.instance = instance;
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
    buffer::list bl;
    EXPECT_LE(0, const_cast<IoCtx&>(io).read(oid, bl, 0, 0));
    return bl.to_str();
  }
  int getxattr(const std::string& oid, const std::string& name,
	       std::string* v = nullptr) {
    buffer::list bl;
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
	      cls_rgw_fsio_replace_ret& ret, const std::string& tags = {}) {
    ObjectWriteOperation op;
    buffer::list out, t, m;
    int rval = 0;
    auto in = replace(fsio::replace_mode::PUBLISH, r);
    in.caller = GW1;
    in.shadow = s;
    in.tags.append(tags);
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
  buffer::list out, d, t, m;
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
  buffer::list g;
  EXPECT_LE(0, io.getxattr(leaf(key, 1000), "user.rgw.fsio.gen", g));
}

/* E5:  the leaf clone's record names the head's tail tag when it has one
 * (a copy's), else its ID tag:  RGW collects the tail under it */
TEST_F(FsioHead, CloneRecordsTheTailTag)
{
  std::string key;
  const auto head = make_head(key);
  {
    ObjectWriteOperation op;
    buffer::list tt, rc;
    tt.append("tail0");
    rc.append("tags");
    op.setxattr("user.rgw.tail_tag", tt);
    op.setxattr("refcount", rc);     // a head a copy or dedup referenced
    ASSERT_EQ(0, ioctx.operate(head, &op));
  }
  join(head, 1, false);
  ObjectWriteOperation op;
  buffer::list out;
  int rval = 0;
  auto in = replace(fsio::replace_mode::DELETE, 1002);
  in.expected_id_tag = "t0";
  cls_rgw_fsio_replace_head(op, in, &out, &rval);
  ASSERT_EQ(0, ioctx.operate(head, &op, OPERATION_RETURNVEC));
  auto io = keyed(head);
  buffer::list g;
  ASSERT_LE(0, io.getxattr(leaf(key, 1002), "user.rgw.fsio.gen", g));
  cls_rgw_fsio_gen rec;
  auto it = g.cbegin();
  decode(rec, it);
  EXPECT_EQ("tail0", rec.gc_tag);
  EXPECT_EQ("C0", rec.manifest.to_str());
  /* A7, A8:  the clone drops the head's manifest (its record carries it)
   * and cls_refcount's tags (X16) */
  buffer::list m;
  EXPECT_EQ(-ENODATA, io.getxattr(leaf(key, 1002), "user.rgw.manifest", m));
  EXPECT_EQ(-ENODATA, io.getxattr(leaf(key, 1002), "refcount", m));
}

/* §3.6, Y2:  a tail-keeping supersede (an append, a copy onto itself):
 * the leaf clone's record names no manifest, so its drain collects the
 * clone and never the tail the live object reuses */
TEST_F(FsioHead, KeepTailSupersedeRecordsNoManifest)
{
  std::string key;
  const auto head = make_head(key);
  join(head, 1, false);
  ObjectWriteOperation op;
  buffer::list out;
  int rval = 0;
  auto in = replace(fsio::replace_mode::SUPERSEDE, 1003);
  in.keep_tail = true;
  cls_rgw_fsio_replace_head(op, in, &out, &rval);
  ASSERT_EQ(0, ioctx.operate(head, &op, OPERATION_RETURNVEC));
  auto io = keyed(head);
  buffer::list g;
  ASSERT_LE(0, io.getxattr(leaf(key, 1003), "user.rgw.fsio.gen", g));
  cls_rgw_fsio_gen rec;
  auto it = g.cbegin();
  decode(rec, it);
  EXPECT_EQ(0u, rec.manifest.length());
  EXPECT_EQ("t0", rec.gc_tag);
}

/* a DELETE with a reader open:  a tracking-only head, no ID tag or
 * manifest, its FSIO state kept (§2.2) */
TEST_F(FsioHead, DeleteWithReaderLeavesTrackingOnly)
{
  std::string key;
  const auto head = make_head(key);
  join(head, 1, false);
  ObjectWriteOperation op;
  buffer::list out;
  int rval = 0;
  auto in = replace(fsio::replace_mode::DELETE, 1001);
  in.expected_id_tag = "t0";
  cls_rgw_fsio_replace_head(op, in, &out, &rval);
  ASSERT_EQ(0, ioctx.operate(head, &op, OPERATION_RETURNVEC));
  EXPECT_EQ(-ENODATA, getxattr(head, "user.rgw.idtag"));
  EXPECT_EQ(-ENODATA, getxattr(head, "user.rgw.manifest"));
  EXPECT_LE(0, getxattr(head, HDR));
}

/* Z7:  a DELETE that read another ID tag than the head's changes nothing
 * and returns ERR_FSIO_CONFLICT, for its caller to re-read */
TEST_F(FsioHead, DeleteWithStaleIdTagConflicts)
{
  std::string key;
  const auto head = make_head(key);
  join(head, 1, false);
  ObjectWriteOperation op;
  buffer::list out;
  int rval = 0;
  auto in = replace(fsio::replace_mode::DELETE, 1006);
  in.expected_id_tag = "stale";
  cls_rgw_fsio_replace_head(op, in, &out, &rval);
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    ioctx.operate(head, &op, OPERATION_RETURNVEC));
  std::string v;
  ASSERT_LE(0, getxattr(head, "user.rgw.idtag", &v));
  EXPECT_EQ("t0", v);                  // the head as it was
  EXPECT_EQ("old", read(head, ioctx));
}

namespace {
/* a version's instance head:  <marker>__:<instance>_<key> */
std::string instance_of(const std::string& key)
{
  return MARKER + "__:i1_" + key;
}
}

/* §2.5:  a version held by an open count is cloned to its sibling, its
 * instance head reset to the open count alone, its tail kept */
TEST_F(FsioHead, RemoveCountedVersionKeepsItsTail)
{
  std::string key;
  make_head(key);                      // for its key;  the instance next
  const auto ih = instance_of(key);
  {
    ObjectWriteOperation op;
    buffer::list d, t, m, tt;
    op.create(true);
    d.append("v1");
    t.append("i0");
    m.append("C0");
    tt.append("tail0");
    op.write_full(d);
    op.setxattr("user.rgw.idtag", t);
    op.setxattr("user.rgw.manifest", m);
    op.setxattr("user.rgw.tail_tag", tt);
    ASSERT_EQ(0, ioctx.operate(ih, &op));
  }
  cls_rgw_fsio_ocnt_op add;
  add.holder = GW1;
  ASSERT_EQ(0, call(ih, cls::rgw::method::fsio_ocnt_add, add,
		    (cls_rgw_fsio_flags_ret*)nullptr));
  ObjectWriteOperation op;
  buffer::list out;
  int rval = 0;
  cls_rgw_fsio_remove_version_op rv;
  rv.r = cls_rgw_fsio_id{1007, 7};
  rv.marker = MARKER;
  cls_rgw_fsio_remove_version(op, rv, &out, &rval);
  ASSERT_EQ(0, ioctx.operate(ih, &op, OPERATION_RETURNVEC));
  cls_rgw_fsio_remove_version_ret ret;
  auto it = out.cbegin();
  decode(ret, it);
  EXPECT_TRUE(ret.keep_tail);
  /* the instance head reset:  its open count alone */
  EXPECT_EQ(-ENODATA, getxattr(ih, "user.rgw.idtag"));
  EXPECT_EQ(-ENODATA, getxattr(ih, "user.rgw.manifest"));
  EXPECT_LE(0, getxattr(ih, "user.rgw.fsio.ocnt"));
  EXPECT_EQ("", read(ih, ioctx));
  /* its content on the sibling, after the bucket's heads (§2.7) */
  const auto sib = fmt::format("{}~fsio_G{}_{}", MARKER,
			       cls_rgw_fsio_id{1007, 7}.to_str(),
			       ih.substr(MARKER.size() + 1));
  auto io = keyed(ih);
  EXPECT_EQ("v1", read(sib, io));
  /* E5:  its record, and the open count's, name the tail tag */
  buffer::list g;
  ASSERT_LE(0, io.getxattr(sib, "user.rgw.fsio.gen", g));
  cls_rgw_fsio_gen rec;
  auto git = g.cbegin();
  decode(rec, git);
  EXPECT_EQ("tail0", rec.gc_tag);
  buffer::list o;
  ASSERT_LE(0, ioctx.getxattr(ih, "user.rgw.fsio.ocnt", o));
  cls_rgw_fsio_ocnt oc;
  auto oit = o.cbegin();
  decode(oc, oit);
  ASSERT_EQ(1u, oc.deferred.size());
  EXPECT_EQ("tail0", oc.deferred[0].gc_tag);
  EXPECT_EQ(-ENODATA, getxattr(ih, "user.rgw.tail_tag"));
}

/* §2.5, Y6 (A13):  a version rewritten in place under its open count,
 * as a lifecycle transition or an admin rewrite writes it:  the head
 * write's supersede clones it to the sibling and defers it in the count,
 * which the recreated instance head keeps;  RGW's write then lands */
TEST_F(FsioHead, RewriteCountedVersionKeepsItsTail)
{
  std::string key;
  make_head(key);                      // for its key;  the instance next
  const auto ih = instance_of(key);
  {
    ObjectWriteOperation op;
    buffer::list d, t, m, tt;
    op.create(true);
    d.append("v1");
    t.append("i0");
    m.append("C0");
    tt.append("tail0");
    op.write_full(d);
    op.setxattr("user.rgw.idtag", t);
    op.setxattr("user.rgw.manifest", m);
    op.setxattr("user.rgw.tail_tag", tt);
    ASSERT_EQ(0, ioctx.operate(ih, &op));
  }
  cls_rgw_fsio_ocnt_op add;
  add.holder = GW1;
  ASSERT_EQ(0, call(ih, cls::rgw::method::fsio_ocnt_add, add,
		    (cls_rgw_fsio_flags_ret*)nullptr));
  ObjectWriteOperation op;
  buffer::list out, d2, t2, m2;
  int rval = 0;
  auto in = replace(fsio::replace_mode::SUPERSEDE, 1013);
  cls_rgw_fsio_replace_head(op, in, &out, &rval);
  d2.append("v2");
  t2.append("i1");
  m2.append("C1");
  op.write_full(d2);
  op.setxattr("user.rgw.idtag", t2);
  op.setxattr("user.rgw.manifest", m2);
  ASSERT_EQ(0, ioctx.operate(ih, &op, OPERATION_RETURNVEC));
  cls_rgw_fsio_replace_ret ret;
  auto it = out.cbegin();
  decode(ret, it);
  EXPECT_TRUE(ret.keep_tail);
  /* the version rewritten:  RGW's write stands, the count kept */
  std::string v;
  ASSERT_LE(0, getxattr(ih, "user.rgw.idtag", &v));
  EXPECT_EQ("i1", v);
  EXPECT_EQ("v2", read(ih, ioctx));
  /* the old content on the sibling, deferred in the count */
  const auto sib = fmt::format("{}~fsio_G{}_{}", MARKER,
			       cls_rgw_fsio_id{1013, 7}.to_str(),
			       ih.substr(MARKER.size() + 1));
  auto io = keyed(ih);
  EXPECT_EQ("v1", read(sib, io));
  buffer::list o;
  ASSERT_LE(0, ioctx.getxattr(ih, "user.rgw.fsio.ocnt", o));
  cls_rgw_fsio_ocnt oc;
  auto oit = o.cbegin();
  decode(oc, oit);
  EXPECT_EQ(1u, oc.holders.size());
  EXPECT_FALSE(oc.removed);
  ASSERT_EQ(1u, oc.deferred.size());
  EXPECT_EQ("tail0", oc.deferred[0].gc_tag);
  EXPECT_EQ("C0", oc.deferred[0].manifest.to_str());
}

/* §2.5:  a version no open count holds goes, as it always has */
TEST_F(FsioHead, RemoveUncountedVersionRemovesIt)
{
  std::string key;
  make_head(key);
  const auto ih = instance_of(key);
  {
    ObjectWriteOperation op;
    buffer::list d, t;
    op.create(true);
    d.append("v1");
    t.append("i0");
    op.write_full(d);
    op.setxattr("user.rgw.idtag", t);
    ASSERT_EQ(0, ioctx.operate(ih, &op));
  }
  ObjectWriteOperation op;
  buffer::list out;
  int rval = 0;
  cls_rgw_fsio_remove_version_op rv;
  rv.r = cls_rgw_fsio_id{1008, 7};
  rv.marker = MARKER;
  cls_rgw_fsio_remove_version(op, rv, &out, &rval);
  ASSERT_EQ(0, ioctx.operate(ih, &op, OPERATION_RETURNVEC));
  cls_rgw_fsio_remove_version_ret ret;
  auto it = out.cbegin();
  decode(ret, it);
  EXPECT_FALSE(ret.keep_tail);
  uint64_t size;
  time_t mtime;
  EXPECT_EQ(-ENOENT, ioctx.stat(ih, &size, &mtime));
}

/* §4.2:  a versioned publish is a standalone call on the OLH head after
 * the version is linked:  nothing is superseded or cloned, the head is
 * not recreated, the publisher's writer open goes, and the tail is kept */
TEST_F(FsioHead, VersionedPublish)
{
  const auto key = fmt::format("key{}", next++);
  const auto head = MARKER + "_" + key;
  {
    ObjectWriteOperation op;
    buffer::list olh;
    olh.append("olh");
    op.create(true);
    op.setxattr("user.rgw.olh.info", olh);
    ASSERT_EQ(0, ioctx.operate(head, &op));
  }
  /* GW1 joins on the OLH head as writer, the OLH naming i1;  its shadow
   * readied, it gives up its write open with the intent for i2 */
  const auto j = join(head, 11, true, GW1, "i1");
  ASSERT_TRUE(j.construct);
  cls_rgw_fsio_runs_op rop;
  rop.caller = GW1;
  rop.cs = j.bound;
  rop.runs = {{0, 0}};
  ASSERT_EQ(0, call(head, cls::rgw::method::fsio_ready, rop,
		    (cls_rgw_fsio_flags_ret*)nullptr));
  cls_rgw_fsio_leave_op lop;
  lop.caller = GW1;
  lop.seq = 12;
  lop.cs = j.bound;
  lop.writer = true;
  lop.intent = cls_rgw_fsio_intent{"i2", "tok", 5};
  cls_rgw_fsio_leave_ret lret;
  const auto ns = [] {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      ceph::real_clock::now().time_since_epoch()).count());
  };
  const auto before = ns();
  ASSERT_EQ(0, call(head, cls::rgw::method::fsio_leave, lop, &lret));
  const auto after = ns();
  ASSERT_TRUE(lret.publishing);
  /* §4.2, X31:  the OSD's time, never the caller's 5 */
  EXPECT_LE(before, lret.olh_epoch);
  EXPECT_GE(after, lret.olh_epoch);
  EXPECT_EQ("i2", lret.instance);

  auto in = replace(fsio::replace_mode::PUBLISH, 1009);
  in.caller = GW1;
  in.shadow = j.bound;
  in.versioned = true;
  cls_rgw_fsio_replace_ret ret;
  ASSERT_EQ(0, call(head, cls::rgw::method::fsio_replace_head, in, &ret));
  EXPECT_TRUE(ret.keep_tail);          // the previous version keeps its tail
  EXPECT_FALSE(ret.born_drained);

  /* the head as it was:  the OLH, no content, no clone */
  std::string v;
  ASSERT_LE(0, getxattr(head, "user.rgw.olh.info", &v));
  EXPECT_EQ("olh", v);
  EXPECT_EQ(-ENODATA, getxattr(head, "user.rgw.idtag"));
  EXPECT_EQ("", read(head, ioctx));
  uint64_t size;
  time_t mtime;
  auto io = keyed(head);
  EXPECT_EQ(-ENOENT, io.stat(leaf(key, 1009), &size, &mtime));

  /* §2.2:  an entry lives only while opens are bound to it or it is
   * retired;  GW1's write open was the shadow's last, so the published
   * entry is settled away, and the header and GW1's record with it (the
   * transition itself is the core's, checked by its unit test and the
   * model's cluster tier) */
  EXPECT_EQ(-ENODATA, getxattr(head, HDR));
  EXPECT_EQ(-ENODATA, getxattr(head, fmt::format("user.rgw.fsio.m.{:016x}",
						  GW1.incarnation)));
}

/* a DELETE with no FSIO state:  the head goes, as cls_rgw_remove_obj */
TEST_F(FsioHead, DeleteWithoutFsioStateRemoves)
{
  std::string key;
  const auto head = make_head(key);
  ObjectWriteOperation op;
  buffer::list out;
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
  buffer::list out, b, a;
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
  buffer::list hb;
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

/* §3.5, Y18, Z15:  a publish keeps the committed head's tags;  onto a
 * tracking-only head it sets the tags the publisher passes */
TEST_F(FsioHead, PublishKeepsTags)
{
  {
    std::string key;
    const auto head = make_head(key);
    buffer::list tg;
    tg.append("tagA");
    ASSERT_EQ(0, ioctx.setxattr(head, "user.rgw.x-amz-tagging", tg));
    const auto s = write_and_close(head);
    cls_rgw_fsio_replace_ret ret;
    ASSERT_EQ(0, publish(head, s, 1010, ret, "ignored"));
    std::string v;
    ASSERT_LE(0, getxattr(head, "user.rgw.x-amz-tagging", &v));
    EXPECT_EQ("tagA", v);
  }
  {
    std::string key;
    const auto head = make_head(key);
    const auto s = write_and_close(head);
    ObjectWriteOperation op;
    buffer::list out;
    int rval = 0;
    auto in = replace(fsio::replace_mode::DELETE, 1011);
    in.expected_id_tag = "t0";
    cls_rgw_fsio_replace_head(op, in, &out, &rval);
    ASSERT_EQ(0, ioctx.operate(head, &op, OPERATION_RETURNVEC));
    ASSERT_EQ(-ENODATA, getxattr(head, "user.rgw.idtag"));   // tracking-only
    cls_rgw_fsio_replace_ret ret;
    ASSERT_EQ(0, publish(head, s, 1012, ret, "tagB"));
    std::string v;
    ASSERT_LE(0, getxattr(head, "user.rgw.x-amz-tagging", &v));
    EXPECT_EQ("tagB", v);
  }
}
