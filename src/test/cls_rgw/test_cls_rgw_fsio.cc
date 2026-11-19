// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* Regression tests for the FSIO head transitions (cls_rgw_fsio.cc), one
 * per settled finding of docs/FSIO_RADOS_ADVERSARIAL_REVIEW.md;  each
 * drives the finding's sequence and asserts what its fix guarantees. */

#include "cls/rgw/cls_rgw_fsio.h"
#include "cls/rgw/cls_rgw_fsio_ops.h"

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

namespace fsio = cls_rgw_fsio;
using state = cls_rgw_fsio_cs_state;

namespace {

/* incarnations, and their handles (design §2.7, Z4):  each gateway's
 * handle 1;  the expirer's handle 0 */
constexpr uint64_t I1 = 1, I2 = 2, I3 = 3, IX = 100;
constexpr cls_rgw_fsio_handle GW1{I1, 1}, GW2{I2, 1}, GW3{I3, 1},
  EXPIRER{IX, 0};

/* each counted call's sequence number (Z22):  increasing across the
 * file, so increasing for every handle */
uint64_t next_seq()
{
  static uint64_t seq = 0;
  return ++seq;
}

cls_rgw_fsio_head head(const std::string& manifest)
{
  cls_rgw_fsio_head h;
  h.exists = true;
  h.id_tag = "t0";
  h.manifest.append(manifest);
  return h;
}

/* joining as writer, which constructs a shadow, then fsio_ready */
cls_rgw_fsio_id construct(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& inc)
{
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  EXPECT_EQ(0, fsio::join(h, {inc, next_seq(), true, 1, 10, {}}, out, fx));
  EXPECT_TRUE(out.construct);
  EXPECT_EQ(0, fsio::ready(h, inc, out.bound, {{0, 0}}));
  return out.bound;
}

/* joining as reader, bound to the published chunk set */
cls_rgw_fsio_id join_reader(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& inc,
			    const std::string& instance = {})
{
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  EXPECT_EQ(0, fsio::join(h, {inc, next_seq(), false, 1, 10, {}, instance}, out, fx));
  return out.bound;
}

/* the last writer gives up its write open:  the shadow goes publishing */
void leave_writer(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& inc,
		  const cls_rgw_fsio_id& cs)
{
  fsio::leave_out out;
  cls_rgw_fsio_effects fx;
  EXPECT_EQ(0, fsio::leave(h, inc, next_seq(), cs, true, {}, out, fx));
  EXPECT_TRUE(out.publishing);
}

int publish(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& inc, const cls_rgw_fsio_id& cs,
	    uint64_t r = 1000)
{
  fsio::replace_in in;
  in.mode = fsio::replace_mode::PUBLISH;
  in.r = cls_rgw_fsio_id{r, 0};
  in.caller = inc;
  in.shadow = cs;
  fsio::replace_out out;
  cls_rgw_fsio_effects fx;
  return fsio::replace_head(h, in, out, fx);
}

/* joining as writer constructs a new shadow:  cur_shadow is clear */
cls_rgw_fsio_id join_writer_constructs(cls_rgw_fsio_head& h, const cls_rgw_fsio_handle& inc)
{
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  EXPECT_EQ(0, fsio::join(h, {inc, next_seq(), true, 1, 10, {}}, out, fx));
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
  ASSERT_EQ(0, fsio::rebind(h, GW1, next_seq(), c0, s1, 1));   // GW1's reader follows
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
  EXPECT_EQ(0, fsio::rebind(h, GW1, next_seq(), s1, s2, 1));
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
	    fsio::fenced_finalize(h, EXPIRER, s, {I1}, to));
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
  ASSERT_EQ(0, fsio::rebind(h, GW2, next_seq(), c0, s, 1));
  leave_writer(h, GW1, s);
  ASSERT_EQ(0, publish(h, GW1, s));
  ASSERT_TRUE(h.hdr && h.hdr->find(s));
  ASSERT_EQ(state::PUBLISHED, h.hdr->find(s)->state);
  cls_rgw_fsio_id to;
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::fenced_finalize(h, EXPIRER, s, {I1}, to));
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
    ASSERT_EQ(0, fsio::expire(h, EXPIRER, {I1}, std::nullopt, out, fx));
  }
  const auto* e = h.hdr->find(s);
  ASSERT_TRUE(e && e->publishing);
  EXPECT_EQ(state::PUBLISHING, e->state);          // still frozen
  EXPECT_EQ(EXPIRER, e->publishing->publisher);
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT, publish(h, GW1, s));
  cls_rgw_fsio_id to;
  EXPECT_EQ(0, fsio::fenced_finalize(h, EXPIRER, s, {I1}, to));
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
  ASSERT_EQ(0, fsio::fenced_finalize(h, EXPIRER, s, {I1}, to));
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
  constexpr cls_rgw_fsio_handle EXPIRER2{101, 0}, EXPIRER3{102, 0};
  auto h = head("C0");
  auto s = construct(h, GW1);
  cls_rgw_fsio_id to, to2, to3;
  ASSERT_EQ(0, fsio::fenced_finalize(h, EXPIRER, s, {I1}, to));
  ASSERT_EQ(0, fsio::fenced_finalize(h, EXPIRER2, s, {I1, IX}, to2));
  EXPECT_EQ(to, to2);
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_BUSY,                 // EXPIRER2 is alive
	    fsio::fenced_finalize(h, EXPIRER3, s, {I1}, to3));
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
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}}, out, fx));
  ASSERT_TRUE(out.construct);
  ASSERT_EQ(1u, fx.clones.size());
  EXPECT_EQ('a', fx.clones[0].role);
  EXPECT_EQ(out.bound, fx.clones[0].r);
  EXPECT_TRUE(fx.clones[0].pm.contents_equal(h.manifest));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, {{0, 0}}));
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
  ASSERT_EQ(0, fsio::rebind(h, GW2, next_seq(), c0, s1, 1));
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
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}}, out, fx));  // GW1 dies
  fsio::expire_out eout;
  cls_rgw_fsio_effects efx;
  ASSERT_EQ(0, fsio::expire(h, EXPIRER, {I1}, out.bound, eout, efx));
  ASSERT_TRUE(h.hdr);
  EXPECT_FALSE(h.hdr->cur_shadow);
}

