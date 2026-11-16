// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab
/*
 * ceph_bench_bluestore_clone:  what a whole-object clone into a same-hash
 * sibling costs in BlueStore, and what sharing costs afterwards.  This is
 * the clone CEPH_OSD_OP_CLONE_TO performs.  A measurement, not a test:  it
 * prints a table and asserts only that the store behaves.
 *
 * Variants (compression, checksums, legacy shared blobs) are chosen with
 * ordinary config options on the command line, e.g.
 *   ceph_bench_bluestore_clone --bluestore_compression_mode=force
 *   ceph_bench_bluestore_clone --bluestore_csum_type=none
 *   ceph_bench_bluestore_clone --bluestore_elastic_shared_blobs=false
 * so each run is one configuration and the program runs the size/extent
 * matrix within it.
 *
 * Options of its own:
 *   --objects N   objects per cell (default 32)
 *   --dir PATH    store directory (default ./bluestore.clone_bench.<time>);
 *                 removed at exit unless --keep
 *   --copy        the control:  write each sibling as an independent copy
 *                 of its source instead of cloning it, so the store holds
 *                 the same objects and bytes without sharing (for fsck)
 *   --deep-fsck   end with a deep fsck, which reads the data, instead of
 *                 a shallow one
 */

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "os/ObjectStore.h"
#include "os/bluestore/BlueStore.h"
#include "os/bluestore/BlueStore_objects.h"

namespace {

using clk = std::chrono::steady_clock;

struct Bench {
  std::unique_ptr<ObjectStore> store;
  coll_t cid;
  ObjectStore::CollectionHandle ch;
  int64_t pool = 1234;
  /* the collection is pg 1234.1, whose range holds hashes with the low bit
   * set;  every object here uses an odd hash so it is listable, which the
   * sibling unshare on remove depends on */
  uint32_t next_hash = 0x10000001;

  BlueStore* bs() { return dynamic_cast<BlueStore*>(store.get()); }

  void apply(ObjectStore::Transaction&& t) {
    int r = store->queue_transaction(ch, std::move(t));
    ceph_assert(r == 0);
  }

  ghobject_t make_source(const std::string& name) {
    uint32_t h = next_hash;
    next_hash += 2;
    return ghobject_t(hobject_t(sobject_t(name, CEPH_NOSNAP), "", h, pool, ""));
  }

  /* <anchor>~<counter><nonce>, with the anchor as locator key (§6.8) */
  static ghobject_t make_sibling(const ghobject_t& anchor, uint64_t counter) {
    ghobject_t sib = anchor;
    sib.hobj.oid.name = fmt::format("{}~{:016x}{:016x}", anchor.hobj.oid.name,
                                    counter, uint64_t{0xfeedfacecafebeef});
    sib.hobj.set_key(anchor.hobj.get_effective_key());
    return sib;
  }

  /* write `size` bytes as `pieces` writes in separate transactions, odd
   * pieces first and even pieces after, so they are not one sequential
   * stream;  the extents BlueStore actually made are counted afterwards */
  void write_pieces(const ghobject_t& o, uint64_t size, unsigned pieces) {
    const uint64_t psz = size / pieces;
    bufferlist bl;
    bl.append(std::string(psz, 'x'));
    for (unsigned pass = 0; pass < 2; ++pass) {
      for (unsigned i = pass; i < pieces; i += 2) {
        ObjectStore::Transaction t;
        t.write(cid, o, i * psz, psz, bl);
        apply(std::move(t));
      }
    }
  }

  size_t count_extents(const ghobject_t& o) {
    auto on = bs()->debug_get_onode(cid, o);
    on->extent_map.fault_range(bs()->get_kv(), 0, on->onode.size);
    return on->extent_map.extent_map.size();
  }

  size_t count_shared_blob_records() {
    auto it = bs()->get_kv()->get_iterator("X");   // PREFIX_SHARED_BLOB
    size_t n = 0;
    for (it->lower_bound(std::string()); it->valid(); it->next()) {
      ++n;
    }
    return n;
  }

  store_statfs_t statfs() {
    store_statfs_t st;
    int r = store->statfs(&st);
    ceph_assert(r == 0);
    return st;
  }
};

double usec(clk::duration d) {
  return std::chrono::duration<double, std::micro>(d).count();
}

} // anonymous namespace

