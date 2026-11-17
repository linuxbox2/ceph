// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "cls_rgw_fsio.h"

#include <algorithm>
#include <cerrno>

#include <fmt/format.h>

/* Each transition follows FSIO_RADOS_DESIGN.md as written (§3, §4), so
 * that the model checker measures the design;  a fix lands in the design
 * and here together. */

std::string cls_rgw_fsio_id::to_str() const
{
  return fmt::format("{:016x}{:016x}", counter, nonce);
}

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
    auto i = o.chunk_sets.find(cs);
    if (i != o.chunk_sets.end()) {
      sum.readers += i->second.readers;
      sum.writers += i->second.writers;
    }
  }
  return sum;
}

namespace cls_rgw_fsio {

#ifdef CLS_RGW_FSIO_SKIP_CHECKS
cls_rgw_fsio_check skipped_check = cls_rgw_fsio_check::NONE;
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
	    cls_rgw_fsio_effects& fx)
{
  for (auto i = h.opens.begin(); i != h.opens.end(); ) {
    auto& m = i->second.chunk_sets;
    for (auto j = m.begin(); j != m.end(); ) {
      if (j->second.readers == 0 && j->second.writers == 0) {
	j = m.erase(j);
      } else {
	++j;
      }
    }
    if (m.empty()) {
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
      if (idle && i->state == state::RETIRED && !i->drained) {
	i->drained = true;
	drained.push_back(i->id);
      }
      ++i;
    }
    if (h.opens.empty() && h.hdr->empty()) {
      h.hdr.reset();
    }
  }
  if (!h.hdr && h.opens.empty() && h.tracking_only()) {
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
    for (auto& [cs, c] : i->second.chunk_sets) {
      c.readers = 0;
    }
  }
}

cls_rgw_fsio_gen record_of_head(const cls_rgw_fsio_head& h)
{
  cls_rgw_fsio_gen rec;
  rec.manifest = h.manifest;
  rec.gc_tag = h.id_tag;
  return rec;
}

} // anonymous namespace

int join(cls_rgw_fsio_head& h, const join_in& in, join_out& out,
	 cls_rgw_fsio_effects& fx)
{
  /* 1. a tracking-only head:  the name is deleted */
  if (h.tracking_only()) {
    return -ENOENT;
  }
  if (!h.exists) {
    return -ENOENT;
  }
  /* 2. lazy expiry, readers only;  what it empties is removed or
   * drained (§2.2), a drain left to the FSIO expirer */
  expire_readers(h, in.expired);
  {
    std::vector<cls_rgw_fsio_id> drained;
    settle(h, drained, fx);
  }
  /* 3. */
  if (!h.hdr) {
    h.hdr.emplace();
    h.hdr->nonce = in.nonce;
    h.hdr->next_counter = in.counter_seed;
    h.hdr->cur_instance = in.instance;
  }
  auto& hdr = *h.hdr;
  /* §2.5a:  joining the null version is the unversioned join */
  const bool versioned = !is_null(in.instance);
  /* 4. versioned:  the OLH names another instance, a new current
   * generation;  the old version's published entry stays at INSTANCE */
  if (!in.instance.empty() && !same_instance(in.instance, hdr.cur_instance)) {
    hdr.cur_instance = in.instance;
    hdr.cur_shadow.reset();
  }
  /* 5. */
  {
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
  if (!in.writer) {
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
    } else {
      cls_rgw_fsio_chunk_set cs;
      cs.id = hdr.draw();
      cs.state = state::CONSTRUCTING;
      cs.manifest_at = at::SHADOW;
      cs.shadow.emplace();
      hdr.chunk_sets.push_back(cs);
      hdr.cur_shadow = cs.id;
      bind = cs.id;
      out.construct = true;
      /* 7. versioned, the OLH holds no content:  the gateway clones the
       * instance head itself (§4.1) */
      if (!versioned) {
	fx.clones.push_back(cls_rgw_fsio_clone{
	    cs.id, 'a', {},
	    FSIO_SKIP_CHECK(W1_SHADOW_STORES_PSEUDO_MANIFEST) ? ceph::buffer::list{} : h.manifest});
      }
    }
  }
  auto& c = h.opens[in.incarnation].chunk_sets[bind];
  if (in.writer) {
    ++c.writers;
  } else {
    ++c.readers;
  }
  out.bound = bind;
  return 0;
}

