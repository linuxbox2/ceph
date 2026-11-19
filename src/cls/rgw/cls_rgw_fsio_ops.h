// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* The fsio_* CLS methods' inputs and replies (FSIO_RADOS_DESIGN.md §2.7,
 * §8).  Each method decodes its input, loads the head's FSIO records,
 * runs the transition in cls_rgw_fsio.cc, stores what changed, applies
 * the effects and encodes its reply.  Every reply has a fixed maximum
 * within osd_max_write_op_reply_len, which FSIO requires at 128 bytes
 * (§2.7 rule 4, W18). */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "include/encoding.h"
#include "include/rados/cls_traits.hpp"
#include "cls/rgw/cls_rgw_ops.h"
#include "cls/rgw/cls_rgw_fsio.h"

/* the claim a versioned publish or finalize carries (§4.2) */
struct cls_rgw_fsio_intent {
  std::string instance;
  std::string token;
  uint64_t now_epoch = 0;

  cls_rgw_fsio::publish_intent to_core() const {
    return {instance, token, now_epoch};
  }
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
    DECODE_FINISH(bl);
  }
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_join_op)

struct cls_rgw_fsio_join_ret {
  cls_rgw_fsio_id bound;
  bool construct = false;
  bool already_applied = false;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(bound, bl);
    encode(construct, bl);
    encode(already_applied, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(bound, bl);
    decode(construct, bl);
    decode(already_applied, bl);
    DECODE_FINISH(bl);
  }
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_join_ret)

/* fsio_ready, fsio_extend */
struct cls_rgw_fsio_runs_op {
  cls_rgw_fsio_handle caller;
  cls_rgw_fsio_id cs;
  std::vector<cls_rgw_fsio_run> runs;   // ready:  the runs cloned;  extend:  one

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(caller, bl);
    encode(cs, bl);
    encode(runs, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(caller, bl);
    decode(cs, bl);
    decode(runs, bl);
    DECODE_FINISH(bl);
  }
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
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_leave_ret)

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
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_flags_ret)

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

  cls_rgw_fsio::replace_in to_core() const {
    cls_rgw_fsio::replace_in in;
    in.mode = static_cast<cls_rgw_fsio::replace_mode>(mode);
    in.r = r;
    in.expected_id_tag = expected_id_tag;
    in.caller = caller;
    in.shadow = shadow;
    in.versioned = versioned;
    in.complete = complete;
    in.expired = expired;
    return in;
  }
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
    DECODE_FINISH(bl);
  }
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
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_replace_ret)

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
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_hint)

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
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_hint_list_ret)

namespace cls::rgw::method {
constexpr auto fsio_join = ClsMethod<RdWrTag, ClassId>("fsio_join");
constexpr auto fsio_ready = ClsMethod<RdWrTag, ClassId>("fsio_ready");
constexpr auto fsio_extend = ClsMethod<RdWrTag, ClassId>("fsio_extend");
constexpr auto fsio_leave = ClsMethod<RdWrTag, ClassId>("fsio_leave");
constexpr auto fsio_rebind = ClsMethod<RdWrTag, ClassId>("fsio_rebind");
constexpr auto fsio_publish_release =
  ClsMethod<RdWrTag, ClassId>("fsio_publish_release");
constexpr auto fsio_fenced_finalize =
  ClsMethod<RdWrTag, ClassId>("fsio_fenced_finalize");
constexpr auto fsio_expire = ClsMethod<RdWrTag, ClassId>("fsio_expire");
constexpr auto fsio_forget = ClsMethod<RdWrTag, ClassId>("fsio_forget");
constexpr auto fsio_drop_seqs = ClsMethod<RdWrTag, ClassId>("fsio_drop_seqs");
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
