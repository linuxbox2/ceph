// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

/* FSIO_RADOS_DESIGN.md §2.7:  a chunk set's id and the names it gives
 * clones, apart from the rest of FSIO's types so the RGW manifest, which
 * names a published FSIO object's chunks with them (plan §6.4), can
 * include them without RGW's own types */

#include <compare>
#include <cstdint>
#include <list>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "include/buffer.h"
#include "include/encoding.h"

namespace ceph { class Formatter; }

struct cls_rgw_fsio_id {
  uint64_t counter = 0;
  uint64_t nonce = 0;

  auto operator<=>(const cls_rgw_fsio_id&) const = default;
  bool empty() const { return counter == 0 && nonce == 0; }
  std::string to_str() const {
    return fmt::format("{:016x}{:016x}", counter, nonce);
  }

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(counter, bl);
    encode(nonce, bl);
    ENCODE_FINISH(bl);
  }
  void decode(ceph::buffer::list::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(counter, bl);
    decode(nonce, bl);
    DECODE_FINISH(bl);
  }
  void dump(ceph::Formatter *f) const;
  static std::list<cls_rgw_fsio_id> generate_test_instances();
};
WRITE_CLASS_ENCODER(cls_rgw_fsio_id)

/* §2.7, E2:  a clone of a head (or instance head) lives after the
 * bucket's heads, <marker>~fsio_<role><id>_<key part>, the key part being
 * the head's oid after <marker>_;  the class and RGW name clones by it */
inline std::string cls_rgw_fsio_clone_name(std::string_view marker, char role,
					   const cls_rgw_fsio_id& id,
					   std::string_view head_oid)
{
  return fmt::format("{}~fsio_{}{}_{}", marker, role, id.to_str(),
		     head_oid.substr(marker.size() + 1));
}
