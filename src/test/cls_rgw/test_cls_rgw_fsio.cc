// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* Regression tests for the FSIO head transitions (cls_rgw_fsio.cc), one
 * per settled finding of docs/FSIO_RADOS_ADVERSARIAL_REVIEW.md;  each
 * drives the finding's sequence and asserts what its fix guarantees. */

#include "cls/rgw/cls_rgw_fsio.h"

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

namespace fsio = cls_rgw_fsio;
using state = cls_rgw_fsio_cs_state;

namespace {

constexpr uint64_t GW1 = 1, GW2 = 2, GW3 = 3, EXPIRER = 100;

cls_rgw_fsio_head head(const std::string& manifest)
{
  cls_rgw_fsio_head h;
  h.exists = true;
  h.id_tag = "t0";
  h.manifest.append(manifest);
  return h;
}

/* a writer's join that constructs a shadow, then fsio_ready */
cls_rgw_fsio_id construct(cls_rgw_fsio_head& h, uint64_t inc)
{
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  EXPECT_EQ(0, fsio::join(h, {inc, true, 1, 10, {}}, out, fx));
  EXPECT_TRUE(out.construct);
  EXPECT_EQ(0, fsio::ready(h, inc, out.bound, 1));
  return out.bound;
}

/* a reader's join, bound to the published chunk set */
cls_rgw_fsio_id join_reader(cls_rgw_fsio_head& h, uint64_t inc,
			    const std::string& instance = {})
{
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  EXPECT_EQ(0, fsio::join(h, {inc, false, 1, 10, {}, instance}, out, fx));
  return out.bound;
}

/* the last writer's leave:  the shadow goes publishing */
void leave_writer(cls_rgw_fsio_head& h, uint64_t inc,
		  const cls_rgw_fsio_id& cs)
{
  fsio::leave_out out;
  cls_rgw_fsio_effects fx;
  EXPECT_EQ(0, fsio::leave(h, inc, cs, true, {}, out, fx));
  EXPECT_TRUE(out.publishing);
}

int publish(cls_rgw_fsio_head& h, uint64_t inc, const cls_rgw_fsio_id& cs,
	    uint64_t r = 1000)
{
  fsio::replace_in in;
  in.mode = fsio::replace_mode::PUBLISH;
  in.r = cls_rgw_fsio_id{r, 0};
  in.incarnation = inc;
  in.shadow = cs;
  fsio::replace_out out;
  cls_rgw_fsio_effects fx;
  return fsio::replace_head(h, in, out, fx);
}

/* a writer join constructs a new shadow:  cur_shadow is clear */
cls_rgw_fsio_id join_writer_constructs(cls_rgw_fsio_head& h, uint64_t inc)
{
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  EXPECT_EQ(0, fsio::join(h, {inc, true, 1, 10, {}}, out, fx));
  EXPECT_TRUE(out.construct);
  return out.bound;
}

int supersede(cls_rgw_fsio_head& h, uint64_t r = 2000)
{
  fsio::replace_in in;
  in.mode = fsio::replace_mode::SUPERSEDE;
  in.r = cls_rgw_fsio_id{r, 0};
  fsio::replace_out out;
  cls_rgw_fsio_effects fx;
  return fsio::replace_head(h, in, out, fx);
}

} // anonymous namespace

/* W4:  after a deferred generation's publish, the published chunk set is
 * the entry at HEAD, whatever its history:  a new reader binds to it,
 * not to a second entry, and its readers can follow the next current
 * shadow (design §2.2).  No check id:  the fix removed generation ids,
 * the structure the defect needed, so there is no guard to skip. */
