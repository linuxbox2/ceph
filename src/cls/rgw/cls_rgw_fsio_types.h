// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* FSIO on RADOS:  the records the fsio_* methods keep on a head, on its
 * clones and instance heads, and in the FSIO open hints
 * (FSIO_RADOS_DESIGN.md §2.7).  Included by cls_rgw_types.h. */

#pragma once

#include <compare>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/ceph_time.h"
#include "common/Formatter.h"
#include "include/buffer.h"
#include "include/encoding.h"
#include "rgw/rgw_basic_types.h"

#define CLS_RGW_ERR_FSIO_BUSY     2310 // also in rgw_common.h, don't change!
#define CLS_RGW_ERR_FSIO_CONFLICT 2311 // also in rgw_common.h, don't change!

struct cls_rgw_fsio_id {
  uint64_t counter = 0;
  uint64_t nonce = 0;

  auto operator<=>(const cls_rgw_fsio_id&) const = default;
  bool empty() const { return counter == 0 && nonce == 0; }
  std::string to_str() const {
    return fmt::format("{:016x}{:016x}", counter, nonce);
  }

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(counter, bl);
    encode(nonce, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(counter, bl);
    decode(nonce, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_id> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_id)

/* chunk indexes, inclusive (§2.7, Z11) */
struct cls_rgw_fsio_run {
  uint64_t first = 0;
  uint64_t last = 0;
  auto operator<=>(const cls_rgw_fsio_run&) const = default;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(first, bl);
    encode(last, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(first, bl);
    decode(last, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_run> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_run)

struct cls_rgw_fsio_shadow {
  uint64_t size_hint = 0;
  ceph::real_time mtime_hint;
  uint64_t trunc_epoch = 0;
  std::optional<uint64_t> trunc_low;
  /* reserved runs:  sorted, disjoint, merged when they touch, at most
   * rgw_fsio_extent_max_runs (§3.7, Z11) */
  std::vector<cls_rgw_fsio_run> extent;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(2, 2, bl);
    encode(size_hint, bl);
    encode(mtime_hint, bl);
    encode(trunc_epoch, bl);
    encode(trunc_low, bl);
    encode(extent, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(2, bl);
    decode(size_hint, bl);
    decode(mtime_hint, bl);
    decode(trunc_epoch, bl);
    decode(trunc_low, bl);
    if (struct_v >= 2) {
      decode(extent, bl);
    } else {
      uint64_t highest;                 // version 1:  one highest index
      decode(highest, bl);
      extent = {{0, highest}};
    }
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_shadow> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_shadow)

/* who holds FSIO state:  one SAL FSIOObject;  liveness is its
 * incarnation's (design §2.7, Z4) */
struct cls_rgw_fsio_handle {
  uint64_t incarnation = 0;    // the gateway process
  uint64_t handle = 0;         // a per-process counter;  0 is the FSIO expirer

  auto operator<=>(const cls_rgw_fsio_handle&) const = default;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(incarnation, bl);
    encode(handle, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(incarnation, bl);
    decode(handle, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_handle> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_handle)

struct cls_rgw_fsio_publishing {
  cls_rgw_fsio_handle publisher;  // a writer, or a fenced finalize's finalizer
  std::string instance;
  std::string token;
  uint64_t olh_epoch = 0;
  std::string identity;           // whose PUT the publish is (§3.5, Z14)

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(publisher, bl);
    encode(instance, bl);
    encode(token, bl);
    encode(olh_epoch, bl);
    encode(identity, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(publisher, bl);
    decode(instance, bl);
    decode(token, bl);
    decode(olh_epoch, bl);
    decode(identity, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_publishing> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_publishing)

enum class cls_rgw_fsio_cs_state : uint8_t {
  CONSTRUCTING = 1,
  READY = 2,
  PUBLISHING = 3,
  PUBLISHED = 4,
  RETIRED = 5,
};

enum class cls_rgw_fsio_manifest_at : uint8_t {
  HEAD = 1,
  CLONE = 2,
  SHADOW = 3,
  INSTANCE = 4,
};

struct cls_rgw_fsio_chunk_set {
  cls_rgw_fsio_id id;
  cls_rgw_fsio_cs_state state = cls_rgw_fsio_cs_state::CONSTRUCTING;
  cls_rgw_fsio_manifest_at manifest_at = cls_rgw_fsio_manifest_at::SHADOW;
  std::optional<cls_rgw_fsio_id> clone;
  std::optional<std::string> instance;
  std::optional<cls_rgw_fsio_shadow> shadow;
  std::optional<cls_rgw_fsio_publishing> publishing;
  bool drained = false;

  bool is_shadow() const {
    return state == cls_rgw_fsio_cs_state::CONSTRUCTING ||
      state == cls_rgw_fsio_cs_state::READY ||
      state == cls_rgw_fsio_cs_state::PUBLISHING;
  }

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(id, bl);
    encode(static_cast<uint8_t>(state), bl);
    encode(static_cast<uint8_t>(manifest_at), bl);
    encode(clone, bl);
    encode(instance, bl);
    encode(shadow, bl);
    encode(publishing, bl);
    encode(drained, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(id, bl);
    uint8_t v;
    decode(v, bl);
    state = static_cast<cls_rgw_fsio_cs_state>(v);
    decode(v, bl);
    manifest_at = static_cast<cls_rgw_fsio_manifest_at>(v);
    decode(clone, bl);
    decode(instance, bl);
    decode(shadow, bl);
    decode(publishing, bl);
    decode(drained, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_chunk_set> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_chunk_set)

struct cls_rgw_fsio_fenced_finalize {
  cls_rgw_fsio_id from;
  cls_rgw_fsio_id to;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(from, bl);
    encode(to, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(from, bl);
    decode(to, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_fenced_finalize> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_fenced_finalize)

struct cls_rgw_fsio_hdr {
  uint64_t nonce = 0;
  uint64_t next_counter = 0;
  std::optional<cls_rgw_fsio_id> cur_shadow;   // the current generation's shadow
  std::string cur_instance;
  std::vector<cls_rgw_fsio_chunk_set> chunk_sets;
  std::vector<cls_rgw_fsio_fenced_finalize> fenced_finalizes;
  std::optional<cls_rgw_fsio_handle> rename_hold;  // the renamer (§6.3, Z4)

  cls_rgw_fsio_id draw() { return cls_rgw_fsio_id{next_counter++, nonce}; }
  cls_rgw_fsio_chunk_set* find(const cls_rgw_fsio_id& id);
  const cls_rgw_fsio_chunk_set* find(const cls_rgw_fsio_id& id) const;
  /* the fenced finalize whose from or to is cs */
  cls_rgw_fsio_fenced_finalize* find_finalize(const cls_rgw_fsio_id& cs);
  bool empty() const {
    return chunk_sets.empty() && fenced_finalizes.empty() && !rename_hold;
  }

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(nonce, bl);
    encode(next_counter, bl);
    encode(cur_shadow, bl);
    encode(cur_instance, bl);
    encode(chunk_sets, bl);
    encode(fenced_finalizes, bl);
    encode(rename_hold, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(nonce, bl);
    decode(next_counter, bl);
    decode(cur_shadow, bl);
    decode(cur_instance, bl);
    decode(chunk_sets, bl);
    decode(fenced_finalizes, bl);
    decode(rename_hold, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_hdr> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_hdr)

struct cls_rgw_fsio_counts {
  uint32_t readers = 0;
  uint32_t writers = 0;
  std::string writer_identity;
  uint64_t joined_seq = 0;     // the header's counter when it joined as writer (Z14)

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(readers, bl);
    encode(writers, bl);
    encode(writer_identity, bl);
    encode(joined_seq, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(readers, bl);
    decode(writers, bl);
    decode(writer_identity, bl);
    decode(joined_seq, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_counts> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_counts)

/* one handle's FSIO opens on the head */
struct cls_rgw_fsio_handle_opens {
  uint64_t last_seq = 0;       // the newest sequenced call applied (Z22)
  std::map<cls_rgw_fsio_id, cls_rgw_fsio_counts> chunk_sets;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(last_seq, bl);
    encode(chunk_sets, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(last_seq, bl);
    decode(chunk_sets, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_handle_opens> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_handle_opens)

/* user.rgw.fsio.m.<incarnation>:  one gateway incarnation's FSIO opens,
 * by handle (Z4) */
struct cls_rgw_fsio_opens {
  std::map<uint64_t, cls_rgw_fsio_handle_opens> handles;
  /* a presumed-dead incarnation's handles whose counts expiry removed:
   * each one's last_seq, kept until its grace has elapsed, so a resend
   * still in flight is answered already applied (§3, Z22) */
  std::map<uint64_t, uint64_t> seqs;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(2, 1, bl);
    encode(handles, bl);
    encode(seqs, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(2, bl);
    decode(handles, bl);
    if (struct_v >= 2) {
      decode(seqs, bl);
    }
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_opens> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_opens)

/* user.rgw.fsio.gen, on a record clone f(head, r) */
struct cls_rgw_fsio_gen {
  ceph::buffer::list manifest;
  std::string gc_tag;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(manifest, bl);
    encode(gc_tag, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(manifest, bl);
    decode(gc_tag, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_gen> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_gen)

/* §2.7:  content an instance head's removal or rewrite deferred */
struct cls_rgw_fsio_replaced {
  ceph::buffer::list manifest;         // the replaced RGW_ATTR_MANIFEST, opaque
  std::string gc_tag;                  // tail tag, else the ID tag
  cls_rgw_fsio_id clone;               // the sibling f(instance, r)

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(manifest, bl);
    encode(gc_tag, bl);
    encode(clone, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(manifest, bl);
    decode(gc_tag, bl);
    decode(clone, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_replaced> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_replaced)

/* user.rgw.fsio.ocnt:  the instance-head open count (§2.5) */
struct cls_rgw_fsio_ocnt {
  std::set<cls_rgw_fsio_handle> holders;  // handles with FSIO opens bound here (Z4)
  std::vector<cls_rgw_fsio_replaced> deferred;
  bool removed = false;                // the version itself is gone

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(holders, bl);
    encode(deferred, bl);
    encode(removed, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(holders, bl);
    decode(deferred, bl);
    decode(removed, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_ocnt> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_ocnt)

/* the FSIO open hints (FSIO_RADOS_DESIGN.md §2.7, §4.3):  omap items of
 * the shard objects fsio_open_hint.NNNNNNNNNN in the zone's log pool,
 * the shard chosen by the hash of the target's oid */
enum class cls_rgw_fsio_hint_kind : uint8_t {
  PRESENCE = 1,   // the incarnation holds FSIO state on the target
  PUBLISH  = 2,   // one publish's leftovers on the target
};

enum class cls_rgw_fsio_hint_target : uint8_t {
  HEAD          = 1,  // a head:  the OLH head, in a versioned bucket
  INSTANCE_HEAD = 2,  // a version's instance head
};

struct cls_rgw_fsio_hint {
  cls_rgw_fsio_hint_kind kind = cls_rgw_fsio_hint_kind::PRESENCE;
  cls_rgw_fsio_hint_target target = cls_rgw_fsio_hint_target::HEAD;
  rgw_bucket bucket;              // to reach the bucket index and the OLH
  rgw_obj_key key;                // the name;  with its instance, for INSTANCE_HEAD
  uint64_t incarnation = 0;       // whose state it covers
  uint64_t handle = 0;            // which of its handles (Z4)
  std::optional<cls_rgw_fsio_id> r;  // PUBLISH on a HEAD:  the leaf clone

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(static_cast<uint8_t>(kind), bl);
    encode(static_cast<uint8_t>(target), bl);
    encode(bucket, bl);
    encode(key, bl);
    encode(incarnation, bl);
    encode(handle, bl);
    encode(r, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    uint8_t k, t;
    decode(k, bl);
    decode(t, bl);
    kind = static_cast<cls_rgw_fsio_hint_kind>(k);
    target = static_cast<cls_rgw_fsio_hint_target>(t);
    decode(bucket, bl);
    decode(key, bl);
    decode(incarnation, bl);
    decode(handle, bl);
    decode(r, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_hint> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_hint)

/* §2.7, E2:  a clone of a head (or instance head) lives after the
 * bucket's heads, <marker>~fsio_<role><id>_<key part>, the key part being
 * the head's oid after <marker>_;  the class and RGW name clones by it */
inline std::string cls_rgw_fsio_clone_name(std::string_view marker, char role,
					   const cls_rgw_fsio_id& id,
					   std::string_view head_oid)
{
  return fmt::format("{}~fsio_{}{}_{}", marker, role, id.to_str(),
		     head_oid.substr(marker.size() + 1));
}
