// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_BASIC_TYPES_H
#define CEPH_RGW_BASIC_TYPES_H

#include <string>

#include "include/types.h"

struct rgw_user {
  std::string tenant;
  std::string id;

  rgw_user() {}
  // cppcheck-suppress noExplicitConstructor
  rgw_user(const std::string& s) {
    from_str(s);
  }
  rgw_user(const std::string& tenant, const std::string& id)
    : tenant(tenant),
      id(id) {
  }
  rgw_user(std::string&& tenant, std::string&& id)
    : tenant(std::move(tenant)),
      id(std::move(id)) {
  }

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    encode(tenant, bl);
    encode(id, bl);
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(tenant, bl);
    decode(id, bl);
    DECODE_FINISH(bl);
  }

  void to_str(std::string& str) const {
    if (!tenant.empty()) {
      str = tenant + '$' + id;
    } else {
      str = id;
    }
  }

  void clear() {
    tenant.clear();
    id.clear();
  }

  bool empty() const {
    return id.empty();
  }

  string to_str() const {
    string s;
    to_str(s);
    return s;
  }

  void from_str(const std::string& str) {
    size_t pos = str.find('$');
    if (pos != std::string::npos) {
      tenant = str.substr(0, pos);
      id = str.substr(pos + 1);
    } else {
      tenant.clear();
      id = str;
    }
  }

  rgw_user& operator=(const string& str) {
    from_str(str);
    return *this;
  }

  int compare(const rgw_user& u) const {
    int r = tenant.compare(u.tenant);
    if (r != 0)
      return r;

    return id.compare(u.id);
  }
  int compare(const string& str) const {
    rgw_user u(str);
    return compare(u);
  }

  bool operator!=(const rgw_user& rhs) const {
    return (compare(rhs) != 0);
  }
  bool operator==(const rgw_user& rhs) const {
    return (compare(rhs) == 0);
  }
  bool operator<(const rgw_user& rhs) const {
    if (tenant < rhs.tenant) {
      return true;
    } else if (tenant > rhs.tenant) {
      return false;
    }
    return (id < rhs.id);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<rgw_user*>& o);
};
WRITE_CLASS_ENCODER(rgw_user)

// Represents an identity. This is more wide-ranging than a
// 'User'. Its purposes is to be matched against by an
// IdentityApplier. The internal representation will doubtless change as
// more types are added. We may want to expose the type enum and make
// the member public so people can switch/case on it.

namespace rgw {
namespace auth {
class Principal {
  enum types { User, Role, Tenant, Wildcard, OidcProvider, AssumedRole };
  types t;
  rgw_user u;
  string idp_url;

  explicit Principal(types t)
    : t(t) {}

  Principal(types t, std::string&& n, std::string i)
    : t(t), u(std::move(n), std::move(i)) {}

  Principal(string&& idp_url)
    : t(OidcProvider), idp_url(std::move(idp_url)) {}

public:

  static Principal wildcard() {
    return Principal(Wildcard);
  }

  static Principal user(std::string&& t, std::string&& u) {
    return Principal(User, std::move(t), std::move(u));
  }

  static Principal role(std::string&& t, std::string&& u) {
    return Principal(Role, std::move(t), std::move(u));
  }

  static Principal tenant(std::string&& t) {
    return Principal(Tenant, std::move(t), {});
  }

  static Principal oidc_provider(string&& idp_url) {
    return Principal(std::move(idp_url));
  }

  static Principal assumed_role(std::string&& t, std::string&& u) {
    return Principal(AssumedRole, std::move(t), std::move(u));
  }

  bool is_wildcard() const {
    return t == Wildcard;
  }

  bool is_user() const {
    return t == User;
  }

  bool is_role() const {
    return t == Role;
  }

  bool is_tenant() const {
    return t == Tenant;
  }

  bool is_oidc_provider() const {
    return t == OidcProvider;
  }

  bool is_assumed_role() const {
    return t == AssumedRole;
  }

  const std::string& get_tenant() const {
    return u.tenant;
  }

  const std::string& get_id() const {
    return u.id;
  }

  const string& get_idp_url() const {
    return idp_url;
  }

  const string& get_role_session() const {
    return u.id;
  }

  const string& get_role() const {
    return u.id;
  }

  bool operator ==(const Principal& o) const {
    return (t == o.t) && (u == o.u);
  }

  bool operator <(const Principal& o) const {
    return (t < o.t) || ((t == o.t) && (u < o.u));
  }
};

std::ostream& operator <<(std::ostream& m, const Principal& p);
}
}

class JSONObj;

void decode_json_obj(rgw_user& val, JSONObj *obj);
void encode_json(const char *name, const rgw_user& val, Formatter *f);
void encode_xml(const char *name, const rgw_user& val, Formatter *f);

inline ostream& operator<<(ostream& out, const rgw_user &u) {
  string s;
  u.to_str(s);
  return out << s;
}

/* rados specific */
struct rgw_pool {
  std::string name;
  std::string ns;

  rgw_pool() = default;
  rgw_pool(const rgw_pool& _p) : name(_p.name), ns(_p.ns) {}
  rgw_pool(rgw_pool&&) = default;
  rgw_pool(const string& _s) {
    from_str(_s);
  }
  rgw_pool(const string& _name, const string& _ns) : name(_name), ns(_ns) {}

  string to_str() const;
  void from_str(const string& s);

  void init(const string& _s) {
    from_str(_s);
  }

  bool empty() const {
    return name.empty();
  }

  int compare(const rgw_pool& p) const {
    int r = name.compare(p.name);
    if (r != 0) {
      return r;
    }
    return ns.compare(p.ns);
  }

