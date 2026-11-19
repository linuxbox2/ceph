// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* FSIO's PUT hook through RGW's own write path (FSIO_RADOS_DESIGN.md
 * §3.6):  librgw writes and overwrites objects, librados joins an FSIO
 * reader on a head between the two.  With the fsio zonegroup feature
 * enabled, an overwrite under a reader leaves the leaf clone and keeps
 * the old tail out of GC;  without a reader, the tail goes to GC as
 * today.  With --fsio-off (the feature disabled), no leaf clone at all.
 *
 *   ceph_test_librgw_file_fsio_put [--fsio-off] [--data-pool P]
 *     [--gc-pool P] [--gc-ns N] [--gc-shards N]
 *   with AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY set
 */

#include <cstdint>
#include <iostream>
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
#include "global/global_context.h"
#include "gtest/gtest.h"

using namespace librados;

namespace {

librgw_t lrgw = nullptr;
struct rgw_fs* fs = nullptr;
struct rgw_file_handle* bucket_fh = nullptr;

std::string uid("testid");
std::string access_key, secret_key;
std::string bucket_name = fmt::format("fsio-put-{}", getpid());
std::string data_pool = "default.rgw.buckets.data";
std::string gc_pool = "default.rgw.log";
std::string gc_ns = "gc";
int gc_shards = 32;
bool fsio_off = false;
bool ready = false;                  // INIT succeeded

Rados rados;
IoCtx data;

constexpr size_t SIZE = 8 << 20;     // past the 4 MiB head:  a tail
constexpr cls_rgw_fsio_handle GW{0x7e57, 1};

struct {
  int argc;
  char** argv;
} saved_args;

/* write `fill` over the whole object through librgw:  RGW's PUT.  The
 * data pool exists once RGW has written an object, so it is opened
 * after the first */
void put(const std::string& name, char fill)
{
  struct rgw_file_handle* fh = nullptr;
  ASSERT_EQ(0, rgw_lookup(fs, bucket_fh, name.c_str(), &fh, nullptr, 0,
			  RGW_LOOKUP_FLAG_CREATE));
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
    ASSERT_EQ(0, rados.ioctx_create(data_pool.c_str(), data));
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
  for (const auto& [oid, loc] : objects()) {
    if (oid.find("__shadow_") != std::string::npos) {
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
  rados.get_min_compatible_osd(&release);
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
  if (rados.ioctx_create(gc_pool.c_str(), gc) < 0) {
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
  ASSERT_EQ(0, rados.init_with_context(g_ceph_context));
  ASSERT_EQ(0, rados.connect());
  /* open now if an earlier run created it, so the first test's snapshot
   * of the pool's tails sees theirs */
  (void)rados.ioctx_create(data_pool.c_str(), data);
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

TEST(FsioPut, SHUTDOWN) {
  if (!ready) {
    GTEST_SKIP() << "INIT failed";
  }
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