TEST(cls_rgw_fsio, cur_shadow_abandon_clears_it)
{
  cur_shadow_abandon_clears_it();
}

/* W15:  a constructor that gives up abandons its shadow with its leave,
 * its GC entry already sent (design §3.4) */
void w15_constructor_abandons()
{
  auto h = head("C0");
  auto s = join_writer_constructs(h, GW1);
  fsio::leave_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::leave(h, GW1, next_seq(), s, true, {}, out, fx));
  EXPECT_TRUE(out.abandoned);
  EXPECT_FALSE(out.publishing);
  EXPECT_TRUE(!h.hdr || !h.hdr->find(s));
  EXPECT_TRUE(!h.hdr || !h.hdr->cur_shadow);
  EXPECT_TRUE(h.opens.empty());
}

TEST(cls_rgw_fsio, W15_constructor_abandons)
{
  w15_constructor_abandons();
}

/* Z4:  two handles of one incarnation are two holders:  one's last leave
 * does not freeze the shadow while the other still writes (design §2.7) */
TEST(cls_rgw_fsio, two_handles_one_incarnation)
{
  constexpr cls_rgw_fsio_handle GW1B{I1, 2};
  auto h = head("C0");
  auto s = construct(h, GW1);
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1B, next_seq(), true, 1, 10, {}}, out, fx));
  ASSERT_EQ(s, out.bound);
  ASSERT_EQ(1u, h.opens.size());                   // one record, two handles
  EXPECT_EQ(2u, h.opens[I1].handles.size());
  fsio::leave_out lout;
  ASSERT_EQ(0, fsio::leave(h, GW1, next_seq(), s, true, {}, lout, fx));
  EXPECT_FALSE(lout.publishing);
  EXPECT_EQ(state::READY, h.hdr->find(s)->state);
  leave_writer(h, GW1B, s);                        // the last writer
  EXPECT_EQ(state::PUBLISHING, h.hdr->find(s)->state);
  EXPECT_EQ(GW1B, h.hdr->find(s)->publishing->publisher);
}

/* Z22:  a counted call resent past the PG log's dup window changes
 * nothing and returns already applied;  a leave whose record is gone
 * returns EINVAL to its resend (design §3) */
