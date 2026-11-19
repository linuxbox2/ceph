// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* FSIO's PUT hook through RGW's own write path (FSIO_RADOS_DESIGN.md
 * §3.6):  librgw writes and overwrites objects, librados joins an FSIO
 * reader on a head between the two.  With the fsio zonegroup feature
 * enabled, an overwrite under a reader leaves the leaf clone and keeps
 * the old tail out of GC;  without a reader, the tail goes to GC as
 * today.  In a versioned bucket, a version's removal under an open
 * count resets its instance head and keeps its tail (§2.5);  the null
 * version's removal under a reader retires it with a leaf clone and
 * keeps the plain head as the OLH (§2.5a).  An FSIO open hint naming a
 * bucket blocks its deletion, and both purges before they delete
 * anything (§6.5).  With
 * --fsio-off (the feature disabled), no leaf clone at all.
 *
 *   ceph_test_librgw_file_fsio_put [--fsio-off] [--data-pool P]
 *     [--gc-pool P] [--gc-ns N] [--gc-shards N] [--hint-shards N]
 *   with AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY set
 */

#include <cstdint>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <unistd.h>

#include "include/rados/librgw.h"
#include "include/rados/rgw_file.h"
#include "include/rados/librados.hpp"
#include "cls/rgw/cls_rgw_fsio_ops.h"
#include "cls/rgw_gc/cls_rgw_gc_client.h"
#include "cls/rgw/cls_rgw_client.h"
#include "common/ceph_argparse.h"
#include "common/dout.h"
#include "rgw_lib.h"
#include "rgw_sal.h"
#include "global/global_context.h"
#include "gtest/gtest.h"

using namespace librados;

