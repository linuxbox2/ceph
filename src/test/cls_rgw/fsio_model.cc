// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* A model checker for FSIO on RADOS's head protocol.  It explores every
 * interleaving of gateways (open, write, follow, close, publish, crash),
 * S3 PUT and DELETE, the FSIO expirer and GC over one head, applying the
 * shared transitions in cls/rgw/cls_rgw_fsio.cc, and checks:
 *   - no open reads storage that was collected;
 *   - the head's published content is never collected;
 *   - chunk-set ids are unique, and at most one entry is PUBLISHED at HEAD;
 *   - no shadow is published twice;
 *   - a shadow's manifest can be found when it is needed;
 *   - at the end:  nothing leaks, no FSIO state is left, and every
 *     acknowledged write was published;  no FSIO open hint is left.
 * Storage is abstract:  a published object's tail is one object "C<n>", a
 * shadow's chunks one object "S<counter>", a record clone "R<counter>".
 * Each gateway hints what it holds state on, and the FSIO expirer
 * reaches a head or an instance head only through a dead incarnation's
 * hint, by §4.3's rule for its kind and target.  A gateway either
 * crashes (a true death, presumed at once) or is partitioned (presumed
 * dead, still running:  a zombie, which stops once its grace has
 * elapsed);  the FSIO expirer decides a fenced finalize from one read
 * and calls it in a later step.
 *
 * The environment bounds a run:  FSIO_MODEL_GATEWAYS, or FSIO_MODEL_ROLES
 * with one letter per gateway (f full, w writer only, r reader only),
 * "+" joining the next letter to the previous gateway as its second
 * handle on the head ("w+r":  one process, a writer FSIOObject and a
 * reader FSIOObject;  FSIO state is keyed by handle, design §2.7, Z4);
 * FSIO_MODEL_S3_OPS;  and switches, each 1 by default:
 * FSIO_MODEL_CRASHES, FSIO_MODEL_ZOMBIES, FSIO_MODEL_EXPIRER_CRASHES.
 * FSIO_MODEL_SKIP_CHECK names one check of the core to skip
 * (cls_rgw_fsio_check), to show the model finds its absence.
 * FSIO_MODEL_MAX_DEPTH (200) bounds a path:  one that reaches it is a
 * cycle drawing new ids, which no visited state ends, and is reported.
 * FSIO_MODEL_STOP_AT_FIRST=1 ends the run at its first violation, as a
 * skipped-check run needs.
 * FSIO_MODEL_VERSIONED=1 runs a versioned bucket (inflight/
 * fsio-model-versioned.md):  the head is the OLH, versions have
 * instance heads with open counts, a publish makes a new version.  The
 * publish follows §4.2 with its claim (placeholder, guarded instance
 * write, link, fsio_publish);  the expirer resolves a dead publisher by
 * the claim (abort and a versioned fenced finalize, or complete), and
 * applies §4.3's publish-hint rule to instance-head hints as written.
 * Versioning may be suspended and enabled again (an S3 operation each);
 * a suspended bucket writes the null version in place, on the head
 * (§2.5a).  FSIO_MODEL_VSTART=null starts with versioning just enabled
 * over content FSIO has open, the head holding the null version.
 * FSIO_MODEL_SHORTEST=1 searches by iterative deepening:  depth-first to
 * a bound raised one step at a time, so the first violation found has the
 * shortest trace;  for STOP_AT_FIRST and skipped-check runs.
 * FSIO_MODEL_LIMIT (2,000,000) caps the states a run visits;  a run
 * that reaches it is a partial sample.  FSIO_MODEL_PROGRESS (100,000)
 * prints a progress line every that many states.  FSIO_MODEL_PREFIX
 * names a vector, one action name per line as traces print them ('#',
 * blank and "=>" lines skipped):  the search takes those actions in
 * order, then goes on from where they lead, or stops there with
 * FSIO_MODEL_PREFIX_ONLY=1.  fsio_vectors/ holds the vectors.
 * FSIO_MODEL_CLUSTER=1, with FSIO_MODEL_WALKS, also sends every core
 * call to a RADOS cluster (CEPH_CONF) as its CLS method, on a head in
 * FSIO_MODEL_POOL (fsio-model, created if absent), and compares the
 * object with the core after each (fsio_model_cluster.h);  not with the
 * search, which a cluster cannot follow, nor with
 * FSIO_MODEL_SKIP_CHECK, which changes only the core in this process. */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gtest/gtest.h>

#include "cls/rgw/cls_rgw_fsio.h"
#include "fsio_model_cluster.h"

namespace {

namespace fsio = cls_rgw_fsio;
using fid_t = cls_rgw_fsio_id;

/* FSIO state is keyed by handle (design §2.7, Z4).  Each gateway here
 * holds one FSIOObject per head, handle 1;  the FSIO expirer is handle 0 */
cls_rgw_fsio_handle xh(uint64_t inc) { return {inc, 0}; }

/* an incarnation's counts by chunk set, summed over its handles */
std::map<fid_t, cls_rgw_fsio_counts>
inc_chunk_sets(const cls_rgw_fsio_opens& o)
{
  std::map<fid_t, cls_rgw_fsio_counts> m;
  for (const auto& [hd, ho] : o.handles) {
    for (const auto& [cs, c] : ho.chunk_sets) {
      m[cs].readers += c.readers;
      m[cs].writers += c.writers;
    }
  }
  return m;
}

/* the open count as incarnations:  whether one holds it, and which do */
bool ocnt_holds(const cls_rgw_fsio_ocnt& n, uint64_t inc)
{
  return std::any_of(n.holders.begin(), n.holders.end(),
		     [inc](const cls_rgw_fsio_handle& c) {
		       return c.incarnation == inc;
		     });
}
std::set<uint64_t> ocnt_incs(const cls_rgw_fsio_ocnt& n)
{
  std::set<uint64_t> s;
  for (const auto& c : n.holders) {
    s.insert(c.incarnation);
  }
  return s;
}
using state = cls_rgw_fsio_cs_state;
using at = cls_rgw_fsio_manifest_at;

constexpr uint64_t EXPIRER = 100;

std::string sname(const fid_t& id)
{
  return fmt::format("S{}.{}", id.nonce, id.counter);
}
std::string rname(const fid_t& r) { return fmt::format("R{}", r.counter); }

ceph::buffer::list tok_bl(const std::string& t)
{
  ceph::buffer::list bl;
  encode(t, bl);
  return bl;
}

std::string tok(const ceph::buffer::list& bl)
{
  if (bl.length() == 0) {
    return {};
  }
  std::string t;
  auto p = bl.cbegin();
  decode(t, p);
  return t;
}

struct LOpen {
  bool writer = false;
  fid_t cs;
  std::string storage;
  std::string instance;          // versioned:  the version it bound through
  void encode(ceph::buffer::list& bl) const {
    using ceph::encode;
    encode(writer, bl); encode(cs, bl); encode(storage, bl);
    encode(instance, bl);
  }
};

/* a hint (§2.7):  cls_rgw_fsio_hint, its target by name rather than by
 * bucket and key;  one shard, since each hint is handled on its own */
enum class HintKind : uint8_t { PRESENCE = 1, PUBLISH = 2 };
enum class HintTarget : uint8_t { HEAD = 1, INSTANCE_HEAD = 2 };
struct Hint {
  HintKind kind;
  HintTarget target;
  std::string instance;          // INSTANCE_HEAD:  "I2";  empty for HEAD
  uint64_t incarnation = 0;
  uint64_t handle = 0;           // which of its handles (design §2.7, Z4)
  std::optional<fid_t> r;        // PUBLISH on a HEAD:  the leaf clone
  auto operator<=>(const Hint&) const = default;
  void encode(ceph::buffer::list& bl) const {
    using ceph::encode;
    encode((uint8_t)kind, bl); encode((uint8_t)target, bl);
    encode(instance, bl); encode(incarnation, bl); encode(handle, bl);
    encode(r, bl);
  }
};
Hint presence_head(const cls_rgw_fsio_handle& c)
{
  return Hint{HintKind::PRESENCE, HintTarget::HEAD, {}, c.incarnation, c.handle};
}
Hint presence_ih(const std::string& inst, const cls_rgw_fsio_handle& c)
{
  return Hint{HintKind::PRESENCE, HintTarget::INSTANCE_HEAD, inst,
	      c.incarnation, c.handle};
}
Hint publish_head(const cls_rgw_fsio_handle& c, const fid_t& r)
{
  return Hint{HintKind::PUBLISH, HintTarget::HEAD, {}, c.incarnation,
	      c.handle, r};
}
Hint publish_ih(const std::string& inst, const cls_rgw_fsio_handle& c)
{
  return Hint{HintKind::PUBLISH, HintTarget::INSTANCE_HEAD, inst,
	      c.incarnation, c.handle};
}

enum Step : uint8_t { NONE, CONSTRUCT, PUBLISH_WRITE, PUBLISH_GC, DRAIN_SEND,
  PUBLISH_HINT, CONSTRUCT_LEAVE,
  /* versioned (stage A) */
  V_COUNT, V_JOIN, V_OCNT_SEND, V_PUB_READ, V_PUB_WRITE, V_PUB_LINK,
  V_PUB_PUBLISH,
  /* versioned (stage B):  the claim */
  V_PUB_HINT, V_PLACE, V_LEAVE, V_UNPLACE, V_PUB_UNHINT, V_PUB_CLEANUP,
  /* a publisher that cannot proceed (§4.2, W10) */
  V_REL_RELEASE };

/* a handle's latest counted call, as sent (§3, Z22):  resent with the
 * same sequence number, it models a reply lost past the PG log's dup
 * window and the op run again */
enum class Counted : uint8_t { JOIN, LEAVE, REBIND };
struct Resend {
  Counted kind = Counted::JOIN;
  uint64_t seq = 0;
  bool writer = false;           // fsio_join, fsio_leave
  uint64_t nonce = 0;            // join
  std::set<uint64_t> expired;    // fsio_join, fsio_leave
  std::string instance;          // join:  versioned
  fid_t cs, to;                  // leave:  cs;  rebind:  cs to `to`
  std::optional<fsio::publish_intent> intent;   // leave
  void encode(ceph::buffer::list& bl) const {
    using ceph::encode;
    encode((uint8_t)kind, bl); encode(writer, bl);   // seq:  the handle's
    encode(nonce, bl); encode(expired, bl); encode(instance, bl);
    encode(cs, bl); encode(to, bl);
    encode(intent.has_value(), bl);
    if (intent) {
      encode(intent->instance, bl); encode(intent->token, bl);
      encode(intent->now_epoch, bl);
    }
  }
};

struct Gw {
  uint64_t inc = 0;
  uint64_t handle = 1;           // its FSIOObject on the head:  a second shares inc (Z4)
  char role = 'f';               // f full, w writer only, r reader only
  bool alive = true;
  std::vector<LOpen> opens;
  uint8_t did_read = 0, did_write = 0, did_io = 0, did_follow = 0,
    did_crash = 0;
  uint8_t zombie = 0;            // presumed dead, still running
  Step step = NONE;
  fid_t step_cs;
  fid_t step_r;
  bool step_born = false;
  std::optional<fid_t> step_drained;
  uint8_t owe_head_hint = 0;     // PRESENCE_HINT_BEFORE_JOIN skipped:  to add
  uint8_t release_failed = 0;    // a release call failed:  keeps its hint (H1)
  std::string owe_ih_hint;       // PRESENCE_HINT_BEFORE_OCNT skipped:  to add
  /* versioned */
  std::string pending;           // the instance an open in progress read
  bool pending_writer = false;
  std::set<std::string> counted; // instances whose open count holds it (§2.5)
  std::string step_inst;         // V_OCNT_SEND:  that instance
  std::string pub_instance;      // the publish's new version
  std::string pub_token;         // its claim (§4.2 step 1)
  std::set<cls_rgw_fsio_handle> pub_incs;   // the handles it read (§4.2 step 3)
  uint64_t pub_epoch = 0;
  bool pub_began_suspended = false;  // the bucket's state when the publish began
  uint8_t released = 0;          // released once:  a bound, else release and
				 // retry would cycle, each drawing a new instance
  uint8_t pub_retry = 0;         // released:  publishes again after the cleanup
  uint64_t seq = 0;              // its counted calls' sequence (Z22)
  std::optional<Resend> resend;  // its latest counted call
  uint8_t resent = 0;            // resends made
  void encode(ceph::buffer::list& bl) const {
    using ceph::encode;
    encode(inc, bl); encode(handle, bl); encode(alive, bl);
    encode((uint32_t)opens.size(), bl);
    for (auto& o : opens) o.encode(bl);
    encode(did_read, bl); encode(did_write, bl); encode(did_io, bl);
    encode(did_follow, bl); encode(did_crash, bl); encode(zombie, bl);
    encode((uint8_t)step, bl); encode(step_cs, bl); encode(step_r, bl);
    encode(step_born, bl); encode(step_drained, bl);
    encode(owe_head_hint, bl); encode(owe_ih_hint, bl);
    encode(release_failed, bl);
    encode(pending, bl); encode(pending_writer, bl); encode(counted, bl);
    encode(step_inst, bl); encode(pub_instance, bl); encode(pub_incs, bl);
    encode(pub_epoch, bl); encode(pub_token, bl);
    encode(released, bl); encode(pub_retry, bl);
    encode(pub_began_suspended, bl);
    encode(resent, bl);            // seq:  equal to its records' last_seq
    encode(resend.has_value(), bl);
    if (resend) {
      resend->encode(bl);
    }
  }
};

/* a gateway handle's identity, and its name in traces:  "1" for an
 * incarnation's first handle, "1.2" for its second (Z4) */
cls_rgw_fsio_handle hd(const Gw& g) { return {g.inc, g.handle}; }
std::string glabel(const Gw& g)
{
  return g.handle == 1 ? fmt::format("{}", g.inc) :
    fmt::format("{}.{}", g.inc, g.handle);
}


/* what a run explores (the header comment) */
struct Config {
  int crashes = 1;
  int zombies = 1;
  int expirer_crashes = 1;
  int versioned = 0;
  bool vstart_null = false;      // FSIO_MODEL_VSTART=null
  int resends = 1;               // FSIO_MODEL_RESENDS:  per handle (Z22)
};

/* versioned:  one instance head and what it holds */
struct Version;
bool decided_by_claim(const Version& v);

struct Version {
  cls_rgw_fsio_instance_head ih;
  std::string tail;              // its content:  "C<n>", or a shadow "S<id>"
  uint64_t epoch = 0;            // linked at
  std::optional<std::string> claim;  // user.rgw.fsio.claim:  a token, "aborted"
  bool placeholder() const { return ih.exists && ih.id_tag.empty() && claim; }
  void encode(ceph::buffer::list& bl) const {
    using ceph::encode;
    encode(ih, bl); encode(tail, bl); encode(epoch, bl); encode(claim, bl);
  }
};

/* §4.2:  an instance head with an ID tag is written, whatever its claim;
 * only one without is decided by its claim (Z6) */
bool decided_by_claim(const Version& v)
{
  return v.ih.exists && v.claim &&
    (v.ih.id_tag.empty() || FSIO_SKIP_CHECK(Z6_RESOLUTION_TESTS_ID_TAG));
}
Config cfg;

/* the core calls, each also sent to the cluster as its CLS method when
 * the cluster tier runs (fsio_model_cluster.h);  the in-memory core is
 * the oracle */
fsio_cluster::Cluster cl;

namespace fc = fsio_cluster;

fc::Obj head_after(const cls_rgw_fsio_head& h, const cls_rgw_fsio_effects& fx)
{
  return fx.remove_head ? fc::Obj{} : fc::head_obj(h);
}
fc::Obj instance_after(const cls_rgw_fsio_instance_head& ih,
		       const cls_rgw_fsio_effects& fx)
{
  return fx.remove_head ? fc::Obj{} : fc::instance_obj(ih);
}
template <typename Op>
ceph::buffer::list enc(const Op& op)
{
  ceph::buffer::list bl;
  encode(op, bl);
  return bl;
}
template <typename Ret>
std::optional<Ret> dec(const char* method, const ceph::buffer::list& bl)
{
  try {
    Ret ret;
    auto it = bl.cbegin();
    decode(ret, it);
    return ret;
  } catch (const ceph::buffer::error&) {
    cl.reply_mismatch(method, "undecodable");
    return std::nullopt;
  }
}
std::optional<cls_rgw_fsio_intent> op_intent(const fsio::publish_intent* i)
{
  if (!i) {
    return std::nullopt;
  }
  return cls_rgw_fsio_intent{i->instance, i->token, i->now_epoch};
}

/* a head method with no reply to compare */
template <typename Core, typename Op, typename Method>
int on_cluster_head(const char* method, const Method& m,
		    cls_rgw_fsio_head& h, cls_rgw_fsio_effects& fx,
		    const Op& op, Core&& core,
		    ceph::buffer::list* reply_out = nullptr)
{
  if (!cl.on) {
    return core();
  }
  const auto pre = fc::head_obj(h);
  const int r = core();
  ceph::buffer::list reply;
  const auto oid = cl.head_oid();
  const int rc = cl.call(oid, pre, fx, m, enc(op), reply);
  cl.check(method, oid, r, rc, pre, head_after(h, fx), fx);
  /* a reply only when the CLS call succeeded:  on an absent head it
   * returns ENOENT where the core, with no such check, may succeed */
  if (reply_out) {
    *reply_out = rc == 0 ? std::move(reply) : ceph::buffer::list{};
  }
  return r;
}
template <typename Core, typename Op, typename Method>
int on_cluster_instance(const char* method, const Method& m,
			const std::string& inst,
			cls_rgw_fsio_instance_head& ih,
			cls_rgw_fsio_effects& fx, const Op& op, Core&& core,
			ceph::buffer::list* reply_out = nullptr)
{
  if (!cl.on) {
    return core();
  }
  const auto pre = fc::instance_obj(ih);
  const int r = core();
  ceph::buffer::list reply;
  const auto oid = cl.instance_oid(inst);
  const int rc = cl.call(oid, pre, fx, m, enc(op), reply);
  cl.check(method, oid, r, rc, pre, instance_after(ih, fx), fx);
  /* a reply only when the CLS call succeeded:  on an absent head it
   * returns ENOENT where the core, with no such check, may succeed */
  if (reply_out) {
    *reply_out = rc == 0 ? std::move(reply) : ceph::buffer::list{};
  }
  return r;
}

int m_join(cls_rgw_fsio_head& h, const fsio::join_in& in, fsio::join_out& out,
	   cls_rgw_fsio_effects& fx)
{
  cls_rgw_fsio_join_op op;
  op.caller = in.caller;
  op.seq = in.seq;
  op.writer = in.writer;
  op.nonce = in.nonce;
  op.counter_seed = in.counter_seed;
  op.expired = in.expired;
  op.instance = in.instance;
  op.marker = cl.marker;
  ceph::buffer::list reply;
  const int r = on_cluster_head("fsio_join", cls::rgw::method::fsio_join, h, fx, op,
				[&] { return fsio::join(h, in, out, fx); }, &reply);
  if (cl.on && r == 0 && reply.length()) {
    if (auto ret = dec<cls_rgw_fsio_join_ret>("fsio_join", reply);
	ret && (ret->bound != out.bound || ret->construct != out.construct ||
		ret->already_applied != out.already_applied)) {
      cl.reply_mismatch("fsio_join", fmt::format(
	"bound {} construct {:d} already_applied {:d};  the core {} {:d} {:d}",
	ret->bound.to_str(), ret->construct, ret->already_applied,
	out.bound.to_str(), out.construct, out.already_applied));
    }
  }
  return r;
}

int m_leave(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	    uint64_t seq, const fid_t& cs, bool writer,
	    const fsio::expired_t& expired, fsio::leave_out& out,
	    cls_rgw_fsio_effects& fx, const fsio::publish_intent* intent)
{
  cls_rgw_fsio_leave_op op;
  op.caller = caller;
  op.seq = seq;
  op.cs = cs;
  op.writer = writer;
  op.expired = expired;
  op.intent = op_intent(intent);
  ceph::buffer::list reply;
  const int r = on_cluster_head("fsio_leave", cls::rgw::method::fsio_leave, h, fx, op, [&] {
      return fsio::leave(h, caller, seq, cs, writer, expired, out, fx, intent);
    }, &reply);
  if (cl.on && r == 0 && reply.length()) {
    if (auto ret = dec<cls_rgw_fsio_leave_ret>("fsio_leave", reply);
	ret && (ret->already_applied != out.already_applied ||
		ret->publishing != out.publishing ||
		ret->abandoned != out.abandoned ||
		ret->olh_epoch != out.olh_epoch ||
		ret->instance != out.instance || ret->drained != out.drained)) {
      cl.reply_mismatch("fsio_leave", "differs from the core's leave_out");
    }
  }
  return r;
}

int m_rebind(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	     uint64_t seq, const fid_t& from, const fid_t& to,
	     uint32_t readers)
{
  cls_rgw_fsio_rebind_op op;
  op.caller = caller;
  op.seq = seq;
  op.from = from;
  op.to = to;
  op.readers = readers;
  cls_rgw_fsio_effects fx;
  fsio::rebind_out out;
  ceph::buffer::list reply;
  const int r = on_cluster_head("fsio_rebind", cls::rgw::method::fsio_rebind, h, fx, op, [&] {
      return fsio::rebind(h, caller, seq, from, to, readers, &out);
    }, &reply);
  if (cl.on && r == 0 && reply.length()) {
    if (auto ret = dec<cls_rgw_fsio_flags_ret>("fsio_rebind", reply);
	ret && ret->already_applied != out.already_applied) {
      cl.reply_mismatch("fsio_rebind", "already_applied differs");
    }
  }
  return r;
}

int m_ready(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	    const fid_t& cs, const std::vector<cls_rgw_fsio_run>& runs)
{
  cls_rgw_fsio_runs_op op;
  op.caller = caller;
  op.cs = cs;
  op.runs = runs;
  cls_rgw_fsio_effects fx;
  return on_cluster_head("fsio_ready", cls::rgw::method::fsio_ready, h, fx, op,
			 [&] { return fsio::ready(h, caller, cs, runs); });
}

int m_publish_release(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
		      const fid_t& cs)
{
  cls_rgw_fsio_cs_op op;
  op.caller = caller;
  op.cs = cs;
  cls_rgw_fsio_effects fx;
  return on_cluster_head("fsio_publish_release", cls::rgw::method::fsio_publish_release, h, fx, op,
			 [&] { return fsio::publish_release(h, caller, cs); });
}

int m_forget(cls_rgw_fsio_head& h, const fid_t& cs, cls_rgw_fsio_effects& fx)
{
  cls_rgw_fsio_cs_op op;
  op.cs = cs;
  return on_cluster_head("fsio_forget", cls::rgw::method::fsio_forget, h, fx, op,
			 [&] { return fsio::forget(h, cs, fx); });
}

int m_drop_seqs(cls_rgw_fsio_head& h, uint64_t incarnation,
		cls_rgw_fsio_effects& fx)
{
  cls_rgw_fsio_drop_seqs_op op;
  op.incarnation = incarnation;
  return on_cluster_head("fsio_drop_seqs", cls::rgw::method::fsio_drop_seqs, h, fx, op,
			 [&] { return fsio::drop_seqs(h, incarnation, fx); });
}

int m_fenced_finalize(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& finalizer,
		      const fid_t& from, const fsio::expired_t& expired,
		      fid_t& to, const fsio::publish_intent* intent = nullptr,
		      const std::string& aborted = {})
{
  cls_rgw_fsio_finalize_op op;
  op.finalizer = finalizer;
  op.from = from;
  op.expired = expired;
  op.intent = op_intent(intent);
  op.aborted = aborted;
  cls_rgw_fsio_effects fx;
  ceph::buffer::list reply;
  const int r = on_cluster_head("fsio_fenced_finalize", cls::rgw::method::fsio_fenced_finalize, h, fx, op, [&] {
      return fsio::fenced_finalize(h, finalizer, from, expired, to, intent,
				   aborted);
    }, &reply);
  if (cl.on && r == 0 && reply.length()) {
    if (auto ret = dec<cls_rgw_fsio_finalize_ret>("fsio_fenced_finalize", reply);
	ret && ret->to != to) {
      cl.reply_mismatch("fsio_fenced_finalize", fmt::format(
	"to {};  the core {}", ret->to.to_str(), to.to_str()));
    }
  }
  return r;
}

int m_expire(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	     const fsio::expired_t& expired,
	     const std::optional<fid_t>& clear, fsio::expire_out& out,
	     cls_rgw_fsio_effects& fx)
{
  cls_rgw_fsio_expire_op op;
  op.caller = caller;
  op.expired = expired;
  op.clear = clear;
  ceph::buffer::list reply;
  const int r = on_cluster_head("fsio_expire", cls::rgw::method::fsio_expire, h, fx, op, [&] {
      return fsio::expire(h, caller, expired, clear, out, fx);
    }, &reply);
  if (cl.on && r == 0 && reply.length()) {
    if (auto ret = dec<cls_rgw_fsio_expire_ret>("fsio_expire", reply);
	ret && ret->drained != !out.drained.empty()) {
      cl.reply_mismatch("fsio_expire", "drained differs");
    }
  }
  return r;
}

int m_replace_head(cls_rgw_fsio_head& h, const fsio::replace_in& in,
		   fsio::replace_out& out, cls_rgw_fsio_effects& fx)
{
  if (!cl.on) {
    return fsio::replace_head(h, in, out, fx);
  }
  const auto pre = fc::head_obj(h);
  const int r = fsio::replace_head(h, in, out, fx);
  cls_rgw_fsio_replace_op op;
  op.mode = static_cast<uint8_t>(in.mode);
  op.r = in.r;
  op.expected_id_tag = in.expected_id_tag;
  op.caller = in.caller;
  op.shadow = in.shadow;
  op.versioned = in.versioned;
  op.complete = in.complete;
  op.expired = in.expired;
  op.keep_attr_prefixes = {std::string(fc::OLH_PREFIX)};
  op.marker = cl.marker;

  /* §3:  a versioned publish stores the records alone;  every other mode
   * recreates the head with the kept attributes and the new records, or
   * removes it if nothing is kept */
  fc::Obj want;
  if (in.mode == fsio::replace_mode::PUBLISH && in.versioned) {
    want = head_after(h, fx);
  } else {
    for (const auto& [k, v] : pre.attrs) {
      if (k.starts_with(fc::OLH_PREFIX)) {
	want.attrs[k] = v;
      }
    }
    if (!fx.remove_head) {
      want.attrs.merge(fc::records_of(h));
    }
    want.exists = !want.attrs.empty();
  }
  ceph::buffer::list reply;
  const auto oid = cl.head_oid();
  const int rc = cl.call(oid, pre, fx, cls::rgw::method::fsio_replace_head, enc(op), reply);
  cl.check("fsio_replace_head", oid, r, rc, pre, want, fx);
  if (rc == 0) {
    static const char* modes[] = {"supersede", "delete", "publish",
				  "remove_null"};
    const auto m = static_cast<size_t>(in.mode);
    cl.count(fmt::format("fsio_replace_head({}{})",
			 m < std::size(modes) ? modes[m] : "?",
			 in.mode == fsio::replace_mode::PUBLISH && in.versioned ?
			 ", versioned" : ""));
  }
  if (r == 0 && rc == 0) {
    if (auto ret = dec<cls_rgw_fsio_replace_ret>("fsio_replace_head", reply);
	ret && (ret->keep_tail != out.keep_tail ||
		ret->born_drained != out.born_drained ||
		ret->drained != out.drained)) {
      cl.reply_mismatch("fsio_replace_head", "differs from the core's replace_out");
    }
  }
  return r;
}

int m_ocnt_add(const std::string& inst, cls_rgw_fsio_instance_head& ih,
	       const cls_rgw_fsio_handle& holder)
{
  cls_rgw_fsio_ocnt_op op;
  op.holder = holder;
  cls_rgw_fsio_effects fx;
  return on_cluster_instance("fsio_ocnt_add", cls::rgw::method::fsio_ocnt_add, inst, ih, fx, op,
			     [&] { return fsio::ocnt_add(ih, holder); });
}

int m_ocnt_remove(const std::string& inst, cls_rgw_fsio_instance_head& ih,
		  const cls_rgw_fsio_handle& holder, fsio::ocnt_drain& d)
{
  cls_rgw_fsio_ocnt_op op;
  op.holder = holder;
  cls_rgw_fsio_effects fx;
  ceph::buffer::list reply;
  const int r = on_cluster_instance("fsio_ocnt_remove", cls::rgw::method::fsio_ocnt_remove, inst, ih, fx, op,
				    [&] { return fsio::ocnt_remove(ih, holder, d); },
				    &reply);
  if (cl.on && r == 0 && reply.length()) {
    if (auto ret = dec<cls_rgw_fsio_flags_ret>("fsio_ocnt_remove", reply);
	ret && (ret->drained != d.drained || ret->removed != d.removed)) {
      cl.reply_mismatch("fsio_ocnt_remove", "drained or removed differs");
    }
  }
  return r;
}

int m_ocnt_expire(const std::string& inst, cls_rgw_fsio_instance_head& ih,
		  const fsio::expired_t& expired, fsio::ocnt_drain& d)
{
  cls_rgw_fsio_ocnt_op op;
  op.expired = expired;
  cls_rgw_fsio_effects fx;
  ceph::buffer::list reply;
  const int r = on_cluster_instance("fsio_ocnt_expire", cls::rgw::method::fsio_ocnt_expire, inst, ih, fx, op,
				    [&] { return fsio::ocnt_expire(ih, expired, d); },
				    &reply);
  if (cl.on && r == 0 && reply.length()) {
    if (auto ret = dec<cls_rgw_fsio_flags_ret>("fsio_ocnt_expire", reply);
	ret && (ret->drained != d.drained || ret->removed != d.removed)) {
      cl.reply_mismatch("fsio_ocnt_expire", "drained or removed differs");
    }
  }
  return r;
}

int m_ocnt_forget(const std::string& inst, cls_rgw_fsio_instance_head& ih,
		  cls_rgw_fsio_effects& fx)
{
  cls_rgw_fsio_ocnt_op op;
  return on_cluster_instance("fsio_ocnt_forget", cls::rgw::method::fsio_ocnt_forget, inst, ih, fx, op,
			     [&] { return fsio::ocnt_forget(ih, fx); });
}

int m_remove_version(const std::string& inst, cls_rgw_fsio_instance_head& ih,
		     const fid_t& rr, fsio::remove_out& out,
		     cls_rgw_fsio_effects& fx)
{
  if (!cl.on) {
    return fsio::remove_version(ih, rr, out, fx);
  }
  const auto pre = fc::instance_obj(ih);
  const int r = fsio::remove_version(ih, rr, out, fx);
  cls_rgw_fsio_remove_version_op op;
  op.r = rr;
  op.marker = cl.marker;
  /* §2.5:  the instance head removed, and recreated with its open count
   * alone while the count keeps it */
  fc::Obj want;
  if (ih.exists && ih.ocnt) {
    want.exists = true;
    encode(*ih.ocnt, want.attrs[std::string(fc::OCNT)]);
  }
  ceph::buffer::list reply;
  const auto oid = cl.instance_oid(inst);
  const int rc = cl.call(oid, pre, fx, cls::rgw::method::fsio_remove_version, enc(op), reply);
  cl.check("fsio_remove_version", oid, r, rc, pre, want, fx);
  if (r == 0 && rc == 0) {
    if (auto ret = dec<cls_rgw_fsio_remove_version_ret>("fsio_remove_version",
							 reply);
	ret && ret->keep_tail != out.keep_tail) {
      cl.reply_mismatch("fsio_remove_version", "keep_tail differs");
    }
  }
  return r;
}


/* the counted calls, numbered per handle and kept for a resend (Z22).
 * A call takes the next number when it applies:  a failed call changes
 * nothing, so its number can be used again.  So a handle record's
 * last_seq always equals the handle's counter, and the state key leaves
 * both out (World::key) */
int counted_join(cls_rgw_fsio_head& h, Gw& g, fsio::join_in& in,
		 fsio::join_out& out, cls_rgw_fsio_effects& fx)
{
  in.seq = g.seq + 1;
  int r = m_join(h, in, out, fx);
  g.resend.reset();
  if (r == 0) {
    g.seq = in.seq;
  }
  if (r == 0 && g.resent < cfg.resends) {
    g.resend = Resend{Counted::JOIN, in.seq, in.writer, in.nonce, in.expired,
		      in.instance};
  }
  return r;
}
int counted_leave(cls_rgw_fsio_head& h, Gw& g, const fid_t& cs, bool writer,
		  const std::set<uint64_t>& expired, fsio::leave_out& out,
		  cls_rgw_fsio_effects& fx,
		  const fsio::publish_intent* intent = nullptr)
{
  const auto seq = g.seq + 1;
  int r = m_leave(h, hd(g), seq, cs, writer, expired, out, fx, intent);
  g.resend.reset();
  if (r == 0) {
    g.seq = seq;
  }
  if (r == 0 && g.resent < cfg.resends) {
    Resend rs{Counted::LEAVE, seq, writer, 0, expired};
    rs.cs = cs;
    if (intent) {
      rs.intent = *intent;
    }
    g.resend = rs;
  }
  return r;
}
int counted_rebind(cls_rgw_fsio_head& h, Gw& g, const fid_t& from,
		   const fid_t& to, uint32_t readers)
{
  const auto seq = g.seq + 1;
  int r = m_rebind(h, hd(g), seq, from, to, readers);
  g.resend.reset();
  if (r == 0) {
    g.seq = seq;
  }
  if (r == 0 && g.resent < cfg.resends) {
    Resend rs{Counted::REBIND, seq};
    rs.cs = from;
    rs.to = to;
    g.resend = rs;
  }
  return r;
}

enum ExPhase : uint8_t { EX_IDLE, EX_FF_PUBLISH, EX_FF_RECLONE, EX_FF_DECIDED,
  /* versioned:  the fenced finalize as §4.2's sequence for `to` */
  VF_DECIDED, VF_CALL, VF_RECLONE, VF_WRITE, VF_LINK, VF_PUBLISH, VF_PLACE };

struct World {
  cls_rgw_fsio_head head;
  std::set<std::string> storage;
  std::map<std::string, std::string> records;    // R<r> -> manifest token
  std::map<std::string, std::string> pms;        // S<id> -> pseudo-manifest token
  std::set<std::string> fenced;                  // shadows whose chunks carry the fence
  std::vector<std::vector<std::string>> gcq;
  std::vector<Gw> gws;
  int s3_budget = 0;
  uint8_t del_phase = 0;
  std::string del_tag, del_tok;
  ExPhase ex = EX_IDLE;
  fid_t ex_to, ex_from;
  uint64_t ex_inc = EXPIRER;     // the expirer's incarnation;  each crash a new one
  int ex_crashes = 1;            // expirer crashes allowed
  std::map<std::string, int> acked;              // shadow -> writes acked
  std::map<std::string, std::string> base;       // shadow -> token cloned from
  std::map<std::string, std::string> recloned;   // to -> from
  std::map<std::string, int> published;          // shadow -> times published
  uint64_t next_c = 1, next_r = 1000, next_tag = 1, next_nonce = 1;
  /* versioned */
  std::map<std::string, Version> versions;       // "I<n>"
  std::string olh_cur;                           // "" a delete marker
  std::string ex_inst, ex_token;                 // a versioned finalize's `to`
  std::string ex_aborted;                        // the claim it aborted (V1)
  std::set<Hint> hints;                          // the FSIO open hints (§2.7)
  std::set<uint64_t> graced;                     // incarnations past their grace (§4.3)
  bool suspended = false;                        // versioning suspended (§2.5a)
  uint64_t olh_epoch = 0, next_i = 2, next_epoch = 1;

