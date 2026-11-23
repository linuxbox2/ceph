// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* FSIO_RADOS_WORK_PLAN.md, the driver's step 4, element 1:  FSIOGeometry
 * against RGW's own manifest iterator and generator, with no cluster */

#include <gtest/gtest.h>

#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "rgw_fsio_geometry.h"
#include "test_rgw_common.h"

using rgw::sal::FSIOChunk;
using rgw::sal::FSIOGeometry;

namespace {

const DoutPrefix* dpp() {
  static const DoutPrefix dp(g_ceph_context, 1, "test rgw fsio geometry: ");
  return &dp;
}

constexpr uint64_t MiB = 1 << 20;

/* an atomic object as the put processor writes it */
RGWObjManifest atomic(test_rgw_env& env, uint64_t size, uint64_t head_max,
		      uint64_t stripe, const std::string& prefix = {})
{
  RGWObjManifest m;
  RGWObjManifest::generator gen;
  rgw_bucket bucket;
  test_rgw_init_bucket(&bucket, "buck");
  m.set_trivial_rule(head_max, stripe);
  const rgw_obj head(bucket, "obj");
  EXPECT_EQ(0, gen.create_begin(g_ceph_context, &m,
				env.zonegroup.default_placement, nullptr,
				bucket, head));
  for (uint64_t ofs = 0; ofs < size; ) {
    ofs = std::min(ofs + gen.cur_stripe_max_size(), size);
    gen.create_next(ofs);
  }
  if (!prefix.empty()) {
    m.set_prefix(prefix);
  }
  return m;
}

/* a multipart object of the given part sizes, as completion appends them */
RGWObjManifest multipart(test_rgw_env& env, const std::vector<uint64_t>& parts,
			 uint64_t stripe)
{
  rgw_bucket bucket;
  test_rgw_init_bucket(&bucket, "buck");
  RGWObjManifest all;
  for (size_t i = 0; i < parts.size(); ++i) {
    RGWObjManifest pm;
    RGWObjManifest::generator gen;
    pm.set_prefix("upload");
    pm.set_multipart_part_rule(stripe, i + 1);
    rgw_obj head;
    const rgw_placement_rule rule(env.zonegroup.default_placement.name,
				  RGW_STORAGE_CLASS_STANDARD);
    EXPECT_EQ(0, gen.create_begin(g_ceph_context, &pm, rule, nullptr, bucket,
				  head));
    for (uint64_t ofs = stripe; ofs < parts[i]; ofs += stripe) {
      gen.create_next(ofs);
    }
    gen.create_next(parts[i]);
    EXPECT_EQ(0, all.append(dpp(), pm, env.zonegroup, env.zone_params));
  }
  return all;
}

FSIOGeometry geometry(test_rgw_env& env, const RGWObjManifest& m)
{
  ceph::buffer::list bl;
  encode(m, bl);
  FSIOGeometry g;
  EXPECT_EQ(0, FSIOGeometry::make(dpp(), bl, env.zonegroup, env.zone_params,
				  &g));
  return g;
}

/* every chunk of m in range, located at its first, middle and last byte,
 * is the iterator's:  its index its position, its offset, its name;  its
 * capacity the stripe's, the last one's at least what it holds */
void matches_the_iterator(test_rgw_env& env, const RGWObjManifest& m)
{
  const auto g = geometry(env, m);
  uint64_t i = 0;
  for (auto it = m.obj_begin(dpp()); it != m.obj_end(dpp()); ++it, ++i) {
    const uint64_t ofs = it.get_stripe_ofs(), len = it.get_stripe_size();
    const bool last = ofs + len >= m.get_obj_size();
    for (const uint64_t at : {ofs, ofs + len / 2, ofs + len - 1}) {
      const FSIOChunk c = g.locate(dpp(), at);
      SCOPED_TRACE(fmt::format("chunk {} at {}", i, at));
      EXPECT_EQ(i, c.index);
      EXPECT_EQ(ofs, c.ofs);
      EXPECT_EQ(env.get_raw(it.get_location()), c.anchor);
      if (last) {
	EXPECT_GE(c.capacity, len);
      } else {
	EXPECT_EQ(len, c.capacity);
      }
    }
  }
  ASSERT_GT(i, 1u);
}

/* past the end of short, every chunk is the one long, generated at the
 * longer size by RGW's own generator, has */
void continues_as(test_rgw_env& env, const RGWObjManifest& short_m,
		  const RGWObjManifest& long_m)
{
  const auto g = geometry(env, short_m);
  uint64_t i = 0, past = 0;
  for (auto it = long_m.obj_begin(dpp()); it != long_m.obj_end(dpp());
       ++it, ++i) {
    const uint64_t ofs = it.get_stripe_ofs();
    const FSIOChunk c = g.locate(dpp(), ofs);
    SCOPED_TRACE(fmt::format("chunk {} at {}", i, ofs));
    EXPECT_EQ(i, c.index);
    EXPECT_EQ(ofs, c.ofs);
    EXPECT_EQ(env.get_raw(it.get_location()), c.anchor);
    if (ofs + it.get_stripe_size() < long_m.get_obj_size()) {
      EXPECT_EQ(it.get_stripe_size(), c.capacity);
    }
    past += ofs >= short_m.get_obj_size();
  }
  ASSERT_GE(past, 2u) << "the long manifest reaches no chunk past the end";
}

} // anonymous namespace

