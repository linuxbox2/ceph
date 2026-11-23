// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "cls/rgw/cls_rgw_fsio_types.h"
#include "rgw_obj_manifest.h"

class RGWZoneGroup;
class RGWZoneParams;

namespace rgw::sal {

/* FSIO_RADOS_DESIGN.md §2.7, plan §6.4:  one chunk of an object's content
 * or of a shadow */
struct FSIOChunk {
  uint64_t index = 0;		// its position in the manifest's iteration order
  uint64_t ofs = 0;		// its first logical byte
  uint64_t capacity = 0;	// its stripe's size, past the object's end too
  rgw_raw_obj anchor;		// the source's name for it
};

/* Where each chunk of an object lives, from its manifest.  Chunks past the
 * object's end continue its last rule, and the head segment holds up to
 * the manifest's max_head_size:  RGW's iterator computes both on a copy of
 * the manifest whose size is unbounded and whose head segment is full.
 * The zone and zonegroup must outlive it. */
class FSIOGeometry {
public:
  /* -ENXIO for a manifest with no rule to continue:  an explicit one
   * (design §6.2), or none */
  static int make(const DoutPrefixProvider* dpp,
		  const ceph::buffer::list& manifest, const RGWZoneGroup& zg,
		  const RGWZoneParams& zp, FSIOGeometry* g);

  FSIOChunk locate(const DoutPrefixProvider* dpp, uint64_t ofs) const;

  /* the chunk a content reads (no id) or a shadow's, f(anchor, id) in role
   * 'd' with the anchor's locator, so every generation of a chunk hashes
   * alike (§2.7) */
  static rgw_raw_obj named(const FSIOChunk& c, std::string_view marker,
			   const std::optional<cls_rgw_fsio_id>& id);

  /* the source's own size, from its manifest */
  uint64_t size() const { return obj_size; }

private:
  RGWObjManifest m;		// the copy, unbounded
  uint64_t obj_size = 0;
  const RGWZoneGroup* zg = nullptr;
  const RGWZoneParams* zp = nullptr;
};

} // namespace rgw::sal
