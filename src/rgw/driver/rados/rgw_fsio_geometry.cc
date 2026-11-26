// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_fsio_geometry.h"

#include "common/dout.h"
#include "common/dout_fmt.h"

#define dout_subsys ceph_subsys_rgw

namespace rgw::sal {

/* far past any object, short of overflowing an offset plus a stripe */
static constexpr uint64_t UNBOUNDED = uint64_t(1) << 62;

int FSIOGeometry::make(const DoutPrefixProvider* dpp,
		       const ceph::buffer::list& manifest,
		       const RGWZoneGroup& zg, const RGWZoneParams& zp,
		       FSIOGeometry* g)
{
  if (!manifest.length()) {
    ldpp_dout_fmt(dpp, 0, "FSIO:  an object with no manifest cannot be served");
    return -ENXIO;
  }
  try {
    auto it = manifest.cbegin();
    decode(g->m, it);
  } catch (const ceph::buffer::error&) {
    return -EIO;
  }
  if (g->m.has_explicit_objs() || g->m.get_rules().empty()) {
    ldpp_dout_fmt(dpp, 0, "FSIO:  an object with an explicit manifest cannot "
		  "be served (design §6.2)");
    return -ENXIO;
  }
  g->obj_size = g->m.get_obj_size();
  g->m.set_obj_size(UNBOUNDED);
  /* §6.4:  anchors are the names without a published id, so a later
   * shadow's names never grow;  the ids name the content */
  g->ids.clear();
  g->head_separate = g->m.fsio_head_separate();
  for (const auto& [ofs, rule] : g->m.get_rules()) {
    g->ids.emplace_back(rule.start_part_num, rule.fsio);
  }
  g->m.clear_fsio();
  if (g->m.get_head_size() < g->m.get_max_head_size()) {
    g->m.set_head_size(g->m.get_max_head_size());
  }
  g->zg = &zg;
  g->zp = &zp;
  return 0;
}

FSIOChunk FSIOGeometry::locate(const DoutPrefixProvider* dpp,
			       uint64_t ofs) const
{
  auto it = m.obj_find(dpp, ofs);
  FSIOChunk c;
  c.ofs = it.get_stripe_ofs();
  c.capacity = it.get_stripe_size();
  c.anchor = it.get_location().get_raw_obj(*zg, *zp);
  /* its index:  the stripes of every part before its own, then its
   * stripe;  a rule's parts each hold ceil(part_size / stripe_max_size)
   * stripes, and part 0's stripe count already includes a head segment */
  const auto part = static_cast<uint64_t>(it.get_cur_part_id());
  uint64_t index = 0;
  const auto& rules = m.get_rules();
  for (auto r = rules.begin(); r != rules.end(); ++r) {
    const auto& rule = r->second;
    if (rule.part_size == 0 || part < rule.start_part_num) {
      break;
    }
    const uint64_t per_part =
      (rule.part_size + rule.stripe_max_size - 1) / rule.stripe_max_size;
    auto next = std::next(r);
    uint64_t parts = part - rule.start_part_num;
    if (next != rules.end() && part >= next->second.start_part_num) {
      parts = next->second.start_part_num - rule.start_part_num;
    }
    index += parts * per_part;
  }
  c.index = index + it.get_cur_stripe();
  for (const auto& [first, id] : ids) {
    if (first <= part) {
      c.content = id;
    }
  }
  /* the head segment is the head object's unless the publish left it
   * separate (a copy's or a dedup's) */
  if (part == 0 && c.ofs < m.get_max_head_size() && !head_separate) {
    c.content.reset();
  }
  return c;
}

uint64_t FSIOGeometry::max_counter() const
{
  uint64_t n = 0;
  for (const auto& [first, id] : ids) {
    if (id) {
      n = std::max(n, id->counter);
    }
  }
  return n;
}

rgw_raw_obj FSIOGeometry::named(const FSIOChunk& c, std::string_view marker,
				const std::optional<cls_rgw_fsio_id>& id)
{
  if (!id) {
    rgw_raw_obj r = c.anchor;
    rgw_obj_select::apply_fsio(c.content, marker, r);
    return r;
  }
  rgw_raw_obj r = c.anchor;
  r.oid = cls_rgw_fsio_clone_name(marker, 'd', *id, c.anchor.oid);
  r.loc = c.anchor.loc.empty() ? c.anchor.oid : c.anchor.loc;
  return r;
}

} // namespace rgw::sal
