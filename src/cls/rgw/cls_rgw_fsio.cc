// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "cls_rgw_fsio.h"

#include <algorithm>
#include <cerrno>

#include <fmt/format.h>

/* Each transition follows FSIO_RADOS_DESIGN.md as written (§3, §4), so
 * that the model checker measures the design;  a fix lands in the design
 * and here together. */

cls_rgw_fsio_chunk_set* cls_rgw_fsio_hdr::find(const cls_rgw_fsio_id& id)
{
  for (auto& cs : chunk_sets) {
    if (cs.id == id) {
      return &cs;
    }
  }
  return nullptr;
}

const cls_rgw_fsio_chunk_set*
cls_rgw_fsio_hdr::find(const cls_rgw_fsio_id& id) const
{
  for (const auto& cs : chunk_sets) {
    if (cs.id == id) {
      return &cs;
    }
  }
  return nullptr;
}

cls_rgw_fsio_fenced_finalize*
cls_rgw_fsio_hdr::find_finalize(const cls_rgw_fsio_id& cs)
{
  for (auto& ff : fenced_finalizes) {
    if (ff.from == cs || ff.to == cs) {
      return &ff;
    }
  }
  return nullptr;
}

cls_rgw_fsio_counts cls_rgw_fsio_head::bound(const cls_rgw_fsio_id& cs) const
{
  cls_rgw_fsio_counts sum;
  for (const auto& [inc, o] : opens) {
    for (const auto& [hd, ho] : o.handles) {
      auto i = ho.chunk_sets.find(cs);
      if (i != ho.chunk_sets.end()) {
	sum.readers += i->second.readers;
	sum.writers += i->second.writers;
      }
    }
  }
  return sum;
}

cls_rgw_fsio_handle_opens*
cls_rgw_fsio_head::find(const cls_rgw_fsio_handle& c)
{
  auto o = opens.find(c.incarnation);
  if (o == opens.end()) {
    return nullptr;
  }
  auto ho = o->second.handles.find(c.handle);
  return ho == o->second.handles.end() ? nullptr : &ho->second;
}

bool cls_rgw_fsio_head::holds_opens() const
{
  for (const auto& [i, o] : opens) {
    if (!o.handles.empty()) {
      return true;
    }
  }
  return false;
}

cls_rgw_fsio_counts& cls_rgw_fsio_head::counts(const cls_rgw_fsio_handle& c,
					       const cls_rgw_fsio_id& cs)
{
  return opens[c.incarnation].handles[c.handle].chunk_sets[cs];
}