int ready(cls_rgw_fsio_head& h, uint64_t incarnation,
	  const cls_rgw_fsio_id& cs, uint64_t extent)
{
  if (!h.hdr) {
    return -EINVAL;
  }
  auto* e = h.hdr->find(cs);
  if (!e || e->state != state::CONSTRUCTING) {
    return -EINVAL;
  }
  auto o = h.opens.find(incarnation);
  if (o == h.opens.end() || !o->second.chunk_sets.count(cs) ||
      o->second.chunk_sets[cs].writers == 0) {
    return -EINVAL;
  }
  e->state = state::READY;
  e->shadow->extent = extent;
  return 0;
}

int extend(cls_rgw_fsio_head& h, uint64_t incarnation,
	   const cls_rgw_fsio_id& cs, uint64_t extent)
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
  auto o = h.opens.find(incarnation);
  if (o == h.opens.end() || !o->second.chunk_sets.count(cs) ||
      o->second.chunk_sets[cs].writers == 0) {
    return -EINVAL;
  }
  e->shadow->extent = std::max(e->shadow->extent, extent);
  return 0;
}

int leave(cls_rgw_fsio_head& h, uint64_t incarnation,
	  const cls_rgw_fsio_id& cs, bool writer, const expired_t& expired,
	  leave_out& out, cls_rgw_fsio_effects& fx,
	  const publish_intent* intent)
{
  if (!h.hdr) {
    return -EINVAL;
  }
  expire_readers(h, expired);
  auto o = h.opens.find(incarnation);
  if (o == h.opens.end() || !o->second.chunk_sets.count(cs)) {
    return -EINVAL;
  }
  auto& c = o->second.chunk_sets[cs];
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
    if (h.bound(cs).writers == 1) {
      /* §3.4:  the last writer freezes its shadow;  only a ready shadow
       * can enter publishing (§4.2) */
      if (e->state != state::READY) {
	return -EINVAL;
      }
      e->state = state::PUBLISHING;
      e->publishing.emplace();
      e->publishing->incarnation = incarnation;
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
  settle(h, drained, fx);
  if (!drained.empty()) {
    out.drained = drained.front();
  }
  return 0;
}

int rebind(cls_rgw_fsio_head& h, uint64_t incarnation,
	   const cls_rgw_fsio_id& from, const cls_rgw_fsio_id& to,
	   uint32_t readers)
{
  if (!h.hdr) {
    return -EINVAL;
  }
  auto* f = h.hdr->find(from);
  auto* t = h.hdr->find(to);
  if (!f || !t ||
      !(t->manifest_at == at::HEAD ||
	(h.hdr->cur_shadow && t->id == *h.hdr->cur_shadow &&
	 (t->state == state::READY || t->state == state::PUBLISHING)))) {
    return -EINVAL;
  }
  /* versioned:  no follower arrives between the publisher's read of the
   * readers and the publish (§3, W7) */
  if (!h.hdr->cur_instance.empty() && t->state == state::PUBLISHING &&
      !FSIO_SKIP_CHECK(W7_REBIND_WAITS_ON_PUBLISHING)) {
    return -CLS_RGW_ERR_FSIO_BUSY;
  }
  auto o = h.opens.find(incarnation);
  if (o == h.opens.end() || !o->second.chunk_sets.count(from) ||
      o->second.chunk_sets[from].readers < readers) {
    return -EINVAL;
  }
  o->second.chunk_sets[from].readers -= readers;
  o->second.chunk_sets[to].readers += readers;
  std::vector<cls_rgw_fsio_id> drained;
  cls_rgw_fsio_effects fx;
  settle(h, drained, fx);
  return 0;
}

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
  const auto publisher = sh->publishing->incarnation;
  if (in.complete ? !in.expired.count(publisher) :
      publisher != in.incarnation) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;
  }
  const auto sh_id = sh->id;
  sh->state = state::PUBLISHED;
  sh->manifest_at = at::INSTANCE;
  sh->instance = sh->publishing->instance;
  sh->publishing.reset();
  if (in.complete && !FSIO_SKIP_CHECK(V2_COMPLETE_REMOVES_DEAD_WRITERS)) {
    /* the writer opens of the dead:  the publisher's, whoever now holds
     * its publish (§4.2, V2) */
    for (auto& [inc, o] : h.opens) {
      auto c = o.chunk_sets.find(sh_id);
      if (c != o.chunk_sets.end() && in.expired.count(inc)) {
	c->second.writers = 0;
      }
    }
  } else if (auto o = h.opens.find(publisher); o != h.opens.end()) {
    auto c = o->second.chunk_sets.find(sh_id);
    if (c != o->second.chunk_sets.end() && c->second.writers) {
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
  settle(h, drained, fx);
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
	sh->publishing->incarnation != in.incarnation) {
      return -CLS_RGW_ERR_FSIO_CONFLICT;
    }
    const auto sh_id = sh->id;
    /* the previous published chunk set:  the head's content, which the
     * publish replaces (§3.5;  the null version when suspended, §2.5a) */
    auto* prev = head_chunk_set(hdr);
    fx.clones.push_back(
      cls_rgw_fsio_clone{in.r, 'g', record_of_head(h)});
    if (prev && prev->id != sh_id) {
      auto b = h.bound(prev->id);
      if (b.readers || b.writers) {
	prev->state = state::RETIRED;
	prev->manifest_at = at::CLONE;
	prev->clone = in.r;
      } else {
	out.born_drained = true;
      }
    } else {
      out.born_drained = true;
    }
    sh = hdr.find(sh_id);
    sh->state = state::PUBLISHED;
    sh->manifest_at = at::HEAD;
    sh->publishing.reset();
    auto o = h.opens.find(in.incarnation);
    if (o != h.opens.end()) {
      auto c = o->second.chunk_sets.find(sh_id);
      if (c != o->second.chunk_sets.end() && c->second.writers) {
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
    settle(h, drained, fx);
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
	  cls_rgw_fsio_clone{in.r, 'g', record_of_head(h)});
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

  /* SUPERSEDE, DELETE:  the head object's content replaced */
  if (!h.hdr) {
    out.keep_tail = false;
    return 0;
  }
  auto& hdr = *h.hdr;
  auto* p = head_chunk_set(hdr);
  bool kept = false;
  if (p) {
    auto b = h.bound(p->id);
    if (b.readers || b.writers) {
      fx.clones.push_back(
	cls_rgw_fsio_clone{in.r, 'g', record_of_head(h)});
      p->state = state::RETIRED;
      p->manifest_at = at::CLONE;
      p->clone = in.r;
      kept = true;
    }
  }
  if (!FSIO_SKIP_CHECK(CUR_SHADOW_SUPERSEDE)) {
    hdr.cur_shadow.reset();        // a new generation
  }
  out.keep_tail = kept;
  if (in.mode == replace_mode::DELETE && h.opens.empty() && hdr.empty()) {
    h.hdr.reset();
  }
  return 0;
}

int publish_release(cls_rgw_fsio_head& h, const cls_rgw_fsio_id& cs)
{
  if (!h.hdr) {
    return -EINVAL;
  }
  auto* e = h.hdr->find(cs);
  if (!e || e->state != state::PUBLISHING) {
    return -EINVAL;
  }
  e->state = state::READY;
  e->publishing.reset();
  return 0;
}

int forget(cls_rgw_fsio_head& h, const cls_rgw_fsio_id& cs,
	   cls_rgw_fsio_effects& fx)
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
  return 0;
}

