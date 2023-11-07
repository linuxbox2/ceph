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

#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <system_error>
#include <sys/types.h>
#include <errno.h>
#include <stdint.h>

#include "rgw_pack.h"
#include "pack/positional_io.h"

#include "common/config.h"
#include "common/ceph_argparse.h"
#include "common/debug.h"
#include "global/global_init.h"
#include "include/ceph_assert.h"
#include "include/str_list.h"

#define dout_subsys ceph_subsys_rgw

namespace {

  using namespace rgw;

  enum class archive_op : uint8_t
    {
      none = 0,
      add,
      list,
      get,
      attrs_operate,
      remove
    };

  std::set<archive_op> file_path_ops({archive_op::add, archive_op::get,
      archive_op::attrs_operate, archive_op::remove});

  archive_op op;
  std::string file_path;
  std::string archive_path;
  bool verbose {false};
}

using namespace std;

void usage()
{
  cout << "usage: radosgw-zpp-arc --op=<archive op> "
       << "<archive_path> <file_path> [options...]"
       << std::endl;
  cout << "\t <archive op> := add | list | remove" << std::endl;
  cout << "\n";
  generic_client_usage();
}

int main(int argc, char **argv)
{
  auto args = argv_to_vec(argc, argv);
  std::string val;
  if (args.empty()) {
    cerr << argv[0] << ": -h or --help for usage" << std::endl;
    exit(1);
  }
  if (ceph_argparse_need_usage(args)) {
    usage();
    exit(0);
  }

  for (auto arg_iter = args.begin(); arg_iter != args.end();) {
    if (ceph_argparse_witharg(args, arg_iter, &val, "--op",
			      (char*) nullptr)) {
      if (val == "add") {
	op = archive_op::add;
      } else if (val == "list") {
	op = archive_op::list;
      }  else if (val == "get") {
	op = archive_op::get;
      } else if (val == "remove") {
	op = archive_op::remove;
      }
      continue;
    } else if (ceph_argparse_flag(args, arg_iter, "--verbose",
					    (char*) nullptr)) {
      verbose = true;
    } else {
      if (archive_path.empty()) {
          archive_path = *arg_iter;
      } else {
	if (file_path.empty()) {
          file_path = *arg_iter;
        }
      }
    }
    ++arg_iter;
  }

  namespace rp = rgw::pack;

  using PositionalIO = rp::PositionalIO;
  using Pack = rp::Pack<rp::PositionalIO>;

  if (archive_path.empty()) {
    cerr << "no archive_path provided" << std::endl;
    exit(1);
  }

  if (file_path_ops.contains(op)) {
    if (file_path.empty()) {
      cerr << "no file_path provided" << std::endl;
      exit(1);
    }
  }

  try {

    PositionalIO pio = rp::make_positional(archive_path);
    Pack pack = Pack::make_pack(pio);

    switch (op) {
    case archive_op::add:
      cout << argv[0] << " add " << file_path << " to archive " << archive_path
           << std::endl;
      // do pack.add_object()
      {
	using AddObj = Pack::AddObj;

        /* XXXX fake call */
	pack.add_object("blunderbus", [&](AddObj& ao) -> uint8_t {
	  ao.add_bytes(ao.get_pos(), static_cast<const char*>("my data 0"),
		       sizeof("my data 0"),
		       AddObj::AB_OK);
	  return AddObj::AB_EOF; // end of byte sequence
	});

      }
      break;
    case archive_op::list:
      cout << argv[0] << " list archive " << archive_path << std::endl;
      // do pack.list_objects()
      break;
    case archive_op::get:
      cout << argv[0] << " get " << file_path << " from archive "
           << archive_path << std::endl;
      // do pack.get_object()
      break;
    case archive_op::attrs_operate:
      cout << argv[0] << " attrs_operate not implemented" << std::endl;
      // do pack.attrs_operate()
      break;
    case archive_op::remove:
      cout << argv[0] << " remove " << file_path << " from archive "
           << archive_path << std::endl;
      // do pack.remove_object()
      break;
    default:
      cerr << "unknown archive_op" << std::endl;
      exit(1);
      break;
    }
  } /* try */
  catch (const std::system_error& e) {
    std::cerr << "Caught system_error with code "
      "[" << e.code() << "] meaning "
      "[" << e.what() << "]"
	      << std::endl;
  }

  return 0;
}