namespace cls_rgw_fsio {

#ifdef CLS_RGW_FSIO_SKIP_CHECKS
cls_rgw_fsio_check skipped_check = cls_rgw_fsio_check::NONE;
cls_rgw_fsio_check also_skipped_check = cls_rgw_fsio_check::NONE;
#endif

namespace {

using state = cls_rgw_fsio_cs_state;
using at = cls_rgw_fsio_manifest_at;

/* §2.5a:  an empty instance and null are the same version, the head's own
 * content */
bool is_null(const std::string& instance)
{
  return instance.empty() ||
    (instance == "null" && !FSIO_SKIP_CHECK(NULL_IS_HEAD));
}

bool same_instance(const std::string& a, const std::string& b)
{
  return a == b || (is_null(a) && is_null(b));
}

/* the chunk set the head object's own content is:  the entry at HEAD */
cls_rgw_fsio_chunk_set* head_chunk_set(cls_rgw_fsio_hdr& hdr)
{
  for (auto& cs : hdr.chunk_sets) {
    if (cs.manifest_at == at::HEAD) {
      return &cs;
    }
  }
  return nullptr;
}

/* §3.6:  the chunk sets tail-keeping writes retired that still have
 * readers:  they share the head's tail */
std::set<cls_rgw_fsio_id> tail_sharers(const cls_rgw_fsio_head& h)
{
  std::set<cls_rgw_fsio_id> s;
  for (const auto& cs : h.hdr->chunk_sets) {
    if (cs.shares_tail && !cs.drained) {
      auto b = h.bound(cs.id);
      if (b.readers || b.writers) {
	s.insert(cs.id);
      }
    }
  }
  return s;
}

/* §3.6:  the head's replacement, not keeping the tail, ends the sharing */
void end_tail_sharing(cls_rgw_fsio_hdr& hdr)
{
  for (auto& cs : hdr.chunk_sets) {
    cs.shares_tail = false;
  }
}

/* the published chunk set:  the entry at HEAD, or at INSTANCE for a
 * bound non-null version in a versioned bucket (§2.2, §2.5a) */
cls_rgw_fsio_chunk_set* published_chunk_set(cls_rgw_fsio_hdr& hdr)
{
  for (auto& cs : hdr.chunk_sets) {
    if (is_null(hdr.cur_instance) ? cs.manifest_at == at::HEAD :
	(cs.manifest_at == at::INSTANCE && cs.state == state::PUBLISHED &&
	 cs.instance == hdr.cur_instance)) {
      return &cs;
    }
  }
  return nullptr;
}

/* the current generation's shadow (§2.2) */
cls_rgw_fsio_chunk_set* current_shadow(cls_rgw_fsio_hdr& hdr)
{
  return hdr.cur_shadow ? hdr.find(*hdr.cur_shadow) : nullptr;
}

/* removes or drains what empties (§2.2, §3.4);  the drained ids go to the
 * caller, which sends their GC entries and calls forget() */
void settle(cls_rgw_fsio_head& h, std::vector<cls_rgw_fsio_id>& drained,
	    cls_rgw_fsio_effects& fx, const cls_rgw_fsio_handle& caller = {})
{
  for (auto i = h.opens.begin(); i != h.opens.end(); ) {
    auto& hs = i->second.handles;
    for (auto k = hs.begin(); k != hs.end(); ) {
      /* a handle record emptied by another's call, so a presumed-dead
       * incarnation's (expiry, a fenced finalize, an abandon, a dead
       * publisher's completion), keeps its last_seq until grace:  its
       * resends may still land (Z22).  Never the caller's own:  its own
       * call emptied it, and a resend of that call finds no record (§3).
       * Records are per handle (Z4), so a sibling handle's call is
       * another's */
      const bool keep =
	!(i->first == caller.incarnation &&
	  (k->first == caller.handle ||
	   FSIO_SKIP_CHECK(SETTLE_KEEPS_SIBLING_SEQ))) &&
	!FSIO_SKIP_CHECK(Z22_EXPIRY_KEEPS_SEQ);
      auto& m = k->second.chunk_sets;
      for (auto j = m.begin(); j != m.end(); ) {
	if (j->second.readers == 0 && j->second.writers == 0) {
	  j = m.erase(j);
	} else {
	  ++j;
	}
      }
      if (m.empty()) {
	if (keep) {
	  auto& s = i->second.seqs[k->first];
	  s = std::max(s, k->second.last_seq);
	}
	k = hs.erase(k);
      } else {
	++k;
      }
    }
    if (hs.empty() && i->second.seqs.empty()) {
      i = h.opens.erase(i);
    } else {
      ++i;
    }
  }
  if (h.hdr) {
    auto& v = h.hdr->chunk_sets;
    for (auto i = v.begin(); i != v.end(); ) {
      auto b = h.bound(i->id);
      bool idle = b.readers == 0 && b.writers == 0;
      if (idle && i->state == state::PUBLISHED) {
	i = v.erase(i);
	continue;
      }
      /* §3.6:  an entry holding a shared tail drains once its sharers
       * are gone, so the fsio_forget of the last reports it */
      const bool waits = std::any_of(i->after.begin(), i->after.end(),
				     [&](const cls_rgw_fsio_id& a) {
				       return h.hdr->find(a) != nullptr;
				     });
      if (idle && i->state == state::RETIRED && !i->drained && !waits) {
	i->drained = true;
	drained.push_back(i->id);
      }
      ++i;
    }
    if (!h.holds_opens() && h.hdr->empty()) {
      h.hdr.reset();
    }
  }
  if (!h.hdr && !h.holds_opens() && h.tracking_only()) {
    fx.remove_head = true;
  }
}

void expire_readers(cls_rgw_fsio_head& h, const expired_t& expired)
{
  for (auto inc : expired) {
    auto i = h.opens.find(inc);
    if (i == h.opens.end()) {
      continue;
    }
    for (auto& [hd, ho] : i->second.handles) {    // every handle (Z4)
      for (auto& [cs, c] : ho.chunk_sets) {
	c.readers = 0;
      }
    }
  }
}

/* E5:  RGW collects a tail under its tail tag when set, else the ID tag
 * (complete_atomic_modification()) */
const std::string& gc_tag_of(const std::string& tail_tag,
			     const std::string& id_tag)
{
  return tail_tag.empty() ? id_tag : tail_tag;
}

cls_rgw_fsio_gen record_of_head(const cls_rgw_fsio_head& h)
{
  cls_rgw_fsio_gen rec;
  rec.manifest = h.manifest;
  rec.gc_tag = gc_tag_of(h.tail_tag, h.id_tag);
  return rec;
}

} // anonymous namespace

namespace {

/* §3, Z22:  a sequenced call not newer than the handle's last applied one
 * is a resend;  with no record for the handle there is nothing to compare
 * against, and the call itself decides */
bool already_applied(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& c,
		     uint64_t seq)
{
  if (FSIO_SKIP_CHECK(Z22_COUNTED_CALL_CHECKS_SEQ)) {
    return false;
  }
  if (auto* o = h.find(c)) {
    return seq <= o->last_seq;
  }
  /* expired:  its kept last_seq answers a resend still in flight */
  auto i = h.opens.find(c.incarnation);
  if (i != h.opens.end()) {
    auto s = i->second.seqs.find(c.handle);
    if (s != i->second.seqs.end()) {
      return seq <= s->second;
    }
  }
  return false;
}

/* stored in the same op as the call's effect;  an fsio_leave that
 * removed the handle's record leaves nothing to store it in */
void applied(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& c, uint64_t seq)
{
  if (auto* o = h.find(c)) {
    o->last_seq = std::max(o->last_seq, seq);
  }
}

int join_once(cls_rgw_fsio_head& h, const join_in& in, join_out& out,
	      cls_rgw_fsio_effects& fx);
int leave_once(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	       const cls_rgw_fsio_id& cs, bool writer, const expired_t& expired,
	       leave_out& out, cls_rgw_fsio_effects& fx,
	       const publish_intent* intent, bool handoff, bool discard);
int rebind_once(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
		const cls_rgw_fsio_id& from, const cls_rgw_fsio_id& to,
		uint32_t readers, rebind_out& out);

} // anonymous namespace

int join(cls_rgw_fsio_head& h, const join_in& in, join_out& out,
	 cls_rgw_fsio_effects& fx)
{
  if (already_applied(h, in.caller, in.seq)) {
    out.already_applied = true;
    return 0;
  }
  int r = join_once(h, in, out, fx);
  if (r == 0) {
    applied(h, in.caller, in.seq);
  }
  return r;
}

int leave(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	  uint64_t seq, const cls_rgw_fsio_id& cs, bool writer,
	  const expired_t& expired, leave_out& out, cls_rgw_fsio_effects& fx,
	  const publish_intent* intent, bool handoff, bool discard)
{
  if (already_applied(h, caller, seq)) {
    out.already_applied = true;
    return 0;
  }
  int r = leave_once(h, caller, cs, writer, expired, out, fx, intent, handoff,
		     discard);
  if (r == 0) {
    applied(h, caller, seq);
  }
  return r;
}

int rebind(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	   uint64_t seq, const cls_rgw_fsio_id& from, const cls_rgw_fsio_id& to,
	   uint32_t readers, rebind_out* out)
{
  rebind_out o;
  if (already_applied(h, caller, seq)) {
    o.already_applied = true;
  } else if (int r = rebind_once(h, caller, from, to, readers, o); r < 0) {
    return r;
  } else {
    applied(h, caller, seq);
  }
  if (out) {
    *out = o;
  }
  return 0;
}

namespace {

/* Z4:  another handle, a sibling in the same process included, with an
 * open bound to the chunk set or to the published chunk set */
bool others_bound(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
		  const cls_rgw_fsio_id& cs)
{
  const auto* pub = published_chunk_set(*h.hdr);
  for (const auto& [inc, o] : h.opens) {
    for (const auto& [hdl, ho] : o.handles) {
      if (cls_rgw_fsio_handle{inc, hdl} == caller) {
	continue;
      }
      for (const auto& [id, c] : ho.chunk_sets) {
	if ((id == cs || (pub && id == pub->id)) && (c.readers || c.writers)) {
	  return true;
	}
      }
    }
  }
  return false;
}

int join_once(cls_rgw_fsio_head& h, const join_in& in, join_out& out,
	      cls_rgw_fsio_effects& fx)
{
  /* 0. a writer's create:  an absent head is made, a tracking-only head
   * whose header (3.) is created;  an S3 object fails an exclusive one
   * (§3.1, Z24) */
  bool creating = false;
  if (in.absent && (!in.writer || in.create == create_mode::NONE)) {
    return -EINVAL;
  }
  if (!h.exists) {
    if (!in.writer || in.create == create_mode::NONE) {
      return -ENOENT;
    }
    h.exists = true;
    creating = true;
  } else if (h.tracking_only()) {
    /* 1. a tracking-only head:  the name is deleted, unless it is being
     * created, which exists from its create:  any open joins it but an
     * exclusive create */
    if (!h.hdr || !h.hdr->created) {
      return -ENOENT;
    }
    if (in.writer && in.create == create_mode::EXCLUSIVE) {
      return -EEXIST;
    }
  } else if (!in.writer && h.olh && in.instance.empty() &&
	     !h.manifest.length() && !(h.hdr && h.hdr->created)) {
    /* a reader naming no version of a head with an OLH and no content of
     * its own:  a delete marker, the name with no current version;  only
     * a name being created is joined (§3.1, 2026-11-25), which an
     * abandoned or completed creation no longer is.  A head holding the
     * null version's content under an OLH is joined at the head (§2.5a) */
    return -ENOENT;
  } else if (in.writer && in.create == create_mode::EXCLUSIVE &&
	     !h.id_tag.empty() && !in.absent) {
    /* the OLH's ID tag says nothing of a name whose current version is
     * a delete marker:  absent decides below */
    return -EEXIST;
  }
  /* 2. lazy expiry, readers only;  what it empties is removed or
   * drained (§2.2), a drain left to the FSIO expirer.  A head being
   * created holds nothing to expire, and before 3. gives it its header
   * settle() would take it for an empty tracking-only head */
  if (!creating) {
    expire_readers(h, in.expired);
    std::vector<cls_rgw_fsio_id> drained;
    settle(h, drained, fx, in.caller);
  }
  /* 3. */
  if (!h.hdr) {
    h.hdr.emplace();
    h.hdr->nonce = in.nonce;
    h.hdr->next_counter = in.counter_seed;
    h.hdr->cur_instance = in.instance;
    h.hdr->created = creating;
  }
  auto& hdr = *h.hdr;
  if (in.absent) {
    /* §3.1 step 0, 8i:  a create over a delete marker:  a creation under
     * way is joined, or refused if exclusive;  otherwise a new generation
     * of a name being created */
    if (hdr.created && hdr.cur_instance.empty()) {
      if (in.create == create_mode::EXCLUSIVE) {
	return -EEXIST;
      }
    } else {
      hdr.cur_instance.clear();
      hdr.cur_shadow.reset();
      hdr.created = true;
    }
  } else if (!is_null(in.instance)) {
    hdr.created = false;	// §3.4, 8i:  the name has a current version
  }
  /* §2.5a:  joining the null version is as in an unversioned bucket */
  const bool versioned = !is_null(in.instance);
  /* 4. versioned:  a reader joining with the instance a publish in
   * flight has linked binds to its shadow, the current generation's or a
   * deferred one's, counted by its own ocnt_add;  not a publish onto the
   * null version, which has no link before it lands;  otherwise the OLH
   * names another instance, a new current generation, the old version's
   * published entry staying at INSTANCE */
  const cls_rgw_fsio_chunk_set* linked = nullptr;
  if (!in.instance.empty() && !same_instance(in.instance, hdr.cur_instance)) {
    if (!in.writer && !FSIO_SKIP_CHECK(JOIN_BINDS_LINKED_PUBLISH)) {
      for (const auto& e : hdr.chunk_sets) {
	if (e.state == state::PUBLISHING && e.publishing &&
	    !is_null(e.publishing->instance) &&
	    same_instance(e.publishing->instance, in.instance)) {
	  linked = &e;
	  break;
	}
      }
    }
    if (!linked) {
      hdr.cur_instance = in.instance;
      hdr.cur_shadow.reset();
    }
  }
  /* 5. */
  if (!linked) {
    auto* sh = current_shadow(hdr);
    if (in.writer && sh && (sh->state == state::CONSTRUCTING ||
			    sh->state == state::PUBLISHING)) {
      return -CLS_RGW_ERR_FSIO_BUSY;
    }
    if (!in.writer && versioned && sh && sh->state == state::PUBLISHING) {
      return -CLS_RGW_ERR_FSIO_BUSY;
    }
  }
  /* 6., 7. */
  cls_rgw_fsio_id bind;
  if (linked) {
    bind = linked->id;
  } else if (!in.writer && hdr.created) {
    /* §3.1:  a name being created has only its shadow */
    auto* sh = current_shadow(hdr);
    if (!sh) {
      return -ENOENT;
    }
    if (sh->state != state::READY) {
      return -CLS_RGW_ERR_FSIO_BUSY;
    }
    bind = sh->id;
  } else if (!in.writer) {
    auto* p = published_chunk_set(hdr);
    if (!p) {
      cls_rgw_fsio_chunk_set cs;
      cs.id = hdr.draw();
      cs.state = state::PUBLISHED;
      if (versioned) {
	cs.manifest_at = at::INSTANCE;
	cs.instance = hdr.cur_instance;
      } else {
	cs.manifest_at = at::HEAD;
      }
      hdr.chunk_sets.push_back(cs);
      bind = cs.id;
    } else {
      bind = p->id;
    }
  } else {
    auto* sh = current_shadow(hdr);
    if (sh) {
      bind = sh->id;
      sh->uncompleted_publish.reset();	// 8j:  its close publishes now
    } else {
      cls_rgw_fsio_chunk_set cs;
      cs.id = hdr.draw();
      cs.state = state::CONSTRUCTING;
      cs.manifest_at = at::SHADOW;
      cs.shadow.emplace();
      if (versioned) {
	cs.shadow->base = hdr.cur_instance;	// §2.2, 8h
      }
      hdr.chunk_sets.push_back(cs);
      hdr.cur_shadow = cs.id;
      bind = cs.id;
      out.construct = true;
      /* 7. versioned, the OLH holds no content:  the gateway clones the
       * instance head itself (§4.1);  a created head holds none either,
       * and the gateway writes the shadow's attribute clone (Z24) */
      if (!versioned && !hdr.created) {
	fx.clones.push_back(cls_rgw_fsio_clone{
	    cs.id, in.head_data ? 'd' : 'a', {},
	    FSIO_SKIP_CHECK(W1_SHADOW_STORES_PSEUDO_MANIFEST) ? ceph::buffer::list{} : h.manifest});
      }
    }
  }
  auto& c = h.counts(in.caller, bind);
  if (in.writer) {
    ++c.writers;
    /* §3.5, Y16, Z14:  the writer's identity, and the order it joined in
     * (a counter value drawn here, so no two writers tie) */
    c.writer_identity = in.identity;
    c.joined_seq = hdr.draw().counter;
  } else {
    ++c.readers;
  }
  out.bound = bind;
  /* §3.1:  the reply carries what the gateway needs from the join, as
   * the join saw it, so it need not read the head again */
  if (const auto* e = hdr.find(bind)) {
    out.state = e->state;
    if (e->shadow) {
      out.trunc_epoch = e->shadow->trunc_epoch;
      out.size_hint = e->shadow->size_hint;
      out.mtime_hint = e->shadow->mtime_hint;
      if (!e->shadow->extent.empty()) {
	out.last_run = e->shadow->extent.back();
      }
    }
  }
  out.others = others_bound(h, in.caller, bind);
  return 0;
}

} // anonymous namespace

void add_run(std::vector<cls_rgw_fsio_run>& extent, cls_rgw_fsio_run run,
	     size_t max_runs)
{
  if (run.last < run.first) {
    std::swap(run.first, run.last);
  }
  extent.push_back(run);
  std::sort(extent.begin(), extent.end());
  std::vector<cls_rgw_fsio_run> merged;
  for (const auto& r : extent) {
    if (!merged.empty() && r.first <= merged.back().last + 1) {
      merged.back().last = std::max(merged.back().last, r.last);
    } else {
      merged.push_back(r);
    }
  }
  /* the cap:  the smallest gap's pair merges, its gap becoming fenced
   * holes at a finalize, never an unbounded header */
  while (merged.size() > std::max<size_t>(max_runs, 1) &&
	 !FSIO_SKIP_CHECK(Z11_EXTENT_CAPS_RUNS)) {
    size_t best = 0;
    for (size_t i = 1; i + 1 < merged.size(); ++i) {
      if (merged[i + 1].first - merged[i].last <
	  merged[best + 1].first - merged[best].last) {
	best = i;
      }
    }
    merged[best].last = merged[best + 1].last;
    merged.erase(merged.begin() + best + 1);
  }
  extent = std::move(merged);
}

int ready(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	  const cls_rgw_fsio_id& cs,
	  const std::vector<cls_rgw_fsio_run>& runs,
	  uint64_t size, ceph::real_time mtime, size_t max_runs)
{
  if (!h.hdr) {
    return -EINVAL;
  }
  auto* e = h.hdr->find(cs);
  if (!e) {
    return -EINVAL;
  }
  auto* o = h.find(caller);
  if (!o || !o->chunk_sets.count(cs) || o->chunk_sets[cs].writers == 0) {
    return -EINVAL;
  }
  /* §2.2, A2:  idempotent by its effect:  a repeat finding the shadow
   * ready changes nothing (not even extent, which fsio_extend may have
   * grown since) and succeeds */
  if (e->state == state::READY) {
    return 0;
  }
  if (e->state != state::CONSTRUCTING) {
    return -EINVAL;
  }
  e->state = state::READY;
  e->shadow->size_hint = size;       // §3.2:  the published object's
  e->shadow->mtime_hint = mtime;
  e->shadow->extent.clear();
  for (const auto& r : runs) {
    add_run(e->shadow->extent, r, max_runs);
  }
  return 0;
}

int writer_identity(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
		    const cls_rgw_fsio_id& cs, const std::string& identity)
{
  auto* o = h.find(caller);
  if (!h.hdr || !o) {
    return -EINVAL;
  }
  auto c = o->chunk_sets.find(cs);
  if (c == o->chunk_sets.end() || c->second.writers == 0) {
    return -EINVAL;
  }
  c->second.writer_identity = identity;
  return 0;
}

int extend(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	   const cls_rgw_fsio_id& cs, const cls_rgw_fsio_run& run,
	   size_t max_runs)
{
  if (!h.hdr) {
    return -EINVAL;
  }
  auto* e = h.hdr->find(cs);
  if (!e || !e->shadow) {
    return -EINVAL;
  }
  if (e->state == state::PUBLISHING) {
    return -CLS_RGW_ERR_FSIO_BUSY;    // frozen, or a fenced finalize's (§3.7)
  }
  auto* o = h.find(caller);
  if (!o || !o->chunk_sets.count(cs) || o->chunk_sets[cs].writers == 0) {
    return -EINVAL;
  }
  add_run(e->shadow->extent, run, max_runs);
  return 0;
}

namespace {

/* the shadow a writer reports on:  it holds a writer open there, as
 * fsio_extend requires */
cls_rgw_fsio_chunk_set* writer_shadow(cls_rgw_fsio_head& h,
				      const cls_rgw_fsio_handle& caller,
				      const cls_rgw_fsio_id& cs)
{
  if (!h.hdr) {
    return nullptr;
  }
  auto* e = h.hdr->find(cs);
  if (!e || !e->shadow) {
    return nullptr;
  }
  auto* o = h.find(caller);
  if (!o || !o->chunk_sets.count(cs) || o->chunk_sets[cs].writers == 0) {
    return nullptr;
  }
  return e;
}

} // anonymous namespace

int hint(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	 const cls_rgw_fsio_id& cs, uint64_t trunc_epoch_seen, uint64_t size,
	 ceph::real_time mtime, hint_out& out)
{
  out = hint_out{};
  auto* e = writer_shadow(h, caller, cs);
  if (!e) {
    return -EINVAL;
  }
  auto& sh = *e->shadow;
  /* W14:  a publishing shadow's size is final;  the hint changes nothing
   * and returns as usual */
  if (e->state != state::PUBLISHING ||
      FSIO_SKIP_CHECK(W14_PUBLISHING_FREEZES_SIZE)) {
    if (trunc_epoch_seen == sh.trunc_epoch ||
	FSIO_SKIP_CHECK(HINT_STALE_MERGES_NOTHING)) {
      sh.size_hint = std::max(sh.size_hint, size);
      sh.mtime_hint = std::max(sh.mtime_hint, mtime);
    } else {
      out.stale = true;
    }
  }
  out.trunc_epoch = sh.trunc_epoch;
  out.size = sh.size_hint;
  out.others = others_bound(h, caller, cs);
  return 0;
}

int truncate(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	     uint64_t seq, const cls_rgw_fsio_id& cs, uint64_t size,
	     ceph::real_time mtime, truncate_out& out)
{
  out = truncate_out{};
  if (already_applied(h, caller, seq)) {
    out.already_applied = true;
    return 0;
  }
  auto* e = writer_shadow(h, caller, cs);
  if (!e) {
    return -EINVAL;
  }
  if (e->state == state::PUBLISHING &&
      !FSIO_SKIP_CHECK(W14_PUBLISHING_FREEZES_SIZE)) {
    return -CLS_RGW_ERR_FSIO_BUSY;    // W14, as fsio_extend
  }
  auto& sh = *e->shadow;
  sh.size_hint = size;
  sh.mtime_hint = mtime;
  ++sh.trunc_epoch;
  sh.trunc_low = sh.trunc_low ? std::min(*sh.trunc_low, size) : size;
  out.trunc_epoch = sh.trunc_epoch;
  applied(h, caller, seq);
  return 0;
}

namespace {

int leave_once(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	       const cls_rgw_fsio_id& cs, bool writer, const expired_t& expired,
	       leave_out& out, cls_rgw_fsio_effects& fx,
	       const publish_intent* intent, bool handoff, bool discard)
{
  if (!h.hdr) {
    return -EINVAL;
  }
  expire_readers(h, expired);
  auto* o = h.find(caller);
  if (!o || !o->chunk_sets.count(cs)) {
    return -EINVAL;
  }
  auto& c = o->chunk_sets[cs];
  auto* e = h.hdr->find(cs);
  /* versioned:  the version of the content the open read (§2.5, W7) */
  if (e && !FSIO_SKIP_CHECK(W7_LEAVE_RETURNS_VERSION_READ)) {
    if (e->manifest_at == at::INSTANCE && e->instance) {
      out.instance = *e->instance;
    } else if (e->publishing) {
      out.instance = e->publishing->instance;
    }
  }
  if (writer) {
    if (c.writers == 0 || !e) {
      return -EINVAL;
    }
    if (handoff) {
      /* §3.4, 8j:  a hand-off:  the writer's open goes and the publish is
       * left uncompleted, nobody holding it:  a ready shadow unfrozen, a
       * written versioned one still publishing */
      if (e->state == state::READY) {
	if (h.bound(cs).writers == 1) {
	  e->uncompleted_publish = c.writer_identity;
	}
      } else if (e->state == state::PUBLISHING && e->publishing &&
		 e->publishing->publisher == caller) {
	e->uncompleted_publish = e->publishing->identity;
      } else {
	return -EINVAL;
      }
      --c.writers;
      std::vector<cls_rgw_fsio_id> drained;
      settle(h, drained, fx, caller);
      if (!drained.empty()) {
	out.drained = drained.front();
      }
      return 0;
    }
    if (h.bound(cs).writers == 1) {
      /* §3.4:  a constructor that gives up abandons its shadow, its GC
       * entry already sent (W15) */
      if (e->state == state::CONSTRUCTING &&
	  !FSIO_SKIP_CHECK(W15_CONSTRUCTOR_MAY_ABANDON)) {
	auto& v = h.hdr->chunk_sets;
	v.erase(std::remove_if(v.begin(), v.end(),
			       [&](const auto& x) { return x.id == cs; }),
		v.end());
	if (h.hdr->cur_shadow && *h.hdr->cur_shadow == cs) {
	  h.hdr->cur_shadow.reset();
	}
	o->chunk_sets.erase(cs);
	out.abandoned = true;
	std::vector<cls_rgw_fsio_id> drained;
	settle(h, drained, fx, caller);
	return 0;
      }
      /* §3.4:  the last writer freezes its shadow;  only a ready shadow
       * can enter publishing (§4.2) */
      if (e->state != state::READY) {
	return -EINVAL;
      }
      if (discard) {
	/* §3.4, §3.5 item 4, 8h:  its publish refused:  retired as
	 * superseded content is, drained once nothing is bound to it */
	e->state = state::RETIRED;
	if (h.hdr->cur_shadow && *h.hdr->cur_shadow == cs) {
	  h.hdr->cur_shadow.reset();
	}
	--c.writers;
	std::vector<cls_rgw_fsio_id> drained;
	settle(h, drained, fx, caller);
	if (!drained.empty()) {
	  out.drained = drained.front();
	}
	return 0;
      }
      e->state = state::PUBLISHING;
      e->publishing.emplace();
      e->publishing->publisher = caller;
      e->publishing->identity = c.writer_identity;   // §3.5, Z14
      if (intent) {
	/* versioned:  the claim and the OSD's epoch (§4.2 step 2) */
	e->publishing->instance = intent->instance;
	e->publishing->token = intent->token;
	e->publishing->olh_epoch = intent->now_epoch;
	out.olh_epoch = intent->now_epoch;
	if (!FSIO_SKIP_CHECK(W7_LEAVE_RETURNS_VERSION_READ)) {
	  out.instance = intent->instance;
	}
      }
      out.publishing = true;
      return 0;
    }
    --c.writers;
  } else {
    if (c.readers == 0) {
      return -EINVAL;
    }
    --c.readers;
  }
  std::vector<cls_rgw_fsio_id> drained;
  settle(h, drained, fx, caller);
  if (!drained.empty()) {
    out.drained = drained.front();
  }
  return 0;
}


int rebind_once(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
		const cls_rgw_fsio_id& from, const cls_rgw_fsio_id& to,
		uint32_t readers, rebind_out& out)
{
  if (!h.hdr) {
    return -EINVAL;
  }
  auto* f = h.hdr->find(from);
  auto* t = h.hdr->find(to);
  /* §3.3:  to the published chunk set (at INSTANCE when versioned, A18)
   * or the current shadow, ready or publishing */
  if (!f || !t ||
      !(t == published_chunk_set(*h.hdr) ||
	(h.hdr->cur_shadow && t->id == *h.hdr->cur_shadow &&
	 (t->state == state::READY || t->state == state::PUBLISHING)))) {
    return -EINVAL;
  }
  /* versioned:  no follower arrives between the publisher's read of the
   * readers and the publish (§3, W7).  A publish that claimed an
   * instance, a created name's included though its header names no
   * version;  or any over a header naming one, a null current version
   * counting, since the header cannot tell suspended from enabled */
  if (t->state == state::PUBLISHING && t->publishing &&
      (!t->publishing->instance.empty() || !h.hdr->cur_instance.empty()) &&
      !FSIO_SKIP_CHECK(W7_REBIND_WAITS_ON_PUBLISHING)) {
    return -CLS_RGW_ERR_FSIO_BUSY;
  }
  auto* o = h.find(caller);
  if (!o || !o->chunk_sets.count(from) ||
      o->chunk_sets[from].readers < readers) {
    return -EINVAL;
  }
  o->chunk_sets[from].readers -= readers;
  o->chunk_sets[to].readers += readers;
  /* §3.4, A19:  what empties is handled as fsio_leave handles it;  a
   * retired chunk set's drain goes to the caller */
  std::vector<cls_rgw_fsio_id> drained;
  cls_rgw_fsio_effects fx;
  settle(h, drained, fx, caller);
  if (!drained.empty()) {
    out.drained = drained.front();
  }
  return 0;
}

} // anonymous namespace

/* §3.5, §4.2:  a versioned publish, the standalone call on the OLH once
 * the version is linked:  nothing is superseded, the previous version
 * keeps its tail;  the complete form finishes a dead publisher's */
int publish_versioned(cls_rgw_fsio_head& h, const replace_in& in,
		      replace_out& out, cls_rgw_fsio_effects& fx)
{
  if (!h.hdr) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;
  }
  auto& hdr = *h.hdr;
  auto* sh = hdr.find(in.shadow);
  if (!sh || sh->state != state::PUBLISHING || !sh->publishing) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;
  }
  const auto publisher = sh->publishing->publisher;
  /* complete:  a dead publisher's, or one handed off (8j) */
  if (in.complete ? (!in.expired.count(publisher.incarnation) &&
		     !sh->uncompleted_publish) :
      publisher != in.caller) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;
  }
  const auto sh_id = sh->id;
  hdr.created = false;               // the name is an S3 object (Z24, 8i)
  sh->state = state::PUBLISHED;
  sh->manifest_at = at::INSTANCE;
  sh->instance = sh->publishing->instance;
  sh->publishing.reset();
  sh->uncompleted_publish.reset();
  if (in.complete && !FSIO_SKIP_CHECK(V2_COMPLETE_REMOVES_DEAD_WRITERS)) {
    /* the writer opens of the dead:  the publisher's, whoever now holds
     * its publish (§4.2, V2) */
    for (auto& [inc, o] : h.opens) {
      if (!in.expired.count(inc)) {
	continue;
      }
      for (auto& [hd, ho] : o.handles) {
	auto c = ho.chunk_sets.find(sh_id);
	if (c != ho.chunk_sets.end()) {
	  c->second.writers = 0;
	}
      }
    }
  } else if (auto* o = h.find(publisher)) {
    auto c = o->chunk_sets.find(sh_id);
    if (c != o->chunk_sets.end() && c->second.writers) {
      --c->second.writers;
    }
  }
  if (auto* ff = hdr.find_finalize(sh_id);
      ff && ff->to == sh_id && !FSIO_SKIP_CHECK(W3_PUBLISHING_TO_RETIRES_FROM)) {
    auto* f = hdr.find(ff->from);
    f->state = state::RETIRED;     // manifest_at stays SHADOW
    f->publishing.reset();
  }
  auto& ffs = hdr.fenced_finalizes;
  ffs.erase(std::remove_if(ffs.begin(), ffs.end(),
			   [&](const auto& ff) { return ff.to == sh_id; }),
	    ffs.end());
  if (!FSIO_SKIP_CHECK(CUR_SHADOW_PUBLISH)) {
    hdr.cur_shadow.reset();
  }
  out.keep_tail = true;
  std::vector<cls_rgw_fsio_id> drained;
  settle(h, drained, fx, in.caller);
  if (!drained.empty()) {
    out.drained = drained.front();
  }
  return 0;
}

