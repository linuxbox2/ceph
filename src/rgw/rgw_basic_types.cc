// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <iostream>
#include <sstream>
#include <string>

#include "rgw_basic_types.h"
#include "rgw_xml.h"
#include "common/ceph_json.h"
#include "cls/user/cls_user_types.h"
#include "cls/rgw/cls_rgw_types.h"

using std::string;
using std::stringstream;

void decode_json_obj(rgw_user& val, JSONObj *obj)
{
  val.from_str(obj->get_data());
}

void encode_json(const char *name, const rgw_user& val, Formatter *f)
{
  f->dump_string(name, val.to_str());
}

void encode_xml(const char *name, const rgw_user& val, Formatter *f)
{
  encode_xml(name, val.to_str(), f);
}

rgw_bucket::rgw_bucket(const rgw_user& u, const cls_user_bucket& b) :
  tenant(u.tenant),
  name(b.name),
  marker(b.marker),
  bucket_id(b.bucket_id),
  explicit_placement(b.explicit_placement.data_pool,
                      b.explicit_placement.data_extra_pool,
                      b.explicit_placement.index_pool) {}

void rgw_bucket::convert(cls_user_bucket *b) const {
    b->name = name;
    b->marker = marker;
    b->bucket_id = bucket_id;
    b->explicit_placement.data_pool = explicit_placement.data_pool.to_str();
    b->explicit_placement.data_extra_pool = explicit_placement.data_extra_pool.to_str();
    b->explicit_placement.index_pool = explicit_placement.index_pool.to_str();
  }

rgw_obj_key::rgw_obj_key(const rgw_obj_index_key& k) {
  parse_index_key(k.name, &name, &ns);
  instance = k.instance;
}

bool rgw_obj_key::set(const rgw_obj_index_key& index_key) {
  if (!parse_raw_oid(index_key.name, this)) {
    return false;
  }
  instance = index_key.instance;
  return true;
}

void rgw_obj_key::get_index_key(rgw_obj_index_key *key) const {
  key->name = get_index_key_name();
  key->instance = instance;
}

namespace rgw {
namespace auth {
ostream& operator <<(ostream& m, const Principal& p) {
  if (p.is_wildcard()) {
    return m << "*";
  }

  m << "arn:aws:iam:" << p.get_tenant() << ":";
  if (p.is_tenant()) {
    return m << "root";
  }
  return m << (p.is_user() ? "user/" : "role/") << p.get_id();
}

} /* auth */
} /* rgw */