TEST(cls_rgw_fsio, W4_deferred_publish_keeps_one_published_entry)
{
  auto h = head("C0");
  auto s1 = construct(h, GW1);
  auto c0 = join_reader(h, GW1);
  ASSERT_EQ(0, fsio::rebind(h, GW1, c0, s1, 1));   // GW1's reader follows
  leave_writer(h, GW1, s1);                        // s1 publishing
  ASSERT_EQ(0, supersede(h));                      // an S3 PUT:  s1 deferred
  ASSERT_EQ(0, publish(h, GW1, s1));               // deferred publish
  EXPECT_EQ(s1, join_reader(h, GW2));              // binds to s1 at HEAD
  int at_head = 0;
  for (const auto& e : h.hdr->chunk_sets) {
    at_head += e.manifest_at == cls_rgw_fsio_manifest_at::HEAD;
  }
  EXPECT_EQ(1, at_head);
  /* the next current shadow is followable from s1 (rebind's half) */
  auto s2 = construct(h, GW2);
  EXPECT_EQ(0, fsio::rebind(h, GW1, s1, s2, 1));
}

/* W5:  the expirer decided a fenced finalize from an earlier read;  the
 * publisher, presumed dead, published since.  The call re-checks and
 * leaves the published shadow alone (design §3.7 step 1). */
TEST(cls_rgw_fsio, W5_finalize_after_zombie_publish_conflicts)
{
  auto h = head("C0");
  auto s = construct(h, GW1);
  leave_writer(h, GW1, s);
  ASSERT_EQ(0, publish(h, GW1, s));
  cls_rgw_fsio_id to;
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::fenced_finalize(h, EXPIRER, s, {GW1}, to));
  /* a published shadow with nothing bound has no entry (§2.2);  the
   * finalize must not have created one, nor a record */
  EXPECT_FALSE(h.hdr && h.hdr->find(s));
  EXPECT_FALSE(h.hdr && !h.hdr->fenced_finalizes.empty());
}

/* W5, the published entry kept alive by a reader following the shadow:
 * the call finds it published and leaves it so */
void w5_finalize_of_published_shadow_with_follower()
{
  auto h = head("C0");
  auto s = construct(h, GW1);
  auto c0 = join_reader(h, GW2);
  ASSERT_EQ(0, fsio::rebind(h, GW2, c0, s, 1));
  leave_writer(h, GW1, s);
  ASSERT_EQ(0, publish(h, GW1, s));
  ASSERT_TRUE(h.hdr && h.hdr->find(s));
  ASSERT_EQ(state::PUBLISHED, h.hdr->find(s)->state);
  cls_rgw_fsio_id to;
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::fenced_finalize(h, EXPIRER, s, {GW1}, to));
  const auto* e = h.hdr->find(s);
  ASSERT_TRUE(e);
  EXPECT_EQ(state::PUBLISHED, e->state);
  EXPECT_FALSE(e->publishing);
  EXPECT_TRUE(h.hdr->fenced_finalizes.empty());
}

TEST(cls_rgw_fsio, W5_finalize_of_published_shadow_with_follower_conflicts)
{
  w5_finalize_of_published_shadow_with_follower();
}

/* W6:  a writer's publish is in flight when it is presumed dead;  the
 * expirer's fsio_expire takes the publish over, so the zombie's publish
 * fails and the expirer may drop its hint, then finalize (design §3.8,
 * §4.3) */
void w6_expire_takes_over_zombie_publish()
{
  auto h = head("C0");
  auto s = construct(h, GW1);
  leave_writer(h, GW1, s);               // publishing under GW1
  {
    fsio::expire_out out;
    cls_rgw_fsio_effects fx;
    ASSERT_EQ(0, fsio::expire(h, EXPIRER, {GW1}, std::nullopt, out, fx));
  }
  const auto* e = h.hdr->find(s);
  ASSERT_TRUE(e && e->publishing);
  EXPECT_EQ(state::PUBLISHING, e->state);          // still frozen
  EXPECT_EQ(EXPIRER, e->publishing->incarnation);
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT, publish(h, GW1, s));
  cls_rgw_fsio_id to;
  EXPECT_EQ(0, fsio::fenced_finalize(h, EXPIRER, s, {GW1}, to));
}

TEST(cls_rgw_fsio, W6_expire_takes_over_zombie_publish)
{
  w6_expire_takes_over_zombie_publish();
}

/* W3:  a fenced finalize of a shadow whose writer is presumed dead:  to is
 * created publishing under the finalizer, so its publish succeeds, and
 * publishing it retires from (design §3.7) */