int replace_head(cls_rgw_fsio_head& h, const replace_in& in, replace_out& out,
		 cls_rgw_fsio_effects& fx)
{
  if (in.mode == replace_mode::DELETE && in.expected_id_tag != h.id_tag) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;   // §3.6, finding Z7
  }
  if (in.mode == replace_mode::PUBLISH && in.versioned) {
    return publish_versioned(h, in, out, fx);
  }
  if (in.mode == replace_mode::PUBLISH) {
    if (!h.hdr) {
      return -CLS_RGW_ERR_FSIO_CONFLICT;
    }
    auto& hdr = *h.hdr;
    auto* sh = hdr.find(in.shadow);
    if (!sh || sh->state != state::PUBLISHING || !sh->publishing ||
	sh->publishing->publisher != in.caller) {
      return -CLS_RGW_ERR_FSIO_CONFLICT;
    }
    const auto sh_id = sh->id;
    /* §3.5, C1:  the tags kept must be the tags the publisher evaluated;
     * a tag write since its read fails the publish, which reads again */
    auto keep = fsio_publish_tags(!h.id_tag.empty(), h.tags, in.tags);
    if (in.expected_tags && !FSIO_SKIP_CHECK(C1_PUBLISH_COMPARES_TAGS) &&
	!in.expected_tags->contents_equal(keep)) {
      return -ECANCELED;
    }
    h.tags = std::move(keep);
    hdr.created = false;               // the name is an S3 object (Z24)
    /* the previous published chunk set:  the head's content, which the
     * publish replaces (§3.5;  the null version when suspended, §2.5a) */
    auto* prev = head_chunk_set(hdr);
    fx.clones.push_back(
      cls_rgw_fsio_clone{in.r, 'G', record_of_head(h)});
    /* §3.6:  the record holding the head's tail waits on its sharers */
    const auto sharers = FSIO_SKIP_CHECK(SHARED_TAIL_AFTER_LAST_READER) ?
      std::set<cls_rgw_fsio_id>{} : tail_sharers(h);
    if (prev && prev->id != sh_id &&
	(h.bound(prev->id).readers || h.bound(prev->id).writers)) {
      prev->state = state::RETIRED;
      prev->manifest_at = at::CLONE;
      prev->clone = in.r;
      prev->after = sharers;
    } else if (!sharers.empty()) {
      cls_rgw_fsio_chunk_set t;
      t.id = hdr.draw();
      t.state = state::RETIRED;
      t.manifest_at = at::CLONE;
      t.clone = in.r;
      t.after = sharers;
      hdr.chunk_sets.push_back(t);
    } else {
      out.born_drained = true;
    }
    end_tail_sharing(hdr);
    sh = hdr.find(sh_id);
    sh->state = state::PUBLISHED;
    sh->manifest_at = at::HEAD;
    sh->publishing.reset();
    sh->uncompleted_publish.reset();
    if (auto* o = h.find(in.caller)) {
      auto c = o->chunk_sets.find(sh_id);
      if (c != o->chunk_sets.end() && c->second.writers) {
	--c->second.writers;
      }
    }
    if (auto* ff = hdr.find_finalize(sh_id);
	ff && ff->to == sh_id && !FSIO_SKIP_CHECK(W3_PUBLISHING_TO_RETIRES_FROM)) {
      auto* f = hdr.find(ff->from);
      f->state = state::RETIRED;     // manifest_at stays SHADOW
      f->publishing.reset();
    }
    auto& ffs = hdr.fenced_finalizes;
    ffs.erase(std::remove_if(ffs.begin(), ffs.end(),
			     [&](const auto& ff) { return ff.to == sh_id; }),
	      ffs.end());
    /* the current shadow published, or a deferred one superseding the
     * current generation:  either way a new generation, no shadow yet */
    if (!FSIO_SKIP_CHECK(CUR_SHADOW_PUBLISH)) {
      hdr.cur_shadow.reset();
    }
    out.keep_tail = true;
    std::vector<cls_rgw_fsio_id> drained;
    settle(h, drained, fx, in.caller);
    if (!drained.empty()) {
      out.drained = drained.front();
    }
    return 0;
  }

  /* REMOVE_NULL (§2.5a):  the null version removed from the head object;
   * its readers are the HEAD entry's, so the header's mechanism retires
   * it, and no open count is involved */
  if (in.mode == replace_mode::REMOVE_NULL) {
    out.keep_tail = false;
    if (!h.hdr || FSIO_SKIP_CHECK(NULL_REMOVAL_RETIRES)) {
      return 0;
    }
    auto& hdr = *h.hdr;
    if (auto* p = head_chunk_set(hdr)) {
      auto b = h.bound(p->id);
      if (b.readers || b.writers) {
	fx.clones.push_back(
	  cls_rgw_fsio_clone{in.r, 'G', record_of_head(h)});
	p->state = state::RETIRED;
	p->manifest_at = at::CLONE;
	p->clone = in.r;
	out.keep_tail = true;
      }
    }
    if (is_null(hdr.cur_instance)) {
      hdr.cur_shadow.reset();        // the current version gone
    }
    return 0;
  }

  /* SUPERSEDE, DELETE:  the head object's content replaced;  its tags
   * go with it (the recreated head keeps the OLH attributes only);  an
   * S3 PUT makes a created name an S3 object (Z24) */
  h.tags.clear();
  if (h.hdr && in.mode == replace_mode::SUPERSEDE) {
    h.hdr->created = false;
  }
  if (!h.hdr) {
    out.keep_tail = false;
    return 0;
  }
  auto& hdr = *h.hdr;
  auto* p = head_chunk_set(hdr);
  bool kept = false;
  const bool keeps = in.mode == replace_mode::SUPERSEDE && in.keep_tail;
  /* §3.6:  a replacement that does not keep the tail ends the sharing;
   * the record holding the tail waits on the sharers */
  const auto sharers = keeps || FSIO_SKIP_CHECK(SHARED_TAIL_AFTER_LAST_READER) ?
    std::set<cls_rgw_fsio_id>{} : tail_sharers(h);
  if (p && (h.bound(p->id).readers || h.bound(p->id).writers)) {
    /* §3.6, Y2:  an append or a copy onto itself reuses the tail;  the
     * record names none, so the drain collects the clone only */
    auto rec = record_of_head(h);
    if (keeps && !FSIO_SKIP_CHECK(A26_KEEP_TAIL_RECORDS_NO_MANIFEST)) {
      rec.manifest.clear();
    }
    fx.clones.push_back(cls_rgw_fsio_clone{in.r, 'G', rec});
    p->state = state::RETIRED;
    p->manifest_at = at::CLONE;
    p->clone = in.r;
    p->shares_tail = keeps;
    p->after = sharers;
    kept = true;
  } else if (!sharers.empty()) {
    fx.clones.push_back(cls_rgw_fsio_clone{in.r, 'G', record_of_head(h)});
    cls_rgw_fsio_chunk_set t;
    t.id = hdr.draw();
    t.state = state::RETIRED;
    t.manifest_at = at::CLONE;
    t.clone = in.r;
    t.after = sharers;
    hdr.chunk_sets.push_back(t);
    kept = true;
  }
  if (!keeps) {
    end_tail_sharing(hdr);
  }
  if (!FSIO_SKIP_CHECK(CUR_SHADOW_SUPERSEDE)) {
    hdr.cur_shadow.reset();        // a new generation
  }
  out.keep_tail = kept;
  if (in.mode == replace_mode::DELETE && !h.holds_opens() && hdr.empty()) {
    h.hdr.reset();
  }
  return 0;
}

