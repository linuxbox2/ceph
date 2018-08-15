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

WiredDB::~WiredDB()
{
} /* WiredDB::~WiredDB() */
