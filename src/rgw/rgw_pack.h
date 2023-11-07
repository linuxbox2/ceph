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
#include <sys/types.h>
#include <stdint.h>
#include <unistd.h>
#include "include/function2.hpp"
#include <boost/container/flat_map.hpp>
#include <xxhash.h>

namespace rgw::pack {

  /* TODO: struct version */

  struct Header
  {
  public:
    static constexpr uint32_t magic = 0x845fed;

    uint32_t hmagic{0};
    uint16_t obj_count{0};
    uint32_t flags{0};
    uint32_t obj_entry_off{128};
    uint32_t toc_off{0};
    uint32_t new_entry_off{0};

    // TODO: implement
  }; /* Header */

  struct ObjEntry
  {
    std::string name; // XXX zap
    std::string cksum; // XXX use mine?
    uint32_t flags;
    uint32_t size;
    uint32_t next_obj_entry;
  }; /* ObjEntry */

  struct TOCEntry
  {
    static constexpr uint16_t FLAG_NONE     = 0x00;
    static constexpr uint16_t FLAG_DELETED  = 0x01;

    std::string name;
    uint32_t obj_entry_off{0};
    uint16_t flags;
  }; /* TOCentry */

  class TableOfContents
  {
    using bcfm = boost::container::flat_map<std::string, std::string>;
    // TODO: something
  }; /* TableOfContents */
  
  /* type erasing i/o types */
  template <typename IO>
  class Pack
  {
  private:
    IO io;
    Header hdr;
    uint32_t flags;

    Pack(IO& io);
    int read_header();

  public:
    static constexpr uint32_t FLAG_NONE =   0x0000;
    static constexpr uint32_t FLAG_HEADER = 0x0001;
    static constexpr uint32_t FLAG_CREATE = 0x0002;
    static constexpr uint32_t FLAG_HDIRTY = 0x0003;

    class AddObj
    {
    private:
      Pack& pack;
      off64_t pos{0};

    public:
      static constexpr uint8_t AB_OK =  0x00;
      static constexpr uint8_t AB_EOF = 0x01;

      off64_t get_pos() const { return pos; }
      AddObj(Pack& pack) : pack(pack) {}
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

    PositionalIO() {}

    template <typename> friend class Pack;
    friend PositionalIO make_positional(std::string &);

  public:
    uint32_t get_flags() const {
      return flags;
    }

    int open(const std::string &archive_path);
    ssize_t read(void *buf, size_t len, off64_t off);
    ssize_t write(const void *buf, size_t len, off64_t off);
    void close();
    ~PositionalIO();
  }; /* PositionalIO */

} /* namespace rgw::pack */
