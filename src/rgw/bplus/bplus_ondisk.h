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

#include <boost/variant.hpp>
#include <boost/container/flat_map.hpp>

namespace rgw::bplus::ondisk {

template <uint16_t SZ = 500 /* 500 * 64 = 32000 chunks, * 4096 = 125M */>
struct Bitmap
{
  std::vector<uint64_t> data;
  Bitmap()
    : data(SZ, 0) {}
  constexpr uint16_t size() const {
    return SZ * 64;
  }
  uint16_t to_chunk(uint16_t bit) const {
    return data[bit/64];
  }
  uint16_t bit_offset(uint16_t bit) const {
    return uint64_t(1) << (bit % 64);
  }
  bool test(uint16_t bit) const {
    return to_chunk(bit) & bit_offset(bit);
  }
  void set(uint16_t bit) {
    to_chunk(bit) |= bit_offset(bit);
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

struct FreeSpace
{
  Bitmap<500> map;
  std::vector<uint16_t> free_list; // short list of free chunks
  uint32_t highest_chunk;
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
}; /* FreeSpace */
WRITE_CLASS_ENCODER(FreeSpace);

struct Page
{
  uint32_t offset;
  uint16_t type;
  uint16_t refcnt;
  uint32_t linked_page_off; // indirect to a new page, or 0

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
};

struct KeyPrefix
{
  uint32_t refcnt;
  std::string prefix;
};

using boost::variant;
using boost::container::flat_map;

struct KeyType
{
  uint16_t flags;
  // XXX optional key prefix
  std::string key;
  variant<std::string,Addr> val;
};

struct KeyPage : public Page
{
  std::vector<KeyType> keys;
};

struct ValType
{
  std::string val;
};

struct ValPage : public Page
{
  std::vector<ValType> vals;
};

struct Header
{
  uint32_t struct_ver;
  uint64_t gen;

  // XXX fence_key upper_bound
  // XXX fence_key lower_bound

  flat_map<uint16_t, KeyPrefix> key_prefixes;
  
};

} /* namespace */

#endif /* BPLUS_ONDISK_H */
