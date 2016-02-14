// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <stdint.h>
#include <errno.h>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <fstream>
#include <boost/algorithm/string.hpp>

#include "common/config.h"
#include "common/ceph_argparse.h"
#include "common/debug.h"
#include "global/global_init.h"
#include "include/assert.h"

#include "common/ceph_json.h"
#include "common/Formatter.h"

#include "include/utime.h"
#include "include/str_list.h"

#define dout_subsys ceph_subsys_rgw

namespace rgw {

  using std::string;
  
  class RGWToken {
  public:
    static constexpr auto type_name = "RGW_TOKEN";

    enum token_type : uint32_t {
      TOKEN_NONE,
	TOKEN_AD,
	TOKEN_KEYSTONE,
	TOKEN_LDAP,
    };

    static enum token_type to_type(const string& s) {
      if (boost::iequals(s, "ad"))
	return TOKEN_AD;
      if (boost::iequals(s, "ldap"))
	return TOKEN_LDAP;
      if (boost::iequals(s, "keystone"))
	return TOKEN_KEYSTONE;
      return TOKEN_NONE;
    }

    static const char* from_type(enum token_type type) {
      switch (type) {
      case TOKEN_AD:
	return "ad";
	break;
      case TOKEN_LDAP:
	return "ldap";
	break;
      case TOKEN_KEYSTONE:
	return "keystone";
	break;
      default:
	return "none";
      };
      return "none";
    }

    token_type type;
    string id;
    string key;
    
    virtual uint32_t version() const { return 1; };

    bool valid() {
      return ((type != TOKEN_NONE) &&
	      (! id.empty()) &&
	      (! key.empty()));
    }

    RGWToken()
      : type(TOKEN_NONE) {};
    
    RGWToken(enum token_type _type, std::string _id, std::string _key)
      : type(_type), id(_id), key(_key) {};

    RGWToken(const string& json) {
      JSONParser p;
      p.parse(json.c_str(), json.length());
      JSONDecoder::decode_json(RGWToken::type_name, *this, &p);
    }

    void encode(bufferlist& bl) const {
      uint32_t ver = version();
      string typestr{from_type(type)};
      ::encode(type_name, bl);
      ::encode(ver, bl);
      ::encode(typestr, bl);
      ::encode(id, bl);
      ::encode(key, bl);
    }

    void decode(bufferlist::iterator& bl) {
      string name;
      string typestr;
      uint32_t version;
      ::decode(name, bl);
      ::decode(version, bl);
      ::decode(typestr, bl);
      type = to_type(typestr.c_str());
      ::decode(id, bl);
      ::decode(key, bl);
    }

    void dump(Formatter* f) const {      
      ::encode_json("version", uint32_t(version()), f);
      ::encode_json("type", from_type(type), f);
      ::encode_json("id", id, f);
      ::encode_json("key", key, f);
    }

    void encode_json(Formatter* f) {
      RGWToken& token = *this;
      f->open_object_section(type_name);
      ::encode_json(type_name, token, f);
      f->close_section();
    }

    void decode_json(JSONObj *obj) {
      uint32_t version;
      string type_name;
      string typestr;
      JSONDecoder::decode_json("version", version, obj);
      JSONDecoder::decode_json("type", typestr, obj);
      type = to_type(typestr.c_str());
      JSONDecoder::decode_json("id", id, obj);
      JSONDecoder::decode_json("key", key, obj);
    }

    friend ostream& operator<<(ostream& os, const RGWToken& token);

    virtual ~RGWToken() {};
  };
  WRITE_CLASS_ENCODER(RGWToken)

  ostream& operator<<(ostream& os, const RGWToken& token)
  {
    os << "<<RGWToken"
       << " type=" << RGWToken::from_type(token.type)
       << " id=" << token.id
       << " key=" << token.key
       << ">>";
    return os;
  }
  
} /* namespace rgw */

namespace {

  using namespace rgw;
  using std::get;
  using std::string;

  RGWToken::token_type type{RGWToken::TOKEN_NONE};
  string access_key{""};
  string secret_key{""};

  Formatter* formatter{nullptr};

  bool verbose {false};
  bool do_encode {false};
  bool do_decode {false};

}

void usage() 
{
  cout << "usage: radosgw-token [options...]" << std::endl;
  cout << "\t(maybe exporting RGW_ACCESS_KEY_ID and RGW_SECRET_ACCESS_KEY)"
       << std::endl;
  cout << "\n";
  generic_client_usage();
}

int main(int argc, char **argv) 
{
  std::string val;
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);
  env_to_vec(args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  char *v{nullptr};
  v = getenv("RGW_ACCESS_KEY_ID");
  if (v) {
    access_key = v;
  }

  v = getenv("RGW_SECRET_ACCESS_KEY");
  if (v) {
    secret_key = v;
  }

  for (auto arg_iter = args.begin(); arg_iter != args.end();) {
    if (ceph_argparse_witharg(args, arg_iter, &val, "--access",
			      (char*) nullptr)) {
      access_key = val;
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--secret",
				     (char*) nullptr)) {
      secret_key = val;
    } else if (ceph_argparse_witharg(args, arg_iter, &val, "--ttype",
				     (char*) nullptr)) {
      for (const auto& ttype : {"ad", "ldap"}) {
	if (boost::iequals(val, ttype)) {
	  type = RGWToken::to_type(val);
	  break;
	}
      }
    } else if (ceph_argparse_flag(args, arg_iter, "--encode",
					    (char*) nullptr)) {
      do_encode = true;
    } else if (ceph_argparse_flag(args, arg_iter, "--decode",
					    (char*) nullptr)) {
      do_decode = true;
    } else if (ceph_argparse_flag(args, arg_iter, "--verbose",
					    (char*) nullptr)) {
      verbose = true;
    } else {
      ++arg_iter;
    }
  }

  if ((! do_encode) ||
      (type == RGWToken::TOKEN_NONE)) {
    usage();
    return -EINVAL;
  }

  formatter = new JSONFormatter(true /* pretty */);

  RGWToken token(type, access_key, secret_key);
  if (do_encode) {
    token.encode_json(formatter);
    std::ostringstream os;
    formatter->flush(os);
    string token_str = os.str();
    std::cout << token_str << std::endl;
    if (do_decode) {
      RGWToken token2(token_str);
      std::cout << "decoded: " << token2 << std::endl;
    }
  }

  return 0;
}
