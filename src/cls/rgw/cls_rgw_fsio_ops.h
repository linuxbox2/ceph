// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* The fsio_* CLS methods' inputs and replies (FSIO_RADOS_DESIGN.md §2.7,
 * §8).  Each method decodes its input, loads the head's FSIO records,
 * runs the transition in cls_rgw_fsio.cc, stores what changed, applies
 * the effects and encodes its reply.  Every reply has a fixed maximum
 * within osd_max_write_op_reply_len, which FSIO requires at 128 bytes
 * (§2.7 rule 4, W18). */

#pragma once

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
}