  void encode(bufferlist& bl) const {
     ENCODE_START(10, 10, bl);
    encode(name, bl);
    encode(ns, bl);
    ENCODE_FINISH(bl);
  }

  void decode_from_bucket(bufferlist::const_iterator& bl);

  void decode(bufferlist::const_iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN(10, 3, 3, bl);

    decode(name, bl);

    if (struct_v < 10) {

    /*
     * note that rgw_pool can be used where rgw_bucket was used before
     * therefore we inherit rgw_bucket's old versions. However, we only
     * need the first field from rgw_bucket. unless we add more fields
     * in which case we'll need to look at struct_v, and check the actual
     * version. Anything older than 10 needs to be treated as old rgw_bucket
     */

    } else {
      decode(ns, bl);
    }

    DECODE_FINISH(bl);
  }

  rgw_pool& operator=(const rgw_pool&) = default;

  bool operator==(const rgw_pool& p) const {
    return (compare(p) == 0);
  }
  bool operator!=(const rgw_pool& p) const {
    return !(*this == p);
  }
  bool operator<(const rgw_pool& p) const {
    int r = name.compare(p.name);
    if (r == 0) {
      return (ns.compare(p.ns) < 0);
    }
    return (r < 0);
  }
};
WRITE_CLASS_ENCODER(rgw_pool)

inline ostream& operator<<(ostream& out, const rgw_pool& p) {
  out << p.to_str();
  return out;
}

struct rgw_data_placement_target {
  rgw_pool data_pool;
  rgw_pool data_extra_pool;
  rgw_pool index_pool;

  rgw_data_placement_target() = default;
  rgw_data_placement_target(const rgw_data_placement_target&) = default;
  rgw_data_placement_target(rgw_data_placement_target&&) = default;

  rgw_data_placement_target(const rgw_pool& data_pool,
                            const rgw_pool& data_extra_pool,
                            const rgw_pool& index_pool)
    : data_pool(data_pool),
      data_extra_pool(data_extra_pool),
      index_pool(index_pool) {
  }

  rgw_data_placement_target&
  operator=(const rgw_data_placement_target&) = default;

  const rgw_pool& get_data_extra_pool() const {
    if (data_extra_pool.empty()) {
      return data_pool;
    }
    return data_extra_pool;
  }

  int compare(const rgw_data_placement_target& t) {
    int c = data_pool.compare(t.data_pool);
    if (c != 0) {
      return c;
    }
    c = data_extra_pool.compare(t.data_extra_pool);
    if (c != 0) {
      return c;
    }
    return index_pool.compare(t.index_pool);
  };

  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
}; /* rgw_data_placement_target */

class cls_user_bucket;

struct rgw_bucket {
  std::string tenant;
  std::string name;
  std::string marker;
  std::string bucket_id;
  rgw_data_placement_target explicit_placement;

  std::string oid; /*
                    * runtime in-memory only info. If not empty, points to the bucket instance object
                    */

  rgw_bucket() { }
  // cppcheck-suppress noExplicitConstructor
  explicit rgw_bucket(const rgw_user& u, const cls_user_bucket& b);
  rgw_bucket(const rgw_bucket&) = default;
  rgw_bucket(rgw_bucket&&) = default;

  void convert(cls_user_bucket *b) const;

  void encode(bufferlist& bl) const {
     ENCODE_START(10, 10, bl);
    encode(name, bl);
    encode(marker, bl);
    encode(bucket_id, bl);
    encode(tenant, bl);
    bool encode_explicit = !explicit_placement.data_pool.empty();
    encode(encode_explicit, bl);
    if (encode_explicit) {
      encode(explicit_placement.data_pool, bl);
      encode(explicit_placement.data_extra_pool, bl);
      encode(explicit_placement.index_pool, bl);
    }
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::const_iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN(10, 3, 3, bl);
    decode(name, bl);
    if (struct_v < 10) {
      decode(explicit_placement.data_pool.name, bl);
    }
    if (struct_v >= 2) {
      decode(marker, bl);
      if (struct_v <= 3) {
        uint64_t id;
        decode(id, bl);
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRIu64, id);
        bucket_id = buf;
      } else {
        decode(bucket_id, bl);
      }
    }
    if (struct_v < 10) {
      if (struct_v >= 5) {
        decode(explicit_placement.index_pool.name, bl);
      } else {
        explicit_placement.index_pool = explicit_placement.data_pool;
      }
      if (struct_v >= 7) {
        decode(explicit_placement.data_extra_pool.name, bl);
      }
    }
    if (struct_v >= 8) {
      decode(tenant, bl);
    }
    if (struct_v >= 10) {
      bool decode_explicit = !explicit_placement.data_pool.empty();
      decode(decode_explicit, bl);
      if (decode_explicit) {
        decode(explicit_placement.data_pool, bl);
        decode(explicit_placement.data_extra_pool, bl);
        decode(explicit_placement.index_pool, bl);
      }
    }
    DECODE_FINISH(bl);
  }

  void update_bucket_id(const string& new_bucket_id) {
    bucket_id = new_bucket_id;
    oid.clear();
  }

  // format a key for the bucket/instance. pass delim=0 to skip a field
  std::string get_key(char tenant_delim = '/',
                      char id_delim = ':',
                      size_t reserve = 0) const;

  const rgw_pool& get_data_extra_pool() const {
    return explicit_placement.get_data_extra_pool();
  }

  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
  static void generate_test_instances(list<rgw_bucket*>& o);

  rgw_bucket& operator=(const rgw_bucket&) = default;

  bool operator<(const rgw_bucket& b) const {
    if (tenant == b.tenant) {
      return name < b.name;
    } else {
      return tenant < b.tenant;
    }
  }

  bool operator==(const rgw_bucket& b) const {
    return (tenant == b.tenant) && (name == b.name) && \
           (bucket_id == b.bucket_id);
  }
  bool operator!=(const rgw_bucket& b) const {
    return (tenant != b.tenant) || (name != b.name) ||
           (bucket_id != b.bucket_id);
  }
  static bool full_equal(const rgw_bucket& a, const rgw_bucket&b) {
    return (a.name == b.name) && (a.bucket_id == b.bucket_id) &&
      (a.tenant == b.tenant);
  }

};
WRITE_CLASS_ENCODER(rgw_bucket)