int take_uncompleted(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
		     const cls_rgw_fsio_id& cs, const publish_intent* intent)
{
  if (!h.hdr) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;
  }
  auto* e = h.hdr->find(cs);
  /* a writer bound since the hand-off:  its close publishes */
  if (!e || e->state != state::READY || !e->uncompleted_publish ||
      h.bound(cs).writers) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;
  }
  e->state = state::PUBLISHING;
  e->publishing.emplace();
  e->publishing->publisher = caller;
  e->publishing->identity = *e->uncompleted_publish;
  if (intent) {
    e->publishing->instance = intent->instance;
    e->publishing->token = intent->token;
    e->publishing->olh_epoch = intent->now_epoch;
  }
  return 0;
}

int publish_release(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
		    const cls_rgw_fsio_id& cs)
{
  if (!h.hdr) {
    return -EINVAL;
  }
  auto* e = h.hdr->find(cs);
  /* §2.2, A3:  idempotent by its effect:  a repeat finding the shadow
   * ready, no publish recorded, changes nothing and succeeds */
  if (e && e->state == state::READY && !e->publishing) {
    return 0;
  }
  if (!e || e->state != state::PUBLISHING) {
    return -EINVAL;
  }
  /* §3:  only the recorded publisher releases, as fsio_publish checks
   * (W20) */
  if ((!e->publishing || e->publishing->publisher != caller) &&
      !FSIO_SKIP_CHECK(W20_RELEASE_CHECKS_PUBLISHER)) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;
  }
  e->state = state::READY;
  e->publishing.reset();
  return 0;
}