int main(int argc, char** argv)
{
  auto args = argv_to_vec(argc, argv);
  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                         CODE_ENVIRONMENT_UTILITY,
                         CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);

  unsigned objects = 32;
  std::string dir = fmt::format("bluestore.clone_bench.{}", time(nullptr));
  bool keep = false;
  bool copy = false;
  bool deep_fsck = false;
  for (auto i = args.begin(); i != args.end(); ++i) {
    std::string_view a{*i};
    if (a == "--objects" && std::next(i) != args.end()) {
      objects = std::atoi(*++i);
    } else if (a == "--dir" && std::next(i) != args.end()) {
      dir = *++i;
    } else if (a == "--keep") {
      keep = true;
    } else if (a == "--copy") {
      copy = true;
    } else if (a == "--deep-fsck") {
      deep_fsck = true;
    }
  }

  g_ceph_context->_conf._clear_safe_to_start_threads();
  g_ceph_context->_conf.set_val_or_die("bluestore_block_size", "21474836480");
  g_ceph_context->_conf.apply_changes(nullptr);

  std::filesystem::create_directories(dir);
  Bench b;
  b.store = ObjectStore::create(g_ceph_context, "bluestore", dir, "");
  ceph_assert(b.store->mkfs() == 0);
  ceph_assert(b.store->mount() == 0);
  b.cid = coll_t(spg_t(pg_t(1, b.pool), shard_id_t::NO_SHARD));
  b.ch = b.store->create_new_collection(b.cid);
  {
    ObjectStore::Transaction t;
    t.create_collection(b.cid, 0);
    b.apply(std::move(t));
  }

  const auto& conf = g_ceph_context->_conf;
  std::cout << fmt::format(
    "# {} compression_mode={} csum_type={} elastic_shared_blobs={} "
    "min_alloc_size={} objects/cell={}\n",
    copy ? "copy (control)" : "clone",
    conf.get_val<std::string>("bluestore_compression_mode"),
    conf.get_val<std::string>("bluestore_csum_type"),
    conf.get_val<bool>("bluestore_elastic_shared_blobs"),
    b.store->get_min_alloc_size(), objects);
  std::cout << "# size pieces extents clone_us stored_delta allocated_delta "
               "sb_records overwrite_shared_us overwrite_plain_us "
               "overwrite_shared_alloc overwrite_plain_alloc\n";

  const std::vector<uint64_t> sizes{64 << 10, 1 << 20, 4 << 20};
  const std::vector<unsigned> pieces{1, 16, 256};
  std::vector<ghobject_t> all_sources;
  unsigned cell = 0;

  for (auto size : sizes) {
    for (auto np : pieces) {
      if (size / np < 4096) {
        continue;
      }
      std::vector<ghobject_t> src, sib;
      for (unsigned i = 0; i < objects; ++i) {
        src.push_back(b.make_source(fmt::format("c{}.o{}", cell, i)));
        b.write_pieces(src.back(), size, np);
        sib.push_back(Bench::make_sibling(src.back(), 1));
      }
      const size_t extents = b.count_extents(src.front());

      /* clone each source to its sibling, one transaction each */
      const auto st0 = b.statfs();
      const size_t sb0 = b.count_shared_blob_records();
      const auto t0 = clk::now();
      for (unsigned i = 0; i < objects; ++i) {
        if (copy) {
          b.write_pieces(sib[i], size, np);
        } else {
          ObjectStore::Transaction t;
          t.clone(b.cid, src[i], sib[i]);
          b.apply(std::move(t));
        }
      }
      const double clone_us = usec(clk::now() - t0) / objects;
      const auto st1 = b.statfs();
      const size_t sb1 = b.count_shared_blob_records();

      /* a 4 KiB overwrite in the middle:  into a shared blob (a sibling),
       * and into an unshared one (a fresh object of the same shape) */
      bufferlist w;
      w.append(std::string(4096, 'y'));
      const uint64_t off = (size / 2) & ~uint64_t{4095};
      const auto a0 = b.statfs().allocated;
      const auto t1 = clk::now();
      for (unsigned i = 0; i < objects; ++i) {
        ObjectStore::Transaction t;
        t.write(b.cid, sib[i], off, w.length(), w);
        b.apply(std::move(t));
      }
      const double ow_shared_us = usec(clk::now() - t1) / objects;
      const auto a1 = b.statfs().allocated;

      std::vector<ghobject_t> plain;
      for (unsigned i = 0; i < objects; ++i) {
        plain.push_back(b.make_source(fmt::format("c{}.p{}", cell, i)));
        b.write_pieces(plain.back(), size, np);
      }
      const auto a2 = b.statfs().allocated;
      const auto t2 = clk::now();
      for (unsigned i = 0; i < objects; ++i) {
        ObjectStore::Transaction t;
        t.write(b.cid, plain[i], off, w.length(), w);
        b.apply(std::move(t));
      }
      const double ow_plain_us = usec(clk::now() - t2) / objects;
      const auto a3 = b.statfs().allocated;

      std::cout << fmt::format(
        "{} {} {} {:.1f} {} {} {} {:.1f} {:.1f} {} {}\n",
        size, np, extents, clone_us,
        int64_t(st1.data_stored) - int64_t(st0.data_stored),
        int64_t(st1.allocated) - int64_t(st0.allocated),
        int64_t(sb1) - int64_t(sb0),
        ow_shared_us, ow_plain_us,
        (int64_t(a1) - int64_t(a0)) / objects,
        (int64_t(a3) - int64_t(a2)) / objects);

      all_sources.insert(all_sources.end(), src.begin(), src.end());
      ++cell;
    }
  }

  /* fsck on the populated store:  shared blobs are part of what it checks.
   * BlueStore's fsck opens the store itself, so it runs unmounted. */
  const size_t sb_total = b.count_shared_blob_records();
  b.ch.reset();
  b.store->umount();
  {
    const auto t0 = clk::now();
    int r = b.store->fsck(deep_fsck);
    std::cout << fmt::format("# fsck deep={} errors={} us={:.0f} shared_blob_records={}\n",
                             deep_fsck, r, usec(clk::now() - t0), sb_total);
  }
  if (!keep) {
    std::filesystem::remove_all(dir);
  }
  return 0;
}