inline ostream& operator<<(ostream& out, const rgw_bucket &b) {
  out << b.name << "[" << b.marker << "]";
  return out;
}

struct cls_rgw_obj_key;
typedef cls_rgw_obj_key rgw_obj_index_key;

struct rgw_obj_key {
  string name;
  string instance;
  string ns;

  rgw_obj_key() {}
  // cppcheck-suppress noExplicitConstructor
  rgw_obj_key(const string& n) : name(n) {}
  rgw_obj_key(const string& n, const string& i) : name(n), instance(i) {}
  rgw_obj_key(const string& n, const string& i, const string& _ns) : name(n), instance(i), ns(_ns) {}
  rgw_obj_key(const rgw_obj_index_key& k);

  static void parse_index_key(const string& key, string *name, string *ns) {
    if (key[0] != '_') {
      *name = key;
      ns->clear();
      return;
    }
    if (key[1] == '_') {
      *name = key.substr(1);
      ns->clear();
      return;
    }
    ssize_t pos = key.find('_', 1);
    if (pos < 0) {
      /* shouldn't happen, just use key */
      *name = key;
      ns->clear();
      return;
    }

    *name = key.substr(pos + 1);
    *ns = key.substr(1, pos -1);
  }

  void set(const string& n) {
    name = n;
    instance.clear();
    ns.clear();
  }

  void set(const string& n, const string& i) {
    name = n;
    instance = i;
    ns.clear();
  }

  void set(const string& n, const string& i, const string& _ns) {
    name = n;
    instance = i;
    ns = _ns;
  }

  bool set(const rgw_obj_index_key& index_key);

  void set_instance(const string& i) {
    instance = i;
  }

  const string& get_instance() const {
    return instance;
  }

  void set_ns(const std::string& _ns) {
    ns = _ns;
  }

  const std::string& get_ns() const {
    return ns;
  }

  string get_index_key_name() const {
    if (ns.empty()) {
      if (name.size() < 1 || name[0] != '_') {
        return name;
      }
      return string("_") + name;
    };

    char buf[ns.size() + 16];
    snprintf(buf, sizeof(buf), "_%s_", ns.c_str());
    return string(buf) + name;
  };

  void get_index_key(rgw_obj_index_key *key) const;

  string get_loc() const {
    /*
     * For backward compatibility. Older versions used to have object locator on all objects,
     * however, the name was the effective object locator. This had the same effect as not
     * having object locator at all for most objects but the ones that started with underscore as
     * these were escaped.
     */
    if (name[0] == '_' && ns.empty()) {
      return name;
    }

    return string();
  }

  bool empty() const {
    return name.empty();
  }

  bool have_null_instance() const {
    return instance == "null";
  }

  bool have_instance() const {
    return !instance.empty();
  }

  bool need_to_encode_instance() const {
    return have_instance() && !have_null_instance();
  }

  string get_oid() const {
    if (ns.empty() && !need_to_encode_instance()) {
      if (name.size() < 1 || name[0] != '_') {
        return name;
      }
      return string("_") + name;
    }

    string oid = "_";
    oid.append(ns);
    if (need_to_encode_instance()) {
      oid.append(string(":") + instance);
    }
    oid.append("_");
    oid.append(name);
    return oid;
  }

  bool operator==(const rgw_obj_key& k) const {
    return (name.compare(k.name) == 0) &&
           (instance.compare(k.instance) == 0);
  }

  bool operator<(const rgw_obj_key& k) const {
    int r = name.compare(k.name);
    if (r == 0) {
      r = instance.compare(k.instance);
    }
    return (r < 0);
  }

  bool operator<=(const rgw_obj_key& k) const {
    return !(k < *this);
  }

  static void parse_ns_field(string& ns, string& instance) {
    int pos = ns.find(':');
    if (pos >= 0) {
      instance = ns.substr(pos + 1);
      ns = ns.substr(0, pos);
    } else {
      instance.clear();
    }
  }

  // takes an oid and parses out the namespace (ns), name, and
  // instance
  static bool parse_raw_oid(const string& oid, rgw_obj_key *key) {
    key->instance.clear();
    key->ns.clear();
    if (oid[0] != '_') {
      key->name = oid;
      return true;
    }

    if (oid.size() >= 2 && oid[1] == '_') {
      key->name = oid.substr(1);
      return true;
    }

    if (oid.size() < 3) // for namespace, min size would be 3: _x_
      return false;

    size_t pos = oid.find('_', 2); // oid must match ^_[^_].+$
    if (pos == string::npos)
      return false;

    key->ns = oid.substr(1, pos - 1);
    parse_ns_field(key->ns, key->instance);

    key->name = oid.substr(pos + 1);
    return true;
  }

  /**
   * Translate a namespace-mangled object name to the user-facing name
   * existing in the given namespace.
   *
   * If the object is part of the given namespace, it returns true
   * and cuts down the name to the unmangled version. If it is not
   * part of the given namespace, it returns false.
   */
  static bool oid_to_key_in_ns(const string& oid, rgw_obj_key *key, const string& ns) {
    bool ret = parse_raw_oid(oid, key);
    if (!ret) {
      return ret;
    }

    return (ns == key->ns);
  }