namespace {

librgw_t lrgw = nullptr;
struct rgw_fs* fs = nullptr;
struct rgw_file_handle* bucket_fh = nullptr;
struct rgw_file_handle* versioned_fh = nullptr;
struct rgw_file_handle* null_fh = nullptr;

std::string uid("testid");
std::string access_key, secret_key;
std::string bucket_name = fmt::format("fsio-put-{}", getpid());
std::string versioned_name = fmt::format("fsio-ver-{}", getpid());
std::string null_name = fmt::format("fsio-null-{}", getpid());
std::string data_pool = "default.rgw.buckets.data";
std::string gc_pool = "default.rgw.log";
std::string gc_ns = "gc";
int gc_shards = 32;
int hint_shards = 127;               // rgw_fsio_open_hints_num_shards
bool fsio_off = false;
bool ready = false;                  // INIT succeeded

Rados cluster;
IoCtx data;

constexpr size_t SIZE = 8 << 20;     // past the 4 MiB head:  a tail
constexpr cls_rgw_fsio_handle GW{0x7e57, 1};
constexpr cls_rgw_fsio_handle GW2{0x7e58, 1};   // a second gateway

struct {
  int argc;
  char** argv;
} saved_args;

/* write `fill` over the whole object through librgw:  RGW's PUT.  The
 * data pool exists once RGW has written an object, so it is opened
 * after the first */
void put(const std::string& name, char fill,
	 struct rgw_file_handle* parent = nullptr)
{
  struct rgw_file_handle* fh = nullptr;
  ASSERT_EQ(0, rgw_lookup(fs, parent ? parent : bucket_fh, name.c_str(), &fh,
			  nullptr, 0, RGW_LOOKUP_FLAG_CREATE));
  ASSERT_EQ(0, rgw_open(fs, fh, 0, 0));
  const std::string chunk(1 << 20, fill);
  for (size_t off = 0; off < SIZE; off += chunk.size()) {
    size_t n = 0;
    ASSERT_EQ(0, rgw_write(fs, fh, off, chunk.size(), &n,
			   (void*)chunk.data(), RGW_WRITE_FLAG_NONE));
  }
  ASSERT_EQ(0, rgw_close(fs, fh, RGW_CLOSE_FLAG_NONE));
  rgw_fh_rele(fs, fh, 0);
  if (!data.is_valid()) {
    ASSERT_EQ(0, cluster.ioctx_create(data_pool.c_str(), data));
  }
}

/* the data pool's objects:  name and locator */
std::vector<std::pair<std::string, std::string>> objects()
{
  std::vector<std::pair<std::string, std::string>> v;
  for (auto i = data.nobjects_begin(); i != data.nobjects_end(); ++i) {
    v.emplace_back(i->get_oid(), i->get_locator());
  }
  return v;
}

/* the head of `name`:  <marker>_<name>, none of FSIO's or RGW's own */
std::string head_of(const std::string& name)
{
  for (const auto& [oid, loc] : objects()) {
    if (oid.ends_with("_" + name) && oid.find("__") == std::string::npos &&
	oid.find('~') == std::string::npos) {
      return oid;
    }
  }
  return {};
}

std::string marker_of(const std::string& head, const std::string& name)
{
  return head.substr(0, head.size() - name.size() - 1);
}

/* every tail object in the pool;  one object's tail is the difference
 * across its first write, the bucket holding the other tests' too */
std::set<std::string> all_tails()
{
  std::set<std::string> s;
  if (!data.is_valid()) {
    return s;
  }
  /* <marker>__shadow_<prefix>_<n>, or a version's
   * <marker>__shadow:<instance>_<prefix>_<n> */
  for (const auto& [oid, loc] : objects()) {
    if (oid.find("__shadow_") != std::string::npos ||
	oid.find("__shadow:") != std::string::npos) {
      s.insert(oid);
    }
  }
  return s;
}

std::set<std::string> written_tails(const std::set<std::string>& before)
{
  std::set<std::string> s;
  for (const auto& t : all_tails()) {
    if (!before.count(t)) {
      s.insert(t);
    }
  }
  return s;
}

std::set<std::string> leaf_clones_of(const std::string& marker,
				     const std::string& name)
{
  std::set<std::string> s;
  for (const auto& [oid, loc] : objects()) {
    if (oid.starts_with(marker + "~fsio_G") && oid.ends_with("_" + name)) {
      s.insert(oid);
    }
  }
  return s;
}

bool exists(const std::string& oid)
{
  uint64_t size;
  time_t mtime;
  return data.stat(oid, &size, &mtime) == 0;
}

/* what the overwrite left, for a failure's reading */
void report(const std::string& head, const std::set<std::string>& tails)
{
  int8_t release = -1;
  cluster.get_min_compatible_osd(&release);
  bufferlist hdr;
  const int h = data.getxattr(head, "user.rgw.fsio.hdr", hdr);
  std::cout << "  OSD release floor " << int(release)
	    << ";  head's user.rgw.fsio.hdr " << (h >= 0 ? "kept" : "gone")
	    << std::endl;
  for (const auto& t : tails) {
    std::cout << "  tail " << t << (exists(t) ? " exists" : " gone")
	      << std::endl;
  }
}

/* every object name in the GC queue */
std::set<std::string> in_gc()
{
  IoCtx gc;
  std::set<std::string> s;
  if (cluster.ioctx_create(gc_pool.c_str(), gc) < 0) {
    return s;
  }
  gc.set_namespace(gc_ns);
  for (int i = 0; i < gc_shards; ++i) {
    std::string marker, next;
    bool truncated = true;
    while (truncated) {
      std::list<cls_rgw_gc_obj_info> entries;
      if (cls_rgw_gc_queue_list_entries(gc, fmt::format("gc.{}", i), marker,
					1000, false, entries, truncated,
					next) < 0) {
	break;
      }
      for (const auto& e : entries) {
	for (const auto& o : e.chain.objs) {
	  s.insert(o.key.name);
	}
      }
      marker = next;
    }
    /* a shard not yet in queue form keeps its entries in its omap */
    marker.clear();
    truncated = true;
    while (truncated) {
      ObjectReadOperation op;
      bufferlist out;
      cls_rgw_gc_list(op, marker, 1000, false, out);
      std::list<cls_rgw_gc_obj_info> entries;
      if (gc.operate(fmt::format("gc.{}", i), &op, nullptr) < 0 ||
	  cls_rgw_gc_list_decode(out, entries, truncated, next) < 0) {
	break;
      }
      for (const auto& e : entries) {
	for (const auto& o : e.chain.objs) {
	  s.insert(o.key.name);
	}
      }
      marker = next;
    }
  }
  return s;
}

/* an FSIO reader joins the head, as a gateway's open would */
void join_reader(const std::string& head, const std::string& marker)
{
  cls_rgw_fsio_join_op op;
  op.caller = GW;
  op.seq = 1;
  op.nonce = 1;
  op.counter_seed = 10;
  op.marker = marker;
  bufferlist in, out;
  encode(op, in);
  int rval = 0;
  ObjectWriteOperation w;
  w.exec(cls::rgw::method::fsio_join, in, &out, &rval);
  ASSERT_EQ(0, data.operate(head, &w, OPERATION_RETURNVEC));
}

} // anonymous namespace

