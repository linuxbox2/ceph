// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2023 IBM, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#pragma once

#include <string>
#include <string_view>
#include <type_traits>
#include "include/function2.hpp"
#include <stdint.h>
#include <xxhash.h>

namespace rgw::pack {

  class Header
  {
  public:
    uint16_t struct_v;
    uint32_t flags;
    // TODO: implement
  }; /* Header */

  class ObjEntry
  {
  public:
    uint16_t struct_v;
    std::string name;
    std::string cksum; // XXX use mine?
    uint32_t flags;
    uint32_t size;
  }; /* ObjEntry */
  
  /* type erasing i/o types */
  template <typename IO>
  class Pack
  {
  private:
    IO io;
  public:

    int add_object(const std::string_view name, void* cb /* add bytes and attrs */);
    int get_object(const std::string_view name, void* cb /* return bytes and attrs */);
    int list_objects(const std::string_view marker, void* namecb);
    int attrs_operate(const std::string_view name); /* TODO RGW-like attrs CRUD op */
    int remove_object(const std::string_view name);

    static Pack make_pack(IO& /* & */ io);
  }; /* Pack */

#include <unistd.h>
#include <stdint.h>

class PositionalIO
{
private:
  int fd;
  uint32_t flags;

  public:
  static constexpr uint32_t FLAG_NONE = 0x0000;
  static constexpr uint32_t FLAG_OPEN = 0x0001;

  PositionalIO() {}
  ~PositionalIO();

  int open(std::string& archive_path); // XXX remove from interface
  ssize_t read(void* buf, size_t len, off64_t off);
  ssize_t write(const void* buf, size_t len, off64_t off);
  void close();
}; /* PositionalIO */

} /* namespace rgw::pack */
