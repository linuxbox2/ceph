// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/ceph_time.h"
#include "include/buffer.h"
#include "include/encoding.h"

/* FSIO on RADOS:  the head's FSIO state and the transitions of the
 * fsio_* methods, as pure functions over decoded state.  The cls_rgw
 * methods are thin wrappers (read the xattrs, call the transition, apply
 * its effects, write the xattrs);  the model checker
 * (test/cls_rgw/fsio_model.cc) explores the same functions. */

#define CLS_RGW_ERR_FSIO_BUSY     2310
#define CLS_RGW_ERR_FSIO_CONFLICT 2311

/* Each fix's guard is a named check (docs/FSIO_RADOS_ADVERSARIAL_REVIEW.md).
 * A build defining CLS_RGW_FSIO_SKIP_CHECKS (the unit test and the model
 * checker, never cls_rgw) can skip one, to show that the tests or the
 * model fail without it. */
enum class cls_rgw_fsio_check : uint8_t {
  NONE,
  W5_FINALIZE_RECHECKS_STATE,          // fsio_fenced_finalize
  W6_EXPIRE_TAKES_OVER_PUBLISH,        // fsio_expire
  W3_FINALIZE_CREATES_TO_PUBLISHING,   // fsio_fenced_finalize
  W3_RESUMED_FINALIZE_TAKES_OVER,      // fsio_fenced_finalize
  W3_PUBLISHING_TO_RETIRES_FROM,       // fsio_publish
  W1_SHADOW_STORES_PSEUDO_MANIFEST,    // fsio_join
  CUR_SHADOW_SUPERSEDE,                // fsio_replace_head:  cleared by a supersede
  CUR_SHADOW_PUBLISH,                  // fsio_publish:  cleared by a publish
  CUR_SHADOW_ABANDON,                  // fsio_expire:  cleared with an abandoned construction
  W7_LEAVE_RETURNS_VERSION_READ,       // fsio_leave
  W7_REBIND_WAITS_ON_PUBLISHING,       // fsio_rebind
  W7_PUBLISH_COUNTS_READERS_ONLY,      // the gateway's publish (the model);  a
                                       // refinement:  skipping it changes no result
  V1_FINALIZE_REQUIRES_ABORTED_CLAIM,  // fsio_fenced_finalize, versioned
  W9_HINT_SPARES_WRITTEN_VERSION,      // the expirer's instance-head hint (model)
  W9_HINT_WAITS_FOR_WRITER,            // the expirer's instance-head hint (model)
  V2_COMPLETE_REMOVES_DEAD_WRITERS,    // fsio_publish, versioned complete form
  V3_FINALIZER_HINTS_PLACEHOLDER,      // the expirer's versioned finalize (model)
  V4_HINT_WAITS_FOR_GRACE,             // the expirer's instance-head hint (model)
  V4_PUBLISHER_KEEPS_HINT,             // the gateway's failed instance write (model)
  NULL_IS_HEAD,                        // fsio_join, published_chunk_set:  null is the head's content
  NULL_REMOVAL_RETIRES,                // fsio_replace_head, remove-null mode
  PUBLISH_FOLLOWS_STATE,               // the gateway's publish, suspended bucket (model)
  PRESENCE_HINT_BEFORE_JOIN,           // the gateway's head presence hint (model)
  PRESENCE_HINT_BEFORE_OCNT,           // the gateway's instance-head presence hint (model)
  PRESENCE_HINT_HELD_UNTIL_RELEASED,   // the gateway's head presence hint (model)
  H1_FAILED_RELEASE_KEEPS_HINT,        // the gateway's head presence hint (model)
  H2_HINT_DROPPED_AFTER_GRACE,         // the expirer's presence-hint drop (model)
  H3_PUBLISHER_HINTS_READERS,          // the gateway's versioned publish (model)
  H3_TRACKING_ONLY_IS_REMOVED,         // the expirer's claim resolution (model)
  H3_HINT_WAITS_FOR_LIVE_CLAIM,        // the expirer's presence-hint drop (model)
  H3_PUBLISH_HINT_REMOVES_READER_HINTS, // the expirer's publish-hint rule (model)
  H3_PUBLISHER_CLEANUP_REMOVES_READER_HINTS,  // the gateway's aborted-placeholder cleanup (model)
  I4_PUBLISHER_NOT_COUNTED,            // the gateway's versioned publish (model)
  W10_RELEASE_ONLY_WITH_LIVE_CLAIM,    // the gateway's versioned release (model)
  W10_RELEASE_ABORTS_CLAIM_FIRST,      // the gateway's versioned release (model)
  W15_CONSTRUCTOR_MAY_ABANDON,         // fsio_leave, a constructing shadow
  W20_RELEASE_CHECKS_PUBLISHER,        // fsio_publish_release
  RESUME_RESOLVES_CLAIM,               // the expirer's resumed versioned finalize (model)
  RESUME_REPLACES_CLAIM,               // fsio_fenced_finalize, a takeover with an intent
  Z22_COUNTED_CALL_CHECKS_SEQ,         // fsio_join, fsio_leave, fsio_rebind
  Z22_ABANDON_WAITS_FOR_GRACE,         // the expirer's abandon of a construction (model)
  Z22_EXPIRY_KEEPS_SEQ,                // settle, an expired handle's last_seq
  Z11_EXTENT_CAPS_RUNS,                // fsio_extend, fsio_ready:  rgw_fsio_extent_max_runs
  Z6_INSTANCE_WRITE_DROPS_CLAIM,       // the versioned instance write's post op (model)
  Z6_RESOLUTION_TESTS_ID_TAG,          // the claim resolution and hint rule (model)
};