int forget(cls_rgw_fsio_head& h, const cls_rgw_fsio_id& cs,
	   cls_rgw_fsio_effects& fx, std::optional<cls_rgw_fsio_id>* out)
{
  if (!h.hdr) {
    return 0;
  }
  auto& v = h.hdr->chunk_sets;
  v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& e) {
	    return e.id == cs && e.drained;
	  }),
	  v.end());
  std::vector<cls_rgw_fsio_id> drained;
  settle(h, drained, fx);
  if (!out) {
    return 0;
  }
  if (FSIO_SKIP_CHECK(FORGET_REPORTS_DRAIN_BY_STATE)) {
    if (!drained.empty()) {
      *out = drained.front();
    }
    return 0;
  }
  /* §3.4:  the drained chunk set that waited on this one, whichever
   * application drained it, so a resend returns what the first did */
  if (h.hdr) {
    for (const auto& e : h.hdr->chunk_sets) {
      if (e.drained &&
	  std::find(e.after.begin(), e.after.end(), cs) != e.after.end()) {
	*out = e.id;
	break;
      }
    }
  }
  return 0;
}

int fenced_finalize(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& finalizer,
		    const cls_rgw_fsio_id& from, const expired_t& expired,
		    cls_rgw_fsio_id& to, const publish_intent* intent,
		    const std::string& aborted)
{
  if (!h.hdr) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;   // no entry (§3.7 step 1)
  }
  auto& hdr = *h.hdr;
  if (auto* ff = hdr.find_finalize(from)) {
    if (FSIO_SKIP_CHECK(W3_RESUMED_FINALIZE_TAKES_OVER) || FSIO_SKIP_CHECK(W3_FINALIZE_CREATES_TO_PUBLISHING)) {
      to = ff->to;            // resumed without taking over
      return 0;
    }
    auto* t = hdr.find(ff->to);
    /* §3.7:  an intent equal to `to`'s recorded claim is a resend of the
     * call that recorded it */
    if (intent && t->publishing->instance == intent->instance &&
	t->publishing->token == intent->token &&
	!FSIO_SKIP_CHECK(FINALIZE_RESEND_RETURNS_TO)) {
      to = ff->to;
      return 0;
    }
    const auto prev = t->publishing->publisher;
    if (prev != finalizer && !expired.count(prev.incarnation)) {
      return -CLS_RGW_ERR_FSIO_BUSY;    // a live finalizer
    }
    /* versioned, resumed under a fresh claim:  the caller aborted `to`'s
     * recorded one (§3.7 step 4) */
    if (intent && !FSIO_SKIP_CHECK(RESUME_REPLACES_CLAIM)) {
      if (t->publishing->instance.empty() ||
	  aborted != t->publishing->instance) {
	return -CLS_RGW_ERR_FSIO_CONFLICT;
      }
      t->publishing->instance = intent->instance;
      t->publishing->token = intent->token;
      t->publishing->olh_epoch = intent->now_epoch;
    }
    t->publishing->publisher = finalizer;    // taken over (§3.7)
    hdr.find(ff->from)->publishing->publisher = finalizer;
    to = ff->to;
    return 0;
  }
  /* §3.7 step 1:  every condition that justifies a finalize, checked in
   * this op, since the expirer decided from an earlier read (W5) */
  auto* f = hdr.find(from);
  if (!f) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;
  }
  if (f->state == state::READY) {
    if (h.bound(from).writers == 0) {
      return -CLS_RGW_ERR_FSIO_CONFLICT;
    }
  } else if (f->state == state::PUBLISHING) {
    if (!f->publishing ||
	(!expired.count(f->publishing->publisher.incarnation) &&
	 f->publishing->publisher != finalizer)) {
      return -CLS_RGW_ERR_FSIO_CONFLICT;
    }
    /* versioned, a publish that claimed an instance:  its claim aborted
     * first (§3.7, V1);  a created name's header names no version */
    if (!f->publishing->instance.empty() &&
	aborted != f->publishing->instance &&
	!FSIO_SKIP_CHECK(V1_FINALIZE_REQUIRES_ABORTED_CLAIM)) {
      return -CLS_RGW_ERR_FSIO_CONFLICT;
    }
  } else if (!FSIO_SKIP_CHECK(W5_FINALIZE_RECHECKS_STATE)) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;   // constructing (§3.8), published, retired
  }
  for (const auto& [inc, o] : h.opens) {
    for (const auto& [hd, ho] : o.handles) {
      auto c = ho.chunk_sets.find(from);
      if (c != ho.chunk_sets.end() && c->second.writers &&
	  !expired.count(inc)) {
	return -CLS_RGW_ERR_FSIO_CONFLICT;
      }
    }
  }
  /* §3.5, Z14:  the publishing identity, the dead writer's that joined
   * last, chosen here, in the op that removes the records it is chosen
   * from, and kept in the publishing records for any finalizer */
  std::string identity = f->publishing ? f->publishing->identity : "";
  uint64_t last = 0;
  for (const auto& [inc, o] : h.opens) {
    for (const auto& [hd, ho] : o.handles) {
      auto c = ho.chunk_sets.find(from);
      if (c != ho.chunk_sets.end() && c->second.writers &&
	  c->second.joined_seq >= last) {
	last = c->second.joined_seq;
	identity = c->second.writer_identity;
      }
    }
  }
  for (auto& [inc, o] : h.opens) {
    for (auto& [hd, ho] : o.handles) {
      auto c = ho.chunk_sets.find(from);
      if (c != ho.chunk_sets.end()) {
	c->second.writers = 0;
      }
    }
  }
  const auto f_extent = f->shadow ? f->shadow->extent :
    std::vector<cls_rgw_fsio_run>{};
  cls_rgw_fsio_chunk_set t;
  t.id = hdr.draw();
  t.state = state::PUBLISHING;
  t.manifest_at = at::SHADOW;
  t.shadow.emplace();
  t.shadow->extent = f_extent;
  if (FSIO_SKIP_CHECK(W3_FINALIZE_CREATES_TO_PUBLISHING)) {
    t.state = state::CONSTRUCTING;
  } else {
    t.publishing.emplace();
    t.publishing->publisher = finalizer;
    t.publishing->identity = identity;
    if (intent) {
      /* versioned:  the claim, created first (§3.7 step 1) */
      t.publishing->instance = intent->instance;
      t.publishing->token = intent->token;
      t.publishing->olh_epoch = intent->now_epoch;
    }
  }
  to = t.id;
  hdr.chunk_sets.push_back(t);
  hdr.fenced_finalizes.push_back(cls_rgw_fsio_fenced_finalize{from, to});
  f = hdr.find(from);
  f->state = state::PUBLISHING;
  f->publishing.emplace();
  f->publishing->publisher = finalizer;
  f->publishing->identity = identity;
  return 0;
}