void z22_resend_is_already_applied()
{
  auto h = head("C0");
  auto s = construct(h, GW1);                      // GW1's write open
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  const auto j = next_seq();
  ASSERT_EQ(0, fsio::join(h, {GW2, j, false, 1, 10, {}}, out, fx));
  const auto c = out.bound;
  fsio::join_out again;
  ASSERT_EQ(0, fsio::join(h, {GW2, j, false, 1, 10, {}}, again, fx));
  EXPECT_TRUE(again.already_applied);
  EXPECT_EQ(1u, h.counts(GW2, c).readers);         // counted once
  const auto l = next_seq();
  fsio::leave_out lout;
  ASSERT_EQ(0, fsio::leave(h, GW1, l, s, true, {}, lout, fx));
  EXPECT_TRUE(lout.publishing);
  fsio::leave_out lagain;
  ASSERT_EQ(0, fsio::leave(h, GW1, l, s, true, {}, lagain, fx));
  EXPECT_TRUE(lagain.already_applied);
  EXPECT_EQ(state::PUBLISHING, h.hdr->find(s)->state);
  const auto r = next_seq();
  fsio::leave_out rout;
  ASSERT_EQ(0, fsio::leave(h, GW2, r, c, false, {}, rout, fx));
  EXPECT_EQ(nullptr, h.find(GW2));                 // its last state, gone
  EXPECT_EQ(-EINVAL, fsio::leave(h, GW2, r, c, false, {}, rout, fx));
}

TEST(cls_rgw_fsio, Z22_resend_is_already_applied)
{
  z22_resend_is_already_applied();
}

/* Z22:  expiry removes a presumed-dead handle's counts but keeps its
 * last_seq until grace, so a resend still in flight changes nothing,
 * even with the header gone (design §3) */
void z22_expiry_keeps_the_seq()
{
  auto h = head("C0");
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  const auto j = next_seq();
  ASSERT_EQ(0, fsio::join(h, {GW1, j, false, 1, 10, {}}, out, fx));
  fsio::expire_out eout;
  ASSERT_EQ(0, fsio::expire(h, EXPIRER, {I1}, std::nullopt, eout, fx));
  EXPECT_EQ(nullptr, h.find(GW1));                 // its counts, gone
  EXPECT_FALSE(h.hdr);                             // and the header
  fsio::join_out again;
  ASSERT_EQ(0, fsio::join(h, {GW1, j, false, 1, 10, {}}, again, fx));
  EXPECT_TRUE(again.already_applied);
  EXPECT_FALSE(h.hdr);                             // not recreated
  ASSERT_EQ(0, fsio::drop_seqs(h, I1, fx));        // after grace
  EXPECT_TRUE(h.opens.empty());
}

TEST(cls_rgw_fsio, Z22_expiry_keeps_the_seq)
{
  z22_expiry_keeps_the_seq();
}

using runs = std::vector<cls_rgw_fsio_run>;

/* Z11:  runs that overlap or touch merge;  one far write is one more
 * run, not a range the size of the file */
TEST(cls_rgw_fsio, Z11_runs_merge)
{
  runs e;
  fsio::add_run(e, {0, 3}, 64);
  fsio::add_run(e, {10, 12}, 64);
  EXPECT_EQ((runs{{0, 3}, {10, 12}}), e);
  fsio::add_run(e, {4, 9}, 64);                    // touches both
  EXPECT_EQ((runs{{0, 12}}), e);
  fsio::add_run(e, {1'000'000'000, 1'000'000'000}, 64);
  EXPECT_EQ((runs{{0, 12}, {1'000'000'000, 1'000'000'000}}), e);
  fsio::add_run(e, {5, 6}, 64);                    // inside one
  EXPECT_EQ(2u, e.size());
}

/* Z11:  past the cap the adjacent pair with the smallest gap merges */
void z11_extent_caps_runs()
{
  runs e;
  fsio::add_run(e, {0, 0}, 2);
  fsio::add_run(e, {10, 10}, 2);
  fsio::add_run(e, {100, 100}, 2);
  EXPECT_EQ((runs{{0, 10}, {100, 100}}), e);
}

TEST(cls_rgw_fsio, Z11_extent_caps_runs)
{
  z11_extent_caps_runs();
}

/* Z11:  fsio_ready records the constructor's runs, fsio_extend adds
 * one, and a fenced finalize carries them to `to` (design §2.7, §3.7) */