void w3_finalize_publishes_to()
{
  auto h = head("C0");
  auto s = construct(h, GW1);            // ready, GW1's writer bound
  cls_rgw_fsio_id to;
  ASSERT_EQ(0, fsio::fenced_finalize(h, EXPIRER, s, {GW1}, to));
  const auto* t = h.hdr->find(to);
  ASSERT_TRUE(t);
  EXPECT_EQ(state::PUBLISHING, t->state);
  EXPECT_EQ(0, publish(h, EXPIRER, to));
  const auto* f = h.hdr ? h.hdr->find(s) : nullptr;
  ASSERT_TRUE(f);                        // retired:  kept until collected
  EXPECT_EQ(state::RETIRED, f->state);
  EXPECT_TRUE(h.hdr->fenced_finalizes.empty());
}

TEST(cls_rgw_fsio, W3_finalize_publishes_to)
{
  w3_finalize_publishes_to();
}

/* W3:  the finalizer dies;  the next one takes the finalize over, the
 * dead one's publish fails, and a live one's call returns BUSY */
void w3_resumed_finalize_takes_over()
{
  constexpr uint64_t EXPIRER2 = 101, EXPIRER3 = 102;
  auto h = head("C0");
  auto s = construct(h, GW1);
  cls_rgw_fsio_id to, to2, to3;
  ASSERT_EQ(0, fsio::fenced_finalize(h, EXPIRER, s, {GW1}, to));
  ASSERT_EQ(0, fsio::fenced_finalize(h, EXPIRER2, s, {GW1, EXPIRER}, to2));
  EXPECT_EQ(to, to2);
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_BUSY,                 // EXPIRER2 is alive
	    fsio::fenced_finalize(h, EXPIRER3, s, {GW1}, to3));
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT, publish(h, EXPIRER, to));
  EXPECT_EQ(0, publish(h, EXPIRER2, to));
}

TEST(cls_rgw_fsio, W3_resumed_finalize_takes_over)
{
  w3_resumed_finalize_takes_over();
}

/* W1:  a shadow's pseudo-manifest is written with its attribute clone, at
 * construction, so it does not depend on its base's record:  a supersede
 * with only writers bound makes no record clone, and the shadow still
 * publishes (design §2.2, §2.4) */
void w1_shadow_carries_its_pseudo_manifest()
{
  auto h = head("C0");
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, true, 1, 10, {}}, out, fx));
  ASSERT_TRUE(out.construct);
  ASSERT_EQ(1u, fx.clones.size());
  EXPECT_EQ('a', fx.clones[0].role);
  EXPECT_EQ(out.bound, fx.clones[0].r);
  EXPECT_TRUE(fx.clones[0].pm.contents_equal(h.manifest));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, 1));
  leave_writer(h, GW1, out.bound);
  /* the S3 PUT finds only a writer bound:  no record of C0 is kept */
  fsio::replace_in in;
  in.mode = fsio::replace_mode::SUPERSEDE;
  in.r = cls_rgw_fsio_id{2000, 0};
  fsio::replace_out sout;
  cls_rgw_fsio_effects sfx;
  ASSERT_EQ(0, fsio::replace_head(h, in, sout, sfx));
  EXPECT_TRUE(sfx.clones.empty());
  EXPECT_FALSE(sout.keep_tail);
  /* the shadow publishes over the PUT's content all the same */
  EXPECT_EQ(0, publish(h, GW1, out.bound));
}

TEST(cls_rgw_fsio, W1_shadow_carries_its_pseudo_manifest)
{
  w1_shadow_carries_its_pseudo_manifest();
}

/* cur_shadow:  after an S3 PUT, a new writer joins new content, not the
 * superseded shadow (design §2.2) */
void cur_shadow_supersede_starts_a_generation()
{
  auto h = head("C0");
  auto s1 = construct(h, GW1);           // GW1 still writing
  ASSERT_EQ(0, supersede(h));
  EXPECT_NE(s1, join_writer_constructs(h, GW2));
}

