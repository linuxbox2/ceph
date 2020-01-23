// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#ifndef BPLUS_ONDISK_H
#define BPLUS_ONDISK_H

#include <errno.h>
#include "include/types.h"

#include <functional>
#include <utility>
#include <limits>
#include <variant>
#include <boost/container/flat_map.hpp>
#include <boost/intrusive/avl_set.hpp>
#include "common/cohort_lru.h"

namespace rgw::bplus::ondisk {

  namespace bi = boost::intrusive;
  
  template <uint16_t SZ = 500 /* 500 * 64 = 32000 chunks, * 4096 = 125M */>
  struct Bitmap
  {
    std::vector<uint64_t> data;
    Bitmap()
      : data(SZ, 0) {}
    constexpr uint16_t size() const {
      return SZ * 64;
    }
    uint64_t& to_chunk(uint16_t bit) const {
      return const_cast<uint64_t&>(data[bit/64]);
    }
    uint64_t bit_offset(uint16_t bit) const {
      return uint64_t(1) << (bit % 64);
    }
    bool test(uint16_t bit) const {
      return to_chunk(bit) & bit_offset(bit);
    }
    void set(uint16_t bit) {
      to_chunk(bit) |= bit_offset(bit);
    }
    void clear(uint16_t bit) {
      to_chunk(bit) &= ~bit_offset(bit);
    }
    void clear() {
      for (auto& chunk : data) {
	chunk = 0;
      }
    }
    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(data, bl);
      ENCODE_FINISH(bl);
    }
    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(data, bl);
      DECODE_FINISH(bl);
    }
  }; /* Bitmap */
  WRITE_CLASS_ENCODER(Bitmap<500>);

  struct FreeSpaceMap
  {
    Bitmap<500> map;
    std::vector<uint16_t> free_list; // short list of free chunks
    static constexpr uint16_t fl_maxsz = 256;
    uint16_t highest_chunk;
    uint16_t last_chunk_searched;

    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(map, bl);
      encode(free_list, bl);
      encode(highest_chunk, bl);
      encode(last_chunk_searched, bl);
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(map, bl);
      decode(free_list, bl);
      decode(highest_chunk, bl);
      decode(last_chunk_searched, bl);
      DECODE_FINISH(bl);
    }
  }; /* FreeSpaceMap */
  WRITE_CLASS_ENCODER(FreeSpaceMap);

  struct Page
  {
    uint32_t offset;
    uint16_t type;
    uint16_t refcnt;
    uint32_t linked_page_off; // indirect to a new page, or 0

    typedef bi::link_mode<bi::safe_link> link_mode; /* XXX normal */
    typedef bi::avl_set_member_hook<link_mode> hook_type;

    hook_type page_hook;
  
  
    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(offset, bl);
      encode(type, bl);
      encode(refcnt, bl);
      encode(linked_page_off, bl);
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(offset, bl);
      decode(type, bl);
      decode(refcnt, bl);
      decode(linked_page_off, bl);
      DECODE_FINISH(bl);
    }
  }; /* Page */
  WRITE_CLASS_ENCODER(Page);

  struct Addr
  {
    uint32_t page_no;
    uint16_t slot_off;
    uint16_t slot_cnt;
    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(page_no, bl);
      encode(slot_off, bl);
      encode(slot_cnt, bl);
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(page_no, bl);
      decode(slot_off, bl);
      decode(slot_cnt, bl);
      DECODE_FINISH(bl);
    }
  };
  WRITE_CLASS_ENCODER(Addr);

  struct KeyPrefix
  {
    uint32_t refcnt;
    std::string prefix;
    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(refcnt, bl);
      encode(prefix, bl);
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(refcnt, bl);
      decode(prefix, bl);
      DECODE_FINISH(bl);
    }
  };
  WRITE_CLASS_ENCODER(KeyPrefix);

  using std::get;
  using std::variant;
  using boost::container::flat_map;

  struct KeyType
  {
    uint16_t flags;
    // XXX optional key prefix
    std::string key;
    variant<std::string,Addr> val;
    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(flags, bl);
      encode(key, bl);
      uint16_t var_type;
      if (std::holds_alternative<std::string>(val)) {
	var_type = 1;
	encode(var_type, bl);
	encode(get<std::string>(val), bl);
      } else {
	var_type = 2;
	encode(var_type, bl);
	encode(get<Addr>(val), bl);
      }
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(flags, bl);
      decode(key, bl);
      uint16_t var_type;
      decode(var_type, bl);
      if (var_type == 1) {
	std::string tval;
	decode(tval, bl);
	val = tval;
      } else {
	Addr tval;
	decode(tval, bl);
	val = tval;
      }
      DECODE_FINISH(bl);
    }
  };
  WRITE_CLASS_ENCODER(KeyType);

  struct KeyPage : public Page
  {
    std::vector<KeyType> keys;
    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(keys, bl);
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(keys, bl);
      DECODE_FINISH(bl);
    }
  };
  WRITE_CLASS_ENCODER(KeyPage);

  struct ValType
  {
    std::string val;
    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(val, bl);
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(val, bl);
      DECODE_FINISH(bl);
    }
  };
  WRITE_CLASS_ENCODER(ValType);

  struct ValPage : public Page
  {
    std::vector<ValType> vals;
    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(vals, bl);
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(vals, bl);
      DECODE_FINISH(bl);
    }
  };
  WRITE_CLASS_ENCODER(ValPage);

  static constexpr uint32_t header_offset = 0;
  static constexpr uint32_t header_len = 4096;

  struct Header
  {
    uint32_t struct_ver;
    uint64_t gen;

    // XXX fence_key upper_bound
    // XXX fence_key lower_bound

    flat_map<uint16_t, KeyPrefix> key_prefixes;
    FreeSpaceMap free_space;

    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(struct_ver, bl);
      encode(gen, bl);
      encode(key_prefixes, bl);
      encode(free_space, bl);
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(struct_ver, bl);
      decode(gen, bl);
      decode(key_prefixes, bl);
      decode(free_space, bl);
      DECODE_FINISH(bl);
    }
  };
  WRITE_CLASS_ENCODER(Header);

} /* namespace */

#endif /* BPLUS_ONDISK_H */