TEST(cls_rgw_fsio, Z11_runs_carried_to_the_finalize)
{
  auto h = head("C0");
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}}, out, fx));
  const auto s = out.bound;
  ASSERT_EQ(0, fsio::ready(h, GW1, s, runs{{0, 3}, {8, 9}}));
  EXPECT_EQ((runs{{0, 3}, {8, 9}}), h.hdr->find(s)->shadow->extent);
  ASSERT_EQ(0, fsio::extend(h, GW1, s, {50, 57}));
  cls_rgw_fsio_id to;
  ASSERT_EQ(0, fsio::fenced_finalize(h, EXPIRER, s, {I1}, to));
  EXPECT_EQ((runs{{0, 3}, {8, 9}, {50, 57}}), h.hdr->find(to)->shadow->extent);
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_BUSY, fsio::extend(h, GW1, s, {60, 60}));
}

/* §2.7 rule 4, W18:  every reply fits osd_max_write_op_reply_len, which
 * FSIO requires at 128 bytes, at its largest */
TEST(cls_rgw_fsio, replies_fit_the_reply_cap)
{
  constexpr size_t cap = 128;
  const cls_rgw_fsio_id big{UINT64_MAX, UINT64_MAX};
  auto size = [](const auto& r) {
    ceph::buffer::list bl;
    encode(r, bl);
    return bl.length();
  };
  cls_rgw_fsio_join_ret j{big, true, true};
  EXPECT_LE(size(j), cap);
  cls_rgw_fsio_leave_ret l{true, true, true, UINT64_MAX,
			   std::string(32, 'i'), big};   // OBJ_INSTANCE_LEN
  EXPECT_LE(size(l), cap);
  EXPECT_LE(size(cls_rgw_fsio_flags_ret{true, true, true}), cap);
  EXPECT_LE(size(cls_rgw_fsio_finalize_ret{big}), cap);
  EXPECT_LE(size(cls_rgw_fsio_expire_ret{true, true}), cap);
  EXPECT_LE(size(cls_rgw_fsio_remove_version_ret{true}), cap);
}

/* W20:  only the recorded publisher may release a publishing shadow
 * (design §3) */
void w20_release_requires_the_publisher()
{
  auto h = head("C0");
  auto s = construct(h, GW1);
  leave_writer(h, GW1, s);
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT, fsio::publish_release(h, GW2, s));
  EXPECT_EQ(state::PUBLISHING, h.hdr->find(s)->state);
  EXPECT_EQ(0, fsio::publish_release(h, GW1, s));
  EXPECT_EQ(state::READY, h.hdr->find(s)->state);
}

TEST(cls_rgw_fsio, W20_release_requires_the_publisher)
{
  w20_release_requires_the_publisher();
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
  ASSERT_EQ(0, fsio::ocnt_expire(ih, {I1}, d));
  EXPECT_FALSE(d.drained);
  ASSERT_EQ(0, fsio::ocnt_expire(ih, {I2}, d));
  EXPECT_TRUE(d.drained);
}

TEST(cls_rgw_fsio_versioned, join_binds_to_the_named_instance)
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  fsio::join_in in{GW1, next_seq(), false, 1, 10, {}, "I1"};
  ASSERT_EQ(0, fsio::join(h, in, out, fx));
  const auto* e = h.hdr->find(out.bound);
  ASSERT_TRUE(e);
  EXPECT_EQ(cls_rgw_fsio_manifest_at::INSTANCE, e->manifest_at);
  EXPECT_EQ("I1", e->instance);
  /* a writer allocates the shadow;  the gateway clones the instance */
  fsio::join_out wout;
  cls_rgw_fsio_effects wfx;
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}, "I1"}, wout, wfx));
  EXPECT_TRUE(wout.construct);
  EXPECT_TRUE(wfx.clones.empty());
  /* the OLH now names I2:  a new current generation */
  fsio::join_out rout;
  cls_rgw_fsio_effects rfx;
  ASSERT_EQ(0, fsio::join(h, {GW2, next_seq(), false, 1, 10, {}, "I2"}, rout, rfx));
  EXPECT_EQ("I2", h.hdr->cur_instance);
  EXPECT_FALSE(h.hdr->cur_shadow);
  EXPECT_NE(out.bound, rout.bound);
}