TEST(cls_rgw_fsio, cur_shadow_supersede_starts_a_generation)
{
  cur_shadow_supersede_starts_a_generation();
}

/* cur_shadow:  after a publish, the next writer constructs from the
 * published content;  a follower keeps the published entry */
void cur_shadow_publish_clears_it()
{
  auto h = head("C0");
  auto s1 = construct(h, GW1);
  auto c0 = join_reader(h, GW2);
  ASSERT_EQ(0, fsio::rebind(h, GW2, c0, s1, 1));
  leave_writer(h, GW1, s1);
  ASSERT_EQ(0, publish(h, GW1, s1));
  EXPECT_NE(s1, join_writer_constructs(h, GW1));
}

TEST(cls_rgw_fsio, cur_shadow_publish_clears_it)
{
  cur_shadow_publish_clears_it();
}

/* cur_shadow:  an abandoned construction, cleared by the expirer, leaves
 * cur_shadow clear;  a reader keeps the header (design §3.8) */
void cur_shadow_abandon_clears_it()
{
  auto h = head("C0");
  join_reader(h, GW2);
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, true, 1, 10, {}}, out, fx));  // GW1 dies
  fsio::expire_out eout;
  cls_rgw_fsio_effects efx;
  ASSERT_EQ(0, fsio::expire(h, EXPIRER, {GW1}, out.bound, eout, efx));
  ASSERT_TRUE(h.hdr);
  EXPECT_FALSE(h.hdr->cur_shadow);
}

TEST(cls_rgw_fsio, cur_shadow_abandon_clears_it)
{
  cur_shadow_abandon_clears_it();
}

/* versioned transitions (design §2.5, §3.1, §3.4, §3.5, §4.2):  each one's
 * basic behaviour */

namespace {

cls_rgw_fsio_instance_head instance_head(const std::string& manifest)
{
  cls_rgw_fsio_instance_head ih;
  ih.exists = true;
  ih.id_tag = "i0";
  ih.manifest.append(manifest);
  return ih;
}

cls_rgw_fsio_head olh()
{
  cls_rgw_fsio_head h;
  h.exists = true;
  h.olh = true;                          // no ID tag:  not tracking-only
  return h;
}

} // anonymous namespace

TEST(cls_rgw_fsio_versioned, ocnt_add_and_remove)
{
  auto ih = instance_head("C1");
  ASSERT_EQ(0, fsio::ocnt_add(ih, GW1));
  ASSERT_EQ(0, fsio::ocnt_add(ih, GW1));           // idempotent
  ASSERT_EQ(0, fsio::ocnt_add(ih, GW2));
  fsio::ocnt_drain d;
  ASSERT_EQ(0, fsio::ocnt_remove(ih, GW1, d));
  EXPECT_FALSE(d.drained);
  ASSERT_TRUE(ih.ocnt);
  ASSERT_EQ(0, fsio::ocnt_remove(ih, GW2, d));
  EXPECT_FALSE(d.drained);                         // nothing deferred
  EXPECT_FALSE(ih.ocnt);                           // removed on empty (§2.2)
}

TEST(cls_rgw_fsio_versioned, ocnt_add_to_a_removed_version_fails)
{
  auto ih = instance_head("C1");
  ASSERT_EQ(0, fsio::ocnt_add(ih, GW1));
  fsio::remove_out ro;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::remove_version(ih, cls_rgw_fsio_id{3000, 0}, ro, fx));
  EXPECT_EQ(-ENOENT, fsio::ocnt_add(ih, GW2));
  cls_rgw_fsio_instance_head absent;
  EXPECT_EQ(-ENOENT, fsio::ocnt_add(absent, GW2));
}