TEST(FsioPut, INIT) {
  ASSERT_EQ(0, librgw_create(&lrgw, saved_args.argc, saved_args.argv));
  ASSERT_EQ(0, rgw_mount2(lrgw, uid.c_str(), access_key.c_str(),
			  secret_key.c_str(), "/", &fs, RGW_MOUNT_FLAG_NONE));
  struct stat st{};
  st.st_mode = 0755;
  struct rgw_file_handle* fh = nullptr;
  ASSERT_EQ(0, rgw_mkdir(fs, fs->root_fh, bucket_name.c_str(), &st,
			 RGW_SETATTR_MODE, &fh, RGW_MKDIR_FLAG_NONE));
  bucket_fh = fh;
  fh = nullptr;
  ASSERT_EQ(0, rgw_mkdir(fs, fs->root_fh, versioned_name.c_str(), &st,
			 RGW_SETATTR_MODE, &fh, RGW_MKDIR_FLAG_NONE));
  versioned_fh = fh;
  ASSERT_EQ(0, rgw_set_versioning(fs, versioned_fh, RGW_VERSIONING_ENABLED, 0));
  fh = nullptr;
  ASSERT_EQ(0, rgw_mkdir(fs, fs->root_fh, null_name.c_str(), &st,
			 RGW_SETATTR_MODE, &fh, RGW_MKDIR_FLAG_NONE));
  null_fh = fh;
  ASSERT_EQ(0, cluster.init_with_context(g_ceph_context));
  ASSERT_EQ(0, cluster.connect());
  /* open now if an earlier run created it, so the first test's snapshot
   * of the pool's tails sees theirs */
  (void)cluster.ioctx_create(data_pool.c_str(), data);
  ready = true;
}

/* an overwrite under an FSIO reader:  the leaf clone holds the old head,
 * the old tail is kept out of GC (§3.6) */
TEST(FsioPut, OverwriteUnderAReader) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  /* this run's own:  the data pool keeps earlier runs' objects */
  const auto name = fmt::format("under-reader-{}", getpid());
  const auto before = all_tails();
  put(name, 'a');
  const auto head = head_of(name);
  ASSERT_FALSE(head.empty());
  const auto marker = marker_of(head, name);
  const auto tails = written_tails(before);
  ASSERT_FALSE(tails.empty());
  join_reader(head, marker);
  put(name, 'b');

  const auto leaves = leaf_clones_of(marker, name);
  const auto gc = in_gc();
  report(head, tails);
  if (fsio_off) {
    EXPECT_TRUE(leaves.empty());       // the gate off:  today's path
    return;
  }
  ASSERT_EQ(1u, leaves.size());
  IoCtx keyed;
  keyed.dup(data);
  keyed.locator_set_key(head);
  bufferlist bl;
  ASSERT_LE(0, keyed.read(*leaves.begin(), bl, 1, 0));
  EXPECT_EQ('a', bl[0]);               // the old head's content
  for (const auto& t : tails) {
    EXPECT_TRUE(exists(t) && !gc.count(t)) << t << " collected under a reader";
  }
}

/* an overwrite with no FSIO open:  no leaf clone, the old tail to GC */
TEST(FsioPut, OverwriteWithNoReader) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  const auto name = fmt::format("no-reader-{}", getpid());
  const auto before = all_tails();
  put(name, 'a');
  const auto head = head_of(name);
  ASSERT_FALSE(head.empty());
  const auto marker = marker_of(head, name);
  const auto tails = written_tails(before);
  ASSERT_FALSE(tails.empty());
  put(name, 'b');
  EXPECT_TRUE(leaf_clones_of(marker, name).empty());
  const auto gc = in_gc();
  report(head, tails);
  /* collected:  queued for GC, or deleted inline where the process has
   * no GC (complete_atomic_modification(), a librgw without GC threads) */
  for (const auto& t : tails) {
    EXPECT_TRUE(!exists(t) || gc.count(t)) << t << " not collected";
  }
}