TEST(cls_rgw_fsio_versioned, leave_records_the_publish_intent)
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, {{0, 0}}));
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, next_seq(), out.bound, true, {}, lout, lfx, &intent));
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
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, {{0, 0}}));
  join_reader(h, GW2, "I1");                       // binds I1's entry
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, next_seq(), out.bound, true, {}, lout, lfx, &intent));
  fsio::replace_in in;
  in.mode = fsio::replace_mode::PUBLISH;
  in.versioned = true;
  in.caller = GW2;                            // not the publisher
  in.shadow = out.bound;
  fsio::replace_out rout;
  cls_rgw_fsio_effects rfx;
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::replace_head(h, in, rout, rfx));
  in.caller = GW1;
  ASSERT_EQ(0, fsio::replace_head(h, in, rout, rfx));
  EXPECT_TRUE(rfx.clones.empty());                 // nothing superseded
  EXPECT_FALSE(h.hdr->cur_shadow);
}

TEST(cls_rgw_fsio_versioned, complete_admits_only_a_dead_publisher)
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, {{0, 0}}));
  auto c0 = join_reader(h, GW2, "I1");
  ASSERT_EQ(0, fsio::rebind(h, GW2, next_seq(), c0, out.bound, 1));  // keeps the entry
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, next_seq(), out.bound, true, {}, lout, lfx, &intent));
  fsio::replace_in in;
  in.mode = fsio::replace_mode::PUBLISH;
  in.versioned = true;
  in.complete = true;
  in.caller = EXPIRER;
  in.shadow = out.bound;
  fsio::replace_out rout;
  cls_rgw_fsio_effects rfx;
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::replace_head(h, in, rout, rfx));    // GW1 is alive
  in.expired = {I1};
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
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, {{0, 0}}));
  auto c0 = join_reader(h, GW2, "I1");
  auto c1 = join_reader(h, GW3, "I1");
  ASSERT_EQ(0, fsio::rebind(h, GW2, next_seq(), c0, out.bound, 1));   // follows
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, next_seq(), out.bound, true, {}, lout, lfx, &intent));
  EXPECT_EQ("I2", lout.instance);
  fsio::leave_out f;
  ASSERT_EQ(0, fsio::leave(h, GW2, next_seq(), out.bound, false, {}, f, lfx));
  EXPECT_EQ("I2", f.instance);                     // a publishing shadow's
  fsio::leave_out r;
  ASSERT_EQ(0, fsio::leave(h, GW3, next_seq(), c1, false, {}, r, lfx));
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
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, {{0, 0}}));
  auto c0 = join_reader(h, GW2, "I1");
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, next_seq(), out.bound, true, {}, lout, lfx, &intent));
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_BUSY, fsio::rebind(h, GW2, next_seq(), c0, out.bound, 1));
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
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, {{0, 0}}));
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, next_seq(), out.bound, true, {}, lout, lfx, &intent));
  fsio::publish_intent to_intent{"I3", "tok3", 78};
  cls_rgw_fsio_id to;
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::fenced_finalize(h, EXPIRER, out.bound, {I1}, to,
				  &to_intent));            // not aborted
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::fenced_finalize(h, EXPIRER, out.bound, {I1}, to,
				  &to_intent, "I9"));      // another claim
  EXPECT_EQ(0, fsio::fenced_finalize(h, EXPIRER, out.bound, {I1}, to,
				     &to_intent, "I2"));
}

TEST(cls_rgw_fsio_versioned, V1_finalize_requires_the_aborted_claim)
{
  v1_finalize_requires_the_aborted_claim();
}

/* §3.7 step 4:  a resumed versioned finalize whose `to` was never
 * written publishes under a fresh claim:  the takeover with an intent
 * replaces `to`'s, once the recorded one is aborted */
void resume_replaces_the_claim()
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, {{0, 0}}));
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, next_seq(), out.bound, true, {}, lout, lfx, &intent));
  fsio::publish_intent i3{"I3", "tok3", 78};
  cls_rgw_fsio_id to;
  ASSERT_EQ(0, fsio::fenced_finalize(h, EXPIRER, out.bound, {I1}, to,
				     &i3, "I2"));
  constexpr cls_rgw_fsio_handle EXPIRER2{101, 0};
  fsio::publish_intent i4{"I4", "tok4", 79};
  cls_rgw_fsio_id to2;
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_CONFLICT,
	    fsio::fenced_finalize(h, EXPIRER2, out.bound, {I1, IX}, to2,
				  &i4, "I9"));             // another claim
  ASSERT_EQ(0, fsio::fenced_finalize(h, EXPIRER2, out.bound, {I1, IX}, to2,
				     &i4, "I3"));
  EXPECT_EQ(to, to2);
  const auto* t = h.hdr->find(to);
  ASSERT_TRUE(t && t->publishing);
  EXPECT_EQ("I4", t->publishing->instance);
  EXPECT_EQ("tok4", t->publishing->token);
  EXPECT_EQ(79u, t->publishing->olh_epoch);
}

