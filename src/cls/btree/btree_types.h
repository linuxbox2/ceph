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

#ifndef CEPH_BTREE_TYPES_H
#define CEPH_BTREE_TYPES_H

#include <errno.h>
#include "include/types.h"
#include <array>

namespace cls { namespace bplus {

template <typename SZ = 4000>
class Bitmap
{
public:
  std::vector<uint64_t> data;
  Bitmap()
    : data(SZ, 0) {}
};

class FreeList
{
  Bitmap<4000> map;
  std::vector<uint32_t> free_list; // short list of free chunks
  uint32_t highest_offset;
  uint16_t last_chunk_searched;
};

class Page
{
  uint32_t offset;
  uint16_t type;
  uint16_t refcnt;
  uint32_t linked_page_off; // indirect to a new page, or 0
};

class KeyType
{
  std::string key;
  uint32_t val_size;
  uint32_t page_no;
  uint16_t slot_off;
  uint16_t slot_cnt;
};

class KeyPage : public Page
{
  std::vector<KeyType> keys;
};

class ValType
{
  std::string val;
};

class ValPage : public Page
{
  std::vector<ValType> vals;
};

class Header
{
  uint32_t struct_ver;
  uint64_t gen;
  
  // XXX fence_key upper_bound
  // XXX fence_key lower_bound
  
};

}} /* namespace */

#endif /* */
