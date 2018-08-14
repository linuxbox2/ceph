// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013- Sage Weil <sage@inktank.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_WIRED_H
#define CEPH_WIRED_H

#include <set>
#include <map>
#include <string>
#include <memory>

#include "include/types.h"
#include "include/buffer_fwd.h"
#include "KeyValueDB.h"
#include <boost/scoped_ptr.hpp>
#include <errno.h>
#include "common/errno.h"
#include "common/dout.h"
#include "include/assert.h"
#include "common/ceph_context.h"

#include "wiredtiger.h"

class WiredDB : public KeyValueDB
{
  CephContext *cct;
  std::string path;
  WT_CONNECTION *conn;
  map<std::string,std::string> kv_options;
  void *priv;
  std::string options_str;

  WiredDB(CephContext* cct, const std::string& path,
	       std::map<std::string,std::string> opt, void *p) :
    cct(cct),
    path(path),
    conn(nullptr),
    kv_options(opt),
    priv(p)
  {}

  ~WiredDB() override;

  /* nested classes */

  class WiredDBTransactionImpl : public KeyValueDB::TransactionImpl {
  public:
    WiredDB* db;
    //WT_SESSION?

    explicit WiredDBTransactionImpl(WiredDB* db);

  public:
    void set(
      const std::string& prefix,
      const std::string& k,
      const buffer::list& bl) override;
    void set(
      const std::string& prefix,
      const char* k,
      size_t keylen,
      const buffer::list& bl) override;
    void rmkey(
      const std::string& prefix,
      const std::string& k) override;
    void rmkey(
      const std::string& prefix,
      const char* k,
      size_t keylen) override;
    void rm_single_key(
      const std::string& prefix,
      const std::string& k) override;
    void rmkeys_by_prefix(
      const std::string& prefix
      ) override;
    void rm_range_keys(
      const std::string& prefix,
      const std::string& start,
      const std::string& end) override;
  }; /* WiredDBTransactionImpl */

  KeyValueDB::Transaction get_transaction() override {
    return std::make_shared<WiredDBTransactionImpl>(this);
  }

  int submit_transaction(KeyValueDB::Transaction t) override;
  int submit_transaction_sync(KeyValueDB::Transaction t) override;
  int get(
    const string &prefix,
    const std::set<string> &key,
    std::map<string, bufferlist> *out
    ) override;
  int get(
    const string &prefix,
    const string &key,
    bufferlist *out
    ) override;
  int get(
    const string &prefix,
    const char *key,
    size_t keylen,
    bufferlist *out) override;

  class WiredDBWholeSpaceIteratorImpl :
    public KeyValueDB::WholeSpaceIteratorImpl {
  protected:
    WT_CURSOR* c;
  public:
    explicit WiredDBWholeSpaceIteratorImpl(WT_CURSOR* c) :
      c(c) { }
    ~WiredDBWholeSpaceIteratorImpl() override;

    int seek_to_first() override;
    int seek_to_first(const std::string& prefix) override;
    int seek_to_last() override;
    int seek_to_last(const std::string& prefix) override;
    int upper_bound(const std::string& prefix,
		    const std::string& after) override;
    int lower_bound(const std::string& prefix, const std::string& to) override;
    bool valid() override;
    int next() override;
    int prev() override;
    std::string key() override;
    pair<std::string,std::string> raw_key() override;
    bool raw_key_is_prefixed(const std::string& prefix) override;
    buffer::list value() override;
    buffer::ptr value_as_ptr() override;
    int status() override;
    size_t key_size() override;
    size_t value_size() override;
  }; /* WiredDBWholeSpaceIteratorImpl */

  KeyValueDB::Iterator get_iterator(const std::string& prefix) override;
  KeyValueDB::WholeSpaceIterator get_wholespace_iterator() override;

}; /* WiredDB */

#endif /* CEPH_WIRED_H */