TEST(FSIOGeometry, AtomicMatchesTheIterator)
{
  test_rgw_env env;
  matches_the_iterator(env, atomic(env, 9 * MiB, 4 * MiB, 4 * MiB));
  matches_the_iterator(env, atomic(env, 21 * MiB + 1000, 512 * 1024, 4 * MiB));
}

TEST(FSIOGeometry, MultipartMatchesTheIterator)
{
  test_rgw_env env;
  matches_the_iterator(env, multipart(env, {10 * MiB, 10 * MiB, 6 * MiB},
				      4 * MiB));
}

TEST(FSIOGeometry, PastTheEndContinuesTheLastRule)
{
  test_rgw_env env;
  const auto short_a = atomic(env, 9 * MiB, 4 * MiB, 4 * MiB);
  continues_as(env, short_a, atomic(env, 21 * MiB, 4 * MiB, 4 * MiB,
				    short_a.get_prefix()));
  /* the last part's rule continues with its part size (plan §6.4) */
  continues_as(env, multipart(env, {10 * MiB, 10 * MiB, 6 * MiB}, 4 * MiB),
	       multipart(env, {10 * MiB, 10 * MiB, 6 * MiB, 6 * MiB, 6 * MiB},
			 4 * MiB));
}

TEST(FSIOGeometry, SmallHeadHoldsItsMaxHeadSize)
{
  test_rgw_env env;
  const auto m = atomic(env, 100, 4 * MiB, 4 * MiB);
  const auto g = geometry(env, m);
  const auto head = env.get_raw(m.get_obj());
  for (const uint64_t at : {uint64_t(0), uint64_t(100), MiB, 4 * MiB - 1}) {
    const FSIOChunk c = g.locate(dpp(), at);
    EXPECT_EQ(0u, c.index) << at;
    EXPECT_EQ(0u, c.ofs) << at;
    EXPECT_EQ(4 * MiB, c.capacity) << at;
    EXPECT_EQ(head, c.anchor) << at;
  }
  const FSIOChunk c = g.locate(dpp(), 4 * MiB);
  EXPECT_EQ(1u, c.index);
  EXPECT_EQ(4 * MiB, c.ofs);
  EXPECT_NE(head, c.anchor);
  EXPECT_EQ(100u, g.size());
}

TEST(FSIOGeometry, ExplicitManifestIsDeclined)
{
  test_rgw_env env;
  rgw_bucket bucket;
  test_rgw_init_bucket(&bucket, "buck");
  std::map<uint64_t, RGWObjManifestPart> objs;
  objs[0].loc = rgw_obj(bucket, "obj");
  objs[0].size = MiB;
  RGWObjManifest m;
  m.set_explicit(MiB, objs);
  ceph::buffer::list bl;
  encode(m, bl);
  FSIOGeometry g;
  EXPECT_EQ(-ENXIO, FSIOGeometry::make(dpp(), bl, env.zonegroup,
				       env.zone_params, &g));
  EXPECT_EQ(-ENXIO, FSIOGeometry::make(dpp(), {}, env.zonegroup,
				       env.zone_params, &g));
}

TEST(FSIOGeometry, ShadowNames)
{
  test_rgw_env env;
  const auto g = geometry(env, atomic(env, 9 * MiB, 4 * MiB, 4 * MiB));
  const cls_rgw_fsio_id id{7, 0xabc};
  for (const uint64_t at : {uint64_t(0), 5 * MiB}) {
    const FSIOChunk c = g.locate(dpp(), at);
    EXPECT_EQ(c.anchor, FSIOGeometry::named(c, "marker", std::nullopt));
    const auto s = FSIOGeometry::named(c, "marker", id);
    EXPECT_EQ(c.anchor.pool, s.pool);
    EXPECT_EQ(cls_rgw_fsio_clone_name("marker", 'd', id, c.anchor.oid), s.oid);
    EXPECT_EQ(c.anchor.loc.empty() ? c.anchor.oid : c.anchor.loc, s.loc);
  }
}

int main(int argc, char** argv)
{
  auto args = argv_to_vec(argc, argv);
  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_UTILITY,
			 CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