  /**
   * Given a mangled object name and an empty namespace string, this
   * function extracts the namespace into the string and sets the object
   * name to be the unmangled version.
   *
   * It returns true after successfully doing so, or
   * false if it fails.
   */
  static bool strip_namespace_from_name(string& name, string& ns, string& instance) {
    ns.clear();
    instance.clear();
    if (name[0] != '_') {
      return true;
    }

    size_t pos = name.find('_', 1);
    if (pos == string::npos) {
      return false;
    }

    if (name[1] == '_') {
      name = name.substr(1);
      return true;
    }

    size_t period_pos = name.find('.');
    if (period_pos < pos) {
      return false;
    }

    ns = name.substr(1, pos-1);
    name = name.substr(pos+1, string::npos);

    parse_ns_field(ns, instance);
    return true;
  }

  void encode(bufferlist& bl) const {
    ENCODE_START(2, 1, bl);
    encode(name, bl);
    encode(instance, bl);
    encode(ns, bl);
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::const_iterator& bl) {
    DECODE_START(2, bl);
    decode(name, bl);
    decode(instance, bl);
    if (struct_v >= 2) {
      decode(ns, bl);
    }
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);

  string to_str() const {
    if (instance.empty()) {
      return name;
    }
    char buf[name.size() + instance.size() + 16];
    snprintf(buf, sizeof(buf), "%s[%s]", name.c_str(), instance.c_str());
    return buf;
  }
};
WRITE_CLASS_ENCODER(rgw_obj_key)

inline ostream& operator<<(ostream& out, const rgw_obj_key &o) {
  return out << o.to_str();
}

static string RGW_STORAGE_CLASS_STANDARD = "STANDARD";

struct rgw_placement_rule {
  std::string name;
  std::string storage_class;

  rgw_placement_rule() {}
  rgw_placement_rule(const string& _n, const string& _sc) : name(_n), storage_class(_sc) {}
  rgw_placement_rule(const rgw_placement_rule& _r, const string& _sc) : name(_r.name) {
    if (!_sc.empty()) {
      storage_class = _sc;
    } else {
      storage_class = _r.storage_class;
    }
  }

  bool empty() const {
    return name.empty() && storage_class.empty();
  }

  void inherit_from(const rgw_placement_rule& r) {
    if (name.empty()) {
      name = r.name;
    }
    if (storage_class.empty()) {
      storage_class = r.storage_class;
    }
  }

  void clear() {
    name.clear();
    storage_class.clear();
  }

  void init(const string& n, const string& c) {
    name = n;
    storage_class = c;
  }

  static const string& get_canonical_storage_class(const string& storage_class) {
    if (storage_class.empty()) {
      return RGW_STORAGE_CLASS_STANDARD;
    }
    return storage_class;
  }

  const string& get_storage_class() const {
    return get_canonical_storage_class(storage_class);
  }
  
  int compare(const rgw_placement_rule& r) const {
    int c = name.compare(r.name);
    if (c != 0) {
      return c;
    }
    return get_storage_class().compare(r.get_storage_class());
  }

  bool operator==(const rgw_placement_rule& r) const {
    return (name == r.name &&
            get_storage_class() == r.get_storage_class());
  }

  bool operator!=(const rgw_placement_rule& r) const {
    return !(*this == r);
  }

  void encode(bufferlist& bl) const {
    /* no ENCODE_START/END due to backward compatibility */
    std::string s = to_str();
    ceph::encode(s, bl);
  }

  void decode(bufferlist::const_iterator& bl) {
    std::string s;
    ceph::decode(s, bl);
    from_str(s);
  } 

  std::string to_str() const {
    if (standard_storage_class()) {
      return name;
    }
    return to_str_explicit();
  }

  std::string to_str_explicit() const {
    return name + "/" + storage_class;
  }

  void from_str(const std::string& s) {
    size_t pos = s.find("/");
    if (pos == std::string::npos) {
      name = s;
      storage_class.clear();
      return;
    }
    name = s.substr(0, pos);
    storage_class = s.substr(pos + 1);
  }

  bool standard_storage_class() const {
    return storage_class.empty() || storage_class == RGW_STORAGE_CLASS_STANDARD;
  }
};
WRITE_CLASS_ENCODER(rgw_placement_rule)

struct rgw_bucket_placement {
  rgw_placement_rule placement_rule;
  rgw_bucket bucket;

  void dump(Formatter *f) const;
};

struct rgw_obj {
  rgw_bucket bucket;
  rgw_obj_key key;

  bool in_extra_data{false}; /* in-memory only member, does not serialize */

  // Represents the hash index source for this object once it is set (non-empty)
  std::string index_hash_source;

  rgw_obj() {}
  rgw_obj(const rgw_bucket& b, const std::string& name) : bucket(b), key(name) {}
  rgw_obj(const rgw_bucket& b, const rgw_obj_key& k) : bucket(b), key(k) {}
  rgw_obj(const rgw_bucket& b, const rgw_obj_index_key& k) : bucket(b), key(k) {}

  void init(const rgw_bucket& b, const std::string& name) {
    bucket = b;
    key.set(name);
  }
  void init(const rgw_bucket& b, const std::string& name, const string& i, const string& n) {
    bucket = b;
    key.set(name, i, n);
  }
  void init_ns(const rgw_bucket& b, const std::string& name, const string& n) {
    bucket = b;
    key.name = name;
    key.instance.clear();
    key.ns = n;
  }

  bool empty() const {
    return key.empty();
  }

  void set_key(const rgw_obj_key& k) {
    key = k;
  }

  string get_oid() const {
    return key.get_oid();
  }

  const string& get_hash_object() const {
    return index_hash_source.empty() ? key.name : index_hash_source;
  }

  void set_in_extra_data(bool val) {
    in_extra_data = val;
  }

