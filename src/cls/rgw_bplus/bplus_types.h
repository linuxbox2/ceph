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

  struct Page : public cohort::lru::Object
  {
    uint32_t offset;
    uint16_t type;
    uint16_t refcnt;

    using link_mode = bi::link_mode<bi::safe_link>; /* XXX normal */
    using hook_type = bi::avl_set_member_hook<link_mode>;

    hook_type page_hook;
  
    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(offset, bl);
      encode(type, bl);
      encode(refcnt, bl);
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(offset, bl);
      decode(type, bl);
      decode(refcnt, bl);
      DECODE_FINISH(bl);
    }
  }; /* Page */
  WRITE_CLASS_ENCODER(Page);

  struct PageLT
  {
    bool operator()(const Page& lhs, const Page& rhs) const
      { return (lhs.offset < rhs.offset); }
    bool operator()(const uint32_t offset, const Page& page) const
      { return offset < page.offset; }
    bool operator()(const Page& page, const uint32_t offset) const
      { return page.offset < offset; }
  };

  struct PageEQ
  {
    bool operator()(const Page& lhs, const Page& rhs) const
      { return (lhs.offset == rhs.offset); }
    bool operator()(const uint32_t offset, const Page& page) const
      { return offset == page.offset; }
    bool operator()(const Page& page, const uint32_t offset) const
      { return page.offset == offset; }
  };

  using PageHook = bi::member_hook<Page, Page::hook_type, &Page::page_hook>;
  using PageAVL = bi::avltree<Page, bi::compare<PageLT>, PageHook>;
  using PageLRU = cohort::lru::LRU<std::mutex>;
  using PageCache =
    cohort::lru::TreeX<Page, PageAVL, PageLT, PageEQ, uint32_t, std::mutex>;

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
    variant<uint16_t,std::string> key;
    variant<std::string,Addr> val;
    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(flags, bl);
      uint16_t var_type;
      if (std::holds_alternative<uint16_t>(key)) {
	var_type = 1;
	encode(var_type, bl);
	encode(get<uint16_t>(key), bl);
      } else {
	var_type = 2;
	encode(var_type, bl);
	encode(get<std::string>(key), bl);
      }
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
      uint16_t var_type;
      if (var_type == 1) {
	uint16_t tkey;
	decode(tkey, bl);
	key = tkey;
      } else {
	std::string tkey;
	decode(tkey, bl);
	key = tkey;
      }
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
    flat_map<uint16_t, KeyPrefix> key_prefixes;
    flat_map<std::string, uint16_t> kp_reverse; // not serialized
    std::vector<KeyType> keys;

    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(key_prefixes, bl);
      encode(keys, bl);
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(key_prefixes, bl);
      /* prefix map is small, so keep a lookup table */
      kp_reverse.reserve(key_prefixes.size());
      for (const auto& it : key_prefixes) {
	kp_reverse.insert(
	  decltype(kp_reverse)::value_type(it.second.prefix, it.first));
      }
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

  struct Header
  {
    uint32_t struct_ver;
    uint64_t gen;

    uint32_t fanout;
    uint32_t size_max;
    uint16_t prefix_min_len;

    FreeSpaceMap free_space;

    void encode(buffer::list& bl) const {
      ENCODE_START(1, 1, bl);
      encode(struct_ver, bl);
      encode(gen, bl);
      encode(fanout, bl);
      encode(size_max, bl);
      encode(prefix_min_len, bl);
      encode(free_space, bl);
      ENCODE_FINISH(bl);
    }

    void decode(buffer::list::const_iterator& bl) {
      DECODE_START(1, bl);
      decode(struct_ver, bl);
      decode(gen, bl);
      decode(fanout, bl);
      decode(size_max, bl);
      decode(prefix_min_len, bl);
      decode(free_space, bl);
      DECODE_FINISH(bl);
    }
  };
  WRITE_CLASS_ENCODER(Header);

  struct Layout
  {
    static constexpr uint32_t header_offset = 0;
    static constexpr uint32_t header_size = 4096;

  };
  
} /* namespace */

#endif /* BPLUS_ONDISK_H */
