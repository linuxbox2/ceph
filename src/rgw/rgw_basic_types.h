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

#endif