  bool is_in_extra_data() const {
    return in_extra_data;
  }

  void encode(bufferlist& bl) const {
    ENCODE_START(6, 6, bl);
    encode(bucket, bl);
    encode(key.ns, bl);
    encode(key.name, bl);
    encode(key.instance, bl);
//    encode(placement_id, bl);
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::const_iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN(6, 3, 3, bl);
    if (struct_v < 6) {
      string s;
      decode(bucket.name, bl); /* bucket.name */
      decode(s, bl); /* loc */
      decode(key.ns, bl);
      decode(key.name, bl);
      if (struct_v >= 2)
        decode(bucket, bl);
      if (struct_v >= 4)
        decode(key.instance, bl);
      if (key.ns.empty() && key.instance.empty()) {
        if (key.name[0] == '_') {
          key.name = key.name.substr(1);
        }
      } else {
        if (struct_v >= 5) {
          decode(key.name, bl);
        } else {
          ssize_t pos = key.name.find('_', 1);
          if (pos < 0) {
            throw buffer::error();
          }
          key.name = key.name.substr(pos + 1);
        }
      }
    } else {
      decode(bucket, bl);
      decode(key.ns, bl);
      decode(key.name, bl);
      decode(key.instance, bl);
//      decode(placement_id, bl);
    }
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<rgw_obj*>& o);

  bool operator==(const rgw_obj& o) const {
    return (key == o.key) &&
           (bucket == o.bucket);
  }
  bool operator<(const rgw_obj& o) const {
    int r = key.name.compare(o.key.name);
    if (r == 0) {
      r = bucket.bucket_id.compare(o.bucket.bucket_id); /* not comparing bucket.name, if bucket_id is equal so will be bucket.name */
      if (r == 0) {
        r = key.ns.compare(o.key.ns);
        if (r == 0) {
          r = key.instance.compare(o.key.instance);
        }
      }
    }

    return (r < 0);
  }

  const rgw_pool& get_explicit_data_pool() {
    if (!in_extra_data || bucket.explicit_placement.data_extra_pool.empty()) {
      return bucket.explicit_placement.data_pool;
    }
    return bucket.explicit_placement.data_extra_pool;
  }
};
WRITE_CLASS_ENCODER(rgw_obj)

struct rgw_raw_obj {
  rgw_pool pool;
  std::string oid;
  std::string loc;

  rgw_raw_obj() {}
  rgw_raw_obj(const rgw_pool& _pool, const std::string& _oid) {
    init(_pool, _oid);
  }
  rgw_raw_obj(const rgw_pool& _pool, const std::string& _oid, const string& _loc) : loc(_loc) {
    init(_pool, _oid);
  }

  void init(const rgw_pool& _pool, const std::string& _oid) {
    pool = _pool;
    oid = _oid;
  }

  bool empty() const {
    return oid.empty();
  }

  void encode(bufferlist& bl) const {
     ENCODE_START(6, 6, bl);
    encode(pool, bl);
    encode(oid, bl);
    encode(loc, bl);
    ENCODE_FINISH(bl);
  }

  void decode_from_rgw_obj(bufferlist::const_iterator& bl);

  void decode(bufferlist::const_iterator& bl) {
    unsigned ofs = bl.get_off();
    DECODE_START(6, bl);
    if (struct_v < 6) {
      /*
       * this object was encoded as rgw_obj, prior to rgw_raw_obj been split out of it,
       * let's decode it as rgw_obj and convert it
       */
      bl.seek(ofs);
      decode_from_rgw_obj(bl);
      return;
    }
    decode(pool, bl);
    decode(oid, bl);
    decode(loc, bl);
    DECODE_FINISH(bl);
  }

  bool operator<(const rgw_raw_obj& o) const {
    int r = pool.compare(o.pool);
    if (r == 0) {
      r = oid.compare(o.oid);
      if (r == 0) {
        r = loc.compare(o.loc);
      }
    }
    return (r < 0);
  }

  bool operator==(const rgw_raw_obj& o) const {
    return (pool == o.pool && oid == o.oid && loc == o.loc);
  }

  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(rgw_raw_obj)

inline ostream& operator<<(ostream& out, const rgw_raw_obj& o) {
  out << o.pool << ":" << o.oid;
  return out;
}

class RGWZoneGroup;
class RGWZoneParams;
class RGWSI_Zone;
class RGWRados;

class rgw_obj_select {
  rgw_placement_rule placement_rule;
  rgw_obj obj;
  rgw_raw_obj raw_obj;
  bool is_raw;

public:
  rgw_obj_select() : is_raw(false) {}
  explicit rgw_obj_select(const rgw_obj& _obj) : obj(_obj), is_raw(false) {}
  explicit rgw_obj_select(const rgw_raw_obj& _raw_obj) : raw_obj(_raw_obj), is_raw(true) {}
  rgw_obj_select(const rgw_obj_select& rhs) {
    placement_rule = rhs.placement_rule;
    is_raw = rhs.is_raw;
    if (is_raw) {
      raw_obj = rhs.raw_obj;
    } else {
      obj = rhs.obj;
    }
  }

  rgw_raw_obj get_raw_obj(const RGWZoneGroup& zonegroup, const RGWZoneParams& zone_params) const;
  rgw_raw_obj get_raw_obj(RGWRados *store) const;

  rgw_obj_select& operator=(const rgw_obj& rhs) {
    obj = rhs;
    is_raw = false;
    return *this;
  }

  rgw_obj_select& operator=(const rgw_raw_obj& rhs) {
    raw_obj = rhs;
    is_raw = true;
    return *this;
  }

  void set_placement_rule(const rgw_placement_rule& rule) {
    placement_rule = rule;
  }

