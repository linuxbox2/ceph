// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* FSIO's records, ops and replies:  dump() and generate_test_instances(),
 * for ceph-dencoder (FSIO_RADOS_DESIGN.md §2.7) */

#include "common/ceph_json.h"
#include "cls/rgw/cls_rgw_types.h"
#include "cls/rgw/cls_rgw_ops.h"

void cls_rgw_fsio_id::dump(ceph::Formatter *f) const
{
  encode_json("counter", counter, f);
  encode_json("nonce", nonce, f);
}

std::list<cls_rgw_fsio_id> cls_rgw_fsio_id::generate_test_instances()
{
  std::list<cls_rgw_fsio_id> o;
  cls_rgw_fsio_id i;
  i.counter = 1;
  i.nonce = 2;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_run::dump(ceph::Formatter *f) const
{
  encode_json("first", first, f);
  encode_json("last", last, f);
}

std::list<cls_rgw_fsio_run> cls_rgw_fsio_run::generate_test_instances()
{
  std::list<cls_rgw_fsio_run> o;
  cls_rgw_fsio_run i;
  i.first = 1;
  i.last = 2;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_shadow::dump(ceph::Formatter *f) const
{
  encode_json("size_hint", size_hint, f);
  encode_json("mtime_hint", mtime_hint, f);
  encode_json("trunc_epoch", trunc_epoch, f);
  encode_json("trunc_low", trunc_low, f);
  encode_json("extent", extent, f);
}

std::list<cls_rgw_fsio_shadow> cls_rgw_fsio_shadow::generate_test_instances()
{
  std::list<cls_rgw_fsio_shadow> o;
  cls_rgw_fsio_shadow i;
  i.size_hint = 1;
  i.mtime_hint = ceph::real_time(std::chrono::seconds(2));
  i.trunc_epoch = 3;
  i.trunc_low = 4;
  i.extent.push_back(cls_rgw_fsio_run{5, 7});
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_handle::dump(ceph::Formatter *f) const
{
  encode_json("incarnation", incarnation, f);
  encode_json("handle", handle, f);
}

std::list<cls_rgw_fsio_handle> cls_rgw_fsio_handle::generate_test_instances()
{
  std::list<cls_rgw_fsio_handle> o;
  cls_rgw_fsio_handle i;
  i.incarnation = 1;
  i.handle = 2;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_publishing::dump(ceph::Formatter *f) const
{
  encode_json("publisher", publisher, f);
  encode_json("instance", instance, f);
  encode_json("token", token, f);
  encode_json("olh_epoch", olh_epoch, f);
  encode_json("identity", identity, f);
}

std::list<cls_rgw_fsio_publishing> cls_rgw_fsio_publishing::generate_test_instances()
{
  std::list<cls_rgw_fsio_publishing> o;
  cls_rgw_fsio_publishing i;
  i.publisher = cls_rgw_fsio_handle{1, 1};
  i.instance = "v2";
  i.token = "v3";
  i.olh_epoch = 4;
  i.identity = "v5";
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_chunk_set::dump(ceph::Formatter *f) const
{
  encode_json("id", id, f);
  encode_json("state", static_cast<int>(state), f);
  encode_json("manifest_at", static_cast<int>(manifest_at), f);
  encode_json("clone", clone, f);
  encode_json("instance", instance, f);
  encode_json("shadow", shadow, f);
  encode_json("publishing", publishing, f);
  encode_json("drained", drained, f);
  encode_json("shares_tail", shares_tail, f);
  f->open_array_section("after");
  for (const auto& a : after) {
    encode_json("id", a, f);
  }
  f->close_section();
}

std::list<cls_rgw_fsio_chunk_set> cls_rgw_fsio_chunk_set::generate_test_instances()
{
  std::list<cls_rgw_fsio_chunk_set> o;
  cls_rgw_fsio_chunk_set i;
  i.id = cls_rgw_fsio_id{1, 7};
  i.clone = cls_rgw_fsio_id{4, 7};
  i.instance = "v5";
  i.shadow = cls_rgw_fsio_shadow::generate_test_instances().front();
  i.publishing = cls_rgw_fsio_publishing::generate_test_instances().front();
  i.drained = true;
  i.shares_tail = true;
  i.after = {cls_rgw_fsio_id{2, 7}, cls_rgw_fsio_id{3, 7}};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_fenced_finalize::dump(ceph::Formatter *f) const
{
  encode_json("from", from, f);
  encode_json("to", to, f);
}

std::list<cls_rgw_fsio_fenced_finalize> cls_rgw_fsio_fenced_finalize::generate_test_instances()
{
  std::list<cls_rgw_fsio_fenced_finalize> o;
  cls_rgw_fsio_fenced_finalize i;
  i.from = cls_rgw_fsio_id{1, 7};
  i.to = cls_rgw_fsio_id{2, 7};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_hdr::dump(ceph::Formatter *f) const
{
  encode_json("nonce", nonce, f);
  encode_json("next_counter", next_counter, f);
  encode_json("cur_shadow", cur_shadow, f);
  encode_json("cur_instance", cur_instance, f);
  encode_json("chunk_sets", chunk_sets, f);
  encode_json("fenced_finalizes", fenced_finalizes, f);
  encode_json("rename_hold", rename_hold, f);
}

std::list<cls_rgw_fsio_hdr> cls_rgw_fsio_hdr::generate_test_instances()
{
  std::list<cls_rgw_fsio_hdr> o;
  cls_rgw_fsio_hdr i;
  i.nonce = 1;
  i.next_counter = 2;
  i.cur_shadow = cls_rgw_fsio_id{3, 7};
  i.cur_instance = "v4";
  i.chunk_sets.push_back(cls_rgw_fsio_chunk_set::generate_test_instances().front());
  i.fenced_finalizes.push_back(cls_rgw_fsio_fenced_finalize::generate_test_instances().front());
  i.rename_hold = cls_rgw_fsio_handle{7, 1};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_counts::dump(ceph::Formatter *f) const
{
  encode_json("readers", readers, f);
  encode_json("writers", writers, f);
  encode_json("writer_identity", writer_identity, f);
  encode_json("joined_seq", joined_seq, f);
}

std::list<cls_rgw_fsio_counts> cls_rgw_fsio_counts::generate_test_instances()
{
  std::list<cls_rgw_fsio_counts> o;
  cls_rgw_fsio_counts i;
  i.readers = 1;
  i.writers = 2;
  i.writer_identity = "v3";
  i.joined_seq = 4;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_handle_opens::dump(ceph::Formatter *f) const
{
  encode_json("last_seq", last_seq, f);
  encode_json("chunk_sets", chunk_sets, f);
}

std::list<cls_rgw_fsio_handle_opens> cls_rgw_fsio_handle_opens::generate_test_instances()
{
  std::list<cls_rgw_fsio_handle_opens> o;
  cls_rgw_fsio_handle_opens i;
  i.last_seq = 1;
  i.chunk_sets[cls_rgw_fsio_id{2, 7}] = cls_rgw_fsio_counts::generate_test_instances().front();
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_opens::dump(ceph::Formatter *f) const
{
  encode_json("handles", handles, f);
  encode_json("seqs", seqs, f);
}

std::list<cls_rgw_fsio_opens> cls_rgw_fsio_opens::generate_test_instances()
{
  std::list<cls_rgw_fsio_opens> o;
  cls_rgw_fsio_opens i;
  i.handles[1] = cls_rgw_fsio_handle_opens::generate_test_instances().front();
  i.seqs[2] = 3;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_gen::dump(ceph::Formatter *f) const
{
  encode_json("manifest", manifest, f);
  encode_json("gc_tag", gc_tag, f);
}

std::list<cls_rgw_fsio_gen> cls_rgw_fsio_gen::generate_test_instances()
{
  std::list<cls_rgw_fsio_gen> o;
  cls_rgw_fsio_gen i;
  i.manifest.append("C1");
  i.gc_tag = "v2";
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_replaced::dump(ceph::Formatter *f) const
{
  encode_json("manifest", manifest, f);
  encode_json("gc_tag", gc_tag, f);
  encode_json("clone", clone, f);
}

std::list<cls_rgw_fsio_replaced> cls_rgw_fsio_replaced::generate_test_instances()
{
  std::list<cls_rgw_fsio_replaced> o;
  cls_rgw_fsio_replaced i;
  i.manifest.append("C1");
  i.gc_tag = "v2";
  i.clone = cls_rgw_fsio_id{3, 7};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_ocnt::dump(ceph::Formatter *f) const
{
  encode_json("holders", holders, f);
  encode_json("deferred", deferred, f);
  encode_json("removed", removed, f);
}

std::list<cls_rgw_fsio_ocnt> cls_rgw_fsio_ocnt::generate_test_instances()
{
  std::list<cls_rgw_fsio_ocnt> o;
  cls_rgw_fsio_ocnt i;
  i.holders.insert(cls_rgw_fsio_handle{1, 1});
  i.deferred.push_back(cls_rgw_fsio_replaced::generate_test_instances().front());
  i.removed = true;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_hint::dump(ceph::Formatter *f) const
{
  encode_json("kind", static_cast<int>(kind), f);
  encode_json("target", static_cast<int>(target), f);
  /* their fields, not rgw_bucket::dump() and rgw_obj_key::dump(), which
   * are RGW's and not in the class's library */
  f->open_object_section("bucket");
  encode_json("tenant", bucket.tenant, f);
  encode_json("name", bucket.name, f);
  encode_json("marker", bucket.marker, f);
  encode_json("bucket_id", bucket.bucket_id, f);
  f->close_section();
  f->open_object_section("key");
  encode_json("name", key.name, f);
  encode_json("instance", key.instance, f);
  encode_json("ns", key.ns, f);
  f->close_section();
  encode_json("incarnation", incarnation, f);
  encode_json("handle", handle, f);
  encode_json("r", r, f);
}

std::list<cls_rgw_fsio_hint> cls_rgw_fsio_hint::generate_test_instances()
{
  std::list<cls_rgw_fsio_hint> o;
  cls_rgw_fsio_hint i;
  i.bucket = rgw_bucket(rgw_bucket_key("", "b3", "m3"));
  i.key = rgw_obj_key("k4");
  i.incarnation = 5;
  i.handle = 6;
  i.r = cls_rgw_fsio_id{7, 7};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_intent::dump(ceph::Formatter *f) const
{
  encode_json("instance", instance, f);
  encode_json("token", token, f);
  encode_json("now_epoch", now_epoch, f);
}

std::list<cls_rgw_fsio_intent> cls_rgw_fsio_intent::generate_test_instances()
{
  std::list<cls_rgw_fsio_intent> o;
  cls_rgw_fsio_intent i;
  i.instance = "v1";
  i.token = "v2";
  i.now_epoch = 3;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_join_op::dump(ceph::Formatter *f) const
{
  encode_json("caller", caller, f);
  encode_json("seq", seq, f);
  encode_json("writer", writer, f);
  encode_json("nonce", nonce, f);
  encode_json("counter_seed", counter_seed, f);
  encode_json("expired", expired, f);
  encode_json("instance", instance, f);
  encode_json("marker", marker, f);
  encode_json("identity", identity, f);
}

std::list<cls_rgw_fsio_join_op> cls_rgw_fsio_join_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_join_op> o;
  cls_rgw_fsio_join_op i;
  i.caller = cls_rgw_fsio_handle{1, 1};
  i.seq = 2;
  i.writer = true;
  i.nonce = 4;
  i.counter_seed = 5;
  i.expired.insert(6);
  i.instance = "v7";
  i.marker = "v8";
  i.identity = "v9";
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_identity_op::dump(ceph::Formatter *f) const
{
  encode_json("caller", caller, f);
  encode_json("cs", cs, f);
  encode_json("identity", identity, f);
}

std::list<cls_rgw_fsio_identity_op> cls_rgw_fsio_identity_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_identity_op> o;
  cls_rgw_fsio_identity_op i;
  i.caller = cls_rgw_fsio_handle{1, 1};
  i.cs = cls_rgw_fsio_id{2, 7};
  i.identity = "v3";
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_join_ret::dump(ceph::Formatter *f) const
{
  encode_json("bound", bound, f);
  encode_json("construct", construct, f);
  encode_json("already_applied", already_applied, f);
  encode_json("state", static_cast<int>(state), f);
  encode_json("others", others, f);
  encode_json("trunc_epoch", trunc_epoch, f);
  encode_json("size_hint", size_hint, f);
  encode_json("mtime_hint", mtime_hint, f);
  encode_json("last_run", last_run, f);
}

std::list<cls_rgw_fsio_join_ret> cls_rgw_fsio_join_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_join_ret> o;
  cls_rgw_fsio_join_ret i;
  i.bound = cls_rgw_fsio_id{1, 7};
  i.construct = true;
  i.already_applied = true;
  i.state = 2;
  i.others = true;
  i.trunc_epoch = 4;
  i.size_hint = 5;
  i.mtime_hint = ceph::real_time(std::chrono::seconds(6));
  i.last_run = cls_rgw_fsio_run{7, 9};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_runs_op::dump(ceph::Formatter *f) const
{
  encode_json("caller", caller, f);
  encode_json("cs", cs, f);
  encode_json("runs", runs, f);
  encode_json("size", size, f);
  encode_json("mtime", mtime, f);
}

std::list<cls_rgw_fsio_runs_op> cls_rgw_fsio_runs_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_runs_op> o;
  cls_rgw_fsio_runs_op i;
  i.caller = cls_rgw_fsio_handle{1, 1};
  i.cs = cls_rgw_fsio_id{2, 7};
  i.runs.push_back(cls_rgw_fsio_run{3, 5});
  i.size = 4;
  i.mtime = ceph::real_time(std::chrono::seconds(5));
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_leave_op::dump(ceph::Formatter *f) const
{
  encode_json("caller", caller, f);
  encode_json("seq", seq, f);
  encode_json("cs", cs, f);
  encode_json("writer", writer, f);
  encode_json("expired", expired, f);
  encode_json("intent", intent, f);
}

std::list<cls_rgw_fsio_leave_op> cls_rgw_fsio_leave_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_leave_op> o;
  cls_rgw_fsio_leave_op i;
  i.caller = cls_rgw_fsio_handle{1, 1};
  i.seq = 2;
  i.cs = cls_rgw_fsio_id{3, 7};
  i.writer = true;
  i.expired.insert(5);
  i.intent = cls_rgw_fsio_intent::generate_test_instances().front();
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_leave_ret::dump(ceph::Formatter *f) const
{
  encode_json("already_applied", already_applied, f);
  encode_json("publishing", publishing, f);
  encode_json("abandoned", abandoned, f);
  encode_json("olh_epoch", olh_epoch, f);
  encode_json("instance", instance, f);
  encode_json("drained", drained, f);
}

std::list<cls_rgw_fsio_leave_ret> cls_rgw_fsio_leave_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_leave_ret> o;
  cls_rgw_fsio_leave_ret i;
  i.already_applied = true;
  i.publishing = true;
  i.abandoned = true;
  i.olh_epoch = 4;
  i.instance = "v5";
  i.drained = cls_rgw_fsio_id{6, 7};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_hint_size_op::dump(ceph::Formatter *f) const
{
  encode_json("caller", caller, f);
  encode_json("cs", cs, f);
  encode_json("trunc_epoch", trunc_epoch, f);
  encode_json("size", size, f);
  encode_json("mtime", mtime, f);
}

std::list<cls_rgw_fsio_hint_size_op> cls_rgw_fsio_hint_size_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_hint_size_op> o;
  cls_rgw_fsio_hint_size_op i;
  i.caller = cls_rgw_fsio_handle{1, 1};
  i.cs = cls_rgw_fsio_id{2, 7};
  i.trunc_epoch = 3;
  i.size = 4;
  i.mtime = ceph::real_time(std::chrono::seconds(5));
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_hint_size_ret::dump(ceph::Formatter *f) const
{
  encode_json("stale", stale, f);
  encode_json("others", others, f);
  encode_json("trunc_epoch", trunc_epoch, f);
  encode_json("size", size, f);
}

std::list<cls_rgw_fsio_hint_size_ret> cls_rgw_fsio_hint_size_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_hint_size_ret> o;
  cls_rgw_fsio_hint_size_ret i;
  i.stale = true;
  i.others = true;
  i.trunc_epoch = 3;
  i.size = 4;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_truncate_op::dump(ceph::Formatter *f) const
{
  encode_json("caller", caller, f);
  encode_json("seq", seq, f);
  encode_json("cs", cs, f);
  encode_json("size", size, f);
  encode_json("mtime", mtime, f);
}

std::list<cls_rgw_fsio_truncate_op> cls_rgw_fsio_truncate_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_truncate_op> o;
  cls_rgw_fsio_truncate_op i;
  i.caller = cls_rgw_fsio_handle{1, 1};
  i.seq = 2;
  i.cs = cls_rgw_fsio_id{3, 7};
  i.size = 4;
  i.mtime = ceph::real_time(std::chrono::seconds(5));
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_truncate_ret::dump(ceph::Formatter *f) const
{
  encode_json("trunc_epoch", trunc_epoch, f);
  encode_json("already_applied", already_applied, f);
}

std::list<cls_rgw_fsio_truncate_ret> cls_rgw_fsio_truncate_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_truncate_ret> o;
  cls_rgw_fsio_truncate_ret i;
  i.trunc_epoch = 1;
  i.already_applied = true;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_rebind_op::dump(ceph::Formatter *f) const
{
  encode_json("caller", caller, f);
  encode_json("seq", seq, f);
  encode_json("from", from, f);
  encode_json("to", to, f);
  encode_json("readers", readers, f);
}

std::list<cls_rgw_fsio_rebind_op> cls_rgw_fsio_rebind_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_rebind_op> o;
  cls_rgw_fsio_rebind_op i;
  i.caller = cls_rgw_fsio_handle{1, 1};
  i.seq = 2;
  i.from = cls_rgw_fsio_id{3, 7};
  i.to = cls_rgw_fsio_id{4, 7};
  i.readers = 5;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_forget_ret::dump(ceph::Formatter *f) const
{
  encode_json("drained", drained, f);
}

std::list<cls_rgw_fsio_forget_ret> cls_rgw_fsio_forget_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_forget_ret> o;
  cls_rgw_fsio_forget_ret i;
  i.drained = cls_rgw_fsio_id{9, 7};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_ocnt_add_ret::dump(ceph::Formatter *f) const
{
  encode_json("gone", gone, f);
  encode_json("held", held, f);
}

std::list<cls_rgw_fsio_ocnt_add_ret> cls_rgw_fsio_ocnt_add_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_ocnt_add_ret> o;
  cls_rgw_fsio_ocnt_add_ret i;
  i.gone = true;
  i.held = true;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_flags_ret::dump(ceph::Formatter *f) const
{
  encode_json("already_applied", already_applied, f);
  encode_json("drained", drained, f);
  encode_json("removed", removed, f);
}

std::list<cls_rgw_fsio_flags_ret> cls_rgw_fsio_flags_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_flags_ret> o;
  cls_rgw_fsio_flags_ret i;
  i.already_applied = true;
  i.drained = true;
  i.removed = true;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_rebind_ret::dump(ceph::Formatter *f) const
{
  encode_json("already_applied", already_applied, f);
  encode_json("drained", drained, f);
}

std::list<cls_rgw_fsio_rebind_ret> cls_rgw_fsio_rebind_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_rebind_ret> o;
  cls_rgw_fsio_rebind_ret i;
  i.already_applied = true;
  i.drained = cls_rgw_fsio_id{2, 7};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_cs_op::dump(ceph::Formatter *f) const
{
  encode_json("caller", caller, f);
  encode_json("cs", cs, f);
}

std::list<cls_rgw_fsio_cs_op> cls_rgw_fsio_cs_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_cs_op> o;
  cls_rgw_fsio_cs_op i;
  i.caller = cls_rgw_fsio_handle{1, 1};
  i.cs = cls_rgw_fsio_id{2, 7};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_finalize_op::dump(ceph::Formatter *f) const
{
  encode_json("finalizer", finalizer, f);
  encode_json("from", from, f);
  encode_json("expired", expired, f);
  encode_json("intent", intent, f);
  encode_json("aborted", aborted, f);
}

std::list<cls_rgw_fsio_finalize_op> cls_rgw_fsio_finalize_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_finalize_op> o;
  cls_rgw_fsio_finalize_op i;
  i.finalizer = cls_rgw_fsio_handle{1, 1};
  i.from = cls_rgw_fsio_id{2, 7};
  i.expired.insert(3);
  i.intent = cls_rgw_fsio_intent::generate_test_instances().front();
  i.aborted = "v5";
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_finalize_ret::dump(ceph::Formatter *f) const
{
  encode_json("to", to, f);
}

std::list<cls_rgw_fsio_finalize_ret> cls_rgw_fsio_finalize_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_finalize_ret> o;
  cls_rgw_fsio_finalize_ret i;
  i.to = cls_rgw_fsio_id{1, 7};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_expire_op::dump(ceph::Formatter *f) const
{
  encode_json("caller", caller, f);
  encode_json("expired", expired, f);
  encode_json("clear", clear, f);
}

std::list<cls_rgw_fsio_expire_op> cls_rgw_fsio_expire_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_expire_op> o;
  cls_rgw_fsio_expire_op i;
  i.caller = cls_rgw_fsio_handle{1, 1};
  i.expired.insert(2);
  i.clear = cls_rgw_fsio_id{3, 7};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_expire_ret::dump(ceph::Formatter *f) const
{
  encode_json("drained", drained, f);
  encode_json("finalize", finalize, f);
}

std::list<cls_rgw_fsio_expire_ret> cls_rgw_fsio_expire_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_expire_ret> o;
  cls_rgw_fsio_expire_ret i;
  i.drained = true;
  i.finalize = true;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_drop_seqs_op::dump(ceph::Formatter *f) const
{
  encode_json("incarnation", incarnation, f);
}

std::list<cls_rgw_fsio_drop_seqs_op> cls_rgw_fsio_drop_seqs_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_drop_seqs_op> o;
  cls_rgw_fsio_drop_seqs_op i;
  i.incarnation = 1;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_ocnt_op::dump(ceph::Formatter *f) const
{
  encode_json("holder", holder, f);
  encode_json("expired", expired, f);
}

std::list<cls_rgw_fsio_ocnt_op> cls_rgw_fsio_ocnt_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_ocnt_op> o;
  cls_rgw_fsio_ocnt_op i;
  i.holder = cls_rgw_fsio_handle{1, 1};
  i.expired.insert(2);
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_remove_version_op::dump(ceph::Formatter *f) const
{
  encode_json("r", r, f);
  encode_json("marker", marker, f);
}

std::list<cls_rgw_fsio_remove_version_op> cls_rgw_fsio_remove_version_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_remove_version_op> o;
  cls_rgw_fsio_remove_version_op i;
  i.r = cls_rgw_fsio_id{1, 7};
  i.marker = "v2";
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_remove_version_ret::dump(ceph::Formatter *f) const
{
  encode_json("keep_tail", keep_tail, f);
}

std::list<cls_rgw_fsio_remove_version_ret> cls_rgw_fsio_remove_version_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_remove_version_ret> o;
  cls_rgw_fsio_remove_version_ret i;
  i.keep_tail = true;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_replace_op::dump(ceph::Formatter *f) const
{
  encode_json("mode", static_cast<int>(mode), f);
  encode_json("r", r, f);
  encode_json("expected_id_tag", expected_id_tag, f);
  encode_json("caller", caller, f);
  encode_json("shadow", shadow, f);
  encode_json("versioned", versioned, f);
  encode_json("complete", complete, f);
  encode_json("expired", expired, f);
  encode_json("keep_attr_prefixes", keep_attr_prefixes, f);
  encode_json("marker", marker, f);
  encode_json("keep_tail", keep_tail, f);
  encode_json("tags", tags, f);
}

std::list<cls_rgw_fsio_replace_op> cls_rgw_fsio_replace_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_replace_op> o;
  cls_rgw_fsio_replace_op i;
  i.mode = 1;
  i.r = cls_rgw_fsio_id{2, 7};
  i.expected_id_tag = "v3";
  i.caller = cls_rgw_fsio_handle{4, 1};
  i.shadow = cls_rgw_fsio_id{5, 7};
  i.versioned = true;
  i.complete = true;
  i.expired.insert(8);
  i.keep_attr_prefixes.push_back("v9");
  i.marker = "v10";
  i.keep_tail = true;
  i.tags.append("C12");
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_replace_ret::dump(ceph::Formatter *f) const
{
  encode_json("keep_tail", keep_tail, f);
  encode_json("born_drained", born_drained, f);
  encode_json("drained", drained, f);
}

std::list<cls_rgw_fsio_replace_ret> cls_rgw_fsio_replace_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_replace_ret> o;
  cls_rgw_fsio_replace_ret i;
  i.keep_tail = true;
  i.born_drained = true;
  i.drained = cls_rgw_fsio_id{3, 7};
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_hint_op::dump(ceph::Formatter *f) const
{
  encode_json("hint", hint, f);
  encode_json("oid", oid, f);
}

std::list<cls_rgw_fsio_hint_op> cls_rgw_fsio_hint_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_hint_op> o;
  cls_rgw_fsio_hint_op i;
  i.hint = cls_rgw_fsio_hint::generate_test_instances().front();
  i.oid = "v2";
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_hint_list_op::dump(ceph::Formatter *f) const
{
  encode_json("marker", marker, f);
  encode_json("max", max, f);
  if (bucket) {                        // its fields, as the hint's
    f->open_object_section("bucket");
    encode_json("tenant", bucket->tenant, f);
    encode_json("name", bucket->name, f);
    encode_json("marker", bucket->marker, f);
    encode_json("bucket_id", bucket->bucket_id, f);
    f->close_section();
  }
  encode_json("incarnation", incarnation, f);
}

std::list<cls_rgw_fsio_hint_list_op> cls_rgw_fsio_hint_list_op::generate_test_instances()
{
  std::list<cls_rgw_fsio_hint_list_op> o;
  cls_rgw_fsio_hint_list_op i;
  i.marker = "v1";
  i.max = 2;
  i.bucket = rgw_bucket(rgw_bucket_key("", "b3", "m3"));
  i.incarnation = 4;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}

void cls_rgw_fsio_hint_list_ret::dump(ceph::Formatter *f) const
{
  encode_json("hints", hints, f);
  encode_json("marker", marker, f);
  encode_json("truncated", truncated, f);
}

std::list<cls_rgw_fsio_hint_list_ret> cls_rgw_fsio_hint_list_ret::generate_test_instances()
{
  std::list<cls_rgw_fsio_hint_list_ret> o;
  cls_rgw_fsio_hint_list_ret i;
  i.hints.push_back(cls_rgw_fsio_hint::generate_test_instances().front());
  i.marker = "v2";
  i.truncated = true;
  o.push_back(std::move(i));
  o.emplace_back();
  return o;
}
