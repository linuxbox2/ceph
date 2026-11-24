// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* The fsio_* CLS methods' inputs and replies (FSIO_RADOS_DESIGN.md §2.7,
 * §8).  Each method decodes its input, loads the head's FSIO records,
 * runs the transition in cls_rgw_fsio.cc, stores what changed, applies
 * the effects and encodes its reply.  Every reply has a fixed maximum
 * within osd_max_write_op_reply_len, which FSIO requires at 128 bytes
 * (§2.7 rule 4, W18). */

#pragma once

#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/encoding.h"
#include "include/rados/cls_traits.hpp"
#include "cls/rgw/cls_rgw_ops.h"

/* the claim a versioned publish or finalize carries (§4.2) */
struct cls_rgw_fsio_intent {
  std::string instance;
  std::string token;
  uint64_t now_epoch = 0;        // ignored:  the OSD's is used (§4.2, X31)

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(instance, bl);
    encode(token, bl);
    encode(now_epoch, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(instance, bl);
    decode(token, bl);
    decode(now_epoch, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_intent> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_intent)

/* fsio_join */
struct cls_rgw_fsio_join_op {
  cls_rgw_fsio_handle caller;
  uint64_t seq = 0;
  bool writer = false;
  uint64_t nonce = 0;
  uint64_t counter_seed = 0;
  std::set<uint64_t> expired;
  std::string instance;          // versioned:  the instance the OLH named
  std::string marker;            // the bucket's, to name a head clone (§2.7)
  std::string identity;          // a writer:  its NFS open's (§3.5, Y16)

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(caller, bl);
    encode(seq, bl);
    encode(writer, bl);
    encode(nonce, bl);
    encode(counter_seed, bl);
    encode(expired, bl);
    encode(instance, bl);
    encode(marker, bl);
    encode(identity, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(caller, bl);
    decode(seq, bl);
    decode(writer, bl);
    decode(nonce, bl);
    decode(counter_seed, bl);
    decode(expired, bl);
    decode(instance, bl);
    decode(marker, bl);
    decode(identity, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_join_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_join_op)

/* fsio_writer_identity (§3.5, Y16) */
struct cls_rgw_fsio_identity_op {
  cls_rgw_fsio_handle caller;
  cls_rgw_fsio_id cs;
  std::string identity;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(caller, bl);
    encode(cs, bl);
    encode(identity, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(caller, bl);
    decode(cs, bl);
    decode(identity, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_identity_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_identity_op)

struct cls_rgw_fsio_join_ret {
  cls_rgw_fsio_id bound;
  bool construct = false;
  bool already_applied = false;
  /* §3.1:  what the joining gateway needs, as the join saw it */
  uint8_t state = 0;             // cls_rgw_fsio_cs_state:  the bound chunk set's
  bool others = false;           // another handle bound here (Z4)
  uint64_t trunc_epoch = 0;      // a shadow's:  its record's
  uint64_t size_hint = 0;
  ceph::real_time mtime_hint;
  std::optional<cls_rgw_fsio_run> last_run;   // extent's highest run (Y23)

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(bound, bl);
    encode(construct, bl);
    encode(already_applied, bl);
    encode(state, bl);
    encode(others, bl);
    encode(trunc_epoch, bl);
    encode(size_hint, bl);
    encode(mtime_hint, bl);
    encode(last_run, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(bound, bl);
    decode(construct, bl);
    decode(already_applied, bl);
    decode(state, bl);
    decode(others, bl);
    decode(trunc_epoch, bl);
    decode(size_hint, bl);
    decode(mtime_hint, bl);
    decode(last_run, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_join_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_join_ret)

/* fsio_ready, fsio_extend */
struct cls_rgw_fsio_runs_op {
  cls_rgw_fsio_handle caller;
  cls_rgw_fsio_id cs;
  std::vector<cls_rgw_fsio_run> runs;   // ready:  the runs cloned;  extend:  one
  uint64_t size = 0;                    // ready:  the starting hint (§3.2)
  ceph::real_time mtime;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(caller, bl);
    encode(cs, bl);
    encode(runs, bl);
    encode(size, bl);
    encode(mtime, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(caller, bl);
    decode(cs, bl);
    decode(runs, bl);
    decode(size, bl);
    decode(mtime, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_runs_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_runs_op)

/* fsio_leave */
struct cls_rgw_fsio_leave_op {
  cls_rgw_fsio_handle caller;
  uint64_t seq = 0;
  cls_rgw_fsio_id cs;
  bool writer = false;
  std::set<uint64_t> expired;
  std::optional<cls_rgw_fsio_intent> intent;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(caller, bl);
    encode(seq, bl);
    encode(cs, bl);
    encode(writer, bl);
    encode(expired, bl);
    encode(intent, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(caller, bl);
    decode(seq, bl);
    decode(cs, bl);
    decode(writer, bl);
    decode(expired, bl);
    decode(intent, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_leave_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_leave_op)

struct cls_rgw_fsio_leave_ret {
  bool already_applied = false;
  bool publishing = false;
  bool abandoned = false;
  uint64_t olh_epoch = 0;
  std::string instance;          // at most OBJ_INSTANCE_LEN, 32 bytes
  std::optional<cls_rgw_fsio_id> drained;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(already_applied, bl);
    encode(publishing, bl);
    encode(abandoned, bl);
    encode(olh_epoch, bl);
    encode(instance, bl);
    encode(drained, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(already_applied, bl);
    decode(publishing, bl);
    decode(abandoned, bl);
    decode(olh_epoch, bl);
    decode(instance, bl);
    decode(drained, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_leave_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_leave_ret)

/* fsio_hint (§3.3):  a writer's size and mtime, with the truncate epoch
 * it last saw */
struct cls_rgw_fsio_hint_size_op {
  cls_rgw_fsio_handle caller;
  cls_rgw_fsio_id cs;
  uint64_t trunc_epoch = 0;
  uint64_t size = 0;
  ceph::real_time mtime;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(caller, bl);
    encode(cs, bl);
    encode(trunc_epoch, bl);
    encode(size, bl);
    encode(mtime, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(caller, bl);
    decode(cs, bl);
    decode(trunc_epoch, bl);
    decode(size, bl);
    decode(mtime, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_hint_size_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_hint_size_op)

/* the current epoch and size, to rebase from;  whether the report was
 * stale, and whether another handle has an open bound to the shadow or
 * the published chunk set */
struct cls_rgw_fsio_hint_size_ret {
  bool stale = false;
  bool others = false;
  uint64_t trunc_epoch = 0;
  uint64_t size = 0;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(stale, bl);
    encode(others, bl);
    encode(trunc_epoch, bl);
    encode(size, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(stale, bl);
    decode(others, bl);
    decode(trunc_epoch, bl);
    decode(size, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_hint_size_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_hint_size_ret)

/* fsio_truncate (§3.3):  the size and mtime ftruncate() set */
struct cls_rgw_fsio_truncate_op {
  cls_rgw_fsio_handle caller;
  uint64_t seq = 0;              // a sequenced call (§2.2, Z22)
  cls_rgw_fsio_id cs;
  uint64_t size = 0;
  ceph::real_time mtime;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(caller, bl);
    encode(seq, bl);
    encode(cs, bl);
    encode(size, bl);
    encode(mtime, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(caller, bl);
    decode(seq, bl);
    decode(cs, bl);
    decode(size, bl);
    decode(mtime, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_truncate_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_truncate_op)

struct cls_rgw_fsio_truncate_ret {
  uint64_t trunc_epoch = 0;      // the new epoch
  bool already_applied = false;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(trunc_epoch, bl);
    encode(already_applied, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(trunc_epoch, bl);
    decode(already_applied, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_truncate_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_truncate_ret)

/* fsio_rebind */
struct cls_rgw_fsio_rebind_op {
  cls_rgw_fsio_handle caller;
  uint64_t seq = 0;
  cls_rgw_fsio_id from;
  cls_rgw_fsio_id to;
  uint32_t readers = 0;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(caller, bl);
    encode(seq, bl);
    encode(from, bl);
    encode(to, bl);
    encode(readers, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(caller, bl);
    decode(seq, bl);
    decode(from, bl);
    decode(to, bl);
    decode(readers, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_rebind_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_rebind_op)

/* a reply of flags only:  rebind (already applied), the instance head's
 * open-count calls (drained, removed) */
struct cls_rgw_fsio_flags_ret {
  bool already_applied = false;
  bool drained = false;
  bool removed = false;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(already_applied, bl);
    encode(drained, bl);
    encode(removed, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(already_applied, bl);
    decode(drained, bl);
    decode(removed, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_flags_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_flags_ret)

/* fsio_forget's reply:  a chunk set the removal let drain (§3.4, §3.6) */
struct cls_rgw_fsio_forget_ret {
  std::optional<cls_rgw_fsio_id> drained;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(drained, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(drained, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_forget_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_forget_ret)

/* fsio_ocnt_add's reply:  on a version that is gone, nothing added, and
 * whether the count holds the caller's handle (§3, §4.3) */
struct cls_rgw_fsio_ocnt_add_ret {
  bool gone = false;
  bool held = false;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(gone, bl);
    encode(held, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(gone, bl);
    decode(held, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_ocnt_add_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_ocnt_add_ret)

/* fsio_rebind's reply */
struct cls_rgw_fsio_rebind_ret {
  bool already_applied = false;
  std::optional<cls_rgw_fsio_id> drained;   // a retired chunk set it emptied

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(already_applied, bl);
    encode(drained, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(already_applied, bl);
    decode(drained, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_rebind_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_rebind_ret)

/* fsio_publish_release, fsio_forget:  a handle and a chunk set */
struct cls_rgw_fsio_cs_op {
  cls_rgw_fsio_handle caller;
  cls_rgw_fsio_id cs;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(caller, bl);
    encode(cs, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(caller, bl);
    decode(cs, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_cs_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_cs_op)

/* fsio_fenced_finalize */
struct cls_rgw_fsio_finalize_op {
  cls_rgw_fsio_handle finalizer;
  cls_rgw_fsio_id from;
  std::set<uint64_t> expired;
  std::optional<cls_rgw_fsio_intent> intent;
  std::string aborted;           // the claim the caller aborted (§3.7, V1)

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(finalizer, bl);
    encode(from, bl);
    encode(expired, bl);
    encode(intent, bl);
    encode(aborted, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(finalizer, bl);
    decode(from, bl);
    decode(expired, bl);
    decode(intent, bl);
    decode(aborted, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_finalize_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_finalize_op)

struct cls_rgw_fsio_finalize_ret {
  cls_rgw_fsio_id to;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(to, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(to, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_finalize_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_finalize_ret)

/* fsio_expire */
struct cls_rgw_fsio_expire_op {
  cls_rgw_fsio_handle caller;
  std::set<uint64_t> expired;
  std::optional<cls_rgw_fsio_id> clear;   // an abandoned construction (§3.8)

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(caller, bl);
    encode(expired, bl);
    encode(clear, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(caller, bl);
    decode(expired, bl);
    decode(clear, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_expire_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_expire_op)

/* §3.8, Y20:  two flags, never the chunk sets, which the expirer reads
 * from the header */
struct cls_rgw_fsio_expire_ret {
  bool drained = false;          // a chunk set drained:  send and forget
  bool finalize = false;         // a shadow needs a fenced finalize

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(drained, bl);
    encode(finalize, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(drained, bl);
    decode(finalize, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_expire_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_expire_ret)

/* fsio_drop_seqs */
struct cls_rgw_fsio_drop_seqs_op {
  uint64_t incarnation = 0;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(incarnation, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(incarnation, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_drop_seqs_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_drop_seqs_op)

/* the instance head's open count:  fsio_ocnt_add, fsio_ocnt_remove,
 * fsio_ocnt_expire, fsio_ocnt_forget (§2.5) */
struct cls_rgw_fsio_ocnt_op {
  cls_rgw_fsio_handle holder;    // add, remove
  std::set<uint64_t> expired;    // expire

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(holder, bl);
    encode(expired, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(holder, bl);
    decode(expired, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_ocnt_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_ocnt_op)

/* fsio_remove_version */
struct cls_rgw_fsio_remove_version_op {
  cls_rgw_fsio_id r;
  std::string marker;            // to name the leaf clone (§2.7)

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(r, bl);
    encode(marker, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(r, bl);
    decode(marker, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_remove_version_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_remove_version_op)

struct cls_rgw_fsio_remove_version_ret {
  bool keep_tail = false;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(keep_tail, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(keep_tail, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_remove_version_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_remove_version_ret)

/* fsio_replace_head:  replaces cls_rgw_remove_obj wherever RGW replaces
 * a head in an FSIO-enabled bucket (design §3), in RGW's own compound op */
struct cls_rgw_fsio_replace_op {
  uint8_t mode = 0;              // cls_rgw_fsio::replace_mode
  cls_rgw_fsio_id r;             // the leaf clone's id, drawn by the caller
  std::string expected_id_tag;   // delete:  the ID tag the caller read (Z7)
  cls_rgw_fsio_handle caller;    // publish:  the publisher
  cls_rgw_fsio_id shadow;        // publish
  bool versioned = false;        // publish:  the standalone call on the OLH
  bool complete = false;         // versioned publish:  for a dead publisher
  std::set<uint64_t> expired;    // with complete
  std::list<std::string> keep_attr_prefixes;  // as cls_rgw_remove_obj's
  std::string marker;            // the bucket's, to name the leaf clone (§2.7)
  bool keep_tail = false;        // supersede:  the new object reuses the tail (Y2)
  ceph::buffer::list tags;       // publish onto a tracking-only head:  the
                                 // tags the content had (§3.5, Z15)
  /* unversioned publish:  the tags the publisher evaluated policy with;
   * ECANCELED if the publish would keep others (§3.5, C1) */
  std::optional<ceph::buffer::list> expected_tags;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(mode, bl);
    encode(r, bl);
    encode(expected_id_tag, bl);
    encode(caller, bl);
    encode(shadow, bl);
    encode(versioned, bl);
    encode(complete, bl);
    encode(expired, bl);
    encode(keep_attr_prefixes, bl);
    encode(marker, bl);
    encode(keep_tail, bl);
    encode(tags, bl);
    encode(expected_tags, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(mode, bl);
    decode(r, bl);
    decode(expected_id_tag, bl);
    decode(caller, bl);
    decode(shadow, bl);
    decode(versioned, bl);
    decode(complete, bl);
    decode(expired, bl);
    decode(keep_attr_prefixes, bl);
    decode(marker, bl);
    decode(keep_tail, bl);
    decode(tags, bl);
    decode(expected_tags, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_replace_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_replace_op)

struct cls_rgw_fsio_replace_ret {
  bool keep_tail = false;        // else the caller collects the replaced chain
  bool born_drained = false;     // publish:  the replaced content named only by the hint
  std::optional<cls_rgw_fsio_id> drained;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(keep_tail, bl);
    encode(born_drained, bl);
    encode(drained, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(keep_tail, bl);
    decode(born_drained, bl);
    decode(drained, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_replace_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_replace_ret)

/* fsio_open_hint_add, fsio_open_hint_remove:  the hint and its target's
 * oid;  the class builds the omap key from both,
 * {incarnation:016x}_{handle:016x}_{kind}_{target oid}[_{r}] (§2.7) */
struct cls_rgw_fsio_hint_op {
  cls_rgw_fsio_hint hint;
  std::string oid;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(hint, bl);
    encode(oid, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(hint, bl);
    decode(oid, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_hint_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_hint_op)

/* fsio_open_hint_list:  a shard's hints after `marker`, at most `max`,
 * those naming `bucket` or held by `incarnation` when given */
struct cls_rgw_fsio_hint_list_op {
  std::string marker;
  uint32_t max = 0;
  std::optional<rgw_bucket> bucket;
  std::optional<uint64_t> incarnation;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(marker, bl);
    encode(max, bl);
    encode(bucket, bl);
    encode(incarnation, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(marker, bl);
    decode(max, bl);
    decode(bucket, bl);
    decode(incarnation, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_hint_list_op> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_hint_list_op)

/* the next call starts after `marker`, the last key the class read:  with
 * a filter, a call may read past many hints and return few or none */
struct cls_rgw_fsio_hint_list_ret {
  std::vector<cls_rgw_fsio_hint> hints;
  std::string marker;
  bool truncated = false;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(hints, bl);
    encode(marker, bl);
    encode(truncated, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(hints, bl);
    decode(marker, bl);
    decode(truncated, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_hint_list_ret> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_hint_list_ret)

namespace cls::rgw::method {
constexpr auto fsio_join = ClsMethod<RdWrTag, ClassId>("fsio_join");
constexpr auto fsio_ready = ClsMethod<RdWrTag, ClassId>("fsio_ready");
constexpr auto fsio_extend = ClsMethod<RdWrTag, ClassId>("fsio_extend");
constexpr auto fsio_hint = ClsMethod<RdWrTag, ClassId>("fsio_hint");
constexpr auto fsio_truncate = ClsMethod<RdWrTag, ClassId>("fsio_truncate");
constexpr auto fsio_leave = ClsMethod<RdWrTag, ClassId>("fsio_leave");
constexpr auto fsio_rebind = ClsMethod<RdWrTag, ClassId>("fsio_rebind");
constexpr auto fsio_publish_release =
  ClsMethod<RdWrTag, ClassId>("fsio_publish_release");
constexpr auto fsio_fenced_finalize =
  ClsMethod<RdWrTag, ClassId>("fsio_fenced_finalize");
constexpr auto fsio_expire = ClsMethod<RdWrTag, ClassId>("fsio_expire");
constexpr auto fsio_forget = ClsMethod<RdWrTag, ClassId>("fsio_forget");
constexpr auto fsio_drop_seqs = ClsMethod<RdWrTag, ClassId>("fsio_drop_seqs");
constexpr auto fsio_writer_identity =
  ClsMethod<RdWrTag, ClassId>("fsio_writer_identity");
constexpr auto fsio_ocnt_add = ClsMethod<RdWrTag, ClassId>("fsio_ocnt_add");
constexpr auto fsio_ocnt_remove = ClsMethod<RdWrTag, ClassId>("fsio_ocnt_remove");
constexpr auto fsio_ocnt_expire = ClsMethod<RdWrTag, ClassId>("fsio_ocnt_expire");
constexpr auto fsio_ocnt_forget = ClsMethod<RdWrTag, ClassId>("fsio_ocnt_forget");
constexpr auto fsio_remove_version =
  ClsMethod<RdWrTag, ClassId>("fsio_remove_version");
constexpr auto fsio_replace_head =
  ClsMethod<RdWrTag, ClassId>("fsio_replace_head");
constexpr auto fsio_open_hint_add =
  ClsMethod<RdWrTag, ClassId>("fsio_open_hint_add");
constexpr auto fsio_open_hint_remove =
  ClsMethod<RdWrTag, ClassId>("fsio_open_hint_remove");
constexpr auto fsio_open_hint_list =
  ClsMethod<RdTag, ClassId>("fsio_open_hint_list");
}
