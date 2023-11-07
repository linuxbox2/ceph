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

#include <cstdint>
#include <string>
#include <string_view>
#include <iostream> // XXX kill
#include <sys/types.h>
#include <type_traits>
#include "include/function2.hpp"
#include <stdint.h>
#include <xxhash.h>
#include <unistd.h>

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

    static constexpr uint8_t AB_OK =  0x00;
    static constexpr uint8_t AB_EOF = 0x01;

    class AddObj
    {
    private:
      Pack& pack;
      off64_t pos{0};
    public:
      AddObj(Pack& pack) : pack(pack) {}

    public:
      off64_t get_pos() const { return pos; }
      void add_bytes(off64_t off, const void* buf, size_t len, uint8_t flags);
      void add_attr(std::string_view name, std::string_view data);
    }; /* AddObj */

    using add_obj_cb_t =
      const fu2::unique_function<uint8_t(AddObj& af) const>;

    int add_object(const std::string_view name, add_obj_cb_t cb);
    int get_object(const std::string_view name, void* cb /* return bytes and attrs */);
    int list_objects(const std::string_view marker, void* namecb);
    int attrs_operate(const std::string_view name); /* TODO RGW-like attrs CRUD op */
    int remove_object(const std::string_view name);

    static Pack make_pack(IO&io);

  }; /* Pack */

  class PositionalIO {
  private:
    int fd;
    uint32_t flags;

    static constexpr uint32_t FLAG_NONE = 0x0000;
    static constexpr uint32_t FLAG_OPEN = 0x0001;
    static constexpr uint32_t FLAG_HEADER = 0x0002;

    PositionalIO() {}

    template <typename> friend class Pack;
    friend PositionalIO make_positional(std::string &);

  public:
    int open(const std::string &archive_path);
    ssize_t read(void *buf, size_t len, off64_t off);
    ssize_t write(const void *buf, size_t len, off64_t off);
    void close();
    ~PositionalIO();
  }; /* PositionalIO */

} /* namespace rgw::pack */