void remove(const std::string& name)
{
  ASSERT_EQ(0, rgw_unlink(fs, bucket_fh, name.c_str(), RGW_UNLINK_FLAG_NONE));
}

/* a DELETE under an FSIO reader:  the head stays, tracking-only, and the
 * tail the reader holds through the leaf clone is not collected (§3.6) */
TEST(FsioPut, DeleteUnderAReader) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  const auto name = fmt::format("delete-under-reader-{}", getpid());
  const auto before = all_tails();
  put(name, 'a');
  const auto head = head_of(name);
  ASSERT_FALSE(head.empty());
  const auto marker = marker_of(head, name);
  const auto tails = written_tails(before);
  ASSERT_FALSE(tails.empty());
  join_reader(head, marker);
  remove(name);
  const auto gc = in_gc();
  report(head, tails);
  if (fsio_off) {
    EXPECT_FALSE(exists(head));        // the gate off:  today's path
    return;
  }
  ASSERT_TRUE(exists(head));
  bufferlist bl;
  EXPECT_EQ(-ENODATA, data.getxattr(head, "user.rgw.idtag", bl));
  EXPECT_EQ(-ENODATA, data.getxattr(head, "user.rgw.manifest", bl));
  EXPECT_LE(0, data.getxattr(head, "user.rgw.fsio.hdr", bl));
  EXPECT_EQ(1u, leaf_clones_of(marker, name).size());
  for (const auto& t : tails) {
    EXPECT_TRUE(exists(t) && !gc.count(t)) << t << " collected under a reader";
  }
}

/* a DELETE with no FSIO open:  the head goes, the tail is collected */
TEST(FsioPut, DeleteWithNoReader) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  const auto name = fmt::format("delete-no-reader-{}", getpid());
  const auto before = all_tails();
  put(name, 'a');
  const auto head = head_of(name);
  ASSERT_FALSE(head.empty());
  const auto tails = written_tails(before);
  ASSERT_FALSE(tails.empty());
  remove(name);
  const auto gc = in_gc();
  report(head, tails);
  EXPECT_FALSE(exists(head));
  for (const auto& t : tails) {
    EXPECT_TRUE(!exists(t) || gc.count(t)) << t << " not collected";
  }
}

namespace {

struct version {
  std::string id;
  bool latest;
};

/* the versions of `name`, newest first, through rgw_list_versions, one
 * entry a page to cross its markers */
std::vector<version> versions_of(const std::string& name,
				 struct rgw_file_handle* bucket = nullptr)
{
  std::vector<version> v;
  std::string key_marker, vid_marker;
  bool truncated = true;
  while (truncated) {
    struct arg_t {
      std::vector<version>* v;
      std::string* km;
      std::string* vm;
      std::string name;
    } arg{&v, &key_marker, &vid_marker, name};
    const int r = rgw_list_versions(
      fs, bucket ? bucket : versioned_fh, name.c_str(), nullptr,
      key_marker.empty() ? nullptr : key_marker.c_str(),
      vid_marker.empty() ? nullptr : vid_marker.c_str(), 1,
      [](const char* n, const char* vid, bool latest, bool, uint64_t,
	 void* a) {
	auto* p = static_cast<arg_t*>(a);
	*p->km = n;
	*p->vm = vid;
	if (p->name == n) {
	  p->v->push_back(version{vid, latest});
	}
	return true;
      }, nullptr, &arg, &truncated, 0);
    EXPECT_EQ(0, r);
    if (r < 0) {
      break;
    }
  }
  return v;
}

/* a version's instance head:  <marker>__:<instance>_<name> */
std::string instance_head_of(const std::string& name, const std::string& vid)
{
  const auto suffix = "__:" + vid + "_" + name;
  for (const auto& [oid, loc] : objects()) {
    if (oid.ends_with(suffix) && oid.find('~') == std::string::npos) {
      return oid;
    }
  }
  return {};
}

/* the sibling clones of an instance head (§2.7):
 * <marker>~fsio_G<id>_<key part> */
std::set<std::string> siblings_of(const std::string& ih)
{
  const auto at = ih.find("__:");
  const auto marker = ih.substr(0, at);
  const auto key_part = ih.substr(at + 1);
  std::set<std::string> s;
  for (const auto& [oid, loc] : objects()) {
    if (oid.starts_with(marker + "~fsio_G") && oid.ends_with(key_part)) {
      s.insert(oid);
    }
  }
  return s;
}

/* an FSIO open counts the version, as a gateway's open of it would */
void count_open(const std::string& ih)
{
  cls_rgw_fsio_ocnt_op op;
  op.holder = GW;
  bufferlist in, out;
  encode(op, in);
  int rval = 0;
  ObjectWriteOperation w;
  w.exec(cls::rgw::method::fsio_ocnt_add, in, &out, &rval);
  ASSERT_EQ(0, data.operate(ih, &w, OPERATION_RETURNVEC));
}

/* two versions of `name`:  returns the older's id and the tails it
 * alone holds */
void two_versions(const std::string& name, std::string& older,
		  std::set<std::string>& tails)
{
  const auto before = all_tails();
  put(name, 'a', versioned_fh);
  tails = written_tails(before);
  put(name, 'b', versioned_fh);
  const auto v = versions_of(name);
  ASSERT_EQ(2u, v.size());
  EXPECT_TRUE(v[0].latest);
  EXPECT_FALSE(v[1].latest);
  older = v[1].id;
}

} // anonymous namespace