  void dump(Formatter *f) const;
}; /* rgw_obj_select */

struct compression_block {
  uint64_t old_ofs;
  uint64_t new_ofs;
  uint64_t len;

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    encode(old_ofs, bl);
    encode(new_ofs, bl);
    encode(len, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::const_iterator& bl) {
     DECODE_START(1, bl);
     decode(old_ofs, bl);
     decode(new_ofs, bl);
     decode(len, bl);
     DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
};
WRITE_CLASS_ENCODER(compression_block)

struct RGWCompressionInfo {
  string compression_type;
  uint64_t orig_size;
  vector<compression_block> blocks;

  RGWCompressionInfo() : compression_type("none"), orig_size(0) {}
  RGWCompressionInfo(const RGWCompressionInfo& cs_info) : compression_type(cs_info.compression_type),
                                                          orig_size(cs_info.orig_size),
                                                          blocks(cs_info.blocks) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    encode(compression_type, bl);
    encode(orig_size, bl);
    encode(blocks, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::const_iterator& bl) {
     DECODE_START(1, bl);
     decode(compression_type, bl);
     decode(orig_size, bl);
     decode(blocks, bl);
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
};
WRITE_CLASS_ENCODER(RGWCompressionInfo)

struct RGWObjManifestPart {
  rgw_obj loc;   /* the object where the data is located */
  uint64_t loc_ofs;  /* the offset at that object where the data is located */
  uint64_t size;     /* the part size */

  RGWObjManifestPart() : loc_ofs(0), size(0) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(2, 2, bl);
    encode(loc, bl);
    encode(loc_ofs, bl);
    encode(size, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::const_iterator& bl) {
     DECODE_START_LEGACY_COMPAT_LEN_32(2, 2, 2, bl);
     decode(loc, bl);
     decode(loc_ofs, bl);
     decode(size, bl);
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWObjManifestPart*>& o);
};
WRITE_CLASS_ENCODER(RGWObjManifestPart)

/*
 The manifest defines a set of rules for structuring the object parts.
 There are a few terms to note:
     - head: the head part of the object, which is the part that contains
       the first chunk of data. An object might not have a head (as in the
       case of multipart-part objects).
     - stripe: data portion of a single rgw object that resides on a single
       rados object.
     - part: a collection of stripes that make a contiguous part of an
       object. A regular object will only have one part (although might have
       many stripes), a multipart object might have many parts. Each part
       has a fixed stripe size, although the last stripe of a part might
       be smaller than that. Consecutive parts may be merged if their stripe
       value is the same.
*/

struct RGWObjManifestRule {
  uint32_t start_part_num;
  uint64_t start_ofs;
  uint64_t part_size; /* each part size, 0 if there's no part size, meaning it's unlimited */
  uint64_t stripe_max_size; /* underlying obj max size */
  string override_prefix;