  /* a gateway passes a fresh nonce with every fsio_join, used only if
   * the call creates the header (§2.2, §3.1):  the next one is available, and taken
   * when it was used */
  uint64_t nonce_for_join() const { return next_nonce; }
  void took_nonce() {
    if (head.hdr && head.hdr->nonce == next_nonce) {
      ++next_nonce;
    }
  }

  std::string key() const {
    using ceph::encode;
    ceph::buffer::list bl;
    /* a handle record's last_seq equals its handle's counter (Z22,
     * counted_join), so neither is part of the state */
    if (head.opens.empty()) {
      encode(head, bl);
    } else {
      auto h = head;
      for (auto& [i, o] : h.opens) {
	for (auto& [k, ho] : o.handles) {
	  ho.last_seq = 0;
	}
	/* a kept seq is the handle's counter, or behind it */
	for (auto& [k, v] : o.seqs) {
	  auto g = std::find_if(gws.begin(), gws.end(), [&](const Gw& x) {
	    return x.inc == i && x.handle == k;
	  });
	  v = g != gws.end() && g->seq == v ? 1 : 2;
	}
      }
      encode(h, bl);
    }
    encode(storage, bl); encode(records, bl);
    encode(pms, bl); encode(fenced, bl);
    encode(gcq, bl);
    for (auto& g : gws) g.encode(bl);
    encode(s3_budget, bl); encode(del_phase, bl); encode(del_tag, bl);
    encode(del_tok, bl); encode((uint8_t)ex, bl); encode(ex_to, bl);
    encode(ex_from, bl); encode(ex_inc, bl); encode(ex_crashes, bl);
    encode(acked, bl); encode(base, bl); encode(recloned, bl);
    encode(published, bl);
    encode(next_c, bl); encode(next_r, bl); encode(next_tag, bl);
    encode(next_nonce, bl);
    encode((uint32_t)versions.size(), bl);
    for (auto& [i, v] : versions) {
      encode(i, bl);
      v.encode(bl);
    }
    encode(olh_cur, bl); encode(olh_epoch, bl); encode(next_i, bl);
    encode(ex_inst, bl); encode(ex_token, bl); encode(ex_aborted, bl);
    encode((uint32_t)hints.size(), bl);
    for (const auto& h : hints) h.encode(bl);
    encode(graced, bl); encode(suspended, bl);
    encode(next_epoch, bl);
    return bl.to_str();
  }