TEST(cls_rgw_fsio_versioned, remove_version_with_open_count_defers)
{
  auto ih = instance_head("C1");
  ASSERT_EQ(0, fsio::ocnt_add(ih, GW1));
  fsio::remove_out ro;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::remove_version(ih, cls_rgw_fsio_id{3000, 0}, ro, fx));
  EXPECT_TRUE(ro.keep_tail);
  ASSERT_EQ(1u, fx.clones.size());                 // the sibling
  ASSERT_TRUE(ih.ocnt);
  EXPECT_TRUE(ih.ocnt->removed);
  ASSERT_EQ(1u, ih.ocnt->deferred.size());
  EXPECT_TRUE(ih.id_tag.empty());                  // tracking-only now
  fsio::ocnt_drain d;
  ASSERT_EQ(0, fsio::ocnt_remove(ih, GW1, d));
  EXPECT_TRUE(d.drained);
  EXPECT_TRUE(d.removed);
  EXPECT_EQ(1u, d.deferred.size());
  ASSERT_TRUE(ih.ocnt);                            // kept until sent
  cls_rgw_fsio_effects ffx;
  ASSERT_EQ(0, fsio::ocnt_forget(ih, ffx));
  EXPECT_TRUE(ffx.remove_head);                    // goes whole
}

TEST(cls_rgw_fsio_versioned, remove_version_without_open_count_collects)
{
  auto ih = instance_head("C1");
  fsio::remove_out ro;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::remove_version(ih, cls_rgw_fsio_id{3000, 0}, ro, fx));
  EXPECT_FALSE(ro.keep_tail);
  EXPECT_TRUE(fx.clones.empty());
  EXPECT_FALSE(ih.exists);
}

TEST(cls_rgw_fsio_versioned, ocnt_expire_removes_only_the_dead)
{
  auto ih = instance_head("C1");
  ASSERT_EQ(0, fsio::ocnt_add(ih, GW1));
  ASSERT_EQ(0, fsio::ocnt_add(ih, GW2));
  fsio::remove_out ro;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::remove_version(ih, cls_rgw_fsio_id{3000, 0}, ro, fx));
  fsio::ocnt_drain d;
  ASSERT_EQ(0, fsio::ocnt_expire(ih, {GW1}, d));
  EXPECT_FALSE(d.drained);
  ASSERT_EQ(0, fsio::ocnt_expire(ih, {GW2}, d));
  EXPECT_TRUE(d.drained);
}

TEST(cls_rgw_fsio_versioned, join_binds_to_the_named_instance)
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  fsio::join_in in{GW1, false, 1, 10, {}, "I1"};
  ASSERT_EQ(0, fsio::join(h, in, out, fx));
  const auto* e = h.hdr->find(out.bound);
  ASSERT_TRUE(e);
  EXPECT_EQ(cls_rgw_fsio_manifest_at::INSTANCE, e->manifest_at);
  EXPECT_EQ("I1", e->instance);
  /* a writer allocates the shadow;  the gateway clones the instance */
  fsio::join_out wout;
  cls_rgw_fsio_effects wfx;
  ASSERT_EQ(0, fsio::join(h, {GW1, true, 1, 10, {}, "I1"}, wout, wfx));
  EXPECT_TRUE(wout.construct);
  EXPECT_TRUE(wfx.clones.empty());
  /* the OLH now names I2:  a new current generation */
  fsio::join_out rout;
  cls_rgw_fsio_effects rfx;
  ASSERT_EQ(0, fsio::join(h, {GW2, false, 1, 10, {}, "I2"}, rout, rfx));
  EXPECT_EQ("I2", h.hdr->cur_instance);
  EXPECT_FALSE(h.hdr->cur_shadow);
  EXPECT_NE(out.bound, rout.bound);
}

TEST(cls_rgw_fsio_versioned, leave_records_the_publish_intent)
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, 1));
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, out.bound, true, {}, lout, lfx, &intent));
  EXPECT_TRUE(lout.publishing);
  EXPECT_EQ(77u, lout.olh_epoch);
  const auto* e = h.hdr->find(out.bound);
  ASSERT_TRUE(e && e->publishing);
  EXPECT_EQ("I2", e->publishing->instance);
  EXPECT_EQ("tok", e->publishing->token);
}

