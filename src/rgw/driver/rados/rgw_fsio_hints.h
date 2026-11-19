// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

/* where the FSIO open hints live (FSIO_RADOS_DESIGN.md §2.7, §4.3):
 * shard objects fsio_open_hint.NNNNNNNNNN in the zone's log pool, a
 * target's shard chosen by the hash of its oid, as the object expirer's
 * obj_delete_at_hint shards are */

#include <string>

#include <fmt/format.h>

#include "services/svc_bi_rados.h"

namespace rgw::fsio {

inline std::string open_hint_shard_oid(uint32_t shard)
{
  return fmt::format("fsio_open_hint.{:010d}", shard);
}

inline uint32_t open_hint_shard(const std::string& target_oid,
				uint32_t num_shards)
{
  return RGWSI_BucketIndex_RADOS::bucket_shard_index(target_oid, num_shards);
}

} // namespace rgw::fsio