  RGWObjManifestRule() : start_part_num(0), start_ofs(0), part_size(0), stripe_max_size(0) {}
  RGWObjManifestRule(uint32_t _start_part_num, uint64_t _start_ofs, uint64_t _part_size, uint64_t _stripe_max_size) :
                       start_part_num(_start_part_num), start_ofs(_start_ofs), part_size(_part_size), stripe_max_size(_stripe_max_size) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(2, 1, bl);
    encode(start_part_num, bl);
    encode(start_ofs, bl);
    encode(part_size, bl);
    encode(stripe_max_size, bl);
    encode(override_prefix, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::const_iterator& bl) {
    DECODE_START(2, bl);
    decode(start_part_num, bl);
    decode(start_ofs, bl);
    decode(part_size, bl);
    decode(stripe_max_size, bl);
    if (struct_v >= 2)
      decode(override_prefix, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
};
WRITE_CLASS_ENCODER(RGWObjManifestRule)

class RGWObjManifest {
protected:
  bool explicit_objs; /* old manifest? */
  map<uint64_t, RGWObjManifestPart> objs;

  uint64_t obj_size;

  rgw_obj obj;
  uint64_t head_size;
  rgw_placement_rule head_placement_rule;

  uint64_t max_head_size;
  string prefix;
  rgw_bucket_placement tail_placement; /* might be different than the original bucket,
                                       as object might have been copied across pools */
  map<uint64_t, RGWObjManifestRule> rules;

  string tail_instance; /* tail object's instance */

  void convert_to_explicit(const RGWZoneGroup& zonegroup, const RGWZoneParams& zone_params);
  int append_explicit(RGWObjManifest& m, const RGWZoneGroup& zonegroup, const RGWZoneParams& zone_params);
  void append_rules(RGWObjManifest& m, map<uint64_t, RGWObjManifestRule>::iterator& iter, string *override_prefix);

  void update_iterators() {
    begin_iter.seek(0);
    end_iter.seek(obj_size);
  }
public:

  RGWObjManifest() : explicit_objs(false), obj_size(0), head_size(0), max_head_size(0),
                     begin_iter(this), end_iter(this) {}
  RGWObjManifest(const RGWObjManifest& rhs) {
    *this = rhs;
  }
  RGWObjManifest& operator=(const RGWObjManifest& rhs) {
    explicit_objs = rhs.explicit_objs;
    objs = rhs.objs;
    obj_size = rhs.obj_size;
    obj = rhs.obj;
    head_size = rhs.head_size;
    max_head_size = rhs.max_head_size;
    prefix = rhs.prefix;
    tail_placement = rhs.tail_placement;
    rules = rhs.rules;
    tail_instance = rhs.tail_instance;

    begin_iter.set_manifest(this);
    end_iter.set_manifest(this);

    begin_iter.seek(rhs.begin_iter.get_ofs());
    end_iter.seek(rhs.end_iter.get_ofs());

    return *this;
  }

  map<uint64_t, RGWObjManifestPart>& get_explicit_objs() {
    return objs;
  }

  void set_explicit(uint64_t _size, map<uint64_t, RGWObjManifestPart>& _objs) {
    explicit_objs = true;
    obj_size = _size;
    objs.swap(_objs);
  }

  void get_implicit_location(uint64_t cur_part_id, uint64_t cur_stripe, uint64_t ofs, string *override_prefix, rgw_obj_select *location);

  void set_trivial_rule(uint64_t tail_ofs, uint64_t stripe_max_size) {
    RGWObjManifestRule rule(0, tail_ofs, 0, stripe_max_size);
    rules[0] = rule;
    max_head_size = tail_ofs;
  }

  void set_multipart_part_rule(uint64_t stripe_max_size, uint64_t part_num) {
    RGWObjManifestRule rule(0, 0, 0, stripe_max_size);
    rule.start_part_num = part_num;
    rules[0] = rule;
    max_head_size = 0;
  }

  void encode(bufferlist& bl) const {
    ENCODE_START(7, 6, bl);
    encode(obj_size, bl);
    encode(objs, bl);
    encode(explicit_objs, bl);
    encode(obj, bl);
    encode(head_size, bl);
    encode(max_head_size, bl);
    encode(prefix, bl);
    encode(rules, bl);
    bool encode_tail_bucket = !(tail_placement.bucket == obj.bucket);
    encode(encode_tail_bucket, bl);
    if (encode_tail_bucket) {
      encode(tail_placement.bucket, bl);
    }
    bool encode_tail_instance = (tail_instance != obj.key.instance);
    encode(encode_tail_instance, bl);
    if (encode_tail_instance) {
      encode(tail_instance, bl);
    }
    encode(head_placement_rule, bl);
    encode(tail_placement.placement_rule, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::const_iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN_32(7, 2, 2, bl);
    decode(obj_size, bl);
    decode(objs, bl);
    if (struct_v >= 3) {
      decode(explicit_objs, bl);
      decode(obj, bl);
      decode(head_size, bl);
      decode(max_head_size, bl);
      decode(prefix, bl);
      decode(rules, bl);
    } else {
      explicit_objs = true;
      if (!objs.empty()) {
        map<uint64_t, RGWObjManifestPart>::iterator iter = objs.begin();
        obj = iter->second.loc;
        head_size = iter->second.size;
        max_head_size = head_size;
      }
    }

    if (explicit_objs && head_size > 0 && !objs.empty()) {
      /* patch up manifest due to issue 16435:
       * the first object in the explicit objs list might not be the one we need to access, use the
       * head object instead if set. This would happen if we had an old object that was created
       * when the explicit objs manifest was around, and it got copied.
       */
      rgw_obj& obj_0 = objs[0].loc;
      if (!obj_0.get_oid().empty() && obj_0.key.ns.empty()) {
        objs[0].loc = obj;
        objs[0].size = head_size;
      }
    }

    if (struct_v >= 4) {
      if (struct_v < 6) {
        decode(tail_placement.bucket, bl);
      } else {
        bool need_to_decode;
        decode(need_to_decode, bl);
        if (need_to_decode) {
          decode(tail_placement.bucket, bl);
        } else {
          tail_placement.bucket = obj.bucket;
        }
      }
    }

    if (struct_v >= 5) {
      if (struct_v < 6) {
        decode(tail_instance, bl);
      } else {
        bool need_to_decode;
        decode(need_to_decode, bl);
        if (need_to_decode) {
          decode(tail_instance, bl);
        } else {
          tail_instance = obj.key.instance;
        }
      }
    } else { // old object created before 'tail_instance' field added to manifest
      tail_instance = obj.key.instance;
    }

    if (struct_v >= 7) {
      decode(head_placement_rule, bl);
      decode(tail_placement.placement_rule, bl);
    }

    update_iterators();
    DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWObjManifest*>& o);

  int append(RGWObjManifest& m, const RGWZoneGroup& zonegroup,
             const RGWZoneParams& zone_params);
  int append(RGWObjManifest& m, RGWSI_Zone *zone_svc);

  bool get_rule(uint64_t ofs, RGWObjManifestRule *rule);

  bool empty() {
    if (explicit_objs)
      return objs.empty();
    return rules.empty();
  }

  bool has_explicit_objs() {
    return explicit_objs;
  }

  bool has_tail() {
    if (explicit_objs) {
      if (objs.size() == 1) {
        map<uint64_t, RGWObjManifestPart>::iterator iter = objs.begin();
        rgw_obj& o = iter->second.loc;
        return !(obj == o);
      }
      return (objs.size() >= 2);
    }
    return (obj_size > head_size);
  }

  void set_head(const rgw_placement_rule& placement_rule, const rgw_obj& _o, uint64_t _s) {
    head_placement_rule = placement_rule;
    obj = _o;
    head_size = _s;

    if (explicit_objs && head_size > 0) {
      objs[0].loc = obj;
      objs[0].size = head_size;
    }
  }

  const rgw_obj& get_obj() {
    return obj;
  }

  void set_tail_placement(const rgw_placement_rule& placement_rule, const rgw_bucket& _b) {
    tail_placement.placement_rule = placement_rule;
    tail_placement.bucket = _b;
  }

  const rgw_bucket_placement& get_tail_placement() {
    return tail_placement;
  }

  const rgw_placement_rule& get_head_placement_rule() {
    return head_placement_rule;
  }

  void set_prefix(const string& _p) {
    prefix = _p;
  }

  const string& get_prefix() {
    return prefix;
  }

  void set_tail_instance(const string& _ti) {
    tail_instance = _ti;
  }

  const string& get_tail_instance() {
    return tail_instance;
  }

  void set_head_size(uint64_t _s) {
    head_size = _s;
  }

  void set_obj_size(uint64_t s) {
    obj_size = s;

    update_iterators();
  }

  uint64_t get_obj_size() {
    return obj_size;
  }

  uint64_t get_head_size() {
    return head_size;
  }

  uint64_t get_max_head_size() {
    return max_head_size;
  }

  class obj_iterator {
    RGWObjManifest *manifest;
    uint64_t part_ofs; /* where current part starts */
    uint64_t stripe_ofs; /* where current stripe starts */
    uint64_t ofs;       /* current position within the object */
    uint64_t stripe_size;      /* current part size */

    int cur_part_id;
    int cur_stripe;
    string cur_override_prefix;

    rgw_obj_select location;

    map<uint64_t, RGWObjManifestRule>::iterator rule_iter;
    map<uint64_t, RGWObjManifestRule>::iterator next_rule_iter;
    map<uint64_t, RGWObjManifestPart>::iterator explicit_iter;

    void init() {
      part_ofs = 0;
      stripe_ofs = 0;
      ofs = 0;
      stripe_size = 0;
      cur_part_id = 0;
      cur_stripe = 0;
    }

    void update_explicit_pos();


  protected:

    void set_manifest(RGWObjManifest *m) {
      manifest = m;
    }

  public:
    obj_iterator() : manifest(NULL) {
      init();
    }
    explicit obj_iterator(RGWObjManifest *_m) : manifest(_m) {
      init();
      if (!manifest->empty()) {
        seek(0);
      }
    }
    obj_iterator(RGWObjManifest *_m, uint64_t _ofs) : manifest(_m) {
      init();
      if (!manifest->empty()) {
        seek(_ofs);
      }
    }
    void seek(uint64_t ofs);

    void operator++();
    bool operator==(const obj_iterator& rhs) const {
      return (ofs == rhs.ofs);
    }
    bool operator!=(const obj_iterator& rhs) const {
      return (ofs != rhs.ofs);
    }
    const rgw_obj_select& get_location() const {
      return location;
    }

    /* start of current stripe */
    uint64_t get_stripe_ofs() const {
      if (manifest->explicit_objs) {
        return explicit_iter->first;
      }
      return stripe_ofs;
    }

    /* current ofs relative to start of rgw object */
    uint64_t get_ofs() const {
      return ofs;
    }

    /* stripe number */
    int get_cur_stripe() const {
      return cur_stripe;
    }

    /* current stripe size */
    uint64_t get_stripe_size() const {
      if (manifest->explicit_objs) {
        return explicit_iter->second.size;
      }
      return stripe_size;
    }

    /* offset where data starts within current stripe */
    uint64_t location_ofs() const {
      if (manifest->explicit_objs) {
        return explicit_iter->second.loc_ofs;
      }
      return 0; /* all stripes start at zero offset */
    }

    void update_location();

    friend class RGWObjManifest;
    void dump(Formatter *f) const;
  };

  const obj_iterator& obj_begin();
  const obj_iterator& obj_end();
  obj_iterator obj_find(uint64_t ofs);

  obj_iterator begin_iter;
  obj_iterator end_iter;

  /*
   * simple object generator. Using a simple single rule manifest.
   */
  class generator {
    RGWObjManifest *manifest;
    uint64_t last_ofs;
    uint64_t cur_part_ofs;
    int cur_part_id;
    int cur_stripe;
    uint64_t cur_stripe_size;
    string cur_oid;
    
    string oid_prefix;

    rgw_obj_select cur_obj;

    RGWObjManifestRule rule;

  public:
    generator() : manifest(NULL), last_ofs(0), cur_part_ofs(0), cur_part_id(0), 
		  cur_stripe(0), cur_stripe_size(0) {}
    int create_begin(CephContext *cct, RGWObjManifest *manifest,
                     const rgw_placement_rule& head_placement_rule,
                     const rgw_placement_rule *tail_placement_rule,
                     const rgw_bucket& bucket,
                     const rgw_obj& obj);

    int create_next(uint64_t ofs);

    rgw_raw_obj get_cur_obj(RGWZoneGroup& zonegroup, RGWZoneParams& zone_params) { return cur_obj.get_raw_obj(zonegroup, zone_params); }
    rgw_raw_obj get_cur_obj(RGWRados *store) const { return cur_obj.get_raw_obj(store); }

    /* total max size of current stripe (including head obj) */
    uint64_t cur_stripe_max_size() const {
      return cur_stripe_size;
    }
  };
};
WRITE_CLASS_ENCODER(RGWObjManifest)

struct RGWUploadPartInfo {
  uint32_t num;
  uint64_t size;
  uint64_t accounted_size{0};
  string etag;
  ceph::real_time modified;
  RGWObjManifest manifest;
  RGWCompressionInfo cs_info;
  std::vector<std::string> failed_prefixes;

  RGWUploadPartInfo() : num(0), size(0) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(5, 2, bl);
    encode(num, bl);
    encode(size, bl);
    encode(etag, bl);
    encode(modified, bl);
    encode(manifest, bl);
    encode(cs_info, bl);
    encode(accounted_size, bl);
    encode(failed_prefixes, bl);
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::const_iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN(4, 2, 2, bl);
    decode(num, bl);
    decode(size, bl);
    decode(etag, bl);
    decode(modified, bl);
    if (struct_v >= 3)
      decode(manifest, bl);
    if (struct_v >= 4) {
      decode(cs_info, bl);
      decode(accounted_size, bl);
    } else {
      accounted_size = size;
    }
    if (struct_v >= 5) {
      decode(failed_prefixes, bl);
    }
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWUploadPartInfo*>& o);
};
WRITE_CLASS_ENCODER(RGWUploadPartInfo)

#endif