TEST(cls_rgw_fsio_versioned, publish_sets_the_shadow_published_at_its_instance)
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, 1));
  join_reader(h, GW2, "I1");                       // binds I1's entry
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, out.bound, true, {}, lout, lfx, &intent));
  fsio::replace_in in;
  in.mode = fsio::replace_mode::PUBLISH;
  in.versioned = true;
  in.incarnation = GW2;                            // not the publisher
  in.shadow = out.bound;
  fsio::replace_out rout;
  cls_rgw_fsio_effects rfx;
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::replace_head(h, in, rout, rfx));
  in.incarnation = GW1;
  ASSERT_EQ(0, fsio::replace_head(h, in, rout, rfx));
  EXPECT_TRUE(rfx.clones.empty());                 // nothing superseded
  EXPECT_FALSE(h.hdr->cur_shadow);
}

TEST(cls_rgw_fsio_versioned, complete_admits_only_a_dead_publisher)
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, 1));
  auto c0 = join_reader(h, GW2, "I1");
  ASSERT_EQ(0, fsio::rebind(h, GW2, c0, out.bound, 1));  // keeps the entry
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, out.bound, true, {}, lout, lfx, &intent));
  fsio::replace_in in;
  in.mode = fsio::replace_mode::PUBLISH;
  in.versioned = true;
  in.complete = true;
  in.incarnation = EXPIRER;
  in.shadow = out.bound;
  fsio::replace_out rout;
  cls_rgw_fsio_effects rfx;
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::replace_head(h, in, rout, rfx));    // GW1 is alive
  in.expired = {GW1};
  ASSERT_EQ(0, fsio::replace_head(h, in, rout, rfx));
  const auto* e = h.hdr->find(out.bound);
  ASSERT_TRUE(e);
  EXPECT_EQ(state::PUBLISHED, e->state);
  EXPECT_EQ("I2", e->instance);
  EXPECT_EQ(0u, h.bound(out.bound).writers);       // the dead writer's open
}

/* W7:  every versioned leave returns the version of the content the open
 * read, so a gateway removes itself from a version a publisher counted
 * it in (design §2.5, §3.4) */
void w7_leave_returns_the_version_read()
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, 1));
  auto c0 = join_reader(h, GW2, "I1");
  auto c1 = join_reader(h, GW3, "I1");
  ASSERT_EQ(0, fsio::rebind(h, GW2, c0, out.bound, 1));   // follows
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, out.bound, true, {}, lout, lfx, &intent));
  EXPECT_EQ("I2", lout.instance);
  fsio::leave_out f;
  ASSERT_EQ(0, fsio::leave(h, GW2, out.bound, false, {}, f, lfx));
  EXPECT_EQ("I2", f.instance);                     // a publishing shadow's
  fsio::leave_out r;
  ASSERT_EQ(0, fsio::leave(h, GW3, c1, false, {}, r, lfx));
  EXPECT_EQ("I1", r.instance);                     // its entry's version
}

TEST(cls_rgw_fsio_versioned, W7_leave_returns_the_version_read)
{
  w7_leave_returns_the_version_read();
}

/* W7:  no follower arrives between the publisher's read of the readers
 * and the publish (design §3) */
void w7_rebind_into_publishing_waits()
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, 1));
  auto c0 = join_reader(h, GW2, "I1");
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, out.bound, true, {}, lout, lfx, &intent));
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_BUSY, fsio::rebind(h, GW2, c0, out.bound, 1));
}

TEST(cls_rgw_fsio_versioned, W7_rebind_into_publishing_waits)
{
  w7_rebind_into_publishing_waits();
}

/* V1:  a versioned finalize of a publishing shadow proceeds only after
 * the publisher's claim, the recorded one, is aborted (design §3.7) */
void v1_finalize_requires_the_aborted_claim()
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, 1));
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, out.bound, true, {}, lout, lfx, &intent));
  fsio::publish_intent to_intent{"I3", "tok3", 78};
  cls_rgw_fsio_id to;
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::fenced_finalize(h, EXPIRER, out.bound, {GW1}, to,
				  &to_intent));            // not aborted
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::fenced_finalize(h, EXPIRER, out.bound, {GW1}, to,
				  &to_intent, "I9"));      // another claim
  EXPECT_EQ(0, fsio::fenced_finalize(h, EXPIRER, out.bound, {GW1}, to,
				     &to_intent, "I2"));
}

