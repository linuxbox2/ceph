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
#include "cls/rgw/cls_rgw_types.h"

/* FSIO on RADOS:  the head's FSIO state and the transitions of the
 * fsio_* methods, as pure functions over decoded state.  The cls_rgw
 * methods are thin wrappers (read the xattrs, call the transition, apply
 * its effects, write the xattrs);  the model checker
 * (test/cls_rgw/fsio_model.cc) explores the same functions. */

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
  HINT_BEFORE_EVERY_OCNT_ADD,          // the gateway's versioned open (model)
  GONE_KEEPS_HINT_ONLY_IF_HELD,        // the gateway's versioned open (model)
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
  A29_CLEAR_RECHECKS_CONSTRUCTOR,      // fsio_expire's clear
  A13_REWRITE_DEFERS,                  // fsio_replace_head on an instance head
  JOIN_BINDS_LINKED_PUBLISH,           // fsio_join, §3.1 step 4
  A26_KEEP_TAIL_RECORDS_NO_MANIFEST,   // fsio_replace_head's supersede, §3.6
  SHARED_TAIL_AFTER_LAST_READER,       // fsio_replace_head, §3.6
  HINT_STALE_MERGES_NOTHING,           // fsio_hint, §3.3
  W14_PUBLISHING_FREEZES_SIZE,         // fsio_hint, fsio_truncate, §3.3
  REBASE_READS_CHUNKS,                 // the gateway's stale size report (model)
  LOST_REPLY_KEEPS_HINT,               // the gateway's lost reply (model)
  SETTLE_KEEPS_SIBLING_SEQ,            // settle():  per handle, Z4, Z22
  LOST_TRUNCATE_KEEPS_EPOCH,           // the gateway's lost truncate (model)
  FINALIZE_RESEND_RETURNS_TO,          // fsio_fenced_finalize, §3.7
  FORGET_REPORTS_DRAIN_BY_STATE,       // fsio_forget, §3.4
  C1_PUBLISH_COMPARES_TAGS,            // fsio_replace_head's publish, §3.5
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
  ceph::buffer::list tags;             // RGW_ATTR_TAGS, opaque;  empty when absent

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
    encode(tags, bl);
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
    decode(tags, bl);
    DECODE_FINISH(bl);
  }
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_head)

/* §3.5, Y18, Z15:  the tags an unversioned publish keeps:  the committed
 * head's when it is an S3 object (it has an ID tag), else the tags the
 * content had, which the publisher passes.  The gateway evaluates policy
 * with the same rule (C1) */
inline ceph::buffer::list fsio_publish_tags(bool head_is_object,
					    const ceph::buffer::list& head_tags,
					    const ceph::buffer::list& passed)
{
  return head_is_object ? head_tags : passed;
}

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
/* §3.1 step 0, Z24:  a writer's create-open */
enum class create_mode : uint8_t { NONE, UNCHECKED, EXCLUSIVE };
struct join_in {
  cls_rgw_fsio_handle caller;
  uint64_t seq = 0;
  bool writer = false;
  uint64_t nonce = 0;
  uint64_t counter_seed = 0;
  expired_t expired;
  std::string instance;             // versioned:  the instance the OLH named
  std::string identity;             // a writer:  its NFS open's (§3.5, Y16)
  /* §2.7:  the head object holds data, an inline head segment, so its
   * clone is that segment's data chunk, role 'd';  else 'a' */
  bool head_data = false;
  create_mode create = create_mode::NONE;
};
struct join_out {
  cls_rgw_fsio_id bound;
  bool construct = false;
  bool already_applied = false;
  /* §3.1:  what the joining gateway needs, as the join saw it */
  cls_rgw_fsio_cs_state state{};        // the bound chunk set's
  bool others = false;                  // another handle bound here (Z4)
  uint64_t trunc_epoch = 0;             // a shadow's:  its record's
  uint64_t size_hint = 0;
  ceph::real_time mtime_hint;
  std::optional<cls_rgw_fsio_run> last_run;   // extent's highest run (Y23)
};
int join(cls_rgw_fsio_head& h, const join_in& in, join_out& out,
	 cls_rgw_fsio_effects& fx);