int fenced_finalize(cls_rgw_fsio_head& h, uint64_t finalizer,
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
    const auto prev = t->publishing->incarnation;
    if (prev != finalizer && !expired.count(prev)) {
      return -CLS_RGW_ERR_FSIO_BUSY;    // a live finalizer
    }
    t->publishing->incarnation = finalizer;    // taken over (§3.7)
    hdr.find(ff->from)->publishing->incarnation = finalizer;
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
    if (!f->publishing || (!expired.count(f->publishing->incarnation) &&
			   f->publishing->incarnation != finalizer)) {
      return -CLS_RGW_ERR_FSIO_CONFLICT;
    }
    /* versioned:  the publisher's claim aborted first (§3.7, V1) */
    if (!hdr.cur_instance.empty() && aborted != f->publishing->instance &&
	!FSIO_SKIP_CHECK(V1_FINALIZE_REQUIRES_ABORTED_CLAIM)) {
      return -CLS_RGW_ERR_FSIO_CONFLICT;
    }
  } else if (!FSIO_SKIP_CHECK(W5_FINALIZE_RECHECKS_STATE)) {
    return -CLS_RGW_ERR_FSIO_CONFLICT;   // constructing (§3.8), published, retired
  }
  for (const auto& [inc, o] : h.opens) {
    auto c = o.chunk_sets.find(from);
    if (c != o.chunk_sets.end() && c->second.writers &&
	!expired.count(inc)) {
      return -CLS_RGW_ERR_FSIO_CONFLICT;
    }
  }
  for (auto& [inc, o] : h.opens) {
    auto c = o.chunk_sets.find(from);
    if (c != o.chunk_sets.end()) {
      c->second.writers = 0;
    }
  }
  const auto f_extent = f->shadow ? f->shadow->extent : 0;
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
    t.publishing->incarnation = finalizer;
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
  f->publishing->incarnation = finalizer;
  return 0;
}