TEST(cls_rgw_fsio_versioned, V1_finalize_requires_the_aborted_claim)
{
  v1_finalize_requires_the_aborted_claim();
}

/* V2:  completing a publish the expirer took over removes the dead
 * writer's open (design §4.2) */
void v2_complete_after_takeover_removes_the_dead_writer()
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, 1));
  join_reader(h, GW2, "I1");                       // keeps the header
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, out.bound, true, {}, lout, lfx, &intent));
  {
    fsio::expire_out eout;
    cls_rgw_fsio_effects efx;
    ASSERT_EQ(0, fsio::expire(h, EXPIRER, {GW1}, std::nullopt, eout, efx));
  }
  fsio::replace_in in;
  in.mode = fsio::replace_mode::PUBLISH;
  in.versioned = true;
  in.complete = true;
  in.incarnation = EXPIRER;
  in.shadow = out.bound;
  in.expired = {GW1, EXPIRER};
  fsio::replace_out rout;
  cls_rgw_fsio_effects rfx;
  ASSERT_EQ(0, fsio::replace_head(h, in, rout, rfx));
  EXPECT_EQ(0u, h.bound(out.bound).writers);
  EXPECT_FALSE(h.opens.count(GW1));
}

TEST(cls_rgw_fsio_versioned, V2_complete_after_takeover_removes_the_dead_writer)
{
  v2_complete_after_takeover_removes_the_dead_writer();
}

/* every check's tests fail without it */
TEST(cls_rgw_fsio, each_check_is_detected)
{
  const std::pair<cls_rgw_fsio_check, void (*)()> table[] = {
    {cls_rgw_fsio_check::W5_FINALIZE_RECHECKS_STATE,
     w5_finalize_of_published_shadow_with_follower},
    {cls_rgw_fsio_check::W6_EXPIRE_TAKES_OVER_PUBLISH, w6_expire_takes_over_zombie_publish},
    {cls_rgw_fsio_check::W3_FINALIZE_CREATES_TO_PUBLISHING, w3_finalize_publishes_to},
    {cls_rgw_fsio_check::W3_PUBLISHING_TO_RETIRES_FROM, w3_finalize_publishes_to},
    {cls_rgw_fsio_check::W3_RESUMED_FINALIZE_TAKES_OVER, w3_resumed_finalize_takes_over},
    {cls_rgw_fsio_check::W1_SHADOW_STORES_PSEUDO_MANIFEST, w1_shadow_carries_its_pseudo_manifest},
    {cls_rgw_fsio_check::CUR_SHADOW_SUPERSEDE,
     cur_shadow_supersede_starts_a_generation},
    {cls_rgw_fsio_check::CUR_SHADOW_PUBLISH, cur_shadow_publish_clears_it},
    {cls_rgw_fsio_check::CUR_SHADOW_ABANDON, cur_shadow_abandon_clears_it},
    {cls_rgw_fsio_check::W7_LEAVE_RETURNS_VERSION_READ, w7_leave_returns_the_version_read},
    {cls_rgw_fsio_check::W7_REBIND_WAITS_ON_PUBLISHING, w7_rebind_into_publishing_waits},
    {cls_rgw_fsio_check::V1_FINALIZE_REQUIRES_ABORTED_CLAIM,
     v1_finalize_requires_the_aborted_claim},
    {cls_rgw_fsio_check::V2_COMPLETE_REMOVES_DEAD_WRITERS,
     v2_complete_after_takeover_removes_the_dead_writer},
  };
  for (const auto& [check, body] : table) {
    fsio::skipped_check = check;
    ::testing::TestPartResultArray results;
    {
      ::testing::ScopedFakeTestPartResultReporter intercept(
	::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ALL_THREADS,
	&results);
      body();
    }
    fsio::skipped_check = cls_rgw_fsio_check::NONE;
    EXPECT_GT(results.size(), 0) << "check " << int(check)
				 << " not detected";
  }
}
