// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <stdint.h>
#include <tuple>
#include <iostream>
#include <vector>
#include <map>
#include <random>

#include "rgw/rgw_ldap.h"

#include "gtest/gtest.h"
#include "common/ceph_argparse.h"
#include "common/debug.h"
#include "global/global_init.h"

#define dout_subsys ceph_subsys_rgw

namespace {

  struct {
    int argc;
    char **argv;
  } saved_args;

  bool do_hexdump = false;

  string access_key("e2FkbWluOjpsaW51eGJveH0K"); // {admin::linuxbox} | base64
  string other_key("e2FkbWluOjpiYWRwYXNzfQo="); // {admin::badpass} | base64
  string hyphen_key("e21hdHQtbGRhcDo6bGludXhib3h9Cg=="); // {matt-ldap::linuxbox} | base64

  string ldap_uri = "ldaps://f23-kdc.rgw.com";
  string ldap_binddn = "uid=admin,cn=users,cn=accounts,dc=rgw,dc=com";
  string ldap_searchdn = "cn=users,cn=accounts,dc=rgw,dc=com";
  string ldap_memberattr = "uid";

  rgw::LDAPHelper ldh(ldap_uri, ldap_binddn, ldap_searchdn, ldap_memberattr);
  
} /* namespace */

TEST(LibRGWLDAP, INIT) {
  int ret = ldh.init();
  ASSERT_EQ(ret, 0);
}

TEST(LibRGWLDAP, BIND) {
  int ret = ldh.bind();
  ASSERT_EQ(ret, 0);
}

TEST(LibRGWLDAP, AUTH) {
  using std::get;
  using namespace rgw;
  int ret = 0;
  auto at1 = ACCTokenHelper::decode(access_key);
  ASSERT_EQ(get<0>(at1), true);
  ret = ldh.auth(get<1>(at1), get<2>(at1));
  ASSERT_EQ(ret, 0);
  auto at2 = ACCTokenHelper::decode(other_key);
  ASSERT_EQ(get<0>(at2), true);
  ret = ldh.auth(get<1>(at2), get<2>(at2));
  ASSERT_NE(ret, 0);
  auto at3 = ACCTokenHelper::decode(hyphen_key);
  ASSERT_EQ(get<0>(at3), true);
  ret = ldh.auth(get<1>(at3), get<2>(at3));
  ASSERT_EQ(ret, 0);
}

TEST(LibRGW, SHUTDOWN) {
  // nothing
}

int main(int argc, char *argv[])
{
  char *v{nullptr};
  string val;
  vector<const char*> args;

  argv_to_vec(argc, const_cast<const char**>(argv), args);
  env_to_vec(args);

  for (auto arg_iter = args.begin(); arg_iter != args.end();) {
    if (ceph_argparse_witharg(args, arg_iter, &val, "--access",
			      (char*) nullptr)) {
      access_key = val;
    } else if (ceph_argparse_flag(args, arg_iter, "--hexdump",
					    (char*) nullptr)) {
      do_hexdump = true;
    } else {
      ++arg_iter;
    }
  }

  /* dont accidentally run as anonymous */
  if (access_key == "") {
    std::cout << argv[0] << " no AWS credentials, exiting" << std::endl;
    return EPERM;
  }

  saved_args.argc = argc;
  saved_args.argv = argv;

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