/* the version calls themselves:  listing in pages, removing "null" of
 * a key with none */
TEST(FsioPut, VersionCalls) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  const auto name = fmt::format("calls-{}", getpid());
  std::string older;
  std::set<std::string> tails;
  two_versions(name, older, tails);
  EXPECT_NE("null", older);
  EXPECT_EQ(-ENOENT, rgw_unlink_version(fs, versioned_fh, "no-such-key",
					"no-such-version", 0));
}

/* a version's removal under an open count:  the instance head reset to
 * its open count, the version's content on the sibling, its tail kept
 * (§2.5) */
TEST(FsioPut, RemoveVersionUnderAReader) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  const auto name = fmt::format("version-under-reader-{}", getpid());
  std::string older;
  std::set<std::string> tails;
  two_versions(name, older, tails);
  ASSERT_FALSE(tails.empty());
  const auto ih = instance_head_of(name, older);
  ASSERT_FALSE(ih.empty());
  count_open(ih);
  ASSERT_EQ(0, rgw_unlink_version(fs, versioned_fh, name.c_str(),
				  older.c_str(), 0));
  const auto gc = in_gc();
  report(ih, tails);
  const auto v = versions_of(name);
  ASSERT_EQ(1u, v.size());             // the version gone from the index
  EXPECT_NE(older, v[0].id);
  if (fsio_off) {
    EXPECT_FALSE(exists(ih));          // the gate off:  today's path
    return;
  }
  ASSERT_TRUE(exists(ih));
  bufferlist bl;
  EXPECT_EQ(-ENODATA, data.getxattr(ih, "user.rgw.idtag", bl));
  EXPECT_EQ(-ENODATA, data.getxattr(ih, "user.rgw.manifest", bl));
  EXPECT_LE(0, data.getxattr(ih, "user.rgw.fsio.ocnt", bl));
  const auto sibs = siblings_of(ih);
  ASSERT_EQ(1u, sibs.size());
  IoCtx keyed;
  keyed.dup(data);
  keyed.locator_set_key(ih);
  bl.clear();
  ASSERT_LE(0, keyed.read(*sibs.begin(), bl, 1, 0));
  EXPECT_EQ('a', bl[0]);               // the removed version's content
  for (const auto& t : tails) {
    EXPECT_TRUE(exists(t) && !gc.count(t)) << t << " collected under a reader";
  }
}

/* a version's removal with no open count:  it goes, as it always has */
TEST(FsioPut, RemoveVersionWithNoReader) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  const auto name = fmt::format("version-no-reader-{}", getpid());
  std::string older;
  std::set<std::string> tails;
  two_versions(name, older, tails);
  ASSERT_FALSE(tails.empty());
  const auto ih = instance_head_of(name, older);
  ASSERT_FALSE(ih.empty());
  ASSERT_EQ(0, rgw_unlink_version(fs, versioned_fh, name.c_str(),
				  older.c_str(), 0));
  const auto gc = in_gc();
  report(ih, tails);
  EXPECT_FALSE(exists(ih));
  EXPECT_TRUE(siblings_of(ih).empty());
  for (const auto& t : tails) {
    EXPECT_TRUE(!exists(t) || gc.count(t)) << t << " not collected";
  }
}