  fsio::expired_t dead() const {
    fsio::expired_t d;
    for (auto& g : gws) {
      if (!g.alive || g.zombie) d.insert(g.inc);
    }
    for (auto i = EXPIRER; i < ex_inc; ++i) {
      d.insert(i);
    }
    return d;
  }
  bool head_real() const { return head.exists && !head.id_tag.empty(); }
  std::string head_tok() const { return head_real() ? tok(head.manifest) : ""; }
};

struct Action {
  std::string name;
  std::function<std::optional<std::string>(World&)> run;  // a violation, if any
  bool failure = false;          // a crash or partition:  optional, never work
  bool pass = false;             // the expirer's pass on the head (§4.3)
};

/* §4.3:  what a gateway holds on the head, which its presence hint
 * covers:  opens, and a step under way (a publish, a drain) until the
 * call that releases it */
bool holds_head(const Gw& g)
{
  if (FSIO_SKIP_CHECK(PRESENCE_HINT_HELD_UNTIL_RELEASED)) {
    return !g.opens.empty();
  }
  return !g.opens.empty() || g.step != NONE;
}

/* the dead incarnations whose hints lead the expirer to the head:  any on
 * the head, and publish hints on instance heads, whose rule runs
 * fsio_expire on the OLH first (§4.3) */
bool head_reached(const World& w, const fsio::expired_t& dead)
{
  for (const auto& h : w.hints) {
    if (dead.count(h.incarnation) &&
	(h.target == HintTarget::HEAD || h.kind == HintKind::PUBLISH)) {
      return true;
    }
  }
  return false;
}

/* the actions added from here to the end of scope are the pass's */
struct MarkPass {
  std::vector<Action>& acts;
  size_t from;
  ~MarkPass() {
    for (auto i = from; i < acts.size(); ++i) {
      acts[i].pass = true;
    }
  }
};

/* the manifest a shadow's chunks are named by, found as the design says
 * (§2.2, §2.4):  the pseudo-manifest on its attribute clone */
std::optional<std::string> design_manifest(const World& w,
					   const cls_rgw_fsio_chunk_set& e)
{
  auto s = sname(e.id);
  if (!w.storage.count(s) || !w.pms.count(s)) {
    return std::nullopt;
  }
  return w.pms.at(s);
}

std::optional<std::string> check_manifest(const World& w, const fid_t& cs,
					  const char* who)
{
  const auto* e = w.head.hdr ? w.head.hdr->find(cs) : nullptr;
  if (!e) {
    return fmt::format("{}: no entry for shadow {}", who, sname(cs));
  }
  auto s = sname(cs);
  auto truth = w.base.count(s) ? w.base.at(s) : std::string{};
  auto m = design_manifest(w, *e);
  if (!m) {
    return fmt::format("{}: manifest for {} not found (base {})", who, s,
		       truth);
  }
  if (*m != truth) {
    return fmt::format("{}: manifest for {} is {}, chunks named from {}",
		       who, s, *m, truth);
  }
  return std::nullopt;
}

void apply_clones(World& w, const cls_rgw_fsio_effects& fx,
		  const std::string& head_tok)
{
  for (const auto& c : fx.clones) {
    if (c.role == 'G') {
      auto rn = rname(c.r);
      w.storage.insert(rn);
      w.records[rn] = head_tok;
    } else if (c.role == 'a') {
      w.pms[sname(c.r)] = tok(c.pm);
    }
  }
}

void apply_remove_head(World& w, const cls_rgw_fsio_effects& fx)
{
  if (fx.remove_head) {
    w.head = cls_rgw_fsio_head{};
  }
}

std::vector<std::string> drain_objects(const World& w,
				       const cls_rgw_fsio_chunk_set& e)
{
  if (e.manifest_at == at::CLONE && e.clone) {
    auto rn = rname(*e.clone);
    std::vector<std::string> v{rn};
    if (w.records.count(rn) && !w.records.at(rn).empty()) {
      v.push_back(w.records.at(rn));
    }
    return v;
  }
  if (e.manifest_at == at::SHADOW) {
    return {sname(e.id)};
  }
  return {};
}

/* §2.5a:  a suspended bucket's write replaces the null version on the head
 * and makes it current */
void set_null_version(World& w, const std::string& tail)
{
  Version v;
  v.ih.exists = true;
  v.ih.id_tag = w.head.id_tag;
  v.ih.manifest = tok_bl(tail);
  v.tail = tail;
  v.epoch = w.next_epoch++;
  w.versions["null"] = v;
  w.olh_cur = "null";
  w.olh_epoch = v.epoch;
}

/* a gateway's death or partition, at any point, a protocol step pending
 * included */
/* §3.4:  a constructor that gives up sends the shadow's GC entry, then
 * abandons it with its fsio_leave (W15) */
void construct_abandon_actions(const Gw& g, size_t gi, uint64_t inc,
			       std::vector<Action>& acts)
{
  if (g.step == CONSTRUCT) {
    acts.push_back({fmt::format("gw{} construct-gc {}", glabel(g), sname(g.step_cs)),
	[gi](World& w) -> std::optional<std::string> {
	  auto& g = w.gws[gi];
	  /* giving up fails the open (EAGAIN):  it serves no more I/O, so
	   * the gateway drops it before the GC entry can run */
	  auto& os = g.opens;
	  os.erase(std::remove_if(os.begin(), os.end(), [&](const LOpen& o) {
		     return o.writer && o.cs == g.step_cs;
		   }), os.end());
	  w.gcq.push_back({sname(g.step_cs)});
	  g.step = CONSTRUCT_LEAVE;
	  return std::nullopt;
	}});
    return;
  }
  acts.push_back({fmt::format("gw{} construct-leave {}", glabel(g), sname(g.step_cs)),
      [gi, inc](World& w) -> std::optional<std::string> {
	auto& g = w.gws[gi];
	fsio::leave_out out;
	cls_rgw_fsio_effects fx;
	auto save = w.head;
	if (counted_leave(w.head, g, g.step_cs, true, w.dead(), out, fx,
			nullptr) < 0 || !out.abandoned) {
	  w.head = save;                 // as first written:  no exit
	  g.step = NONE;
	  return std::nullopt;
	}
	apply_remove_head(w, fx);
	g.step = NONE;                   // the open failed:  EAGAIN
	return std::nullopt;
      }});
}

void fail_actions(const Gw& g, size_t gi, std::vector<Action>& acts)
{
  /* a process fails once, through its first handle;  the action takes
   * every handle of the incarnation */
  if (g.handle != 1) {
    return;
  }
  const auto inc = g.inc;
  bool may_partition = true;
  /* a zombie may also die for real, leaving what it did as a zombie */
  if ((!g.did_crash || g.zombie) && cfg.crashes) {
    acts.push_back({fmt::format("gw{} crash", glabel(g)),
	[gi](World& w) -> std::optional<std::string> {
	  /* a process dies whole:  every handle of its incarnation (Z4) */
	  const auto inc = w.gws[gi].inc;
	  for (auto& g : w.gws) {
	    if (g.inc != inc) {
	      continue;
	    }
	    g.alive = false;
	    g.did_crash = 1;
	    g.opens.clear();
	    g.step = NONE;
	  }
	  return std::nullopt;
	}, true});
  }
  if (!g.did_crash && cfg.zombies && may_partition) {
    acts.push_back({fmt::format("gw{} partition", glabel(g)),
	[gi](World& w) -> std::optional<std::string> {
	  const auto inc = w.gws[gi].inc;
	  for (auto& g : w.gws) {
	    if (g.inc == inc) {
	      g.zombie = 1;
	      g.did_crash = 1;
	    }
	  }
	  return std::nullopt;
	}, true});
  }
}

void gw_actions(const World& w0, size_t gi, std::vector<Action>& acts)
{
  const auto& g = w0.gws[gi];
  if (!g.alive) {
    return;
  }
  if (g.zombie && w0.graced.count(g.inc)) {
    return;                  // the lease:  stopped, its ops timed out (§4.3)
  }
  auto inc = g.inc;
  if (g.step != NONE) {
    switch (g.step) {
    case CONSTRUCT:
      acts.push_back({fmt::format("gw{} ready {}", glabel(g), sname(g.step_cs)),
	  [gi, inc](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    m_ready(w.head, hd(g), g.step_cs, {{0, 0}});
	    g.step = NONE;
	    return std::nullopt;
	  }});
      construct_abandon_actions(g, gi, inc, acts);
      break;
    case CONSTRUCT_LEAVE:
      construct_abandon_actions(g, gi, inc, acts);
      break;
    case PUBLISH_HINT:
      /* §3.5:  the hint naming f(head, r) is added before the publish */
      acts.push_back({fmt::format("gw{} publish-hint", glabel(g)),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    g.step_r = fid_t{w.next_r++, 0};
	    w.hints.insert(publish_head(hd(g), g.step_r));
	    g.step = PUBLISH_WRITE;
	    return std::nullopt;
	  }});
      break;
    case PUBLISH_WRITE:
      acts.push_back({fmt::format("gw{} publish {}", glabel(g), sname(g.step_cs)),
	  [gi, inc](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    /* fsio_publish's own check (§3.5):  a zombie whose shadow was
	     * finalized fails here, before it reads anything */
	    const auto* e = w.head.hdr ? w.head.hdr->find(g.step_cs) : nullptr;
	    if (!e || e->state != state::PUBLISHING || !e->publishing ||
		e->publishing->publisher.incarnation != inc) {
	      w.hints.erase(publish_head(hd(g), g.step_r));
	      g.release_failed = 1;           // keeps its hint (H1)
	      g.step = NONE;
	      return std::nullopt;
	    }
	    if (auto v = check_manifest(w, g.step_cs, "publish")) {
	      return v;
	    }
	    auto old = w.head_tok();
	    fsio::replace_in in;
	    in.mode = fsio::replace_mode::PUBLISH;
	    in.r = g.step_r;
	    in.caller = hd(g);
	    in.shadow = g.step_cs;
	    fsio::replace_out out;
	    cls_rgw_fsio_effects fx;
	    int r = m_replace_head(w.head, in, out, fx);
	    if (r < 0) {
	      w.hints.erase(publish_head(hd(g), g.step_r));
	      g.release_failed = 1;           // keeps its hint (H1)
	      g.step = NONE;
	      return std::nullopt;
	    }
	    apply_clones(w, fx, old);
	    auto s = sname(g.step_cs);
	    w.head.exists = true;
	    w.head.manifest = tok_bl(s);
	    w.head.id_tag = fmt::format("t{}", w.next_tag++);
	    if (cfg.versioned) {
	      set_null_version(w, s);        // suspended:  the null version (§2.5a)
	    }
	    if (++w.published[s] > 1) {
	      return fmt::format("{} published twice", s);
	    }
	    g.step = PUBLISH_GC;
	    g.step_r = in.r;
	    g.step_born = out.born_drained;
	    g.step_drained = out.drained;
	    return std::nullopt;
	  }});
      break;
    case PUBLISH_GC:
      acts.push_back({fmt::format("gw{} publish-gc", glabel(g)),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    if (g.step_born) {
	      auto rn = rname(g.step_r);
	      std::vector<std::string> v{rn};
	      if (!w.records[rn].empty()) {
		v.push_back(w.records[rn]);
	      }
	      w.gcq.push_back(v);
	    }
	    w.hints.erase(publish_head(hd(g), g.step_r));
	    if (g.step_drained) {
	      g.step = DRAIN_SEND;
	      g.step_cs = *g.step_drained;
	      g.step_drained.reset();
	    } else {
	      g.step = NONE;
	    }
	    return std::nullopt;
	  }});
      break;
    case DRAIN_SEND:
      acts.push_back({fmt::format("gw{} drain {}", glabel(g), sname(g.step_cs)),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    const auto* e = w.head.hdr ? w.head.hdr->find(g.step_cs) : nullptr;
	    if (e) {
	      if (e->manifest_at == at::SHADOW) {
		if (auto v = check_manifest(w, e->id, "drain")) {
		  return v;
		}
	      }
	      w.gcq.push_back(drain_objects(w, *e));
	      cls_rgw_fsio_effects fx;
	      m_forget(w.head, g.step_cs, fx);
	      apply_remove_head(w, fx);
	    }
	    g.step = NONE;
	    return std::nullopt;
	  }});
      break;
    default:
      break;
    }
    fail_actions(g, gi, acts);
    return;
  }

  if (!g.did_read && g.role != 'w') {
    acts.push_back({fmt::format("gw{} open-read", glabel(g)),
	[gi, inc](World& w) -> std::optional<std::string> {
	  auto& g = w.gws[gi];
	  g.did_read = 1;
	  fsio::join_in in{hd(g), 0, false, w.nonce_for_join(), 10, w.dead()};
	  fsio::join_out out;
	  cls_rgw_fsio_effects fx;
	  auto save = w.head;
	  if (counted_join(w.head, g, in, out, fx) < 0) {
	    w.head = save;
	    return std::nullopt;
	  }
	  w.took_nonce();
	  g.opens.push_back(LOpen{false, out.bound, w.head_tok()});
	  return std::nullopt;
	}});
  }
  if (!g.did_write && g.role != 'r') {
    acts.push_back({fmt::format("gw{} open-write", glabel(g)),
	[gi, inc](World& w) -> std::optional<std::string> {
	  auto& g = w.gws[gi];
	  g.did_write = 1;
	  fsio::join_in in{hd(g), 0, true, w.nonce_for_join(), 10, w.dead()};
	  fsio::join_out out;
	  cls_rgw_fsio_effects fx;
	  auto save = w.head;
	  if (counted_join(w.head, g, in, out, fx) < 0) {
	    w.head = save;
	    return std::nullopt;
	  }
	  w.took_nonce();
	  apply_clones(w, fx, w.head_tok());
	  auto s = sname(out.bound);
	  g.opens.push_back(LOpen{true, out.bound, s});
	  if (out.construct) {
	    w.storage.insert(s);
	    w.base[s] = w.head_tok();
	    g.step = CONSTRUCT;
	    g.step_cs = out.bound;
	  } else if (w.base[s] != w.head_tok()) {
	    /* a new open sees the current content (plan §6.5) */
	    return fmt::format("gw{}'s new write open joins {}, built on {}, "
			       "but the head is {}", inc, s, w.base[s],
			       w.head_tok());
	  }
	  return std::nullopt;
	}});
  }
  for (size_t oi = 0; oi < g.opens.size(); ++oi) {
    const auto& o = g.opens[oi];
    if (o.writer && !g.did_io) {
      acts.push_back({fmt::format("gw{} write {}", glabel(g), o.storage),
	  [gi, oi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    g.did_io = 1;
	    auto& o = g.opens[oi];
	    const auto* e = w.head.hdr ? w.head.hdr->find(o.cs) : nullptr;
	    if (!e) {
	      return std::nullopt;           // fails to the client
	    }
	    if (w.fenced.count(o.storage)) {
	      return std::nullopt;           // fenced:  fails to the client
	    }
	    if (!w.storage.count(o.storage)) {
	      return fmt::format("write to {}, collected", o.storage);
	    }
	    if (e->state == state::PUBLISHED) {
	      return fmt::format("write into published {}", o.storage);
	    }
	    ++w.acked[o.storage];
	    return std::nullopt;
	  }});
    }
    if (!o.writer && !g.did_follow && w0.head.hdr) {
      const auto* e = w0.head.hdr->find(o.cs);
      if (e && e->state == state::PUBLISHED) {
	for (const auto& t : w0.head.hdr->chunk_sets) {
	  const auto& c = w0.head.hdr->cur_shadow;
	  if (c && t.id == *c && t.state == state::READY) {
	    auto to = t.id;
	    acts.push_back({fmt::format("gw{} follow {}", glabel(g), sname(to)),
		[gi, oi, inc, to](World& w) -> std::optional<std::string> {
		  auto& g = w.gws[gi];
		  g.did_follow = 1;
		  auto& o = g.opens[oi];
		  if (counted_rebind(w.head, g, o.cs, to, 1) == 0) {
		    o.cs = to;
		    o.storage = sname(to);
		  }
		  return std::nullopt;
		}});
	  }
	}
      }
    }
    acts.push_back({fmt::format("gw{} close-{} {}", glabel(g),
				o.writer ? "write" : "read", o.storage),
	[gi, oi, inc](World& w) -> std::optional<std::string> {
	  auto& g = w.gws[gi];
	  auto o = g.opens[oi];
	  g.opens.erase(g.opens.begin() + oi);
	  fsio::leave_out out;
	  cls_rgw_fsio_effects fx;
	  auto save = w.head;
	  if (counted_leave(w.head, g, o.cs, o.writer, w.dead(), out, fx) < 0) {
	    w.head = save;
	    g.release_failed = 1;         // keeps its hint (H1)
	    return std::nullopt;
	  }
	  apply_remove_head(w, fx);
	  if (out.publishing) {
	    g.step = PUBLISH_HINT;
	    g.step_cs = o.cs;
	  } else if (out.drained) {
	    g.step = DRAIN_SEND;
	    g.step_cs = *out.drained;
	  }
	  return std::nullopt;
	}});
  }
  fail_actions(g, gi, acts);
}

/* versioned:  the content an OLH entry names (§2.2) */
std::string entry_tail(const World& w, const cls_rgw_fsio_chunk_set& e)
{
  if (e.manifest_at == at::INSTANCE && e.instance) {
    auto v = w.versions.find(*e.instance);
    return v == w.versions.end() ? std::string{} : v->second.tail;
  }
  if (e.manifest_at == at::HEAD) {
    return w.head_tok();                 // the head's own content (§2.5a)
  }
  if (e.manifest_at == at::CLONE && e.clone) {
    auto r = w.records.find(rname(*e.clone));
    return r == w.records.end() ? std::string{} : r->second;
  }
  return sname(e.id);
}

/* versioned:  the version whose content an open reads, from the header
 * (§2.5):  its entry's instance, or a publishing shadow's new version;
 * a shadow not yet publishing is no version's */
std::string open_version(const World& w, const LOpen& o)
{
  const auto* e = w.head.hdr ? w.head.hdr->find(o.cs) : nullptr;
  if (!e) {
    return o.instance;
  }
  if (e->manifest_at == at::INSTANCE && e->instance) {
    return *e->instance;
  }
  if (e->publishing) {
    return e->publishing->instance;
  }
  if (e->manifest_at == at::HEAD) {
    return "null";                       // the head's content (§2.5a)
  }
  return {};
}

/* versioned:  the newest live version, after a removal of the current one */
void relink(World& w)
{
  w.olh_cur.clear();
  uint64_t best = 0;
  for (const auto& [i, v] : w.versions) {
    if (v.ih.exists && !v.ih.id_tag.empty() && v.epoch >= best) {
      best = v.epoch;
      w.olh_cur = i;
    }
  }
}

/* versioned:  an open count drained;  its GC entries, then ocnt_forget */
void ocnt_send(World& w, const std::string& inst)
{
  auto v = w.versions.find(inst);
  if (v == w.versions.end() || !v->second.ih.ocnt) {
    return;
  }
  std::vector<std::string> objs;
  for (const auto& d : v->second.ih.ocnt->deferred) {
    objs.push_back(tok(d.manifest));
    objs.push_back(fmt::format("B{}", d.clone.counter));
  }
  if (!objs.empty()) {
    w.gcq.push_back(objs);
  }
  cls_rgw_fsio_effects fx;
  m_ocnt_forget(v->first, v->second.ih, fx);
  if (fx.remove_head) {
    w.versions.erase(v);
  }
}

/* a versioned bucket's gateway (stage A):  an open reads the OLH, adds its
 * incarnation to that version's open count, then joins (§2.5, §3.1);  a
 * close leaves, and the count goes when nothing is bound to the version;
 * the last writer's close publishes a new version (§4.2, normal path) */