TEST(cls_rgw_fsio_versioned, resume_replaces_the_claim)
{
  resume_replaces_the_claim();
}

/* V2:  completing a publish the expirer took over removes the dead
 * writer's open (design §4.2) */
void v2_complete_after_takeover_removes_the_dead_writer()
{
  auto h = olh();
  fsio::join_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::join(h, {GW1, next_seq(), true, 1, 10, {}, "I1"}, out, fx));
  ASSERT_EQ(0, fsio::ready(h, GW1, out.bound, {{0, 0}}));
  join_reader(h, GW2, "I1");                       // keeps the header
  fsio::publish_intent intent{"I2", "tok", 77};
  fsio::leave_out lout;
  cls_rgw_fsio_effects lfx;
  ASSERT_EQ(0, fsio::leave(h, GW1, next_seq(), out.bound, true, {}, lout, lfx, &intent));
  {
    fsio::expire_out eout;
    cls_rgw_fsio_effects efx;
    ASSERT_EQ(0, fsio::expire(h, EXPIRER, {I1}, std::nullopt, eout, efx));
  }
  fsio::replace_in in;
  in.mode = fsio::replace_mode::PUBLISH;
  in.versioned = true;
  in.complete = true;
  in.caller = EXPIRER;
  in.shadow = out.bound;
  in.expired = {I1, IX};
  fsio::replace_out rout;
  cls_rgw_fsio_effects rfx;
  ASSERT_EQ(0, fsio::replace_head(h, in, rout, rfx));
  EXPECT_EQ(0u, h.bound(out.bound).writers);
  EXPECT_EQ(nullptr, h.find(GW1));    // its last_seq kept until grace (Z22)
}

TEST(cls_rgw_fsio_versioned, V2_complete_after_takeover_removes_the_dead_writer)
{
  v2_complete_after_takeover_removes_the_dead_writer();
}

/* §2.5a:  versioning enabled over content FSIO has open makes it the null
 * version in place:  an fsio_join naming null binds to the same chunk-set entry
 * at HEAD and starts no generation */
void null_is_the_heads_content()
{
  auto h = head("C0");
  h.olh = true;
  auto s = construct(h, GW1);                      // unversioned:  no instance
  auto c0 = join_reader(h, GW1);
  auto c0n = join_reader(h, GW2, "null");          // versioning now enabled
  EXPECT_EQ(c0, c0n);
  ASSERT_TRUE(h.hdr->cur_shadow);
  EXPECT_EQ(s, *h.hdr->cur_shadow);                // no new generation
  int published = 0;
  for (const auto& e : h.hdr->chunk_sets) {
    published += e.state == state::PUBLISHED;
  }
  EXPECT_EQ(1, published);
}

TEST(cls_rgw_fsio_versioned, null_is_the_heads_content)
{
  null_is_the_heads_content();
}

/* §2.5a:  removing the null version with readers bound retires the HEAD
 * chunk-set entry with a leaf clone and keeps the tail */
void null_removal_retires_the_head_chunk_set()
{
  auto h = head("C0");
  h.olh = true;
  auto c0 = join_reader(h, GW1, "null");
  fsio::replace_in in;
  in.mode = fsio::replace_mode::REMOVE_NULL;
  in.r = cls_rgw_fsio_id{3000, 0};
  fsio::replace_out out;
  cls_rgw_fsio_effects fx;
  ASSERT_EQ(0, fsio::replace_head(h, in, out, fx));
  EXPECT_TRUE(out.keep_tail);
  EXPECT_EQ(1u, fx.clones.size());                 // the leaf clone
  const auto* e = h.hdr->find(c0);
  ASSERT_TRUE(e);
  EXPECT_EQ(state::RETIRED, e->state);
  EXPECT_EQ(cls_rgw_fsio_manifest_at::CLONE, e->manifest_at);
}

TEST(cls_rgw_fsio_versioned, null_removal_retires_the_head_chunk_set)
{
  null_removal_retires_the_head_chunk_set();
}

/* every check's tests fail without it */
/* §3.3, §2.7:  a writer's size and mtime hint merges by max within a
 * truncate epoch */