namespace {

/* a noncurrent null version of `name` under a current instance:  'a'
 * written suspended, on the plain head;  'b' written enabled.  Returns
 * the plain head and the tails the null version alone holds */
void null_under_a_version(const std::string& name, std::string& head,
			  std::set<std::string>& tails,
			  std::string* current = nullptr)
{
  ASSERT_EQ(0, rgw_set_versioning(fs, null_fh, RGW_VERSIONING_SUSPENDED, 0));
  const auto before = all_tails();
  put(name, 'a', null_fh);
  tails = written_tails(before);
  head = head_of(name);
  ASSERT_EQ(0, rgw_set_versioning(fs, null_fh, RGW_VERSIONING_ENABLED, 0));
  put(name, 'b', null_fh);
  const auto v = versions_of(name, null_fh);
  ASSERT_EQ(2u, v.size());
  EXPECT_NE("null", v[0].id);
  EXPECT_EQ("null", v[1].id);
  if (current) {
    *current = v[0].id;
  }
}

/* an FSIO writer joins the current instance, from a second gateway:
 * its shadow becomes the header's cur_shadow */
cls_rgw_fsio_id join_writer(const std::string& head, const std::string& marker,
			    const std::string& instance)
{
  cls_rgw_fsio_join_op op;
  op.caller = GW2;
  op.seq = 1;
  op.writer = true;
  op.instance = instance;
  op.nonce = 2;
  op.counter_seed = 20;
  op.marker = marker;
  bufferlist in, out;
  encode(op, in);
  int rval = 0;
  ObjectWriteOperation w;
  w.exec(cls::rgw::method::fsio_join, in, &out, &rval);
  EXPECT_EQ(0, data.operate(head, &w, OPERATION_RETURNVEC));
  cls_rgw_fsio_join_ret ret;
  auto it = out.cbegin();
  decode(ret, it);
  return ret.bound;
}

std::optional<cls_rgw_fsio_id> cur_shadow_of(const std::string& head)
{
  bufferlist bl;
  if (data.getxattr(head, "user.rgw.fsio.hdr", bl) < 0) {
    return std::nullopt;
  }
  cls_rgw_fsio_hdr hdr;
  auto it = bl.cbegin();
  decode(hdr, it);
  return hdr.cur_shadow;
}

} // anonymous namespace

/* the null version's removal under an FSIO reader:  the HEAD chunk set
 * retired with a leaf clone, its tail kept, the plain head kept as the
 * OLH (§2.5a).  A writer on the current version keeps its shadow:  the
 * null version's removal leaves the current generation, where the
 * delete would start a new one */
TEST(FsioPut, RemoveNullVersionUnderAReader) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  const auto name = fmt::format("null-under-reader-{}", getpid());
  std::string head, current;
  std::set<std::string> tails;
  null_under_a_version(name, head, tails, &current);
  ASSERT_FALSE(head.empty());
  ASSERT_FALSE(tails.empty());
  const auto marker = marker_of(head, name);
  std::optional<cls_rgw_fsio_id> shadow;
  if (!fsio_off) {
    shadow = join_writer(head, marker, current);
    ASSERT_EQ(shadow, cur_shadow_of(head));
  }
  join_reader(head, marker);
  ASSERT_EQ(0, rgw_unlink_version(fs, null_fh, name.c_str(), "null", 0));
  const auto gc = in_gc();
  report(head, tails);
  const auto v = versions_of(name, null_fh);
  ASSERT_EQ(1u, v.size());             // the null version gone from the index
  EXPECT_NE("null", v[0].id);
  ASSERT_TRUE(exists(head));           // the OLH
  bufferlist bl;
  EXPECT_LE(0, data.getxattr(head, "user.rgw.olh.info", bl));
  if (fsio_off) {
    EXPECT_TRUE(leaf_clones_of(marker, name).empty());
    return;
  }
  EXPECT_EQ(-ENODATA, data.getxattr(head, "user.rgw.idtag", bl));
  EXPECT_EQ(-ENODATA, data.getxattr(head, "user.rgw.manifest", bl));
  EXPECT_LE(0, data.getxattr(head, "user.rgw.fsio.hdr", bl));
  EXPECT_EQ(shadow, cur_shadow_of(head)) << "the current generation reset";
  const auto leaves = leaf_clones_of(marker, name);
  ASSERT_EQ(1u, leaves.size());
  IoCtx keyed;
  keyed.dup(data);
  keyed.locator_set_key(head);
  bl.clear();
  ASSERT_LE(0, keyed.read(*leaves.begin(), bl, 1, 0));
  EXPECT_EQ('a', bl[0]);               // the null version's content
  for (const auto& t : tails) {
    EXPECT_TRUE(exists(t) && !gc.count(t)) << t << " collected under a reader";
  }
}