void gw_actions_v(const World& w0, size_t gi, std::vector<Action>& acts)
{
  const auto& g = w0.gws[gi];
  if (!g.alive) {
    return;
  }
  if (g.zombie && w0.graced.count(g.inc)) {
    return;                  // the lease:  stopped, its ops timed out (§4.3)
  }
  if (g.step == PUBLISH_HINT || g.step == PUBLISH_WRITE ||
      g.step == PUBLISH_GC) {
    gw_actions(w0, gi, acts);  // a suspended bucket's publish (§2.5a)
    return;
  }
  auto inc = g.inc;
  if (g.step != NONE) {
    switch (g.step) {
    case V_COUNT:
      acts.push_back({fmt::format("gw{} count {}", glabel(g), g.pending),
	  [gi, inc](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    /* §4.3:  the presence hint on the instance head comes first */
	    const bool had = g.counted.count(g.pending);
	    if (!had) {
	      if (FSIO_SKIP_CHECK(PRESENCE_HINT_BEFORE_OCNT)) {
		g.owe_ih_hint = g.pending;   // in a step of its own, after
	      } else {
		w.hints.insert(presence_ih(g.pending, hd(g)));
	      }
	    }
	    auto v = w.versions.find(g.pending);
	    if (v == w.versions.end() ||
		m_ocnt_add(v->first, v->second.ih, hd(g)) < 0) {
	      if (!had) {
		w.hints.erase(presence_ih(g.pending, hd(g)));
		g.owe_ih_hint.clear();
	      }
	      g.step = NONE;                 // the open fails
	      return std::nullopt;
	    }
	    g.counted.insert(g.pending);
	    g.step = V_JOIN;
	    return std::nullopt;
	  }});
      break;
    case V_JOIN:
      acts.push_back({fmt::format("gw{} open-{} {}", glabel(g),
				  g.pending_writer ? "write" : "read",
				  g.pending),
	  [gi, inc](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    g.step = NONE;
	    if (w.olh_cur != g.pending) {
	      return std::nullopt;           // the OLH guard:  ECANCELED
	    }
	    fsio::join_in in{hd(g), 0, g.pending_writer, w.nonce_for_join(), 10,
			     w.dead(), g.pending};
	    fsio::join_out out;
	    cls_rgw_fsio_effects fx;
	    auto save = w.head;
	    if (counted_join(w.head, g, in, out, fx) < 0) {
	      w.head = save;
	      return std::nullopt;
	    }
	    w.took_nonce();
	    const auto cur = w.versions.at(w.olh_cur).tail;
	    if (!g.pending_writer) {
	      const auto* e = w.head.hdr->find(out.bound);
	      auto t = entry_tail(w, *e);
	      g.opens.push_back(LOpen{false, out.bound, t, g.pending});
	      if (t != cur) {
		return fmt::format("gw{}'s new read open binds to {}, but the "
				   "OLH names {}", inc, t, cur);
	      }
	      return std::nullopt;
	    }
	    auto sn = sname(out.bound);
	    g.opens.push_back(LOpen{true, out.bound, sn, g.pending});
	    apply_clones(w, fx, w.head_tok());   // the null version's (§2.5a)
	    if (out.construct) {
	      /* §4.1:  the gateway clones the instance head, its manifest the
	       * pseudo-manifest */
	      w.storage.insert(sn);
	      w.base[sn] = cur;
	      w.pms[sn] = cur;
	      g.step = CONSTRUCT;
	      g.step_cs = out.bound;
	    } else if (w.base[sn] != cur) {
	      return fmt::format("gw{}'s new write open joins {}, built on {}, "
				 "but the OLH names {}", inc, sn, w.base[sn],
				 cur);
	    }
	    return std::nullopt;
	  }});
      break;
    case CONSTRUCT:
      acts.push_back({fmt::format("gw{} ready {}", glabel(g), sname(g.step_cs)),
	  [gi, inc](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    m_ready(w.head, hd(g), g.step_cs, {{0, 0}});
	    g.step = NONE;
	    return std::nullopt;
	  }});
      construct_abandon_actions(g, gi, inc, acts);
      break;
    case CONSTRUCT_LEAVE:
      construct_abandon_actions(g, gi, inc, acts);
      break;
    case V_OCNT_SEND:
      acts.push_back({fmt::format("gw{} ocnt-send {}", glabel(g), g.step_inst),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    ocnt_send(w, g.step_inst);
	    w.hints.erase(presence_ih(g.step_inst, hd(g)));
	    g.step = NONE;
	    return std::nullopt;
	  }});
      break;
    case V_PUB_READ:
      /* §4.2 step 3:  the incarnations bound to the shadow, read now that
       * joining as reader waits */
      acts.push_back({fmt::format("gw{} publish-read {}", glabel(g),
				  sname(g.step_cs)),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    g.pub_incs.clear();
	    for (const auto& [i, o] : w.head.opens) {
	      for (const auto& [hdl, ho] : o.handles) {   // per handle (Z4)
		auto c = ho.chunk_sets.find(g.step_cs);
		if (c != ho.chunk_sets.end() &&
		    (c->second.readers ||
		     (FSIO_SKIP_CHECK(W7_PUBLISH_COUNTS_READERS_ONLY) &&
		      c->second.writers))) {
		  g.pub_incs.insert({i, hdl});  // readers only (§4.2 step 3, W7)
		}
	      }
	    }
	    /* §4.2 step 3:  a presence hint on the new instance head for
	     * each, keyed by its incarnation (H3) */
	    if (!FSIO_SKIP_CHECK(H3_PUBLISHER_HINTS_READERS)) {
	      for (auto i : g.pub_incs) {
		w.hints.insert(presence_ih(g.pub_instance, i));
	      }
	    }
	    g.step = V_PUB_WRITE;
	    return std::nullopt;
	  }});
      break;
    case V_PUB_WRITE:
      /* §4.2 step 4:  the instance head, its open count those incarnations */
      acts.push_back({fmt::format("gw{} instance-write {}", glabel(g),
				  g.pub_instance),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    auto sn = sname(g.step_cs);
	    /* the write's guard:  cmpxattr(fsio.claim, EQ, token);  an aborted
	     * or absent placeholder fails it, and the publisher stops:  the
	     * expirer owns the shadow (§4.2) */
	    auto vi = w.versions.find(g.pub_instance);
	    if (vi == w.versions.end() || vi->second.claim != g.pub_token) {
	      g.release_failed = 1;          // the publish failed (H1)
	      if (vi != w.versions.end() && vi->second.claim == "aborted" &&
		  !FSIO_SKIP_CHECK(V4_PUBLISHER_KEEPS_HINT)) {
		g.step = V_PUB_CLEANUP;      // keeps its hint (§4.2, V4)
		return std::nullopt;
	      }
	      w.hints.erase(publish_ih(g.pub_instance, hd(g)));
	      g.step = NONE;
	      return std::nullopt;
	    }
	    if (auto v = check_manifest(w, g.step_cs, "publish")) {
	      return v;
	    }
	    if (g.pub_began_suspended) {
	      return fmt::format("a publish begun in a suspended bucket made "
				 "version {}", g.pub_instance);
	    }
	    Version v;
	    v.ih.exists = true;
	    v.ih.id_tag = fmt::format("i{}", w.next_tag++);
	    v.ih.manifest = tok_bl(sn);
	    if (!g.pub_incs.empty()) {
	      v.ih.ocnt.emplace();
	      v.ih.ocnt->holders = g.pub_incs;
	    }
	    v.tail = sn;
	    v.epoch = g.pub_epoch;
	    w.versions[g.pub_instance] = v;
	    /* the claim's removal goes after the head's replacement in the
	     * op, or the replacement writes it back (§3, the ordering rule) */
	    if (FSIO_SKIP_CHECK(Z6_INSTANCE_WRITE_DROPS_CLAIM)) {
	      w.versions[g.pub_instance].claim = g.pub_token;
	    }
	    if (++w.published[sn] > 1) {
	      return fmt::format("{} linked as a version twice", sn);
	    }
	    g.step = V_PUB_LINK;
	    return std::nullopt;
	  }});
      break;
    case V_PUB_LINK:
      /* set_olh() at the epoch recorded at the transition (§4.2) */
      acts.push_back({fmt::format("gw{} link {}", glabel(g), g.pub_instance),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    if (g.pub_epoch > w.olh_epoch) {
	      w.olh_cur = g.pub_instance;
	      w.olh_epoch = g.pub_epoch;
	    }
	    g.step = V_PUB_PUBLISH;
	    return std::nullopt;
	  }});
      break;
    case V_PUB_PUBLISH:
      acts.push_back({fmt::format("gw{} publish {}", glabel(g), sname(g.step_cs)),
	  [gi, inc](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    fsio::replace_in in;
	    in.mode = fsio::replace_mode::PUBLISH;
	    in.versioned = true;
	    in.caller = hd(g);
	    in.shadow = g.step_cs;
	    fsio::replace_out out;
	    cls_rgw_fsio_effects fx;
	    auto save = w.head;
	    if (m_replace_head(w.head, in, out, fx) < 0) {
	      w.head = save;
	      w.hints.erase(publish_ih(g.pub_instance, hd(g)));
	      g.release_failed = 1;           // keeps its hint (H1)
	      g.step = NONE;
	      return std::nullopt;
	    }
	    apply_remove_head(w, fx);
	    g.step_drained = out.drained;
	    g.step = V_PUB_UNHINT;
	    return std::nullopt;
	  }});
      break;
    case V_PUB_HINT:
      /* §4.2 step 1:  an instance id and a token, and a publish hint
       * naming the instance head */
      acts.push_back({fmt::format("gw{} publish-hint", glabel(g)),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    g.pub_instance = fmt::format("I{}", w.next_i++);
	    g.pub_token = fmt::format("k{}", w.next_tag++);
	    w.hints.insert(publish_ih(g.pub_instance, hd(g)));
	    g.step = V_PLACE;
	    return std::nullopt;
	  }});
      break;
    case V_PLACE:
      acts.push_back({fmt::format("gw{} placeholder {}", glabel(g), g.pub_instance),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    Version v;                           // exclusive create
	    v.ih.exists = true;
	    v.claim = g.pub_token;
	    w.versions.emplace(g.pub_instance, v);
	    g.step = V_LEAVE;
	    return std::nullopt;
	  }});
      break;
    case V_LEAVE:
      /* §4.2 step 2:  fsio_leave records the instance, token and epoch */
      acts.push_back({fmt::format("gw{} close-leave {}", glabel(g),
				  sname(g.step_cs)),
	  [gi, inc](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    fsio::leave_out out;
	    cls_rgw_fsio_effects fx;
	    fsio::publish_intent intent{g.pub_instance, g.pub_token,
					w.next_epoch};
	    auto save = w.head;
	    if (counted_leave(w.head, g, g.step_cs, true, w.dead(), out, fx,
			    &intent) < 0) {
	      w.head = save;
	      g.release_failed = 1;         // keeps its hint (H1)
	      g.step = V_UNPLACE;
	      return std::nullopt;
	    }
	    apply_remove_head(w, fx);
	    /* §2.5, I4:  giving up its write open, the publisher's
	     * fsio_leave returns the version its publish creates, but no
	     * publish counts the writer:  it is not a
	     * version this handle is in, and taking it for one skipped the
	     * presence hint on a later count of it (H1) */
	    if (!out.instance.empty() &&
		FSIO_SKIP_CHECK(I4_PUBLISHER_NOT_COUNTED)) {
	      g.counted.insert(out.instance);
	    }
	    if (out.publishing) {
	      ++w.next_epoch;
	      g.pub_epoch = out.olh_epoch;
	      g.step = V_PUB_READ;
	    } else {
	      g.step = V_UNPLACE;              // another writer remains
	    }
	    return std::nullopt;
	  }});
      break;
    case V_UNPLACE:
      acts.push_back({fmt::format("gw{} unplace {}", glabel(g), g.pub_instance),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    auto vi = w.versions.find(g.pub_instance);
	    if (vi != w.versions.end() && vi->second.claim == g.pub_token &&
		vi->second.ih.id_tag.empty()) {
	      w.versions.erase(vi);            // guarded:  claim == token
	    }
	    w.hints.erase(publish_ih(g.pub_instance, hd(g)));
	    g.step = NONE;
	    return std::nullopt;
	  }});
      break;
    case V_PUB_UNHINT:
      acts.push_back({fmt::format("gw{} publish-unhint {}", glabel(g),
				  g.pub_instance),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    w.hints.erase(publish_ih(g.pub_instance, hd(g)));
	    if (g.step_drained) {
	      g.step = DRAIN_SEND;
	      g.step_cs = *g.step_drained;
	      g.step_drained.reset();
	    } else {
	      g.step = NONE;
	    }
	    return std::nullopt;
	  }});
      break;
    case DRAIN_SEND:
      acts.push_back({fmt::format("gw{} drain {}", glabel(g), sname(g.step_cs)),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    const auto* e = w.head.hdr ? w.head.hdr->find(g.step_cs) : nullptr;
	    if (e) {
	      w.gcq.push_back(drain_objects(w, *e));
	      cls_rgw_fsio_effects fx;
	      m_forget(w.head, g.step_cs, fx);
	      apply_remove_head(w, fx);
	    }
	    g.step = NONE;
	    return std::nullopt;
	  }});
      break;
    case V_PUB_CLEANUP: {
      /* §4.2:  once no chunk-set entry names the instance, delete the
       * aborted placeholder, then drop the hint (V4) */
      bool named = false;
      if (w0.head.hdr) {
	for (const auto& e : w0.head.hdr->chunk_sets) {
	  named |= (e.instance && *e.instance == g.pub_instance) ||
	    (e.publishing && e.publishing->instance == g.pub_instance);
	}
      }
      if (!named) {
	acts.push_back({fmt::format("gw{} unplace-aborted {}", glabel(g),
				    g.pub_instance),
	    [gi](World& w) -> std::optional<std::string> {
	      auto& g = w.gws[gi];
	      /* first the presence hints it wrote for its readers (H3f) */
	      if (!FSIO_SKIP_CHECK(H3_PUBLISHER_CLEANUP_REMOVES_READER_HINTS)) {
		for (auto i : g.pub_incs) {
		  w.hints.erase(presence_ih(g.pub_instance, i));
		}
	      }
	      auto vi = w.versions.find(g.pub_instance);
	      if (vi != w.versions.end() && vi->second.claim == "aborted" &&
		  vi->second.ih.id_tag.empty()) {
		w.versions.erase(vi);
	      }
	      w.hints.erase(publish_ih(g.pub_instance, hd(g)));
	      g.pub_incs.clear();
	      g.step = g.pub_retry ? V_PUB_HINT : NONE;
	      g.pub_retry = 0;
	      return std::nullopt;
	    }});
      }
      break;
    }
    case V_REL_RELEASE:
      acts.push_back({fmt::format("gw{} publish-release {}", glabel(g),
				  sname(g.step_cs)),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    if (m_publish_release(w.head, hd(g), g.step_cs) < 0) {
	      g.release_failed = 1;          // the expirer took the shadow over
	    } else {
	      g.pub_retry = 1;               // its idle timer publishes again
	    }
	    g.step = V_PUB_CLEANUP;
	    return std::nullopt;
	  }});
      break;
    default:
      break;
    }
    /* §4.2:  a publisher that cannot proceed releases only before its
     * instance write:  it aborts its own claim, releases, then cleans up
     * as after a failed instance write (W10) */
    if (!g.released &&
	(g.step == V_PUB_READ || g.step == V_PUB_WRITE ||
	 (FSIO_SKIP_CHECK(W10_RELEASE_ONLY_WITH_LIVE_CLAIM) &&
	  (g.step == V_PUB_LINK || g.step == V_PUB_PUBLISH)))) {
      acts.push_back({fmt::format("gw{} release-abort {}", glabel(g),
				  g.pub_instance),
	  [gi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    auto vi = w.versions.find(g.pub_instance);
	    g.released = 1;
	    if (vi != w.versions.end() && vi->second.claim == g.pub_token) {
	      if (FSIO_SKIP_CHECK(W10_RELEASE_ABORTS_CLAIM_FIRST)) {
		w.versions.erase(vi);        // as first written:  unplace first
	      } else {
		vi->second.claim = "aborted";
	      }
	      g.step = V_REL_RELEASE;
	    } else if (FSIO_SKIP_CHECK(W10_RELEASE_ONLY_WITH_LIVE_CLAIM) &&
		       vi != w.versions.end() && !vi->second.claim) {
	      g.step = V_REL_RELEASE;        // as first written:  after the write
	    } else {
	      g.release_failed = 1;          // already aborted:  the expirer's
	      g.step = V_PUB_CLEANUP;
	    }
	    return std::nullopt;
	  }});
    }
    fail_actions(g, gi, acts);
    return;
  }

  /* an open:  read the OLH first (§2.5) */
  for (int writer = 0; writer < 2; ++writer) {
    if (writer ? (g.did_write || g.role == 'r') : (g.did_read || g.role == 'w')) {
      continue;
    }
    acts.push_back({fmt::format("gw{} read-olh-{}", glabel(g),
				writer ? "write" : "read"),
	[gi, writer](World& w) -> std::optional<std::string> {
	  auto& g = w.gws[gi];
	  (writer ? g.did_write : g.did_read) = 1;
	  if (w.olh_cur.empty()) {
	    return std::nullopt;             // a delete marker:  ENOENT
	  }
	  g.pending = w.olh_cur;
	  g.pending_writer = writer;
	  /* §2.5a:  the null version has no open count;  joining is as in
	   * an unversioned bucket */
	  g.step = w.olh_cur == "null" && !FSIO_SKIP_CHECK(NULL_IS_HEAD) ?
	    V_JOIN : V_COUNT;
	  return std::nullopt;
	}});
  }
  for (size_t oi = 0; oi < g.opens.size(); ++oi) {
    const auto& o = g.opens[oi];
    if (o.writer && !g.did_io) {
      acts.push_back({fmt::format("gw{} write {}", glabel(g), o.storage),
	  [gi, oi](World& w) -> std::optional<std::string> {
	    auto& g = w.gws[gi];
	    g.did_io = 1;
	    auto& o = g.opens[oi];
	    const auto* e = w.head.hdr ? w.head.hdr->find(o.cs) : nullptr;
	    if (!e || w.fenced.count(o.storage)) {
	      return std::nullopt;           // fails to the client
	    }
	    if (!w.storage.count(o.storage)) {
	      return fmt::format("write to {}, collected", o.storage);
	    }
	    if (e->state == state::PUBLISHED) {
	      return fmt::format("write into published {}", o.storage);
	    }
	    ++w.acked[o.storage];
	    return std::nullopt;
	  }});
    }
    if (!o.writer && !g.did_follow && w0.head.hdr) {
      const auto* e = w0.head.hdr->find(o.cs);
      const auto& c = w0.head.hdr->cur_shadow;
      const auto* t = c ? w0.head.hdr->find(*c) : nullptr;
      if (e && e->state == state::PUBLISHED && t &&
	  (t->state == state::READY || t->state == state::PUBLISHING)) {
	auto to = t->id;
	/* as written:  a rebind takes no count (W7(a));  into a ready or a
	 * publishing shadow, as fsio_rebind allows (§3) */
	acts.push_back({fmt::format("gw{} follow {}", glabel(g), sname(to)),
	    [gi, oi, inc, to](World& w) -> std::optional<std::string> {
	      auto& g = w.gws[gi];
	      auto& o = g.opens[oi];
	      if (counted_rebind(w.head, g, o.cs, to, 1) == 0) {
		g.did_follow = 1;
		o.cs = to;
		o.storage = sname(to);
	      }                              // BUSY:  follows later (§3, W7)
	      return std::nullopt;
	    }});
      }
    }
    acts.push_back({fmt::format("gw{} close-{} {}", glabel(g),
				o.writer ? "write" : "read", o.storage),
	[gi, oi, inc](World& w) -> std::optional<std::string> {
	  auto& g = w.gws[gi];
	  auto o = g.opens[oi];
	  g.opens.erase(g.opens.begin() + oi);
	  if (o.writer && !(w.suspended &&
			    !FSIO_SKIP_CHECK(PUBLISH_FOLLOWS_STATE))) {
	    /* §4.2 step 1:  the claim comes before fsio_leave;  a publish
	     * keeps the versioning state it began with, as an RGW PUT does */
	    g.step = V_PUB_HINT;
	    g.step_cs = o.cs;
	    g.pub_began_suspended = w.suspended;
	    return std::nullopt;
	  }
	  if (o.writer) {
	    /* §2.5a:  suspended, a publish overwrites the null version:  the
	     * unversioned publish */
	    fsio::leave_out out;
	    cls_rgw_fsio_effects fx;
	    auto save = w.head;
	    if (counted_leave(w.head, g, o.cs, true, w.dead(), out, fx) < 0) {
	      w.head = save;
	      g.release_failed = 1;         // keeps its hint (H1)
	      return std::nullopt;
	    }
	    apply_remove_head(w, fx);
	    if (out.publishing) {
	      g.step = PUBLISH_HINT;
	      g.step_cs = o.cs;
	    }
	    return std::nullopt;
	  }
	  fsio::leave_out out;
	  cls_rgw_fsio_effects fx;
	  auto save = w.head;
	  if (counted_leave(w.head, g, o.cs, false, w.dead(), out, fx) < 0) {
	    w.head = save;
	    g.release_failed = 1;         // keeps its hint (H1)
	    return std::nullopt;
	  }
	  apply_remove_head(w, fx);
	  if (!out.instance.empty()) {
	    g.counted.insert(out.instance);  // to remove itself from (§2.5)
	  }
	  if (out.drained) {
	    g.step = DRAIN_SEND;
	    g.step_cs = *out.drained;
	  }
	  return std::nullopt;
	}});
  }
  /* §2.5:  a version none of the gateway's opens reads any more;  one
   * whose instance head is a placeholder with a live claim is removed
   * from once written (H3c) */
  for (const auto& inst : g.counted) {
    bool bound = false;
    for (const auto& o : g.opens) {
      bound |= open_version(w0, o) == inst;
    }
    if (bound) {
      continue;
    }
    {
      /* written:  remove now;  a placeholder with a live claim:  wait;
       * aborted or absent:  nothing to remove (H3c:  the placeholder
       * precedes fsio_leave's intent, so an absent one cannot appear) */
      auto vi = w0.versions.find(inst);
      bool wait = false;
      if (vi != w0.versions.end()) {
	wait = vi->second.placeholder() && *vi->second.claim != "aborted";
      }
      if (wait) {
	continue;
      }
    }
    acts.push_back({fmt::format("gw{} uncount {}", glabel(g), inst),
	[gi, inc, inst](World& w) -> std::optional<std::string> {
	  auto& g = w.gws[gi];
	  g.counted.erase(inst);
	  auto v = w.versions.find(inst);
	  if (v == w.versions.end()) {
	    w.hints.erase(presence_ih(inst, hd(g)));
	    return std::nullopt;
	  }
	  fsio::ocnt_drain d;
	  m_ocnt_remove(v->first, v->second.ih, hd(g), d);
	  if (d.drained) {
	    g.step = V_OCNT_SEND;            // the hint goes after the send
	    g.step_inst = inst;
	  } else {
	    w.hints.erase(presence_ih(inst, hd(g)));
	  }
	  return std::nullopt;
	}});
  }
  fail_actions(g, gi, acts);
}

