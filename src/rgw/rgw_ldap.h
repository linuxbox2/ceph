// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef RGW_LDAP_H
#define RGW_LDAP_H

#define LDAP_DEPRECATED 1
#include "ldap.h"

#include <stdint.h>
#include <tuple>
#include <vector>
#include <string>
#include <iostream>

#include <boost/regex.hpp>
#include <include/buffer.h>

namespace rgw {

  class ACCTokenHelper
  {
  public:
    typedef std::tuple<bool,std::string,std::string> DecodeResult;

    static constexpr const char* CRED_REGEX = "{([\\w-]+)::([\\w-]+)}.+";

    static boost::regex rgx;

    static DecodeResult decode(const std::string& encoded_token) {
      buffer::list bl, decoded_bl;
      bl.append(encoded_token);
      decoded_bl.decode_base64(bl);
      std::string str{decoded_bl.c_str()};
      boost::cmatch match;
      if (boost::regex_match(str.c_str(), match, rgx)) {
	std::string uid = match[1];
	std::string pwd = match[2];
	return DecodeResult{true, std::move(uid), std::move(pwd)};
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
	//std::cout << dn << std::endl;
	ret = simple_bind(dn, pwd);
	ldap_memfree(dn);
	ldap_msgfree(answer);
      }
      return ret;
    }
    
    ~LDAPHelper() {
      if (ldap)
	ldap_unbind(ldap);
    }

  };

} /* namespace rgw */

#endif /* RGW_LDAP_H */