/* the null version's removal with no FSIO open:  collected as usual,
 * the plain head kept as the OLH */
TEST(FsioPut, RemoveNullVersionWithNoReader) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  const auto name = fmt::format("null-no-reader-{}", getpid());
  std::string head;
  std::set<std::string> tails;
  null_under_a_version(name, head, tails);
  ASSERT_FALSE(head.empty());
  ASSERT_FALSE(tails.empty());
  const auto marker = marker_of(head, name);
  ASSERT_EQ(0, rgw_unlink_version(fs, null_fh, name.c_str(), "null", 0));
  const auto gc = in_gc();
  report(head, tails);
  const auto v = versions_of(name, null_fh);
  ASSERT_EQ(1u, v.size());
  EXPECT_NE("null", v[0].id);
  ASSERT_TRUE(exists(head));
  bufferlist bl;
  EXPECT_LE(0, data.getxattr(head, "user.rgw.olh.info", bl));
  EXPECT_TRUE(leaf_clones_of(marker, name).empty());
  for (const auto& t : tails) {
    EXPECT_TRUE(!exists(t) || gc.count(t)) << t << " not collected";
  }
}

namespace {

/* an FSIO open hint naming `bucket`, in the last shard of the zone's log
 * pool (gc_pool's), so the check reads every shard */
void hint(const std::string& bucket, const std::string& bucket_id,
	  const std::string& name, const std::string& head, bool add)
{
  IoCtx log;
  ASSERT_EQ(0, cluster.ioctx_create(gc_pool.c_str(), log));
  cls_rgw_fsio_hint_op op;
  op.hint.bucket.name = bucket;
  op.hint.bucket.bucket_id = bucket_id;
  op.hint.bucket.marker = bucket_id;
  op.hint.key = rgw_obj_key(name);
  op.hint.incarnation = GW.incarnation;
  op.hint.handle = GW.handle;
  op.oid = head;
  bufferlist in, out;
  encode(op, in);
  ObjectWriteOperation w;
  w.exec(add ? cls::rgw::method::fsio_open_hint_add :
	 cls::rgw::method::fsio_open_hint_remove, in, &out, nullptr);
  ASSERT_EQ(0, log.operate(fmt::format("fsio_open_hint.{:010d}", hint_shards - 1),
			   &w));
}

} // anonymous namespace

/* a hint naming the bucket blocks its deletion;  without it, the empty
 * bucket goes (§6.5) */
TEST(FsioPut, HintBlocksBucketDelete) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  const auto bname = fmt::format("fsio-hint-{}", getpid());
  struct stat st{};
  st.st_mode = 0755;
  struct rgw_file_handle* fh = nullptr;
  ASSERT_EQ(0, rgw_mkdir(fs, fs->root_fh, bname.c_str(), &st,
			 RGW_SETATTR_MODE, &fh, RGW_MKDIR_FLAG_NONE));
  /* one object, written and removed:  its head names the bucket's marker,
   * a new bucket's bucket_id */
  const auto name = fmt::format("hinted-{}", getpid());
  put(name, 'a', fh);
  const auto head = head_of(name);
  ASSERT_FALSE(head.empty());
  const auto marker = marker_of(head, name);
  ASSERT_EQ(0, rgw_unlink(fs, fh, name.c_str(), RGW_UNLINK_FLAG_NONE));
  rgw_fh_rele(fs, fh, 0);

  hint(bname, marker, name, head, true);
  const int r = rgw_unlink(fs, fs->root_fh, bname.c_str(), RGW_UNLINK_FLAG_NONE);
  if (fsio_off) {
    EXPECT_EQ(0, r);                   // the gate off:  today's path
    hint(bname, marker, name, head, false);
    return;
  }
  EXPECT_EQ(-ENOTEMPTY, r);
  hint(bname, marker, name, head, false);
  EXPECT_EQ(0, rgw_unlink(fs, fs->root_fh, bname.c_str(), RGW_UNLINK_FLAG_NONE));
}