void s3_actions(const World& w0, std::vector<Action>& acts)
{
  if (w0.s3_budget <= 0) {
    return;
  }
  if (w0.del_phase == 0) {
    acts.push_back({"s3 put",
	[](World& w) -> std::optional<std::string> {
	  --w.s3_budget;
	  auto c = fmt::format("C{}", w.next_c++);
	  w.storage.insert(c);
	  auto old = w.head_tok();
	  fsio::replace_in in;
	  in.mode = fsio::replace_mode::SUPERSEDE;
	  in.r = fid_t{w.next_r++, 0};
	  fsio::replace_out out;
	  cls_rgw_fsio_effects fx;
	  m_replace_head(w.head, in, out, fx);
	  apply_clones(w, fx, old);
	  if (!out.keep_tail && !old.empty()) {
	    w.gcq.push_back({old});
	  }
	  w.head.exists = true;
	  w.head.manifest = tok_bl(c);
	  w.head.id_tag = fmt::format("t{}", w.next_tag++);
	  return std::nullopt;
	}});
    if (w0.head_real()) {
      acts.push_back({"s3 delete-read",
	  [](World& w) -> std::optional<std::string> {
	    w.del_phase = 1;
	    w.del_tag = w.head.id_tag;
	    w.del_tok = w.head_tok();
	    return std::nullopt;
	  }});
    }
    acts.push_back({"s3 stop",
	[](World& w) -> std::optional<std::string> {
	  w.s3_budget = 0;
	  return std::nullopt;
	}});
  } else {
    acts.push_back({"s3 delete",
	[](World& w) -> std::optional<std::string> {
	  fsio::replace_in in;
	  in.mode = fsio::replace_mode::DELETE;
	  in.r = fid_t{w.next_r++, 0};
	  in.expected_id_tag = w.del_tag;
	  fsio::replace_out out;
	  cls_rgw_fsio_effects fx;
	  auto old = w.head_tok();
	  if (m_replace_head(w.head, in, out, fx) < 0) {
	    w.del_phase = 0;          // re-read and retry
	    return std::nullopt;
	  }
	  --w.s3_budget;
	  w.del_phase = 0;
	  apply_clones(w, fx, old);
	  if (!out.keep_tail && !w.del_tok.empty()) {
	    w.gcq.push_back({w.del_tok});    // RGW's cached chain
	  }
	  if (w.head.hdr || !w.head.opens.empty()) {
	    w.head.id_tag.clear();            // tracking-only
	    w.head.manifest.clear();
	  } else {
	    w.head = cls_rgw_fsio_head{};
	  }
	  return std::nullopt;
	}});
  }
}

/* §3.7 step 1:  records the finalize, or takes a recorded one over */
std::optional<std::string> start_finalize(World& w, const fid_t& from,
					  const fsio::expired_t& dead)
{
  fid_t to;
  if (m_fenced_finalize(w.head, xh(w.ex_inc), from, dead, to) < 0) {
    w.ex = EX_IDLE;              // drop the decision, re-read next pass
    return std::nullopt;
  }
  w.ex = EX_FF_RECLONE;
  w.ex_from = from;
  w.ex_to = to;
  return std::nullopt;
}

void expirer_actions(const World& w0, std::vector<Action>& acts)
{
  auto dead = w0.dead();
  if (dead.empty()) {
    return;
  }
  if (w0.ex != EX_IDLE && w0.ex_crashes > 0) {
    acts.push_back({"expirer crash",
	[](World& w) -> std::optional<std::string> {
	  ++w.ex_inc;
	  --w.ex_crashes;
	  w.ex = EX_IDLE;
	  return std::nullopt;
	}, true});
  }
  if (w0.ex == EX_FF_DECIDED) {
    acts.push_back({fmt::format("expirer fenced-finalize {}", sname(w0.ex_from)),
	[](World& w) -> std::optional<std::string> {
	  return start_finalize(w, w.ex_from, w.dead());
	}});
    return;
  }
  if (w0.ex == EX_FF_RECLONE) {
    acts.push_back({fmt::format("expirer reclone {}", sname(w0.ex_to)),
	[](World& w) -> std::optional<std::string> {
	  auto from = w.ex_from;
	  if (auto v = check_manifest(w, from, "fenced finalize")) {
	    return v;
	  }
	  auto fs = sname(from), ts = sname(w.ex_to);
	  if (!w.storage.count(fs)) {
	    return fmt::format("fenced finalize:  {} already collected", fs);
	  }
	  if (w.published.count(fs)) {
	    return fmt::format("fenced finalize of {}, already published", fs);
	  }
	  w.fenced.insert(fs);
	  w.storage.insert(ts);
	  w.acked[ts] = w.acked[fs];
	  w.base[ts] = w.base[fs];
	  w.pms[ts] = w.pms[fs];
	  w.recloned[ts] = fs;
	  w.ex = EX_FF_PUBLISH;
	  return std::nullopt;
	}});
    return;
  }
  if (w0.ex == EX_FF_PUBLISH) {
    acts.push_back({fmt::format("expirer publish {}", sname(w0.ex_to)),
	[](World& w) -> std::optional<std::string> {
	  fsio::replace_in in;
	  in.mode = fsio::replace_mode::PUBLISH;
	  in.r = fid_t{w.next_r++, 0};
	  in.caller = xh(w.ex_inc);
	  in.shadow = w.ex_to;
	  fsio::replace_out out;
	  cls_rgw_fsio_effects fx;
	  auto save = w.head;
	  if (auto v = check_manifest(w, w.ex_to, "fenced finalize publish")) {
	    return v;
	  }
	  auto old = w.head_tok();
	  if (int r = m_replace_head(w.head, in, out, fx); r < 0) {
	    w.head = save;
	    return fmt::format("fenced finalize cannot publish {}:  {}",
			       sname(w.ex_to), r);
	  }
	  apply_clones(w, fx, old);
	  auto s = sname(w.ex_to);
	  w.head.exists = true;
	  w.head.manifest = tok_bl(s);
	  w.head.id_tag = fmt::format("t{}", w.next_tag++);
	  if (cfg.versioned) {
	    set_null_version(w, s);          // suspended:  the null version (§2.5a)
	  }
	  if (out.born_drained) {
	    auto rn = rname(in.r);
	    std::vector<std::string> v{rn};
	    if (!w.records[rn].empty()) {
	      v.push_back(w.records[rn]);
	    }
	    w.gcq.push_back(v);
	  }
	  w.ex = EX_IDLE;
	  if (++w.published[s] > 1) {
	    return fmt::format("{} published twice", s);
	  }
	  return std::nullopt;
	}});
    return;
  }
  /* grace (§4.3):  rgw_fsio_op_timeout after presumption;  after it a
   * presumed-dead incarnation can do nothing more */
  for (auto inc : dead) {
    if (w0.graced.count(inc)) continue;
    acts.push_back({fmt::format("expirer grace {}", inc),
	[inc](World& w) -> std::optional<std::string> {
	  w.graced.insert(inc);
	  return std::nullopt;
	}});
  }
  /* the pass (§4.3):  only a dead incarnation's hint leads the expirer to
   * the head;  everything below is the pass's work */
  if (!head_reached(w0, dead)) {
    return;
  }
  MarkPass mark{acts, acts.size()};
  /* reader expiry, and the drains it causes */
  bool has_dead_opens = false;
  for (auto inc : dead) {
    auto i = w0.head.opens.find(inc);
    if (i != w0.head.opens.end()) {
      for (auto& [cs, c] : inc_chunk_sets(i->second)) {
	if (c.readers) has_dead_opens = true;
      }
    }
  }
  bool has_drained = false;
  if (w0.head.hdr) {
    for (auto& e : w0.head.hdr->chunk_sets) {
      if (e.drained) has_drained = true;
    }
  }
  if (has_dead_opens || has_drained) {
    acts.push_back({"expirer expire",
	[dead](World& w) -> std::optional<std::string> {
	  fsio::expire_out out;
	  cls_rgw_fsio_effects fx;
	  m_expire(w.head, xh(w.ex_inc), dead, std::nullopt, out, fx);
	  std::vector<fid_t> all = out.drained;
	  if (w.head.hdr) {
	    for (auto& e : w.head.hdr->chunk_sets) {
	      if (e.drained && std::find(all.begin(), all.end(), e.id) ==
		  all.end()) {
		all.push_back(e.id);
	      }
	    }
	  }
	  for (auto& id : all) {
	    const auto* e = w.head.hdr ? w.head.hdr->find(id) : nullptr;
	    if (!e) continue;
	    if (e->manifest_at == at::SHADOW) {
	      if (auto v = check_manifest(w, id, "expirer drain")) {
		return v;
	      }
	    }
	    w.gcq.push_back(drain_objects(w, *e));
	    cls_rgw_fsio_effects f2;
	    m_forget(w.head, id, f2);
	    fx.remove_head |= f2.remove_head;
	  }
	  apply_remove_head(w, fx);
	  return std::nullopt;
	}});
  }
  /* dead incarnations' publish hints on the head (§4.3 step 1) */
  for (const auto& h : w0.hints) {
    if (h.kind != HintKind::PUBLISH || h.target != HintTarget::HEAD ||
	!dead.count(h.incarnation)) {
      continue;
    }
    {
      auto r = *h.r;
      acts.push_back({fmt::format("expirer hint {}", rname(r)),
	  [h, r, dead](World& w) -> std::optional<std::string> {
	    /* §4.3 step 1:  fsio_expire first, taking over the dead
	     * incarnation's publishes in flight, then the head, then the
	     * hint */
	    {
	      fsio::expire_out out;
	      cls_rgw_fsio_effects fx;
	      m_expire(w.head, xh(w.ex_inc), dead, std::nullopt, out, fx);
	      apply_remove_head(w, fx);
	    }
	    w.hints.erase(h);
	    if (w.head.hdr) {
	      for (auto& e : w.head.hdr->chunk_sets) {
		if (e.clone && *e.clone == r) {
		  return std::nullopt;
		}
	      }
	    }
	    auto rn = rname(r);
	    if (w.storage.count(rn)) {
	      std::vector<std::string> v{rn};
	      if (!w.records[rn].empty()) v.push_back(w.records[rn]);
	      w.gcq.push_back(v);
	    }
	    return std::nullopt;
	  }});
    }
  }
  if (!w0.head.hdr) {
    return;
  }
  for (const auto& e : w0.head.hdr->chunk_sets) {
    auto b = w0.head.bound(e.id);
    bool live_writer = false, in_grace = false;
    for (auto& [inc, o] : w0.head.opens) {
      auto ocs = inc_chunk_sets(o);
      auto c = ocs.find(e.id);
      if (c != ocs.end() && c->second.writers && !dead.count(inc)) {
	live_writer = true;
      }
      if (c != ocs.end() && c->second.writers && dead.count(inc) &&
	  !w0.graced.count(inc)) {
	in_grace = true;
      }
    }
    /* abandoned construction (§3.8):  once the constructor's grace has
     * elapsed, so no resent join of its can recreate the shadow (Z22) */
    if (e.state == state::CONSTRUCTING && !live_writer &&
	(!in_grace || FSIO_SKIP_CHECK(Z22_ABANDON_WAITS_FOR_GRACE))) {
      auto id = e.id;
      acts.push_back({fmt::format("expirer abandon {}", sname(id)),
	  [id, dead](World& w) -> std::optional<std::string> {
	    w.gcq.push_back({sname(id)});
	    fsio::expire_out out;
	    cls_rgw_fsio_effects fx;
	    m_expire(w.head, xh(w.ex_inc), dead, id, out, fx);
	    apply_remove_head(w, fx);
	    return std::nullopt;
	  }});
    }
    /* fenced finalize (§3.7) */
    bool ff = (e.state == state::READY && b.writers && !live_writer) ||
      (e.state == state::PUBLISHING && e.publishing &&
       (dead.count(e.publishing->publisher.incarnation) ||
	e.publishing->publisher.incarnation == w0.ex_inc));
    if (ff && !w0.head.hdr->fenced_finalizes.size() && !cfg.versioned) {
      auto from = e.id;
      acts.push_back({fmt::format("expirer decide-finalize {}", sname(from)),
	  [from](World& w) -> std::optional<std::string> {
	    w.ex = EX_FF_DECIDED;
	    w.ex_from = from;
	    return std::nullopt;
	  }});
    }
  }
  /* a recorded finalize whose finalizer died:  resumed by takeover (§3.7);
   * versioned buckets resume in expirer_actions_v */
  for (const auto& ff : w0.head.hdr->fenced_finalizes) {
    if (cfg.versioned) {
      break;
    }
    auto from = ff.from;
    acts.push_back({fmt::format("expirer resume-finalize {}", sname(from)),
	[from, dead](World& w) -> std::optional<std::string> {
	  return start_finalize(w, from, dead);
	}});
  }
}

/* a versioned bucket's S3:  a PUT makes a new version, a DELETE a delete
 * marker, DELETE ?versionId= removes one (§2.5) */
/* §2.5a:  the null version removed from the head object (DELETE
 * ?versionId=null, or a suspended bucket's DELETE):  the remove-null mode */
void remove_null(World& w)
{
  auto vi = w.versions.find("null");
  if (vi == w.versions.end()) {
    return;
  }
  auto old = w.head_tok();
  fsio::replace_in in;
  in.mode = fsio::replace_mode::REMOVE_NULL;
  in.r = fid_t{w.next_r++, 0};
  fsio::replace_out out;
  cls_rgw_fsio_effects fx;
  m_replace_head(w.head, in, out, fx);
  apply_clones(w, fx, old);
  if (!out.keep_tail) {
    w.gcq.push_back({vi->second.tail});
  }
  w.head.id_tag.clear();                 // the OLH stays, without content
  w.head.manifest.clear();
  w.versions.erase(vi);
  if (w.olh_cur == "null") {
    relink(w);
  }
}

/* a versioned bucket's S3:  enabled, a PUT makes a new version and a
 * DELETE a delete marker;  suspended, both act on the null version, in
 * place (§2.5a);  DELETE ?versionId= removes one;  versioning may be
 * suspended and enabled again */
void s3_actions_v(const World& w0, std::vector<Action>& acts)
{
  if (w0.s3_budget <= 0) {
    return;
  }
  if (!w0.suspended) {
    acts.push_back({"s3 put",
	[](World& w) -> std::optional<std::string> {
	  --w.s3_budget;
	  auto inst = fmt::format("I{}", w.next_i++);
	  auto c = fmt::format("C{}", w.next_c++);
	  w.storage.insert(c);
	  Version v;
	  v.ih.exists = true;
	  v.ih.id_tag = fmt::format("i{}", w.next_tag++);
	  v.ih.manifest = tok_bl(c);
	  v.tail = c;
	  v.epoch = w.next_epoch++;
	  w.versions[inst] = v;
	  w.olh_cur = inst;
	  w.olh_epoch = v.epoch;
	  return std::nullopt;
	}});
  } else {
    acts.push_back({"s3 put (suspended)",
	[](World& w) -> std::optional<std::string> {
	  /* overwrites the null version on the head:  a supersede */
	  --w.s3_budget;
	  auto c = fmt::format("C{}", w.next_c++);
	  w.storage.insert(c);
	  auto old = w.versions.count("null") ? w.head_tok() : std::string{};
	  fsio::replace_in in;
	  in.mode = fsio::replace_mode::SUPERSEDE;
	  in.r = fid_t{w.next_r++, 0};
	  fsio::replace_out out;
	  cls_rgw_fsio_effects fx;
	  m_replace_head(w.head, in, out, fx);
	  apply_clones(w, fx, old);
	  if (!out.keep_tail && !old.empty()) {
	    w.gcq.push_back({old});
	  }
	  w.head.exists = true;
	  w.head.manifest = tok_bl(c);
	  w.head.id_tag = fmt::format("t{}", w.next_tag++);
	  set_null_version(w, c);
	  return std::nullopt;
	}});
  }
  acts.push_back({w0.suspended ? "s3 delete (suspended)" : "s3 delete",
      [](World& w) -> std::optional<std::string> {
	--w.s3_budget;
	if (w.suspended) {
	  remove_null(w);                    // the null version goes too
	}
	w.olh_cur.clear();                   // a delete marker
	w.olh_epoch = w.next_epoch++;
	return std::nullopt;
      }});
  acts.push_back({w0.suspended ? "s3 enable-versioning" : "s3 suspend-versioning",
      [](World& w) -> std::optional<std::string> {
	--w.s3_budget;
	w.suspended = !w.suspended;
	return std::nullopt;
      }});
  for (const auto& [i, v] : w0.versions) {
    if (!v.ih.exists || v.ih.id_tag.empty()) {
      continue;
    }
    auto inst = i;
    acts.push_back({fmt::format("s3 delete-version {}", inst),
	[inst](World& w) -> std::optional<std::string> {
	  --w.s3_budget;
	  if (inst == "null") {
	    remove_null(w);
	    return std::nullopt;
	  }
	  auto& v = w.versions.at(inst);
	  fsio::remove_out out;
	  cls_rgw_fsio_effects fx;
	  fid_t r{w.next_r++, 0};
	  m_remove_version(inst, v.ih, r, out, fx);
	  if (out.keep_tail) {
	    w.storage.insert(fmt::format("B{}", r.counter));   // the sibling
	  } else {
	    w.gcq.push_back({v.tail});
	  }
	  if (!v.ih.exists) {
	    w.versions.erase(inst);
	  }
	  if (w.olh_cur == inst) {
	    relink(w);
	  }
	  return std::nullopt;
	}});
  }
  acts.push_back({"s3 stop",
      [](World& w) -> std::optional<std::string> {
	w.s3_budget = 0;
	return std::nullopt;
      }});
}

/* versioned fenced finalize, after the call:  adopt `to`'s recorded claim */
void adopt_to(World& w, const fid_t& to)
{
  const auto* t = w.head.hdr->find(to);
  w.ex_to = to;
  w.ex_inst = t->publishing->instance;
  w.ex_token = t->publishing->token;
  w.ex = VF_RECLONE;
}

/* the expirer in a versioned bucket:  the OLH as unversioned (reader
 * expiry, abandoned constructions), each version's open count (§2.5),
 * a dead publisher resolved by its claim (§4.2), the versioned fenced
 * finalize run as §4.2's sequence for `to` (§3.7), and §4.3's rule for
 * a dead gateway's publish hints, applied to instance-head hints as
 * written (W9) */