int expire(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& caller,
	   const expired_t& expired,
	   const std::optional<cls_rgw_fsio_id>& clear, expire_out& out,
	   cls_rgw_fsio_effects& fx)
{
  /* §3.8, A29:  a clear is justified only by a dead constructor;  checked
   * in this op, before any change, as a fenced finalize checks (W5) */
  if (clear && h.hdr && h.hdr->find(*clear) &&
      !FSIO_SKIP_CHECK(A29_CLEAR_RECHECKS_CONSTRUCTOR)) {
    for (const auto& [inc, o] : h.opens) {
      for (const auto& [hd, ho] : o.handles) {
	auto c = ho.chunk_sets.find(*clear);
	if (c != ho.chunk_sets.end() && c->second.writers &&
	    !expired.count(inc)) {
	  return -CLS_RGW_ERR_FSIO_CONFLICT;
	}
      }
    }
  }
  expire_readers(h, expired);
  /* §3.8:  take over the dead incarnations' publishes in flight (W6) */
  if (h.hdr && !FSIO_SKIP_CHECK(W6_EXPIRE_TAKES_OVER_PUBLISH)) {
    for (auto& e : h.hdr->chunk_sets) {
      if (e.state == state::PUBLISHING && e.publishing &&
	  expired.count(e.publishing->publisher.incarnation)) {
	e.publishing->publisher = caller;
      }
    }
  }
  if (clear && h.hdr) {
    auto& v = h.hdr->chunk_sets;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& e) {
	      return e.id == *clear && e.state == state::CONSTRUCTING;
	    }),
	    v.end());
    if (!FSIO_SKIP_CHECK(CUR_SHADOW_ABANDON) &&
	h.hdr->cur_shadow && *h.hdr->cur_shadow == *clear &&
	!h.hdr->find(*clear)) {
      h.hdr->cur_shadow.reset();
    }
    for (auto& [inc, o] : h.opens) {
      for (auto& [hd, ho] : o.handles) {
	ho.chunk_sets.erase(*clear);
      }
    }
  }
  settle(h, out.drained, fx, caller);
  /* §3.8, A28:  a ready shadow whose writers are all presumed dead, or a
   * publishing one whose publisher is, this call's takeover included */
  if (h.hdr) {
    for (const auto& e : h.hdr->chunk_sets) {
      if (e.state == state::PUBLISHING && e.publishing &&
	  (expired.count(e.publishing->publisher.incarnation) ||
	   e.publishing->publisher == caller)) {
	out.finalize = true;
      }
      if (e.state == state::READY && h.bound(e.id).writers) {
	bool live = false;
	for (const auto& [inc, o] : h.opens) {
	  for (const auto& [k, ho] : o.handles) {
	    auto c = ho.chunk_sets.find(e.id);
	    if (c != ho.chunk_sets.end() && c->second.writers &&
		!expired.count(inc)) {
	      live = true;
	    }
	  }
	}
	out.finalize |= !live;
      }
    }
  }
  return 0;
}