/* rgw_fsio_extent_max_runs's default (§7) */
constexpr size_t extent_max_runs = 64;

/* adds a run to an extent:  runs that overlap or touch merge, and past
 * max_runs the adjacent pair with the smallest gap merges (§2.7, Z11) */
void add_run(std::vector<cls_rgw_fsio_run>& extent, cls_rgw_fsio_run run,
	     size_t max_runs);

/* §3.5, Y16:  a later write open on the handle with another identity:
 * its writer count on the chunk set takes it;  idempotent by its
 * effect, so not a sequenced call */
int writer_identity(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
		    const cls_rgw_fsio_id& cs, const std::string& identity);

/* the constructor's clones done:  the shadow is ready, its extent the
 * runs it cloned, its hint the published object's size and mtime */
int ready(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	  const cls_rgw_fsio_id& cs,
	  const std::vector<cls_rgw_fsio_run>& runs,
	  uint64_t size = 0, ceph::real_time mtime = {},
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
  uint64_t now_epoch = 0;           // the OSD's, set by the CLS wrapper (X31)
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
	  const publish_intent* intent = nullptr, bool handoff = false);

/* §3.8, 8j:  the expirer takes an uncompleted publish:  a ready shadow
 * with uncompleted_publish and no writer bound becomes publishing, the
 * caller its publisher;  versioned, with the caller's claim and epoch */
int take_uncompleted(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
		     const cls_rgw_fsio_id& cs,
		     const publish_intent* intent = nullptr);

struct rebind_out {
  bool already_applied = false;
  std::optional<cls_rgw_fsio_id> drained;   // a retired chunk set it emptied (§3.4)
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
  ceph::buffer::list tags;          // PUBLISH onto a tracking-only head (Z15)
  /* unversioned PUBLISH:  the tags the publisher evaluated policy with;
   * the publish fails with ECANCELED if it would keep others (C1) */
  std::optional<ceph::buffer::list> expected_tags;
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

/* §3.4, §3.6:  `drained`, a chunk set the removal let drain (a shared
 * tail's holder), which the caller sends and forgets in turn */
int forget(cls_rgw_fsio_head& h, const cls_rgw_fsio_id& cs,
	   cls_rgw_fsio_effects& fx,
	   std::optional<cls_rgw_fsio_id>* drained = nullptr);

int fenced_finalize(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& finalizer,
		    const cls_rgw_fsio_id& from, const expired_t& expired,
		    cls_rgw_fsio_id& to, const publish_intent* intent = nullptr,
		    const std::string& aborted = {});

struct expire_out {
  std::vector<cls_rgw_fsio_id> drained;
  bool finalize = false;    // a shadow needs a fenced finalize (§3.8)
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

/* §3, §4.3:  on a version that is gone (absent, or its count removed),
 * nothing is added:  gone, and whether the count holds the holder */
struct ocnt_add_out {
  bool gone = false;
  bool held = false;
};
int ocnt_add(cls_rgw_fsio_instance_head& ih, const cls_rgw_fsio_handle& holder,
	     ocnt_add_out* out = nullptr);
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
/* §2.5, Y6:  a version rewritten in place (a lifecycle or cloud-tier
 * transition, a restore, an admin rewrite) under a non-empty open count:
 * its old content cloned to a sibling and recorded in `deferred`, the
 * tail kept;  the version stays.  A write reusing the tail records no
 * manifest (§3.6, Y2) */
int rewrite_version(cls_rgw_fsio_instance_head& ih, const cls_rgw_fsio_id& r,
		    bool keep_tail, remove_out& out, cls_rgw_fsio_effects& fx);

} // namespace cls_rgw_fsio