void expirer_actions_v(const World& w0, std::vector<Action>& acts)
{
  auto dead = w0.dead();
  if (dead.empty()) {
    return;
  }
  if (w0.ex >= VF_DECIDED) {
    if (w0.ex_crashes > 0) {
      acts.push_back({"expirer crash",
	  [](World& w) -> std::optional<std::string> {
	    ++w.ex_inc;
	    --w.ex_crashes;
	    w.ex = EX_IDLE;
	    return std::nullopt;
	  }, true});
    }
    switch (w0.ex) {
    case VF_DECIDED:
      /* §3.7 step 1, versioned:  a publish hint on the instance head under
       * the finalizer's own incarnation, then the claim (V3) */
      acts.push_back({fmt::format("expirer publish-hint for {}",
				  sname(w0.ex_from)),
	  [](World& w) -> std::optional<std::string> {
	    w.ex_inst = fmt::format("I{}", w.next_i++);
	    w.ex_token = fmt::format("k{}", w.next_tag++);
	    if (!FSIO_SKIP_CHECK(V3_FINALIZER_HINTS_PLACEHOLDER)) {
	      w.hints.insert(publish_ih(w.ex_inst, xh(w.ex_inc)));
	    }
	    w.ex = VF_PLACE;
	    return std::nullopt;
	  }});
      break;
    case VF_PLACE:
      acts.push_back({fmt::format("expirer placeholder {}", w0.ex_inst),
	  [](World& w) -> std::optional<std::string> {
	    Version v;
	    v.ih.exists = true;
	    v.claim = w.ex_token;
	    w.versions.emplace(w.ex_inst, v);
	    w.ex = VF_CALL;
	    return std::nullopt;
	  }});
      break;
    case VF_CALL:
      acts.push_back({fmt::format("expirer fenced-finalize {}",
				  sname(w0.ex_from)),
	  [](World& w) -> std::optional<std::string> {
	    fid_t to;
	    fsio::publish_intent intent{w.ex_inst, w.ex_token, w.next_epoch};
	    if (m_fenced_finalize(w.head, xh(w.ex_inc), w.ex_from, w.dead(), to,
				      &intent, w.ex_aborted) < 0) {
	      auto vi = w.versions.find(w.ex_inst);
	      if (vi != w.versions.end() && vi->second.claim == w.ex_token) {
		w.versions.erase(vi);        // its own placeholder
	      }
	      w.hints.erase(publish_ih(w.ex_inst, xh(w.ex_inc)));
	      w.ex = EX_IDLE;
	      return std::nullopt;
	    }
	    ++w.next_epoch;
	    adopt_to(w, to);
	    return std::nullopt;
	  }});
      break;
    case VF_RECLONE:
      acts.push_back({fmt::format("expirer reclone {}", sname(w0.ex_to)),
	  [](World& w) -> std::optional<std::string> {
	    auto from = w.head.hdr ? [&]() -> std::optional<fid_t> {
	      for (const auto& ff : w.head.hdr->fenced_finalizes) {
		if (ff.to == w.ex_to) return ff.from;
	      }
	      return std::nullopt;
	    }() : std::nullopt;
	    if (!from) {
	      w.ex = EX_IDLE;
	      return std::nullopt;
	    }
	    w.ex_from = *from;
	    if (auto v = check_manifest(w, *from, "fenced finalize")) {
	      return v;
	    }
	    auto fs = sname(*from), ts = sname(w.ex_to);
	    if (!w.storage.count(fs)) {
	      return fmt::format("fenced finalize:  {} already collected", fs);
	    }
	    if (w.published.count(fs)) {
	      return fmt::format("fenced finalize of {}, already published", fs);
	    }
	    w.fenced.insert(fs);
	    w.storage.insert(ts);
	    w.acked[ts] = w.acked[fs];
	    w.base[ts] = w.base[fs];
	    w.pms[ts] = w.pms[fs];
	    w.recloned[ts] = fs;
	    w.ex = VF_WRITE;
	    return std::nullopt;
	  }});
      break;
    case VF_WRITE:
      acts.push_back({fmt::format("expirer instance-write {}", w0.ex_inst),
	  [](World& w) -> std::optional<std::string> {
	    auto vi = w.versions.find(w.ex_inst);
	    if (vi == w.versions.end() || vi->second.claim != w.ex_token) {
	      w.ex = EX_IDLE;
	      return std::nullopt;
	    }
	    const auto* t = w.head.hdr ? w.head.hdr->find(w.ex_to) : nullptr;
	    auto ts = sname(w.ex_to);
	    Version v;
	    v.ih.exists = true;
	    v.ih.id_tag = fmt::format("i{}", w.next_tag++);
	    v.ih.manifest = tok_bl(ts);
	    v.tail = ts;
	    v.epoch = t && t->publishing ? t->publishing->olh_epoch : 0;
	    vi->second = v;
	    if (FSIO_SKIP_CHECK(Z6_INSTANCE_WRITE_DROPS_CLAIM)) {
	      vi->second.claim = w.ex_token;     // written back (§3, Z6)
	    }
	    if (++w.published[ts] > 1) {
	      return fmt::format("{} linked as a version twice", ts);
	    }
	    w.ex = VF_LINK;
	    return std::nullopt;
	  }});
      break;
    case VF_LINK:
      acts.push_back({fmt::format("expirer link {}", w0.ex_inst),
	  [](World& w) -> std::optional<std::string> {
	    /* set_olh() of a version removed since its write fails;  the
	     * publish goes on */
	    auto vi = w.versions.find(w.ex_inst);
	    if (vi != w.versions.end() && !vi->second.ih.id_tag.empty() &&
		vi->second.epoch > w.olh_epoch) {
	      w.olh_cur = w.ex_inst;
	      w.olh_epoch = vi->second.epoch;
	    }
	    w.ex = VF_PUBLISH;
	    return std::nullopt;
	  }});
      break;
    case VF_PUBLISH:
      acts.push_back({fmt::format("expirer publish {}", sname(w0.ex_to)),
	  [](World& w) -> std::optional<std::string> {
	    fsio::replace_in in;
	    in.mode = fsio::replace_mode::PUBLISH;
	    in.versioned = true;
	    in.caller = xh(w.ex_inc);
	    in.shadow = w.ex_to;
	    fsio::replace_out out;
	    cls_rgw_fsio_effects fx;
	    auto save = w.head;
	    if (int r = m_replace_head(w.head, in, out, fx); r < 0) {
	      w.head = save;
	      return fmt::format("fenced finalize cannot publish {}:  {}",
				 sname(w.ex_to), r);
	    }
	    apply_remove_head(w, fx);
	    w.hints.erase(publish_ih(w.ex_inst, xh(w.ex_inc)));   // its own (V3)
	    w.ex = EX_IDLE;
	    return std::nullopt;
	  }});
      break;
    default:
      break;
    }
    return;
  }
  expirer_actions(w0, acts);
  if (w0.ex != EX_IDLE) {
    return;
  }
  /* a dead incarnation's presence hints on instance heads (§4.3):
   * ocnt_expire, the drain's send, and the hint dropped once the open
   * count no longer holds the incarnation */
  for (const auto& h : w0.hints) {
    if (h.kind != HintKind::PRESENCE ||
	h.target != HintTarget::INSTANCE_HEAD || !dead.count(h.incarnation)) {
      continue;
    }
    acts.push_back({fmt::format("expirer ocnt {} for {}", h.instance,
				h.incarnation),
	[h, dead](World& w) -> std::optional<std::string> {
	  auto vi = w.versions.find(h.instance);
	  if (vi != w.versions.end() && vi->second.ih.ocnt) {
	    fsio::ocnt_drain d;
	    m_ocnt_expire(vi->first, vi->second.ih, dead, d);
	    if (vi->second.ih.ocnt && vi->second.ih.ocnt->holders.empty()) {
	      ocnt_send(w, h.instance);
	    }
	  }
	  vi = w.versions.find(h.instance);
	  const bool live_claim = vi != w.versions.end() &&
	    vi->second.placeholder() && *vi->second.claim != "aborted" &&
	    !FSIO_SKIP_CHECK(H3_HINT_WAITS_FOR_LIVE_CLAIM);
	  if (!live_claim &&            // a publish may still set it (H3b)
	      (vi == w.versions.end() || !vi->second.ih.ocnt ||
	       !ocnt_holds(*vi->second.ih.ocnt, h.incarnation)) &&
	      (w.graced.count(h.incarnation) ||
	       FSIO_SKIP_CHECK(H2_HINT_DROPPED_AFTER_GRACE))) {
	    w.hints.erase(h);              // after grace (H2)
	  }
	  return std::nullopt;
	}});
  }
  /* a dead incarnation's publish hints on instance heads (§4.3, W9):
   * keep while a chunk-set entry names the instance;  never collect a
   * written version;  delete a placeholder once its incarnation holds no
   * writer open.  `drop` removes the hint from wherever it is kept:  a
   * gateway's, or a finalizer's own (V3) */
  auto hint_rule = [dead](World& w, const std::string& inst, uint64_t owner,
			  const std::function<void(World&)>& drop) {
    {
      fsio::expire_out out;
      cls_rgw_fsio_effects fx;
      m_expire(w.head, xh(w.ex_inc), dead, std::nullopt, out, fx);
      apply_remove_head(w, fx);
    }
    if (w.head.hdr) {
      for (const auto& e : w.head.hdr->chunk_sets) {
	if ((e.instance && *e.instance == inst) ||
	    (e.publishing && e.publishing->instance == inst)) {
	  return;                          // 1. the claim resolution's
	}
      }
    }
    const bool graced = w.graced.count(owner) ||
      FSIO_SKIP_CHECK(V4_HINT_WAITS_FOR_GRACE);
    /* 4., 5.:  the presence hints the publisher wrote for its readers
     * (§4.2 step 3) cover nothing once the claim is aborted (H3d, H3e) */
    auto drop_presence = [&inst](World& w) {
      if (FSIO_SKIP_CHECK(H3_PUBLISH_HINT_REMOVES_READER_HINTS)) {
	return;
      }
      std::erase_if(w.hints, [&inst](const Hint& p) {
	return p.kind == HintKind::PRESENCE &&
	  p.target == HintTarget::INSTANCE_HEAD && p.instance == inst;
      });
    };
    auto vi = w.versions.find(inst);
    if (vi == w.versions.end()) {
      if (graced) {
	drop_presence(w);
	drop(w);                           // 5., after grace (V4)
      }
      return;
    }
    if (!vi->second.claim || !decided_by_claim(vi->second)) {
      /* 2. written by the publish:  its ID tag (Z6), or no claim, live
       * or since removed (V5:  a removal clears the ID tag) */
      if (FSIO_SKIP_CHECK(W9_HINT_SPARES_WRITTEN_VERSION)) {
	w.gcq.push_back({vi->second.tail});   // as first written
      }
      drop(w);
      return;
    }
    if (vi->second.claim && *vi->second.claim != "aborted") {
      bool writer = false;
      auto o = w.head.opens.find(owner);
      if (o != w.head.opens.end()) {
	for (const auto& [cs, c] : inc_chunk_sets(o->second)) {
	  writer |= c.writers > 0;
	}
      }
      if (writer && !FSIO_SKIP_CHECK(W9_HINT_WAITS_FOR_WRITER)) {
	return;                            // 3.  a zombie could still leave
      }
    } else if (!graced) {
      return;                              // 4. waits for grace (V4)
    } else {
      drop_presence(w);                    // 4.
    }
    w.versions.erase(vi);                  // 3., 4.
    drop(w);
  };
  for (const auto& h : w0.hints) {
    if (h.kind != HintKind::PUBLISH ||
	h.target != HintTarget::INSTANCE_HEAD || !dead.count(h.incarnation)) {
      continue;
    }
    acts.push_back({fmt::format("expirer hint {} for {}", h.instance,
				h.incarnation),
	[h, hint_rule](World& w) -> std::optional<std::string> {
	  hint_rule(w, h.instance, h.incarnation,
		    [h](World& w) { w.hints.erase(h); });
	  return std::nullopt;
	}});
  }
  /* the pass's versioned work on the OLH head, reached by a hint (§4.3) */
  if (!head_reached(w0, dead)) {
    return;
  }
  MarkPass mark{acts, acts.size()};
  if (!w0.head.hdr) {
    return;
  }
  /* a recorded finalize whose finalizer died:  resumed by takeover */
  for (const auto& ff : w0.head.hdr->fenced_finalizes) {
    auto from = ff.from;
    const auto* t = w0.head.hdr->find(ff.to);
    const bool versioned = t && t->publishing &&
      !t->publishing->instance.empty();
    /* §3.7 step 4:  `to` never written:  its placeholder's claim aborted,
     * then a fresh claim through the takeover with an intent
     * (VF_DECIDED to VF_CALL);  written, the resume below completes it */
    if (versioned) {
      const auto inst = t->publishing->instance;
      const auto token = t->publishing->token;
      auto vi = w0.versions.find(inst);
      if (vi != w0.versions.end() && decided_by_claim(vi->second)) {
	if (*vi->second.claim == token) {
	  acts.push_back({fmt::format("expirer abort {}", inst),
	      [inst](World& w) -> std::optional<std::string> {
		w.versions.at(inst).claim = "aborted";
		return std::nullopt;
	      }});
	} else {
	  acts.push_back({fmt::format("expirer decide-finalize {}", sname(from)),
	      [from, inst](World& w) -> std::optional<std::string> {
		w.ex = VF_DECIDED;
		w.ex_from = from;
		w.ex_aborted = inst;       // `to`'s claim, aborted
		return std::nullopt;
	      }});
	}
	continue;
      }
    }
    acts.push_back({fmt::format("expirer resume-finalize {}", sname(from)),
	[from, dead, versioned](World& w) -> std::optional<std::string> {
	  if (!versioned) {
	    return start_finalize(w, from, dead);   // a suspended bucket's
	  }
	  fid_t to;
	  if (m_fenced_finalize(w.head, xh(w.ex_inc), from, dead, to) < 0) {
	    return std::nullopt;
	  }
	  w.ex_from = from;
	  adopt_to(w, to);
	  /* §3.7 step 4:  `to`'s claim resolved as §4.2 does before the
	   * finalize goes on:  written, it completes through set_olh();
	   * written then removed, it publishes */
	  auto vi = w.versions.find(w.ex_inst);
	  if (FSIO_SKIP_CHECK(RESUME_RESOLVES_CLAIM)) {
	    return std::nullopt;
	  }
	  if (vi != w.versions.end() && !vi->second.ih.id_tag.empty() &&
	      !decided_by_claim(vi->second)) {
	    w.ex = VF_LINK;                  // the ID tag first (Z6)
	  } else if (vi == w.versions.end() ||
		     (!vi->second.claim && vi->second.ih.id_tag.empty())) {
	    w.ex = VF_PUBLISH;
	  }
	  return std::nullopt;
	}});
  }
  if (!w0.head.hdr->fenced_finalizes.empty()) {
    return;
  }
  /* a dead publisher, resolved by its claim first (§4.2);  a ready shadow
   * whose writers are all dead, finalized */
  for (const auto& e : w0.head.hdr->chunk_sets) {
    const auto b = w0.head.bound(e.id);
    bool live_writer = false;
    for (const auto& [inc, o] : w0.head.opens) {
      auto ocs = inc_chunk_sets(o);
      auto c = ocs.find(e.id);
      if (c != ocs.end() && c->second.writers && !dead.count(inc)) {
	live_writer = true;
      }
    }
    auto from = e.id;
    /* a suspended bucket's shadow publishes over the null version:  the
     * unversioned fenced finalize (§2.5a) */
    auto decide_unversioned = [&acts, from]() {
      acts.push_back({fmt::format("expirer decide-finalize {}", sname(from)),
	  [from](World& w) -> std::optional<std::string> {
	    w.ex = EX_FF_DECIDED;
	    w.ex_from = from;
	    return std::nullopt;
	  }});
    };
    if (e.state == state::READY && b.writers && !live_writer) {
      if (w0.suspended) {
	decide_unversioned();
	continue;
      }
      acts.push_back({fmt::format("expirer decide-finalize {}", sname(from)),
	  [from](World& w) -> std::optional<std::string> {
	    w.ex = VF_DECIDED;
	    w.ex_from = from;
	    w.ex_aborted.clear();            // no claim:  ready
	    return std::nullopt;
	  }});
      continue;
    }
    if (e.state != state::PUBLISHING || !e.publishing ||
	!(dead.count(e.publishing->publisher.incarnation) ||
	  e.publishing->publisher.incarnation == w0.ex_inc)) {
      continue;
    }
    if (e.publishing->instance.empty()) {
      decide_unversioned();                // an unversioned publish's
      continue;
    }
    const auto inst = e.publishing->instance;
    const auto token = e.publishing->token;
    auto vi = w0.versions.find(inst);
    auto complete = [from, dead](World& w, bool link)
      -> std::optional<std::string> {
      const auto* e = w.head.hdr->find(from);
      if (link && e->publishing->olh_epoch > w.olh_epoch) {
	w.olh_cur = e->publishing->instance;
	w.olh_epoch = e->publishing->olh_epoch;
      }
      fsio::replace_in in;
      in.mode = fsio::replace_mode::PUBLISH;
      in.versioned = true;
      in.complete = true;
      in.caller = xh(w.ex_inc);
      in.shadow = from;
      in.expired = dead;
      in.expired.insert(w.ex_inc);           // a publish it took over
      fsio::replace_out out;
      cls_rgw_fsio_effects fx;
      auto save = w.head;
      if (m_replace_head(w.head, in, out, fx) < 0) {
	w.head = save;
      }
      apply_remove_head(w, fx);
      return std::nullopt;
    };
    /* the ID tag first (Z6):  written, whatever its claim */
    if (vi != w0.versions.end() && !vi->second.ih.id_tag.empty() &&
	!decided_by_claim(vi->second)) {
      acts.push_back({fmt::format("expirer complete {}", inst),
	  [complete](World& w) { return complete(w, true); }});
    } else if (vi == w0.versions.end() ||
	(!vi->second.claim && vi->second.ih.id_tag.empty() &&
	 !FSIO_SKIP_CHECK(H3_TRACKING_ONLY_IS_REMOVED))) {
      /* written, then removed (§4.2):  absent, or tracking-only (H3) */
      acts.push_back({fmt::format("expirer complete-removed {}", sname(from)),
	  [complete](World& w) { return complete(w, false); }});
    } else if (decided_by_claim(vi->second) && *vi->second.claim == token) {
      acts.push_back({fmt::format("expirer abort {}", inst),
	  [inst](World& w) -> std::optional<std::string> {
	    w.versions.at(inst).claim = "aborted";
	    return std::nullopt;
	  }});
    } else if (decided_by_claim(vi->second)) {
      acts.push_back({fmt::format("expirer decide-finalize {}", sname(from)),
	  [from, inst](World& w) -> std::optional<std::string> {
	    w.ex = VF_DECIDED;
	    w.ex_from = from;
	    w.ex_aborted = inst;             // the claim it aborted (V1)
	    return std::nullopt;
	  }});
    }
  }
}

/* §3, Z22:  a handle resends its latest counted call, with the same
 * sequence number, once the original has applied;  the reply is the one
 * the handle never saw, so it changes nothing on the handle's side */
void resend_actions(const World& w0, size_t gi, std::vector<Action>& acts)
{
  const auto& g = w0.gws[gi];
  if (!g.resend || g.resent >= cfg.resends || !g.alive ||
      (g.zombie && w0.graced.count(g.inc))) {
    return;
  }
  static const char* names[] = {"join", "leave", "rebind"};
  acts.push_back({fmt::format("gw{} resend {}", glabel(g),
			      names[(int)g.resend->kind]),
      [gi](World& w) -> std::optional<std::string> {
	auto& g = w.gws[gi];
	const auto r = *g.resend;
	g.resend.reset();
	++g.resent;
	cls_rgw_fsio_effects fx;
	auto save = w.head;
	int ret = 0;
	switch (r.kind) {
	case Counted::JOIN: {
	  fsio::join_in in{hd(g), r.seq, r.writer, r.nonce, 10, r.expired,
			   r.instance};
	  fsio::join_out out;
	  const auto tok = w.head_tok();
	  ret = m_join(w.head, in, out, fx);
	  if (ret == 0) {
	    apply_clones(w, fx, tok);      // its effects run in the op
	  }
	  break;
	}
	case Counted::LEAVE: {
	  fsio::leave_out out;
	  ret = m_leave(w.head, hd(g), r.seq, r.cs, r.writer, r.expired,
			    out, fx, r.intent ? &*r.intent : nullptr);
	  if (ret == 0) {
	    apply_remove_head(w, fx);
	  }
	  break;
	}
	case Counted::REBIND:
	  ret = m_rebind(w.head, hd(g), r.seq, r.cs, r.to, 1);
	  break;
	}
	if (ret < 0) {
	  w.head = save;
	}
	return std::nullopt;
      }});
}

/* §4.3:  a gateway's presence hint on the head, added before the call
 * that creates its first state there and removed after the call that
 * releases its last;  folded into those actions.  With
 * PRESENCE_HINT_BEFORE_JOIN skipped, it is added in a step of its own
 * after the call */