int drop_seqs(cls_rgw_fsio_head& h, uint64_t incarnation,
	      cls_rgw_fsio_effects& fx)
{
  auto i = h.opens.find(incarnation);
  if (i == h.opens.end()) {
    return 0;
  }
  i->second.seqs.clear();
  std::vector<cls_rgw_fsio_id> drained;
  settle(h, drained, fx);              // the record goes if nothing else is in it
  return 0;
}

/* §2.5:  before joining on the OLH head, a gateway adds its incarnation;  a version
 * that is gone gains no opens */
int ocnt_add(cls_rgw_fsio_instance_head& ih, const cls_rgw_fsio_handle& holder,
	     ocnt_add_out* out)
{
  ocnt_add_out o;
  if (!ih.exists || (ih.ocnt && ih.ocnt->removed)) {
    o.gone = true;
    o.held = ih.ocnt && ih.ocnt->holders.count(holder);
    if (out) {
      *out = o;
    }
    return 0;
  }
  if (out) {
    *out = o;
  }
  if (ih.id_tag.empty()) {
    return -ENOENT;                  // a placeholder:  the OLH never names one
  }
  if (!ih.ocnt) {
    ih.ocnt.emplace();
  }
  ih.ocnt->holders.insert(holder);
  return 0;
}

namespace {

/* the count emptied:  with content deferred or the version removed, the
 * record stays until the caller has sent its GC entries (§3.4);  with
 * nothing deferred it goes (§2.2) */
void ocnt_settle(cls_rgw_fsio_instance_head& ih, ocnt_drain& out)
{
  if (!ih.ocnt || !ih.ocnt->holders.empty()) {
    return;
  }
  if (ih.ocnt->deferred.empty() && !ih.ocnt->removed) {
    ih.ocnt.reset();
    return;
  }
  out.drained = true;
  out.deferred = ih.ocnt->deferred;
  out.removed = ih.ocnt->removed;
}

} // anonymous namespace