/* both purges, as radosgw-admin bucket rm --purge-objects and
 * --bypass-gc call them (rgw_bucket.cc):  a hint naming the bucket fails
 * each before it deletes anything;  without it, the purge goes (§6.5) */
TEST(FsioPut, HintBlocksPurge) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  const auto bname = fmt::format("fsio-purge-{}", getpid());
  struct stat st{};
  st.st_mode = 0755;
  struct rgw_file_handle* fh = nullptr;
  ASSERT_EQ(0, rgw_mkdir(fs, fs->root_fh, bname.c_str(), &st,
			 RGW_SETATTR_MODE, &fh, RGW_MKDIR_FLAG_NONE));
  const auto name = fmt::format("purged-{}", getpid());
  put(name, 'a', fh);
  rgw_fh_rele(fs, fh, 0);
  const auto head = head_of(name);
  ASSERT_FALSE(head.empty());
  const auto marker = marker_of(head, name);

  const DoutPrefix dp(g_ceph_context, ceph_subsys_rgw, "fsio_put test: ");
  auto* driver = rgw::g_rgwlib->get_driver();
  auto load = [&]() {
    std::unique_ptr<rgw::sal::Bucket> b;
    rgw_bucket key;
    key.name = bname;
    EXPECT_EQ(0, driver->load_bucket(&dp, key, &b, null_yield));
    return b;
  };

  hint(bname, marker, name, head, true);
  {
    auto b = load();
    ASSERT_TRUE(b);
    const int r = b->remove(&dp, true, null_yield);
    if (fsio_off) {
      EXPECT_EQ(0, r);                 // the gate off:  today's path
      hint(bname, marker, name, head, false);
      return;
    }
    EXPECT_EQ(-ENOTEMPTY, r);
  }
  EXPECT_TRUE(exists(head)) << "--purge-objects deleted before the check";
  {
    auto b = load();
    ASSERT_TRUE(b);
    EXPECT_EQ(-ENOTEMPTY, b->remove_bypass_gc(128, true, null_yield, &dp));
  }
  EXPECT_TRUE(exists(head)) << "--bypass-gc deleted before the check";

  hint(bname, marker, name, head, false);
  {
    auto b = load();
    ASSERT_TRUE(b);
    EXPECT_EQ(0, b->remove(&dp, true, null_yield));
  }
  EXPECT_FALSE(exists(head));
  std::unique_ptr<rgw::sal::Bucket> gone;
  rgw_bucket key;
  key.name = bname;
  EXPECT_EQ(-ENOENT, driver->load_bucket(&dp, key, &gone, null_yield));
}

TEST(FsioPut, SHUTDOWN) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
  rgw_fh_rele(fs, null_fh, 0);
  rgw_fh_rele(fs, versioned_fh, 0);
  rgw_fh_rele(fs, bucket_fh, 0);
  rgw_umount(fs, RGW_UMOUNT_FLAG_NONE);
  librgw_shutdown(lrgw);
}

int main(int argc, char* argv[])
{
  auto args = argv_to_vec(argc, argv);
  env_to_vec(args);
  if (auto v = getenv("AWS_ACCESS_KEY_ID")) {
    access_key = v;
  }
  if (auto v = getenv("AWS_SECRET_ACCESS_KEY")) {
    secret_key = v;
  }
  std::string val;
  for (auto i = args.begin(); i != args.end();) {
    if (ceph_argparse_flag(args, i, "--fsio-off", (char*)nullptr)) {
      fsio_off = true;
    } else if (ceph_argparse_witharg(args, i, &val, "--data-pool", (char*)nullptr)) {
      data_pool = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--gc-pool", (char*)nullptr)) {
      gc_pool = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--gc-ns", (char*)nullptr)) {
      gc_ns = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--gc-shards", (char*)nullptr)) {
      gc_shards = std::stoi(val);
    } else if (ceph_argparse_witharg(args, i, &val, "--hint-shards", (char*)nullptr)) {
      hint_shards = std::stoi(val);
    } else if (ceph_argparse_witharg(args, i, &val, "--uid", (char*)nullptr)) {
      uid = val;
    } else {
      ++i;
    }
  }
  if (access_key.empty() || secret_key.empty()) {
    std::cout << argv[0] << " no AWS credentials, exiting" << std::endl;
    return EPERM;
  }
  saved_args.argc = argc;
  saved_args.argv = argv;
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