void presence_hints(const World& w0, size_t gi, std::vector<Action>& acts,
		    size_t first)
{
  for (auto i = first; i < acts.size(); ++i) {
    if (acts[i].failure) {
      continue;
    }
    auto inner = std::move(acts[i].run);
    acts[i].run = [gi, inner](World& w) -> std::optional<std::string> {
      const auto h = presence_head(hd(w.gws[gi]));
      const bool before = holds_head(w.gws[gi]);
      if (!before && !FSIO_SKIP_CHECK(PRESENCE_HINT_BEFORE_JOIN)) {
	w.hints.insert(h);
      }
      auto v = inner(w);
      auto& g = w.gws[gi];
      if (!holds_head(g) &&
	  !(g.release_failed && !FSIO_SKIP_CHECK(H1_FAILED_RELEASE_KEEPS_HINT))) {
	w.hints.erase(h);
	g.owe_head_hint = 0;
      } else if (!before && FSIO_SKIP_CHECK(PRESENCE_HINT_BEFORE_JOIN)) {
	g.owe_head_hint = 1;
      }
      return v;
    };
  }
  const auto& g = w0.gws[gi];
  if (!g.alive || (g.zombie && w0.graced.count(g.inc))) {
    return;
  }
  if (g.owe_head_hint) {
    acts.push_back({fmt::format("gw{} presence-hint", glabel(g)),
	[gi](World& w) -> std::optional<std::string> {
	  auto& g = w.gws[gi];
	  w.hints.insert(presence_head(hd(g)));
	  g.owe_head_hint = 0;
	  return std::nullopt;
	}});
  }
  if (!g.owe_ih_hint.empty()) {
    acts.push_back({fmt::format("gw{} presence-hint {}", glabel(g), g.owe_ih_hint),
	[gi](World& w) -> std::optional<std::string> {
	  auto& g = w.gws[gi];
	  if (g.counted.count(g.owe_ih_hint)) {
	    w.hints.insert(presence_ih(g.owe_ih_hint, hd(g)));
	  }
	  g.owe_ih_hint.clear();
	  return std::nullopt;
	}});
  }
}

/* §4.3 step 6:  the pass done, a dead incarnation's presence hint on the
 * head is dropped:  no pass work is left, and the head holds no opens of
 * that incarnation */
void drop_presence_hints(const World& w0, std::vector<Action>& acts)
{
  if (w0.ex != EX_IDLE) {
    return;
  }
  for (const auto& a : acts) {
    if (a.pass) {
      return;
    }
  }
  const auto dead = w0.dead();
  for (const auto& h : w0.hints) {
    if (h.kind != HintKind::PRESENCE || h.target != HintTarget::HEAD ||
	!dead.count(h.incarnation)) {
      continue;
    }
    if (auto i = w0.head.opens.find(h.incarnation);
	i != w0.head.opens.end() && !i->second.handles.empty()) {
      continue;                  // its opens, not only its kept seqs
    }
    if (!w0.graced.count(h.incarnation) &&
	!FSIO_SKIP_CHECK(H2_HINT_DROPPED_AFTER_GRACE)) {
      continue;                  // a late call may still land (H2)
    }
    acts.push_back({fmt::format("expirer drop-hint {}", h.incarnation),
	[h](World& w) -> std::optional<std::string> {
	  /* §4.3 step 6:  the last_seq its expired handles kept goes
	   * first, then the hint (Z22) */
	  cls_rgw_fsio_effects fx;
	  m_drop_seqs(w.head, h.incarnation, fx);
	  apply_remove_head(w, fx);
	  w.hints.erase(h);
	  return std::nullopt;
	}});
  }
}

void gc_actions(const World& w0, std::vector<Action>& acts)
{
  for (size_t i = 0; i < w0.gcq.size(); ++i) {
    acts.push_back({fmt::format("gc {}", fmt::join(w0.gcq[i], ",")),
	[i](World& w) -> std::optional<std::string> {
	  for (auto& o : w.gcq[i]) {
	    w.storage.erase(o);
	  }
	  w.gcq.erase(w.gcq.begin() + i);
	  return std::nullopt;
	}});
  }
}