#ifdef CLS_RGW_FSIO_SKIP_CHECKS
namespace cls_rgw_fsio {
/* defined in cls_rgw_fsio.cc;  a second, for a check observable only
 * once another is skipped too */
extern cls_rgw_fsio_check skipped_check, also_skipped_check;
}
#define FSIO_SKIP_CHECK(c) \
  (::cls_rgw_fsio::skipped_check == cls_rgw_fsio_check::c || \
   ::cls_rgw_fsio::also_skipped_check == cls_rgw_fsio_check::c)
#else
#define FSIO_SKIP_CHECK(c) false
#endif

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
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_handle)

struct cls_rgw_fsio_publishing {
  cls_rgw_fsio_handle publisher;  // a writer, or a fenced finalize's finalizer
  std::string instance;
  std::string token;
  uint64_t olh_epoch = 0;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(publisher, bl);
    encode(instance, bl);
    encode(token, bl);
    encode(olh_epoch, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(publisher, bl);
    decode(instance, bl);
    decode(token, bl);
    decode(olh_epoch, bl);
    DECODE_FINISH(bl);
  }
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
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_gen)

/* what one fsio_* method sees and changes:  the head's RGW attributes it
 * reads, and every user.rgw.fsio. xattr */
struct cls_rgw_fsio_head {
  bool exists = false;
  std::string id_tag;                  // RGW_ATTR_ID_TAG;  empty when tracking-only
  std::string tail_tag;                // RGW_ATTR_TAIL_TAG, when set
  ceph::buffer::list manifest;         // RGW_ATTR_MANIFEST, opaque
  std::optional<cls_rgw_fsio_hdr> hdr;
  std::map<uint64_t, cls_rgw_fsio_opens> opens;  // by incarnation
  bool olh = false;                    // carries RGW_ATTR_OLH_INFO (versioned)

  /* §2.2:  no ID tag and no OLH info */
  bool tracking_only() const { return exists && id_tag.empty() && !olh; }
  /* every handle's counts bound to cs, summed */
  cls_rgw_fsio_counts bound(const cls_rgw_fsio_id& cs) const;
  /* one handle's opens, or null */
  cls_rgw_fsio_handle_opens* find(const cls_rgw_fsio_handle& c);
  /* any handle's opens;  an expired handle's kept last_seq is not one */
  bool holds_opens() const;
  /* one handle's counts on cs, created if absent */
  cls_rgw_fsio_counts& counts(const cls_rgw_fsio_handle& c,
			      const cls_rgw_fsio_id& cs);

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(exists, bl);
    encode(id_tag, bl);
    encode(manifest, bl);
    encode(hdr, bl);
    encode(opens, bl);
    encode(olh, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(exists, bl);
    decode(id_tag, bl);
    decode(manifest, bl);
    decode(hdr, bl);
    decode(opens, bl);
    decode(olh, bl);
    DECODE_FINISH(bl);
  }
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_head)

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
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_ocnt)

