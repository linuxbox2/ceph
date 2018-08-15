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

#include "WiredDB.h"

WiredDB::WiredDB(CephContext* cct, const std::string& path,
		 std::map<std::string,std::string> opt, void *p) :
  cct(cct),
  path(path),
  conn(nullptr),
  kv_options(opt),
  priv(p)
{
  abort();
} /* WiredDB::WiredDB */

int WiredDB::init(std::string option_str)
{
  abort();
} /* WiredDB::init */

int WiredDB::open(std::ostream &out, const std::vector<ColumnFamily>& cfm)
{
  abort();
} /* WiredDB::open */

int WiredDB::create_and_open(std::ostream &out, const vector<ColumnFamily>& cfm)
{
  abort();
} /* WiredDB::create_and_open */

void WiredDB::close()
{
  abort();
} /* WiredDB::close() */

int WiredDB::submit_transaction(KeyValueDB::Transaction t)
{
  abort();
} /* WiredDB::submit_transaction */

int WiredDB::submit_transaction_sync(KeyValueDB::Transaction t)
{
  abort();
} /* WiredDB::submit_transaction_sync*/

int WiredDB::get(
  const string &prefix,
  const std::set<string> &key,
  std::map<string, bufferlist> *out)
{
  abort();
} /* WiredDB::get 1 */

int WiredDB::get(
  const string &prefix,
  const string &key,
  bufferlist *out)
{
  abort();
} /* WiredDB::get 2 */

int WiredDB::get(
  const string &prefix,
  const char *key,
  size_t keylen,
  bufferlist *out)
{
  abort();
} /* WiredDB::get 3 */

KeyValueDB::Iterator WiredDB::get_iterator(const std::string& prefix)
{
  abort();
} /* WiredDB::get_iterator */

KeyValueDB::WholeSpaceIterator WiredDB::get_wholespace_iterator()
{
  abort();
} /* WiredDB::get_wholespace_iterator */

WiredDB::~WiredDB()
{
} /* WiredDB::~WiredDB() */

WiredDB::WiredDBTransactionImpl::WiredDBTransactionImpl(WiredDB* db)
{
  abort();
} /* WiredDB::WiredDBTransactionImpl::WiredDBTransactionImpl */

void WiredDB::WiredDBTransactionImpl::set(
  const std::string& prefix,
  const std::string& k,
  const buffer::list& bl)
{
  abort();
} /* WiredDB::WiredDBTransactionImpl::set 1 */

void WiredDB::WiredDBTransactionImpl::set(
  const std::string& prefix,
  const char* k,
  size_t keylen,
  const buffer::list& bl)
{
  abort();
} /* WiredDB::WiredDBTransactionImpl::set 2 */

void WiredDB::WiredDBTransactionImpl::rmkey(
  const std::string& prefix,
  const std::string& k)
{
  abort();
} /* WiredDB::WiredDBTransactionImpl::rmkey 1 */

void WiredDB::WiredDBTransactionImpl::rmkey(
  const std::string& prefix,
  const char* k,
  size_t keylen)
{
  abort();
} /* WiredDB::WiredDBTransactionImpl::rmkey 2 */

void WiredDB::WiredDBTransactionImpl::rm_single_key(
  const std::string& prefix,
  const std::string& k)
{
  abort();
} /* WiredDB::WiredDBTransactionImpl::rm_single_key */

void WiredDB::WiredDBTransactionImpl::rmkeys_by_prefix(
  const std::string& prefix)
{
  abort();
} /* WiredDB::WiredDBTransactionImpl::rmkeys_by_prefix */

void WiredDB::WiredDBTransactionImpl::rm_range_keys(
  const std::string& prefix,
  const std::string& start,
  const std::string& end)
{
  abort();
} /* WiredDB::WiredDBTransactionImpl::rm_range_keys */
