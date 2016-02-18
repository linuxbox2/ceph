// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 Red Hat, Inc
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef RGW_TOKEN_H
#define RGW_TOKEN_H

#include <stdint.h>
#include <boost/algorithm/string.hpp>

#include "common/ceph_json.h"
#include "common/Formatter.h"

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

#endif /* RGW_TOKEN_H */