/* what the instance-head methods see and change */
struct cls_rgw_fsio_instance_head {
  bool exists = false;
  std::string id_tag;                  // empty once removed:  tracking-only
  std::string tail_tag;                // RGW_ATTR_TAIL_TAG, when set
  ceph::buffer::list manifest;         // RGW_ATTR_MANIFEST, opaque
  std::optional<cls_rgw_fsio_ocnt> ocnt;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(2, 1, bl);
    encode(exists, bl);
    encode(id_tag, bl);
    encode(manifest, bl);
    encode(ocnt, bl);
    encode(tail_tag, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(2, bl);
    decode(exists, bl);
    decode(id_tag, bl);
    decode(manifest, bl);
    decode(ocnt, bl);
    if (struct_v >= 2) {
      decode(tail_tag, bl);
    }
    DECODE_FINISH(bl);
  }
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_instance_head)

/* a CLONE_TO the method issues before it touches the head (§3) */
struct cls_rgw_fsio_clone {
  cls_rgw_fsio_id r;
  char role = 'G';           // sorts before 'a' and 'd':  a clone source (§2.7)
  cls_rgw_fsio_gen record;   // set_attrs, for role 'G'
  ceph::buffer::list pm;     // set_attrs, for role 'a':  the pseudo-manifest (§2.4)
};

struct cls_rgw_fsio_effects {
  std::vector<cls_rgw_fsio_clone> clones;
  bool remove_head = false;   // tracking-only, nothing left (§2.2)
};