TEST(cls_rgw_fsio_size, hint_merges_by_max_within_an_epoch)
{
  auto h = head("C0");
  const auto s = construct(h, GW1);
  fsio::hint_out out;
  ASSERT_EQ(0, fsio::hint(h, GW1, s, 0, 100, 5, out));
  EXPECT_FALSE(out.stale);
  EXPECT_EQ(100u, out.size);
  ASSERT_EQ(0, fsio::hint(h, GW1, s, 0, 50, 9, out));
  EXPECT_EQ(100u, out.size);           // a lower size does not lower it
  const auto& sh = *h.hdr->find(s)->shadow;
  EXPECT_EQ(100u, sh.size_hint);
  EXPECT_EQ(9u, sh.mtime_hint);
}

/* a truncate sets the size exactly in a new epoch, and lowers trunc_low
 * only when smaller */
TEST(cls_rgw_fsio_size, truncate_sets_exactly)
{
  auto h = head("C0");
  const auto s = construct(h, GW1);
  fsio::hint_out out;
  ASSERT_EQ(0, fsio::hint(h, GW1, s, 0, 100, 5, out));
  uint64_t epoch = 0;
  ASSERT_EQ(0, fsio::truncate(h, GW1, s, 60, 6, epoch));
  EXPECT_EQ(1u, epoch);
  const auto& sh = *h.hdr->find(s)->shadow;
  EXPECT_EQ(60u, sh.size_hint);
  EXPECT_EQ(6u, sh.mtime_hint);
  ASSERT_TRUE(sh.trunc_low);
  EXPECT_EQ(60u, *sh.trunc_low);
  ASSERT_EQ(0, fsio::truncate(h, GW1, s, 80, 7, epoch));
  EXPECT_EQ(2u, epoch);
  EXPECT_EQ(80u, sh.size_hint);        // exactly, though larger
  EXPECT_EQ(60u, *sh.trunc_low);       // the smallest size set
}

/* a report from an older epoch merges nothing:  the writer gets the
 * current epoch and size to rebase from */
TEST(cls_rgw_fsio_size, a_stale_hint_merges_nothing)
{
  auto h = head("C0");
  const auto s = construct(h, GW1);
  uint64_t epoch = 0;
  ASSERT_EQ(0, fsio::truncate(h, GW1, s, 40, 7, epoch));
  fsio::hint_out out;
  ASSERT_EQ(0, fsio::hint(h, GW1, s, 0, 500, 8, out));
  EXPECT_TRUE(out.stale);
  EXPECT_EQ(1u, out.trunc_epoch);
  EXPECT_EQ(40u, out.size);
  EXPECT_EQ(40u, h.hdr->find(s)->shadow->size_hint);
  ASSERT_EQ(0, fsio::hint(h, GW1, s, out.trunc_epoch, 500, 8, out));
  EXPECT_FALSE(out.stale);             // rebased:  merged
  EXPECT_EQ(500u, out.size);
}

/* the hint says whether another handle has an open on the shadow or the
 * published chunk set:  another gateway's, or a sibling's (Z4) */
TEST(cls_rgw_fsio_size, hint_reports_other_opens)
{
  auto h = head("C0");
  const auto s = construct(h, GW1);
  fsio::hint_out out;
  ASSERT_EQ(0, fsio::hint(h, GW1, s, 0, 10, 1, out));
  EXPECT_FALSE(out.others);
  join_reader(h, GW2);                 // bound to the published set
  ASSERT_EQ(0, fsio::hint(h, GW1, s, 0, 10, 1, out));
  EXPECT_TRUE(out.others);

  auto h2 = head("C0");
  const auto s2 = construct(h2, GW1);
  join_reader(h2, cls_rgw_fsio_handle{I1, 2});   // GW1's sibling handle
  ASSERT_EQ(0, fsio::hint(h2, GW1, s2, 0, 10, 1, out));
  EXPECT_TRUE(out.others);
}

/* W14:  a publishing shadow's size is final:  the hint changes nothing
 * and returns as usual, the truncate returns ERR_FSIO_BUSY */
TEST(cls_rgw_fsio_size, publishing_freezes_the_size)
{
  auto h = head("C0");
  const auto s = construct(h, GW1);
  fsio::hint_out out;
  ASSERT_EQ(0, fsio::hint(h, GW1, s, 0, 100, 5, out));
  leave_writer(h, GW1, s);             // the open stays, the shadow publishing
  ASSERT_EQ(0, fsio::hint(h, GW1, s, 0, 500, 9, out));
  EXPECT_EQ(100u, out.size);
  EXPECT_EQ(100u, h.hdr->find(s)->shadow->size_hint);
  uint64_t epoch = 0;
  EXPECT_EQ(-CLS_RGW_ERR_FSIO_BUSY, fsio::truncate(h, GW1, s, 10, 9, epoch));
  EXPECT_EQ(0u, h.hdr->find(s)->shadow->trunc_epoch);
}