int expire(cls_rgw_fsio_head& h, uint64_t caller, const expired_t& expired,
	   const std::optional<cls_rgw_fsio_id>& clear, expire_out& out,
	   cls_rgw_fsio_effects& fx)
{
  expire_readers(h, expired);
  /* §3.8:  take over the dead incarnations' publishes in flight (W6) */
  if (h.hdr && !FSIO_SKIP_CHECK(W6_EXPIRE_TAKES_OVER_PUBLISH)) {
    for (auto& e : h.hdr->chunk_sets) {
      if (e.state == state::PUBLISHING && e.publishing &&
	  expired.count(e.publishing->incarnation)) {
	e.publishing->incarnation = caller;
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
      o.chunk_sets.erase(*clear);
    }
  }
  settle(h, out.drained, fx);
  return 0;
}

/* §2.5:  before the OLH join, a gateway adds its incarnation;  a version
 * that is gone gains no opens */
int ocnt_add(cls_rgw_fsio_instance_head& ih, uint64_t incarnation)
{
  if (!ih.exists || ih.id_tag.empty() || (ih.ocnt && ih.ocnt->removed)) {
    return -ENOENT;
  }
  if (!ih.ocnt) {
    ih.ocnt.emplace();
  }
  ih.ocnt->incarnations.insert(incarnation);
  return 0;
}

namespace {

/* the count emptied:  with content deferred or the version removed, the
 * record stays until the caller has sent its GC entries (§3.4);  with
 * nothing deferred it goes (§2.2) */
void ocnt_settle(cls_rgw_fsio_instance_head& ih, ocnt_drain& out)
{
  if (!ih.ocnt || !ih.ocnt->incarnations.empty()) {
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
int ocnt_remove(cls_rgw_fsio_instance_head& ih, uint64_t incarnation,
		ocnt_drain& out)
{
  if (!ih.ocnt) {
    return 0;
  }
  ih.ocnt->incarnations.erase(incarnation);
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
  for (auto inc : expired) {
    ih.ocnt->incarnations.erase(inc);
  }
  ocnt_settle(ih, out);
  return 0;
}

/* after the drain's GC entries are sent;  a removed version's head, left
 * with no FSIO xattr and no ID tag, goes whole (§2.2) */
int ocnt_forget(cls_rgw_fsio_instance_head& ih, cls_rgw_fsio_effects& fx)
{
  if (!ih.ocnt || !ih.ocnt->incarnations.empty()) {
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
int remove_version(cls_rgw_fsio_instance_head& ih, const cls_rgw_fsio_id& r,
		   remove_out& out, cls_rgw_fsio_effects& fx)
{
  if (!ih.exists || ih.id_tag.empty()) {
    return -ENOENT;
  }
  if (!ih.ocnt || ih.ocnt->incarnations.empty()) {
    out.keep_tail = false;
    ih = cls_rgw_fsio_instance_head{};
    return 0;
  }
  cls_rgw_fsio_gen rec;
  rec.manifest = ih.manifest;
  rec.gc_tag = ih.id_tag;
  fx.clones.push_back(cls_rgw_fsio_clone{r, 'g', rec});
  ih.ocnt->deferred.push_back(cls_rgw_fsio_replaced{ih.manifest, ih.id_tag, r});
  ih.ocnt->removed = true;
  ih.id_tag.clear();
  ih.manifest.clear();
  out.keep_tail = true;
  return 0;
}

} // namespace cls_rgw_fsio