namespace cls_rgw_fsio {

using expired_t = std::set<uint64_t>;

/* the sequenced calls (fsio_join, fsio_leave, fsio_rebind,
 * fsio_truncate) carry the handle's sequence number;  one not newer
 * than the handle's last_seq changes nothing and returns
 * already_applied (§3, Z22) */
struct join_in {
  cls_rgw_fsio_handle caller;
  uint64_t seq = 0;
  bool writer = false;
  uint64_t nonce = 0;
  uint64_t counter_seed = 0;
  expired_t expired;
  std::string instance;             // versioned:  the instance the OLH named
};
struct join_out {
  cls_rgw_fsio_id bound;
  bool construct = false;
  bool already_applied = false;
};
int join(cls_rgw_fsio_head& h, const join_in& in, join_out& out,
	 cls_rgw_fsio_effects& fx);

/* rgw_fsio_extent_max_runs's default (§7) */
constexpr size_t extent_max_runs = 64;

/* adds a run to an extent:  runs that overlap or touch merge, and past
 * max_runs the adjacent pair with the smallest gap merges (§2.7, Z11) */
void add_run(std::vector<cls_rgw_fsio_run>& extent, cls_rgw_fsio_run run,
	     size_t max_runs);

/* the constructor's clones done:  the shadow is ready, its extent the
 * runs it cloned */
int ready(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	  const cls_rgw_fsio_id& cs,
	  const std::vector<cls_rgw_fsio_run>& runs,
	  size_t max_runs = extent_max_runs);

/* a writer reserves a run before creating a chunk in it */
int extend(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	   const cls_rgw_fsio_id& cs, const cls_rgw_fsio_run& run,
	   size_t max_runs = extent_max_runs);

/* §3.3, §2.7:  a writer's size and mtime hint, merged by max within a
 * truncate epoch;  an older epoch merges nothing, and the writer rebases
 * from the current epoch and size returned */
struct hint_out {
  bool stale = false;          // the caller's epoch was older:  nothing merged
  bool others = false;         // another handle has an open on the shadow or the published set
  uint64_t trunc_epoch = 0;
  uint64_t size = 0;
};
int hint(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	 const cls_rgw_fsio_id& cs, uint64_t trunc_epoch_seen, uint64_t size,
	 ceph::real_time mtime, hint_out& out);

/* §3.3:  ftruncate():  size and mtime set exactly, a new truncate epoch,
 * trunc_low lowered to the size;  after the chunks changed.  A sequenced
 * call (§2.2, Z22):  one no newer than the handle's last_seq changes
 * nothing and returns already applied */
struct truncate_out {
  uint64_t trunc_epoch = 0;    // the new epoch
  bool already_applied = false;
};
int truncate(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	     uint64_t seq, const cls_rgw_fsio_id& cs, uint64_t size,
	     ceph::real_time mtime, truncate_out& out);

/* versioned:  the publish the last writer's transition records (§4.2) */
struct publish_intent {
  std::string instance;             // chosen before the placeholder
  std::string token;                // the claim on the placeholder
  uint64_t now_epoch = 0;           // the OSD's, read by the CLS wrapper
};

struct leave_out {
  bool already_applied = false;
  bool publishing = false;
  bool abandoned = false;           // a constructing shadow, its constructor gave up (§3.4)
  uint64_t olh_epoch = 0;           // versioned:  recorded at the transition
  std::string instance;             // versioned:  the version the open read (§2.5)
  std::optional<cls_rgw_fsio_id> drained;
};
int leave(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	  uint64_t seq, const cls_rgw_fsio_id& cs, bool writer,
	  const expired_t& expired, leave_out& out, cls_rgw_fsio_effects& fx,
	  const publish_intent* intent = nullptr);

struct rebind_out {
  bool already_applied = false;
};
int rebind(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	   uint64_t seq, const cls_rgw_fsio_id& from, const cls_rgw_fsio_id& to,
	   uint32_t readers, rebind_out* out = nullptr);

enum class replace_mode { SUPERSEDE, DELETE, PUBLISH,
  REMOVE_NULL };                   // version removal of the null version (§2.5a)
struct replace_in {
  replace_mode mode = replace_mode::SUPERSEDE;
  cls_rgw_fsio_id r;
  std::string expected_id_tag;      // DELETE
  cls_rgw_fsio_handle caller;       // PUBLISH:  the publisher
  cls_rgw_fsio_id shadow;           // PUBLISH
  bool versioned = false;           // PUBLISH:  the standalone call on the OLH
  bool complete = false;            // versioned PUBLISH:  for a dead publisher
  expired_t expired;                // with complete
  bool keep_tail = false;           // SUPERSEDE:  the new object reuses the tail (Y2)
};
struct replace_out {
  bool keep_tail = false;           // else the caller collects the replaced chain
  bool born_drained = false;        // PUBLISH:  the replaced content named only by the hint
  std::optional<cls_rgw_fsio_id> drained;
};
int replace_head(cls_rgw_fsio_head& h, const replace_in& in, replace_out& out,
		 cls_rgw_fsio_effects& fx);

int publish_release(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
		    const cls_rgw_fsio_id& cs);

/* after an incarnation's grace:  the last_seq its expired handles kept
 * goes (§4.3, Z22) */
int drop_seqs(cls_rgw_fsio_head& h, uint64_t incarnation,
	      cls_rgw_fsio_effects& fx);

int forget(cls_rgw_fsio_head& h, const cls_rgw_fsio_id& cs,
	   cls_rgw_fsio_effects& fx);

int fenced_finalize(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& finalizer,
		    const cls_rgw_fsio_id& from, const expired_t& expired,
		    cls_rgw_fsio_id& to, const publish_intent* intent = nullptr,
		    const std::string& aborted = {});

struct expire_out {
  std::vector<cls_rgw_fsio_id> drained;
};
int expire(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	   const expired_t& expired,
	   const std::optional<cls_rgw_fsio_id>& clear, expire_out& out,
	   cls_rgw_fsio_effects& fx);

/* the instance head's methods (§2.5) */

/* when the open count empties with content deferred or the version
 * removed:  what the caller sends to GC before ocnt_forget() */
struct ocnt_drain {
  bool drained = false;
  std::vector<cls_rgw_fsio_replaced> deferred;
  bool removed = false;
};

int ocnt_add(cls_rgw_fsio_instance_head& ih, const cls_rgw_fsio_handle& holder);
int ocnt_remove(cls_rgw_fsio_instance_head& ih, const cls_rgw_fsio_handle& holder,
		ocnt_drain& out);
int ocnt_expire(cls_rgw_fsio_instance_head& ih, const expired_t& expired,
		ocnt_drain& out);
int ocnt_forget(cls_rgw_fsio_instance_head& ih, cls_rgw_fsio_effects& fx);

struct remove_out {
  bool keep_tail = false;           // else the caller collects the chain
};
int remove_version(cls_rgw_fsio_instance_head& ih, const cls_rgw_fsio_id& r,
		   remove_out& out, cls_rgw_fsio_effects& fx);

} // namespace cls_rgw_fsio