std::optional<std::string> check_state(const World& w)
{
  for (const auto& g : w.gws) {
    if (!g.alive || g.zombie) continue;   // a zombie's read fails:  a reopen
    for (const auto& o : g.opens) {
      if (!w.storage.count(o.storage)) {
	return fmt::format("gw{}'s open reads {}, collected", glabel(g),
			   o.storage);
      }
    }
  }
  if (w.head_real() && !w.storage.count(w.head_tok())) {
    return fmt::format("published content {} collected", w.head_tok());
  }
  for (const auto& [i, v] : w.versions) {
    if (v.ih.exists && !v.ih.id_tag.empty() && !w.storage.count(v.tail)) {
      return fmt::format("live version {}'s tail {} collected", i, v.tail);
    }
    if (v.ih.ocnt && !v.ih.ocnt->holders.empty()) {
      for (const auto& d : v.ih.ocnt->deferred) {
	if (!w.storage.count(tok(d.manifest))) {
	  return fmt::format("{}'s deferred tail {} collected while its open "
			     "count holds {}", i, tok(d.manifest),
			     fmt::join(ocnt_incs(*v.ih.ocnt), ","));
	}
      }
    }
  }
  if (w.head.hdr) {
    std::set<fid_t> ids;
    int at_head = 0;
    for (const auto& e : w.head.hdr->chunk_sets) {
      if (!ids.insert(e.id).second) {
	return fmt::format("two entries for {}", sname(e.id));
      }
      if (e.state == state::PUBLISHED && e.manifest_at == at::HEAD) {
	++at_head;
      }
      if (cfg.versioned && e.state == state::PUBLISHED) {
	for (const auto& f : w.head.hdr->chunk_sets) {
	  if (f.id != e.id && f.state == state::PUBLISHED &&
	      entry_tail(w, f) == entry_tail(w, e) &&
	      !entry_tail(w, e).empty()) {
	    return fmt::format("two chunk-set entries publish {}",
			       entry_tail(w, e));
	  }
	}
      }
      if (e.state == state::PUBLISHED && e.manifest_at == at::INSTANCE) {
	for (const auto& f : w.head.hdr->chunk_sets) {
	  if (f.id != e.id && f.state == state::PUBLISHED &&
	      f.manifest_at == at::INSTANCE && f.instance == e.instance) {
	    return fmt::format("two entries PUBLISHED at {}", *e.instance);
	  }
	}
      }
    }
    if (at_head > 1) {
      return "two entries PUBLISHED at HEAD";
    }
    if (const auto& c = w.head.hdr->cur_shadow) {
      const auto* e = w.head.hdr->find(*c);
      if (!e || !e->is_shadow()) {
	return fmt::format("cur_shadow {} names no shadow", sname(*c));
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string> check_end(const World& w)
{
  for (const auto& h : w.hints) {
    return fmt::format("hint left at the end:  {} on {} of {}",
		       h.kind == HintKind::PRESENCE ? "presence" : "publish",
		       h.target == HintTarget::HEAD ? "the head" : h.instance,
		       h.incarnation);
  }
  if (w.head.hdr || !w.head.opens.empty()) {
    ceph::buffer::list bl;
    return fmt::format("FSIO state left at the end ({} entries, {} "
		       "incarnation records)",
		       w.head.hdr ? w.head.hdr->chunk_sets.size() : 0,
		       w.head.opens.size());
  }
  std::set<std::string> left = w.storage;
  left.erase(w.head_tok());
  for (const auto& [i, v] : w.versions) {
    if (v.claim) {
      return fmt::format("placeholder {} left at the end (claim {})", i,
			 *v.claim);
    }
    if (v.ih.ocnt) {
      return fmt::format("{}'s open count left at the end:  incarnations "
			 "{{{}}}, {} deferred", i,
			 fmt::join(ocnt_incs(*v.ih.ocnt), ","),
			 v.ih.ocnt->deferred.size());
    }
    if (v.ih.exists && !v.ih.id_tag.empty()) {
      left.erase(v.tail);
    }
  }
  if (!left.empty()) {
    return fmt::format("leaked:  {}", fmt::join(left, ","));
  }
  for (const auto& [s, n] : w.acked) {
    if (!n) continue;
    bool done = w.published.count(s);
    for (const auto& [to, from] : w.recloned) {
      if (from == s && w.published.count(to)) done = true;
    }
    if (!done) {
      return fmt::format("{} acknowledged writes to {} never published", n,
			 s);
    }
  }
  return std::nullopt;
}

/* hash compaction:  a visited state is kept as the 128-bit hash of its
 * encoding, as Spin and TLC can;  two states colliding would merge, which
 * at these sizes (well under 2^40 states) has negligible probability */
struct StateHash {
  size_t operator()(const XXH128_hash_t& h) const { return h.low64; }
};
struct StateEq {
  bool operator()(const XXH128_hash_t& a, const XXH128_hash_t& b) const {
    return XXH128_isEqual(a, b);
  }
};

struct Explorer {
  std::unordered_set<XXH128_hash_t, StateHash, StateEq> seen;
  /* iterative deepening:  each visited state's hash, with the shallowest
   * depth it was reached at, so a state first reached deep is explored
   * again when reached shallower */
  std::unordered_map<XXH128_hash_t, uint16_t, StateHash, StateEq> seen_at;
  bool shortest = false;
  size_t bound = SIZE_MAX;       // the current pass's depth bound
  bool cut = false;              // a path stopped at the bound this pass
  unsigned passes = 0;
  std::map<std::string, std::vector<std::string>> found;   // first trace
  std::vector<std::string> path;
  uint64_t states = 0, ends = 0;
  uint64_t limit = 50'000'000;
  uint64_t progress = 100'000;
  size_t max_depth = 200;
  bool stop_at_first = false;
  /* FSIO_MODEL_PREFIX:  a vector, one action name per line as traces
   * print them;  step i takes only the action named on line i, then the
   * search goes on from where it leads (or stops, FSIO_MODEL_PREFIX_ONLY) */
  std::vector<std::string> prefix;
  bool prefix_only = false;
  bool prefix_missed = false;
  std::chrono::steady_clock::time_point start =
    std::chrono::steady_clock::now();

  void status() const {
    using namespace std::chrono;
    double secs = duration<double>(steady_clock::now() - start).count();
    std::cerr << fmt::format("[fsio-model] {:.0f}s  {} states ({:.0f}/s)  "
			     "{} ends  depth {}  {} violation kinds\n",
			     secs, states, secs > 0 ? states / secs : 0.0,
			     ends, path.size(), found.size())
	      << std::flush;
  }

  static std::string kind(const std::string& v) {
    auto p = v.find_first_of("0123456789");
    return p == std::string::npos ? v : v.substr(0, p);
  }
  void report(const std::string& v) {
    auto k = kind(v);
    if (!found.count(k)) {
      auto t = path;
      t.push_back("=> " + v);
      std::cerr << fmt::format("[fsio-model] new violation, after {} "
			       "states:  {}\n", states, v);
      for (const auto& step : t) {
	std::cerr << "    " << step << "\n";
      }
      std::cerr << std::flush;
      found[k] = t;
    }
  }

  static std::vector<Action> actions_of(const World& w) {
    std::vector<Action> acts;
    for (size_t gi = 0; gi < w.gws.size(); ++gi) {
      auto first = acts.size();
      if (cfg.versioned) {
	gw_actions_v(w, gi, acts);
      } else {
	gw_actions(w, gi, acts);
      }
      presence_hints(w, gi, acts, first);
      resend_actions(w, gi, acts);
    }
    if (cfg.versioned) {
      s3_actions_v(w, acts);
      expirer_actions_v(w, acts);
    } else {
      s3_actions(w, acts);
      expirer_actions(w, acts);
    }
    drop_presence_hints(w, acts);
    gc_actions(w, acts);
    return acts;
  }

  /* FSIO_MODEL_COMMUTE_SAMPLE=N:  at every Nth state, apply each pair of
   * available actions in both orders, and count the pairs that commute:
   * each still available after the other, no violation, the same state.
   * What sleep sets could save */
  uint64_t commute_sample = 0, pairs = 0, commuting = 0;
  std::map<std::string, std::pair<uint64_t, uint64_t>> by_kind;  // pairs, commuting
  static std::string act_kind(const std::string& name) {
    auto sp = name.find(' ');
    auto head = name.substr(0, sp);
    if (head.starts_with("gw")) {
      head = "gw";
    }
    auto rest = sp == std::string::npos ? std::string{} : name.substr(sp + 1);
    return head + " " + rest.substr(0, rest.find(' '));
  }
  static std::optional<World> after(const World& w, const std::string& a,
				    const std::string& b) {
    World n = w;
    for (const auto& name : {a, b}) {
      auto acts = actions_of(n);
      auto i = std::find_if(acts.begin(), acts.end(),
			    [&](const Action& x) { return x.name == name; });
      if (i == acts.end()) return std::nullopt;
      if (i->run(n) || check_state(n)) return std::nullopt;
    }
    return n;
  }
  void measure_commute(const World& w, const std::vector<Action>& acts) {
    for (size_t i = 0; i < acts.size(); ++i) {
      for (size_t j = i + 1; j < acts.size(); ++j) {
	if (acts[i].failure || acts[j].failure) continue;
	auto ab = after(w, acts[i].name, acts[j].name);
	auto ba = after(w, acts[j].name, acts[i].name);
	bool c = ab && ba && ab->key() == ba->key();
	++pairs;
	commuting += c;
	auto k = act_kind(acts[i].name) + " | " + act_kind(acts[j].name);
	auto& [p, q] = by_kind[k];
	++p;
	q += c;
      }
    }
  }

  /* FSIO_MODEL_WALKS:  random walks instead of the search.  A walk takes
   * one available action at a time, a failure with probability fail_p
   * when one is available, else a work action uniformly;  at an end it
   * applies the end checks, then stops or, as often, takes a failure and
   * goes on (recovery after a completed run).  Walk i of seed s draws
   * from its own generator, so FSIO_MODEL_SEED=s FSIO_MODEL_WALK=i
   * replays it alone */
  double fail_p = 0.05;
  uint64_t walks = 0;
  void walk(const World& w0, uint64_t seed, uint64_t i) {
    std::seed_seq ss{seed, i};
    std::mt19937_64 rng(ss);
    auto pick = [&](const std::vector<size_t>& v) {
      return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng)];
    };
    auto coin = [&](double p) {
      return std::bernoulli_distribution(p)(rng);
    };
    auto found_one = [&](const std::string& v) {
      if (!found.count(kind(v))) {
	std::cerr << fmt::format("[fsio-model] walk {} of seed {}:\n", i, seed);
      }
      report(v);
    };
    World w = w0;
    path.clear();
    ++walks;
    if (cl.on) {
      cl.begin_walk(seed, i);
    }
    for (;;) {
      auto k = w.key();
      if (seen.insert(XXH3_128bits(k.data(), k.size())).second) {
	++states;
      }
      if (path.size() >= max_depth) {
	found_one(fmt::format("walk exceeds {} steps without an end", max_depth));
	return;
      }
      auto acts = actions_of(w);
      std::vector<size_t> work, fail;
      for (size_t j = 0; j < acts.size(); ++j) {
	(acts[j].failure ? fail : work).push_back(j);
      }
      size_t a;
      if (path.size() < prefix.size()) {
	auto j = std::find_if(acts.begin(), acts.end(), [&](const Action& x) {
	  return x.name == prefix[path.size()];
	});
	if (j == acts.end()) {
	  prefix_missed = true;
	  return;
	}
	a = j - acts.begin();
      } else if (work.empty()) {
	++ends;
	if (auto v = check_end(w)) {
	  found_one(*v);
	  return;
	}
	if (fail.empty() || !coin(0.5)) {
	  return;
	}
	a = pick(fail);
      } else if (!fail.empty() && coin(fail_p)) {
	a = pick(fail);
      } else {
	a = pick(work);
      }
      path.push_back(acts[a].name);
      auto v = acts[a].run(w);
      if (!v && cl.on) v = cl.take();
      if (!v) v = check_state(w);
      if (v) {
	found_one(*v);
	return;
      }
    }
  }

  /* the livelock check (FSIO_MODEL_LIVELOCK, on by default;  off with
   * SHORTEST):  every explored state must still reach an end state.
   * Tarjan's strongly connected components, run in the search:  per
   * state its DFS index, lowlink, whether it is on the component stack,
   * and whether an end state is reachable from it.  A component that
   * finishes with no end state reachable is a livelock.  States whose
   * successors were not all explored (the state limit, the depth limit,
   * a violation, a prefix step) count as reaching an end:  never
   * reported, never trusted */
  struct Node {
    uint32_t index = 0, low = 0;
    bool on_stack = false, reach = false;
  };
  bool livelock = true;
  std::unordered_map<XXH128_hash_t, Node, StateHash, StateEq> nodes;
  std::vector<XXH128_hash_t> component;
  uint32_t next_index = 0;
  static constexpr uint32_t NO_LOW = UINT32_MAX;
  struct Visit {
    uint32_t low = NO_LOW;       // a lowlink for the caller, if on the stack
    bool reach = true;
  };

  /* a state's successors are done:  if it is its component's root, the
   * component is complete, and stuck if no member reaches an end */
  Visit finish(const XXH128_hash_t& hash, bool lv, uint32_t low, bool reach) {
    if (!lv) {
      return {};
    }
    auto& me = nodes[hash];
    me.low = low;
    me.reach = me.reach || reach;
    if (me.low != me.index) {
      return {me.low, me.reach};       // the component goes on above
    }
    const bool r = me.reach;
    size_t n = 0;
    for (;;) {
      auto h = component.back();
      component.pop_back();
      auto& m = nodes[h];
      m.on_stack = false;
      m.reach = r;
      ++n;
      if (XXH128_isEqual(h, hash)) {
	break;
      }
    }
    if (!r) {
      report(fmt::format("livelock:  {} states from which no end state is "
			 "reachable", n));
    }
    return {NO_LOW, r};
  }

  /* FSIO_MODEL_DUMP=1:  during a prefix, what each step left */
  bool dump = false;
  static void dump_state(const World& w, const std::string& step) {
    std::string s = fmt::format("[fsio-model] after {}:\n  chunk sets:", step);
    if (w.head.hdr) {
      for (const auto& e : w.head.hdr->chunk_sets) {
	s += fmt::format(" {}={}", sname(e.id), (int)e.state);
      }
      if (w.head.hdr->cur_shadow) {
	s += fmt::format("  cur_shadow {}", sname(*w.head.hdr->cur_shadow));
      }
    } else {
      s += " (no header)";
    }
    s += "\n  opens:";
    for (const auto& [i, o] : w.head.opens) {
      for (const auto& [k, ho] : o.handles) {
	for (const auto& [cs, c] : ho.chunk_sets) {
	  s += fmt::format(" {}.{}:{} r{} w{}", i, k, sname(cs), c.readers,
			   c.writers);
	}
      }
    }
    s += fmt::format("\n  storage: {}\n  gc queue:", fmt::join(w.storage, " "));
    for (const auto& q : w.gcq) {
      s += fmt::format(" [{}]", fmt::join(q, ","));
    }
    /* the versions' open counts, the hints, each gateway's counted set */
    s += "\n  open counts:";
    for (const auto& [i, v] : w.versions) {
      if (v.ih.ocnt) {
	std::vector<std::string> hs;
	for (const auto& h : v.ih.ocnt->holders) {
	  hs.push_back(fmt::format("{}.{}", h.incarnation, h.handle));
	}
	s += fmt::format(" {}{{{}}}{}", i, fmt::join(hs, ","),
			 v.ih.ocnt->removed ? " removed" : "");
      }
    }
    s += "\n  hints:";
    for (const auto& h : w.hints) {
      s += fmt::format(" {}{}{}/{}.{}", h.kind == HintKind::PRESENCE ? "P" : "U",
		       h.target == HintTarget::HEAD ? "H" : "I", h.instance,
		       h.incarnation, h.handle);
    }
    s += "\n  counted:";
    for (const auto& g : w.gws) {
      s += fmt::format(" gw{}{{{}}}", g.inc, fmt::join(g.counted, ","));
    }
    std::cerr << s << "\n";
  }

  uint64_t transitions = 0, revisits = 0, background_only = 0;
  Visit dfs(const World& w) {
    if (states >= limit) return {};
    if (stop_at_first && !found.empty()) return {};
    if (path.size() >= max_depth) {
      report(fmt::format("path exceeds {} steps:  a cycle drawing new ids",
			 max_depth));
      return {};
    }
    if (path.size() > bound) {
      cut = true;
      return {};
    }
    const bool lv = livelock && !shortest;
    auto k = w.key();
    auto hash = XXH3_128bits(k.data(), k.size());
    if (shortest) {
      auto [i, fresh] = seen_at.try_emplace(hash, path.size());
      if (!fresh) {
	if (i->second <= path.size()) return {};
	i->second = path.size();         // reached shallower:  explore again
      }
    } else if (lv) {
      auto [i, fresh] = nodes.try_emplace(hash);
      if (!fresh) {
	++revisits;
	/* on the stack:  the same component, its reach not yet known;
	 * finished:  its reach is */
	return i->second.on_stack ? Visit{i->second.index, false} :
	  Visit{NO_LOW, i->second.reach};
      }
      i->second.index = i->second.low = next_index++;
      i->second.on_stack = true;
      component.push_back(hash);
    } else if (!seen.insert(hash).second) {
      ++revisits;
      return {};
    }
    uint32_t low = lv ? nodes[hash].index : NO_LOW;
    bool reach = false;
    ++states;
    if (progress && states % progress == 0) {
      status();
    }
    std::vector<Action> acts = actions_of(w);
    /* only the expirer and GC can act:  no gateway or S3 action available */
    if (std::all_of(acts.begin(), acts.end(), [](const Action& a) {
	  return a.failure || a.name.starts_with("expirer") ||
	    a.name.starts_with("gc ");
	}) &&
	std::any_of(acts.begin(), acts.end(),
		    [](const Action& a) { return !a.failure; })) {
      ++background_only;
    }
    if (commute_sample && states % commute_sample == 0) {
      measure_commute(w, acts);
    }
    /* terminal:  nothing left to do but fail.  With no action at all,
     * everyone failed and recovery ran;  with only failures left, the
     * run completed normally, its gateways alive, and the end checks
     * apply to that world too (failures are still explored from it) */
    if (std::all_of(acts.begin(), acts.end(),
		    [](const Action& a) { return a.failure; })) {
      ++ends;
      reach = true;
      if (auto v = check_end(w)) report(*v);
      if (acts.empty()) {
	return finish(hash, lv, low, reach);
      }
    }
    if (path.size() < prefix.size()) {
      const auto& want = prefix[path.size()];
      auto i = std::find_if(acts.begin(), acts.end(),
			    [&](const Action& a) { return a.name == want; });
      if (i == acts.end()) {
	if (!prefix_missed) {
	  std::cerr << fmt::format("[fsio-model] prefix step {} \"{}\" is not "
				   "available;  the actions are:\n",
				   path.size() + 1, want);
	  for (const auto& a : acts) {
	    std::cerr << "    " << a.name << "\n";
	  }
	  std::cerr << std::flush;
	}
	prefix_missed = true;
	return finish(hash, lv, low, true);
      }
      auto a = std::move(*i);
      acts.clear();
      acts.push_back(std::move(a));
      reach = true;                      // its other successors unexplored
    } else if (prefix_only && !prefix.empty()) {
      return finish(hash, lv, low, true);
    }
    for (auto& a : acts) {
      ++transitions;
      World n = w;
      path.push_back(a.name);
      auto v = a.run(n);
      if (dump && path.size() <= prefix.size()) {
	dump_state(n, a.name);
      }
      if (!v) v = check_state(n);
      if (v) {
	report(*v);
	reach = true;
      } else {
	auto r = dfs(n);
	low = std::min(low, r.low);
	reach = reach || r.reach;
      }
      path.pop_back();
    }
    return finish(hash, lv, low, reach);
  }
};

/* raise the bound until a violation is found, or a pass cuts no path */
void deepen(Explorer& x, const World& w0)
{
  for (x.bound = 0; x.bound < x.max_depth; ++x.bound) {
    ++x.passes;
    x.seen_at.clear();
    x.cut = false;
    x.dfs(w0);
    if (!x.found.empty() || !x.cut || x.states >= x.limit) {
      return;
    }
  }
}

World initial(const std::string& roles, int s3_ops)
{
  World w;
  w.storage.insert("C0");
  if (cfg.versioned && cfg.vstart_null) {
    /* versioning just enabled over the head's content:  the null version,
     * on the head itself (§2.5a) */
    w.head.exists = true;
    w.head.olh = true;
    w.head.id_tag = "t0";
    w.head.manifest = tok_bl("C0");
    Version v;
    v.ih.exists = true;
    v.ih.id_tag = "t0";
    v.ih.manifest = tok_bl("C0");
    v.tail = "C0";
    w.versions["null"] = v;
    w.olh_cur = "null";
  } else if (cfg.versioned) {
    w.head.exists = true;                  // the OLH:  no content of its own
    w.head.olh = true;
    Version v;
    v.ih.exists = true;
    v.ih.id_tag = "i0";
    v.ih.manifest = tok_bl("C0");
    v.tail = "C0";
    w.versions["I1"] = v;
    w.olh_cur = "I1";
  } else {
    w.head.exists = true;
    w.head.id_tag = "t0";
    w.head.manifest = tok_bl("C0");
  }
  /* a role letter per handle;  "+" joins the next one to the previous
   * gateway, as its second FSIOObject on the head (Z4):  "w+r" is one
   * gateway with a writer handle and a reader handle */
  uint64_t inc = 0, handle = 0;
  for (size_t i = 0; i < roles.size(); ++i) {
    if (roles[i] == '+') {
      continue;
    }
    if (i > 0 && roles[i - 1] == '+') {
      ++handle;
    } else {
      ++inc;
      handle = 1;
    }
    Gw g;
    g.inc = inc;
    g.handle = handle;
    g.role = roles[i];
    w.gws.push_back(g);
  }
  w.s3_budget = s3_ops;
  w.ex_crashes = cfg.expirer_crashes;
  return w;
}

int env_int(const char* name, int dflt)
{
  auto v = std::getenv(name);
  return v ? std::atoi(v) : dflt;
}

} // anonymous namespace

TEST(FSIOModel, Explore)
{
  Explorer x;
  x.limit = env_int("FSIO_MODEL_LIMIT", 2'000'000);
  x.progress = env_int("FSIO_MODEL_PROGRESS", 100'000);
  x.max_depth = env_int("FSIO_MODEL_MAX_DEPTH", 200);
  x.stop_at_first = env_int("FSIO_MODEL_STOP_AT_FIRST", 0);
  x.commute_sample = env_int("FSIO_MODEL_COMMUTE_SAMPLE", 0);
  x.livelock = env_int("FSIO_MODEL_LIVELOCK", 1);
  x.dump = env_int("FSIO_MODEL_DUMP", 0);
  if (std::getenv("FSIO_MODEL_CLUSTER") &&
      std::string(std::getenv("FSIO_MODEL_CLUSTER")) == "1") {
    if (env_int("FSIO_MODEL_WALKS", 0) <= 0 ||
	std::getenv("FSIO_MODEL_SKIP_CHECK")) {
      std::cerr << "FSIO_MODEL_CLUSTER needs FSIO_MODEL_WALKS, and no "
		   "FSIO_MODEL_SKIP_CHECK\n";
      std::exit(2);
    }
    const char* pool = std::getenv("FSIO_MODEL_POOL");
    if (int r = cl.connect(pool ? pool : "fsio-model"); r < 0) {
      std::cerr << fmt::format("FSIO_MODEL_CLUSTER:  connecting returned {}\n",
			       r);
      std::exit(2);
    }
  }
  if (auto c = std::getenv("FSIO_MODEL_SKIP_CHECK")) {
    const std::map<std::string, cls_rgw_fsio_check> checks = {
      {"W5_FINALIZE_RECHECKS_STATE", cls_rgw_fsio_check::W5_FINALIZE_RECHECKS_STATE},
      {"W6_EXPIRE_TAKES_OVER_PUBLISH", cls_rgw_fsio_check::W6_EXPIRE_TAKES_OVER_PUBLISH},
      {"W3_FINALIZE_CREATES_TO_PUBLISHING", cls_rgw_fsio_check::W3_FINALIZE_CREATES_TO_PUBLISHING},
      {"W3_RESUMED_FINALIZE_TAKES_OVER", cls_rgw_fsio_check::W3_RESUMED_FINALIZE_TAKES_OVER},
      {"W3_PUBLISHING_TO_RETIRES_FROM", cls_rgw_fsio_check::W3_PUBLISHING_TO_RETIRES_FROM},
      {"W1_SHADOW_STORES_PSEUDO_MANIFEST", cls_rgw_fsio_check::W1_SHADOW_STORES_PSEUDO_MANIFEST},
      {"CUR_SHADOW_SUPERSEDE", cls_rgw_fsio_check::CUR_SHADOW_SUPERSEDE},
      {"CUR_SHADOW_PUBLISH", cls_rgw_fsio_check::CUR_SHADOW_PUBLISH},
      {"CUR_SHADOW_ABANDON", cls_rgw_fsio_check::CUR_SHADOW_ABANDON},
      {"W7_LEAVE_RETURNS_VERSION_READ", cls_rgw_fsio_check::W7_LEAVE_RETURNS_VERSION_READ},
      {"W7_REBIND_WAITS_ON_PUBLISHING", cls_rgw_fsio_check::W7_REBIND_WAITS_ON_PUBLISHING},
      {"W7_PUBLISH_COUNTS_READERS_ONLY", cls_rgw_fsio_check::W7_PUBLISH_COUNTS_READERS_ONLY},
      {"V1_FINALIZE_REQUIRES_ABORTED_CLAIM",
       cls_rgw_fsio_check::V1_FINALIZE_REQUIRES_ABORTED_CLAIM},
      {"W9_HINT_SPARES_WRITTEN_VERSION",
       cls_rgw_fsio_check::W9_HINT_SPARES_WRITTEN_VERSION},
      {"W9_HINT_WAITS_FOR_WRITER", cls_rgw_fsio_check::W9_HINT_WAITS_FOR_WRITER},
      {"V2_COMPLETE_REMOVES_DEAD_WRITERS",
       cls_rgw_fsio_check::V2_COMPLETE_REMOVES_DEAD_WRITERS},
      {"V3_FINALIZER_HINTS_PLACEHOLDER",
       cls_rgw_fsio_check::V3_FINALIZER_HINTS_PLACEHOLDER},
      {"V4_HINT_WAITS_FOR_GRACE", cls_rgw_fsio_check::V4_HINT_WAITS_FOR_GRACE},
      {"V4_PUBLISHER_KEEPS_HINT", cls_rgw_fsio_check::V4_PUBLISHER_KEEPS_HINT},
      {"NULL_IS_HEAD", cls_rgw_fsio_check::NULL_IS_HEAD},
      {"NULL_REMOVAL_RETIRES", cls_rgw_fsio_check::NULL_REMOVAL_RETIRES},
      {"PUBLISH_FOLLOWS_STATE", cls_rgw_fsio_check::PUBLISH_FOLLOWS_STATE},
      {"PRESENCE_HINT_BEFORE_JOIN", cls_rgw_fsio_check::PRESENCE_HINT_BEFORE_JOIN},
      {"PRESENCE_HINT_BEFORE_OCNT", cls_rgw_fsio_check::PRESENCE_HINT_BEFORE_OCNT},
      {"PRESENCE_HINT_HELD_UNTIL_RELEASED",
       cls_rgw_fsio_check::PRESENCE_HINT_HELD_UNTIL_RELEASED},
      {"H1_FAILED_RELEASE_KEEPS_HINT",
       cls_rgw_fsio_check::H1_FAILED_RELEASE_KEEPS_HINT},
      {"H2_HINT_DROPPED_AFTER_GRACE",
       cls_rgw_fsio_check::H2_HINT_DROPPED_AFTER_GRACE},
      {"H3_PUBLISHER_HINTS_READERS",
       cls_rgw_fsio_check::H3_PUBLISHER_HINTS_READERS},
      {"I4_PUBLISHER_NOT_COUNTED",
       cls_rgw_fsio_check::I4_PUBLISHER_NOT_COUNTED},
      {"H3_TRACKING_ONLY_IS_REMOVED",
       cls_rgw_fsio_check::H3_TRACKING_ONLY_IS_REMOVED},
      {"H3_HINT_WAITS_FOR_LIVE_CLAIM",
       cls_rgw_fsio_check::H3_HINT_WAITS_FOR_LIVE_CLAIM},
      {"H3_PUBLISH_HINT_REMOVES_READER_HINTS",
       cls_rgw_fsio_check::H3_PUBLISH_HINT_REMOVES_READER_HINTS},
      {"H3_PUBLISHER_CLEANUP_REMOVES_READER_HINTS",
       cls_rgw_fsio_check::H3_PUBLISHER_CLEANUP_REMOVES_READER_HINTS},
      {"W10_RELEASE_ONLY_WITH_LIVE_CLAIM",
       cls_rgw_fsio_check::W10_RELEASE_ONLY_WITH_LIVE_CLAIM},
      {"W10_RELEASE_ABORTS_CLAIM_FIRST",
       cls_rgw_fsio_check::W10_RELEASE_ABORTS_CLAIM_FIRST},
      {"W15_CONSTRUCTOR_MAY_ABANDON",
       cls_rgw_fsio_check::W15_CONSTRUCTOR_MAY_ABANDON},
      {"RESUME_RESOLVES_CLAIM", cls_rgw_fsio_check::RESUME_RESOLVES_CLAIM},
      {"RESUME_REPLACES_CLAIM", cls_rgw_fsio_check::RESUME_REPLACES_CLAIM},
      {"Z22_COUNTED_CALL_CHECKS_SEQ",
       cls_rgw_fsio_check::Z22_COUNTED_CALL_CHECKS_SEQ},
      {"Z22_ABANDON_WAITS_FOR_GRACE",
       cls_rgw_fsio_check::Z22_ABANDON_WAITS_FOR_GRACE},
      {"Z22_EXPIRY_KEEPS_SEQ", cls_rgw_fsio_check::Z22_EXPIRY_KEEPS_SEQ},
      {"Z11_EXTENT_CAPS_RUNS", cls_rgw_fsio_check::Z11_EXTENT_CAPS_RUNS},
      {"Z6_INSTANCE_WRITE_DROPS_CLAIM",
       cls_rgw_fsio_check::Z6_INSTANCE_WRITE_DROPS_CLAIM},
      {"Z6_RESOLUTION_TESTS_ID_TAG", cls_rgw_fsio_check::Z6_RESOLUTION_TESTS_ID_TAG},
      {"W20_RELEASE_CHECKS_PUBLISHER",
       cls_rgw_fsio_check::W20_RELEASE_CHECKS_PUBLISHER},
    };
    /* one check, or two:  "A,B" */
    std::string names = c;
    auto comma = names.find(',');
    std::vector<std::string> list{names.substr(0, comma)};
    if (comma != std::string::npos) {
      list.push_back(names.substr(comma + 1));
    }
    for (size_t k = 0; k < list.size(); ++k) {
      auto i = checks.find(list[k]);
      ASSERT_NE(i, checks.end()) << "unknown check " << list[k];
      (k == 0 ? fsio::skipped_check : fsio::also_skipped_check) = i->second;
    }
    std::cout << "skipping check " << c << "\n";
  }
  cfg.crashes = env_int("FSIO_MODEL_CRASHES", 1);
  cfg.zombies = env_int("FSIO_MODEL_ZOMBIES", 1);
  cfg.expirer_crashes = env_int("FSIO_MODEL_EXPIRER_CRASHES", 1);
  cfg.resends = env_int("FSIO_MODEL_RESENDS", 1);
  cfg.versioned = env_int("FSIO_MODEL_VERSIONED", 0);
  if (auto v = std::getenv("FSIO_MODEL_VSTART")) {
    cfg.vstart_null = std::string(v) == "null";
  }
  std::string roles(env_int("FSIO_MODEL_GATEWAYS", 2), 'f');
  if (auto r = std::getenv("FSIO_MODEL_ROLES")) {
    roles = r;
  }
  std::cout << fmt::format("roles {}, s3 ops {}, crashes {}, zombies {}, "
			   "expirer crashes {}{}\n", roles,
			   env_int("FSIO_MODEL_S3_OPS", 2), cfg.crashes,
			   cfg.zombies, cfg.expirer_crashes,
			   cfg.versioned ? ", versioned" : "");
  x.shortest = env_int("FSIO_MODEL_SHORTEST", 0);
  if (auto p = std::getenv("FSIO_MODEL_PREFIX")) {
    /* a trace pasted as is:  leading blanks and its "=>" line ignored */
    std::ifstream f(p);
    ASSERT_TRUE(f) << "cannot read " << p;
    for (std::string l; std::getline(f, l); ) {
      auto b = l.find_first_not_of(" \t");
      if (b == std::string::npos || l.compare(b, 2, "=>") == 0 ||
	  l[b] == '#') {
	continue;
      }
      x.prefix.push_back(l.substr(b));
    }
    x.prefix_only = env_int("FSIO_MODEL_PREFIX_ONLY", 0);
    std::cout << fmt::format("prefix:  {} steps{}\n", x.prefix.size(),
			     x.prefix_only ? ", replayed only" : "");
  }
  if (x.shortest) {
    deepen(x, initial(roles, env_int("FSIO_MODEL_S3_OPS", 2)));
  } else if (auto nw = env_int("FSIO_MODEL_WALKS", 0); nw > 0) {
    const uint64_t seed = env_int("FSIO_MODEL_SEED", 1);
    x.fail_p = std::getenv("FSIO_MODEL_FAIL_P") ?
      std::stod(std::getenv("FSIO_MODEL_FAIL_P")) : 0.05;
    const auto w0 = initial(roles, env_int("FSIO_MODEL_S3_OPS", 2));
    const int only = env_int("FSIO_MODEL_WALK", -1);
    for (uint64_t i = only >= 0 ? only : 0;
	 i < (only >= 0 ? (uint64_t)only + 1 : (uint64_t)nw); ++i) {
      if (x.stop_at_first && !x.found.empty()) break;
      x.walk(w0, seed, i);
      if (x.progress && x.walks % 100000 == 0) {
	std::cerr << fmt::format("[fsio-model] {} walks", x.walks);
	x.status();
      }
    }
    x.status();
    std::cout << fmt::format("{} states (sampled by {} walks, seed {}), {} end "
			     "states, {} kinds of violation\n", x.states,
			     x.walks, seed, x.ends, x.found.size());
    if (cl.on) {
      std::cout << "calls sent to the cluster:\n" << cl.reach();
    }
    for (const auto& [k, t] : x.found) {
      std::cout << "\n--- " << t.back().substr(3) << "\n";
      for (const auto& s : t) {
	std::cout << "    " << s << "\n";
      }
    }
    return;
  } else {
    x.dfs(initial(roles, env_int("FSIO_MODEL_S3_OPS", 2)));
  }
  x.status();
  std::cout << fmt::format("{} transitions, {} to a visited state;  {} "
			   "states where only the expirer and GC act\n",
			   x.transitions, x.revisits, x.background_only);
  if (x.commute_sample) {
    std::cout << fmt::format("{} action pairs sampled, {} commute ({:.1f}%)\n",
			     x.pairs, x.commuting,
			     x.pairs ? 100.0 * x.commuting / x.pairs : 0.0);
    std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> v(
      x.by_kind.begin(), x.by_kind.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) {
      return a.second.first > b.second.first;
    });
    for (size_t i = 0; i < v.size() && i < 25; ++i) {
      std::cout << fmt::format("  {:>8} pairs {:>5.1f}% commute  {}\n",
			       v[i].second.first,
			       100.0 * v[i].second.second / v[i].second.first,
			       v[i].first);
    }
  }
  if (x.states >= x.limit) {
    std::cout << "state limit reached:  the space was not exhausted\n";
  }
  std::cout << fmt::format("{} states{}, {} end states, {} kinds of "
			   "violation\n", x.states,
			   x.shortest ? fmt::format(" (over {} passes)",
						    x.passes) : "",
			   x.ends, x.found.size());
  for (const auto& [k, t] : x.found) {
    std::cout << "\n--- " << t.back().substr(3) << "\n";
    for (const auto& s : t) {
      std::cout << "    " << s << "\n";
    }
  }
  EXPECT_FALSE(x.prefix_missed) << "the prefix did not apply";
  EXPECT_TRUE(x.found.empty());
}
