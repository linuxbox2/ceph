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

#define LDAP_DEPRECATED 1
#include "ldap.h"

#include <stdint.h>
#include <tuple>
#include <iostream>
#include <vector>
#include <map>
#include <random>

#include <boost/regex.hpp>

#include "include/rados/librgw.h"
#include "include/rados/rgw_file.h"

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

  class ACCTokenHelper
  {
  public:
    typedef std::tuple<bool,std::string,std::string> DecodeResult;

    static DecodeResult decode(const std::string& encoded_token) {
      buffer::list bl, decoded_bl;
      bl.append(encoded_token);
      decoded_bl.decode_base64(bl);
      string str{decoded_bl.c_str()};
      boost::regex rgx("{(\\w+)::(\\w+)}.+");
      boost::cmatch match;
      if (boost::regex_match(str.c_str(), match, rgx)) {
	std::string uid = match[1];
	std::string pwd = match[2];
	return DecodeResult{true, uid, pwd};
      }
      return DecodeResult{false, "", ""};
    }
  private:
    ACCTokenHelper();
  };

  class LDAPHelper
  {
    std::string uri;
    std::string binddn;
    std::string searchdn;
    std::string memberattr;
    LDAP *ldap, *tldap;

  public:
    LDAPHelper(std::string _uri, std::string _binddn, std::string _searchdn,
	      std::string _memberattr)
      : uri(std::move(_uri)), binddn(std::move(_binddn)), searchdn(_searchdn),
	memberattr(_memberattr), ldap(nullptr) {
      // nothing
    }

    int init() {
      int ret;
      ret = ldap_initialize(&ldap, uri.c_str());
      return ret;
    }

    int bind() {
      return ldap_simple_bind_s(ldap, nullptr, nullptr);
    }

    int simple_bind(const char *dn, const std::string& pwd) {
      int ret = ldap_initialize(&tldap, uri.c_str());
      ret = ldap_simple_bind_s(tldap, dn, pwd.c_str());
      if (ret == LDAP_SUCCESS) {
	ldap_unbind(tldap);
	return 0;
      }
      return -1;
    }

    int auth(const std::string uid, const std::string pwd) {
      int ret;
      std::string filter;
      filter = "(";
      filter += memberattr;
      filter += "=";
      filter += uid;
      filter += ")";
      char *attrs[] = { const_cast<char*>(memberattr.c_str()), nullptr };
      LDAPMessage *answer, *entry;
      ret = ldap_search_s(ldap, searchdn.c_str(), LDAP_SCOPE_SUBTREE,
			  filter.c_str(), attrs, 0, &answer);
      if (ret == LDAP_SUCCESS) {
	entry = ldap_first_entry(ldap, answer);
	char *dn = ldap_get_dn(ldap, entry);
	std::cout << dn << std::endl;
	ret = simple_bind(dn, pwd);
	ldap_memfree(dn);
	ldap_msgfree(answer);
      }
      return ret;
    }
    
    ~LDAPHelper() {}

  };

  bool do_hexdump = false;

  string access_key("e2FkbWluOjpsaW51eGJveH0K"); // {admin::linuxbox} | base64
  string other_key("e2FkbWluOjpiYWRwYXNzfQo="); // {admin::badpass} | base64

  string ldap_uri = "ldaps://f23-kdc.rgw.com";
  string ldap_binddn = "uid=admin,cn=users,cn=accounts,dc=rgw,dc=com";
  string ldap_searchdn = "cn=users,cn=accounts,dc=rgw,dc=com";
  string ldap_memberattr = "uid";

  LDAPHelper ldh(ldap_uri, ldap_binddn, ldap_searchdn, ldap_memberattr);
  
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
  int ret = 0;
  auto at1 = ACCTokenHelper::decode(access_key);
  ASSERT_EQ(get<0>(at1), true);
  ret = ldh.auth(get<1>(at1), get<2>(at1));
  ASSERT_EQ(ret, 0);
  auto at2 = ACCTokenHelper::decode(other_key);
  ASSERT_EQ(get<0>(at2), true);
  ret = ldh.auth(get<1>(at2), get<2>(at2));
  ASSERT_NE(ret, 0);
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

  v = getenv("AWS_ACCESS_KEY_ID");
  if (v) {
    access_key = v;
  }

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