/* §2.5:  when a gateway holds no FSIO open bound to this version */
int ocnt_remove(cls_rgw_fsio_instance_head& ih, const cls_rgw_fsio_handle& holder,
		ocnt_drain& out)
{
  if (!ih.ocnt) {
    return 0;
  }
  ih.ocnt->holders.erase(holder);
  ocnt_settle(ih, out);
  return 0;
}

/* §2.5:  the FSIO expirer removes an incarnation only if presumed dead */
int ocnt_expire(cls_rgw_fsio_instance_head& ih, const expired_t& expired,
		ocnt_drain& out)
{
  if (!ih.ocnt) {
    return 0;
  }
  /* every handle of an expired incarnation (Z4) */
  std::erase_if(ih.ocnt->holders, [&](const cls_rgw_fsio_handle& c) {
    return expired.count(c.incarnation) > 0;
  });
  ocnt_settle(ih, out);
  return 0;
}

/* after the drain's GC entries are sent;  a removed version's head, left
 * with no FSIO xattr and no ID tag, goes whole (§2.2) */
int ocnt_forget(cls_rgw_fsio_instance_head& ih, cls_rgw_fsio_effects& fx)
{
  if (!ih.ocnt || !ih.ocnt->holders.empty()) {
    return -EINVAL;
  }
  ih.ocnt.reset();
  if (ih.exists && ih.id_tag.empty()) {
    fx.remove_head = true;
  }
  return 0;
}

/* §2.5:  DELETE ?versionId= (and noncurrent expiration, apply_olh_log()):
 * with incarnations in the open count, the instance head is cloned to
 * its sibling, the content deferred and the version removed in one op;
 * without, the caller collects as today */
int rewrite_version(cls_rgw_fsio_instance_head& ih, const cls_rgw_fsio_id& r,
		    bool keep_tail, remove_out& out, cls_rgw_fsio_effects& fx)
{
  if (!ih.exists) {
    return -ENOENT;
  }
  out.keep_tail = false;
  if (!ih.ocnt || ih.ocnt->holders.empty() || ih.id_tag.empty() ||
      FSIO_SKIP_CHECK(A13_REWRITE_DEFERS)) {
    return 0;
  }
  cls_rgw_fsio_gen rec;
  if (!keep_tail) {
    rec.manifest = ih.manifest;
  }
  rec.gc_tag = gc_tag_of(ih.tail_tag, ih.id_tag);
  fx.clones.push_back(cls_rgw_fsio_clone{r, 'G', rec});
  ih.ocnt->deferred.push_back(cls_rgw_fsio_replaced{rec.manifest, rec.gc_tag, r});
  out.keep_tail = true;
  return 0;
}

int remove_version(cls_rgw_fsio_instance_head& ih, const cls_rgw_fsio_id& r,
		   remove_out& out, cls_rgw_fsio_effects& fx)
{
  if (!ih.exists || ih.id_tag.empty()) {
    return -ENOENT;
  }
  if (!ih.ocnt || ih.ocnt->holders.empty()) {
    out.keep_tail = false;
    ih = cls_rgw_fsio_instance_head{};
    return 0;
  }
  cls_rgw_fsio_gen rec;
  rec.manifest = ih.manifest;
  rec.gc_tag = gc_tag_of(ih.tail_tag, ih.id_tag);
  fx.clones.push_back(cls_rgw_fsio_clone{r, 'G', rec});
  ih.ocnt->deferred.push_back(cls_rgw_fsio_replaced{ih.manifest, rec.gc_tag, r});
  ih.ocnt->removed = true;
  ih.id_tag.clear();
  ih.tail_tag.clear();
  ih.manifest.clear();
  out.keep_tail = true;
  return 0;
}

} // namespace cls_rgw_fsio