/* only a handle with a writer open on the shadow reports on it */
TEST(cls_rgw_fsio_size, only_a_writer_reports)
{
  auto h = head("C0");
  const auto s = construct(h, GW1);
  join_reader(h, GW2);
  fsio::hint_out out;
  EXPECT_EQ(-EINVAL, fsio::hint(h, GW2, s, 0, 10, 1, out));
  uint64_t epoch = 0;
  EXPECT_EQ(-EINVAL, fsio::truncate(h, GW2, s, 10, 1, epoch));
  EXPECT_EQ(-EINVAL, fsio::hint(h, GW1, cls_rgw_fsio_id{999, 9}, 0, 10, 1, out));
}

/* A2, §2.2:  fsio_ready is idempotent by its effect:  a repeat finds the
 * shadow ready, succeeds and changes nothing, extent included */
TEST(cls_rgw_fsio_repeat, ready_repeat_changes_nothing)
{
  auto h = head("C0");
  const auto s = construct(h, GW1);
  ASSERT_EQ(0, fsio::extend(h, GW1, s, {5, 7}));
  const auto extent = h.hdr->find(s)->shadow->extent;
  EXPECT_EQ(0, fsio::ready(h, GW1, s, {{0, 0}}));
  EXPECT_EQ(cls_rgw_fsio_cs_state::READY, h.hdr->find(s)->state);
  EXPECT_EQ(extent, h.hdr->find(s)->shadow->extent);
}

/* …but a shadow that has moved on is a stale view:  EINVAL */
TEST(cls_rgw_fsio_repeat, ready_after_freezing_is_einval)
{
  auto h = head("C0");
  const auto s = construct(h, GW1);
  leave_writer(h, GW1, s);             // publishing
  EXPECT_EQ(-EINVAL, fsio::ready(h, GW1, s, {{0, 0}}));
}

/* A3, §2.2:  fsio_publish_release is idempotent by its effect:  a repeat
 * finds the shadow ready with no publish recorded */
TEST(cls_rgw_fsio_repeat, release_repeat_changes_nothing)
{
  auto h = head("C0");
  const auto s = construct(h, GW1);
  leave_writer(h, GW1, s);
  ASSERT_EQ(0, fsio::publish_release(h, GW1, s));
  EXPECT_EQ(0, fsio::publish_release(h, GW1, s));
  const auto* e = h.hdr->find(s);
  EXPECT_EQ(cls_rgw_fsio_cs_state::READY, e->state);
  EXPECT_FALSE(e->publishing);
}

/* …but after the shadow has published, a release is stale:  EINVAL */
TEST(cls_rgw_fsio_repeat, release_after_publish_is_einval)
{
  auto h = head("C0");
  const auto s = construct(h, GW1);
  join_reader(h, GW2);                 // keeps the published entry
  leave_writer(h, GW1, s);
  ASSERT_EQ(0, publish(h, GW1, s));
  EXPECT_EQ(-EINVAL, fsio::publish_release(h, GW1, s));
}

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
    {cls_rgw_fsio_check::NULL_IS_HEAD, null_is_the_heads_content},
    {cls_rgw_fsio_check::NULL_REMOVAL_RETIRES,
     null_removal_retires_the_head_chunk_set},
    {cls_rgw_fsio_check::W15_CONSTRUCTOR_MAY_ABANDON, w15_constructor_abandons},
    {cls_rgw_fsio_check::W20_RELEASE_CHECKS_PUBLISHER,
     w20_release_requires_the_publisher},
    {cls_rgw_fsio_check::RESUME_REPLACES_CLAIM, resume_replaces_the_claim},
    {cls_rgw_fsio_check::Z22_COUNTED_CALL_CHECKS_SEQ,
     z22_resend_is_already_applied},
    {cls_rgw_fsio_check::Z22_EXPIRY_KEEPS_SEQ, z22_expiry_keeps_the_seq},
    {cls_rgw_fsio_check::Z11_EXTENT_CAPS_RUNS, z11_extent_caps_runs},
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
