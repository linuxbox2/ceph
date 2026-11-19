// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

/* The model's cluster tier (FSIO_RADOS_WORK_PLAN.md, Phase 4):  with
 * FSIO_MODEL_CLUSTER=1, every core call a walk makes is also sent, as
 * its CLS method, to a real head on a RADOS cluster, and the object is
 * compared with the in-memory core, the oracle.  Before each call the
 * target object is set to the oracle's state before the call (its RGW
 * part and FSIO's records), so the call tests the CLS layer's load,
 * store and apply alone;  after it, the object's records, its RGW part,
 * the clones the call made and its reply must be what the core says.
 * The first divergence in a walk is its violation. */

#include <cstring>
#include <map>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "include/rados/librados.hpp"
#include "cls/rgw/cls_rgw_fsio.h"
#include "cls/rgw/cls_rgw_fsio_ops.h"

namespace fsio_cluster {

/* the RGW and FSIO attribute names, as RGW and §2.7 give them */
constexpr std::string_view ID_TAG = "user.rgw.idtag";
constexpr std::string_view TAIL_TAG = "user.rgw.tail_tag";
constexpr std::string_view MANIFEST = "user.rgw.manifest";
constexpr std::string_view OLH_INFO = "user.rgw.olh.info";
constexpr std::string_view OLH_PREFIX = "user.rgw.olh.";
constexpr std::string_view FSIO_PREFIX = "user.rgw.fsio.";
constexpr std::string_view HDR = "user.rgw.fsio.hdr";
constexpr std::string_view OCNT = "user.rgw.fsio.ocnt";
constexpr std::string_view GEN = "user.rgw.fsio.gen";
constexpr std::string_view PM = "user.rgw.fsio.pm";

using attrs_t = std::map<std::string, ceph::buffer::list>;

/* an object as the cluster holds it, or as it should */
struct Obj {
  bool exists = false;
  std::string data;
  attrs_t attrs;
};

inline std::string opens_name(uint64_t incarnation)
{
  return fmt::format("{}m.{:016x}", FSIO_PREFIX, incarnation);
}

inline ceph::buffer::list str_bl(const std::string& s)
{
  ceph::buffer::list bl;
  bl.append(s);
  return bl;
}

/* FSIO's records of a head, by §2.7's names */
inline attrs_t records_of(const cls_rgw_fsio_head& h)
{
  attrs_t m;
  if (h.hdr) {
    encode(*h.hdr, m[std::string(HDR)]);
  }
  for (const auto& [inc, o] : h.opens) {
    encode(o, m[opens_name(inc)]);
  }
  return m;
}

/* RGW's part of a head:  its data stands in as the manifest token */
inline Obj rgw_part(bool exists, const std::string& id_tag,
		    const std::string& tail_tag,
		    const ceph::buffer::list& manifest, bool olh)
{
  Obj o;
  o.exists = exists;
  if (!exists) {
    return o;
  }
  if (!id_tag.empty()) {
    o.data = manifest.to_str();
    o.attrs[std::string(ID_TAG)] = str_bl(id_tag);
  }
  if (!tail_tag.empty()) {
    o.attrs[std::string(TAIL_TAG)] = str_bl(tail_tag);
  }
  if (manifest.length()) {
    o.attrs[std::string(MANIFEST)] = manifest;
  }
  if (olh) {
    o.attrs[std::string(OLH_INFO)] = str_bl("olh");
  }
  return o;
}

inline Obj head_obj(const cls_rgw_fsio_head& h)
{
  auto o = rgw_part(h.exists, h.id_tag, h.tail_tag, h.manifest, h.olh);
  if (o.exists) {
    o.attrs.merge(records_of(h));
  }
  return o;
}

inline Obj instance_obj(const cls_rgw_fsio_instance_head& ih)
{
  auto o = rgw_part(ih.exists, ih.id_tag, ih.tail_tag, ih.manifest, false);
  if (o.exists && ih.ocnt) {
    encode(*ih.ocnt, o.attrs[std::string(OCNT)]);
  }
  return o;
}

/* a result as -ENAME, or 0;  FSIO's own codes by number */
inline std::string err(int r)
{
  if (r >= 0) {
    return std::to_string(r);
  }
  if (const char* n = strerrorname_np(-r)) {
    return fmt::format("-{}", n);
  }
  return std::to_string(r);
}

inline std::string describe(const Obj& o)
{
  if (!o.exists) {
    return "absent";
  }
  std::string s = fmt::format("data '{}'", o.data);
  for (const auto& [k, v] : o.attrs) {
    s += fmt::format(", {} ({} bytes)", k, v.length());
  }
  return s;
}

class Cluster {
  librados::Rados rados;
  librados::IoCtx io;
  std::optional<std::string> divergence;

  int read(const std::string& oid, Obj& o) {
    librados::ObjectReadOperation op;
    ceph::buffer::list data;
    int rd = 0, rx = 0;
    op.read(0, 0, &data, &rd);
    op.getxattrs(&o.attrs, &rx);
    int r = io.operate(oid, &op, nullptr);
    if (r == -ENOENT) {
      o = Obj{};
      return 0;
    }
    if (r < 0) {
      return r;
    }
    o.exists = true;
    o.data = data.to_str();
    return 0;
  }

  /* the object set to `o`:  removed, then created with o's data and
   * attributes */
  int write(const std::string& oid, const Obj& o) {
    librados::ObjectWriteOperation rm;
    rm.remove();
    int r = io.operate(oid, &rm);
    if (r < 0 && r != -ENOENT) {
      return r;
    }
    if (!o.exists) {
      return 0;
    }
    librados::ObjectWriteOperation w;
    w.create(true);
    if (!o.data.empty()) {
      w.write_full(str_bl(o.data));
    }
    for (const auto& [k, v] : o.attrs) {
      w.setxattr(k.c_str(), v);
    }
    return io.operate(oid, &w);
  }

  void diverge(std::string s) {
    if (!divergence) {
      divergence = std::move(s);
    }
  }

  /* the calls that succeeded on the cluster, by method (and replace
   * mode):  the tier's reach */
  std::map<std::string, uint64_t> calls;

public:
  bool on = false;
  std::string marker;            // per walk
  const std::string key = "k";

  int connect(const std::string& pool) {
    int r = rados.init(nullptr);
    if (r >= 0) {
      r = rados.conf_read_file(nullptr);
    }
    if (r >= 0) {
      r = rados.conf_parse_env(nullptr);
    }
    if (r >= 0) {
      r = rados.connect();
    }
    if (r < 0) {
      return r;
    }
    r = rados.ioctx_create(pool.c_str(), io);
    if (r == -ENOENT) {
      if ((r = rados.pool_create(pool.c_str())) < 0 && r != -EEXIST) {
	return r;
      }
      r = rados.ioctx_create(pool.c_str(), io);
    }
    on = r >= 0;
    return r;
  }

  /* each walk its own names:  <marker>_k, after §2.7's form */
  void begin_walk(uint64_t seed, uint64_t walk) {
    marker = fmt::format("fsiom.{:x}.{:x}.{:x}", getpid(), seed, walk);
    divergence.reset();
  }

  std::optional<std::string> take() {
    auto d = std::move(divergence);
    divergence.reset();
    return d;
  }

  std::string head_oid() const { return marker + "_" + key; }
  std::string instance_oid(const std::string& inst) const {
    return inst == "null" ? head_oid() :
      fmt::format("{}__:{}_{}", marker, inst, key);
  }

  /* one call:  the target set to `pre`, the clones the core made cleared
   * of any left from a call the model rolled back, the method sent with
   * RETURNVEC;  returns its result, its reply in `reply` */
  template <typename Method>
  int call(const std::string& oid, const Obj& pre,
	   const cls_rgw_fsio_effects& fx, const Method& method,
	   const ceph::buffer::list& in, ceph::buffer::list& reply) {
    if (int r = write(oid, pre); r < 0) {
      diverge(fmt::format("cluster:  setting {} returned {}", oid, err(r)));
      return r;
    }
    for (const auto& c : fx.clones) {
      librados::IoCtx keyed;
      keyed.dup(io);
      keyed.locator_set_key(oid);
      keyed.remove(cls_rgw_fsio_clone_name(marker, c.role, c.r, oid));
    }
    librados::ObjectWriteOperation op;
    int rval = 0;
    auto inbl = in;                // librados's exec takes it non-const
    op.exec(method, inbl, &reply, &rval);
    call_start = now_ns();
    int r = io.operate(oid, &op, librados::OPERATION_RETURNVEC);
    call_end = now_ns();
    return r < 0 ? r : rval;
  }

  /* after a call:  its result, the object, and each clone */
  void check(const char* method, const std::string& oid, int r_core,
	     int r_cls, const Obj& pre, const Obj& want,
	     const cls_rgw_fsio_effects& fx) {
    if (r_cls == 0) {
      ++calls[method];
    }
    if (divergence) {
      return;
    }
    /* §2.2:  no method creates a head, and each returns ENOENT on an
     * absent one;  the core has no such check, and where the oracle's head
     * is absent its result has no counterpart:  RGW's PUT makes no call
     * there (state->exists), a gateway or the expirer gets ENOENT */
    if (!pre.exists) {
      Obj got;
      if (r_cls != -ENOENT) {
	diverge(fmt::format("cluster:  {} on the absent {} returned {}, not "
			    "-ENOENT", method, oid, err(r_cls)));
      } else if (read(oid, got) == 0 && got.exists) {
	diverge(fmt::format("cluster:  {} on the absent {} created it:  {}",
			    method, oid, describe(got)));
      }
      return;
    }
    if (r_core != r_cls) {
      diverge(fmt::format("cluster:  {} on {} returned {};  the core returned "
			  "{}", method, oid, err(r_cls), err(r_core)));
      return;
    }
    Obj got;
    if (int r = read(oid, got); r < 0) {
      diverge(fmt::format("cluster:  reading {} returned {}", oid, err(r)));
      return;
    }
    const Obj& expect = r_core < 0 ? pre : want;
    if (r_core == 0 && !olh_epoch_from_osd(method, got, expect)) {
      return;
    }
    if (got.exists != expect.exists || got.data != expect.data ||
	got.attrs != expect.attrs) {
      diverge(fmt::format("cluster:  after {} (returned {}), {} is {};  the "
			  "core says {}", method, err(r_core), oid, describe(got),
			  describe(expect)));
      return;
    }
    if (r_core < 0) {
      return;
    }
    librados::IoCtx keyed;
    keyed.dup(io);
    keyed.locator_set_key(oid);
    for (const auto& c : fx.clones) {
      const auto name = cls_rgw_fsio_clone_name(marker, c.role, c.r, oid);
      Obj want_clone;
      want_clone.exists = true;
      want_clone.data = pre.data;
      /* a clone drops FSIO's records, the manifest and refcount (§2.3,
       * §2.4) */
      for (const auto& [k, v] : pre.attrs) {
	if (!k.starts_with(FSIO_PREFIX) && k != MANIFEST && k != "refcount") {
	  want_clone.attrs[k] = v;
	}
      }
      if (c.role == 'G') {
	encode(c.record, want_clone.attrs[std::string(GEN)]);
      } else if (c.role == 'a' && c.pm.length()) {
	want_clone.attrs[std::string(PM)] = c.pm;
      }
      Obj got_clone;
      librados::ObjectReadOperation op;
      ceph::buffer::list data;
      int rd = 0, rx = 0;
      op.read(0, 0, &data, &rd);
      op.getxattrs(&got_clone.attrs, &rx);
      int r = keyed.operate(name, &op, nullptr);
      got_clone.exists = r == 0;
      got_clone.data = data.to_str();
      if (r < 0) {
	got_clone.attrs.clear();
      }
      if (got_clone.exists != want_clone.exists ||
	  got_clone.data != want_clone.data ||
	  got_clone.attrs != want_clone.attrs) {
	diverge(fmt::format("cluster:  after {}, clone {} is {};  the core "
			    "says {}", method, name, describe(got_clone),
			    describe(want_clone)));
	return;
      }
    }
  }

  /* the client's clock around the last call:  vstart's OSDs share it */
  uint64_t call_start = 0, call_end = 0;
  static uint64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
      ceph::real_clock::now().time_since_epoch()).count();
  }
  bool in_call(uint64_t epoch) const {
    return epoch >= call_start && epoch <= call_end;
  }

  /* §4.2, X31:  fsio_leave and fsio_fenced_finalize record the OSD's time
   * as a publish's OLH epoch, where the model's core records its logical
   * one;  each entry whose epoch differs must hold one taken during the
   * call, and then takes the core's, so the rest compares exactly */
  bool olh_epoch_from_osd(const char* method, Obj& got, const Obj& expect) {
    if (std::string_view(method) != "fsio_leave" &&
	std::string_view(method) != "fsio_fenced_finalize") {
      return true;
    }
    auto g = got.attrs.find(std::string(HDR));
    auto e = expect.attrs.find(std::string(HDR));
    if (g == got.attrs.end() || e == expect.attrs.end()) {
      return true;
    }
    cls_rgw_fsio_hdr gh, eh;
    try {
      auto gi = g->second.cbegin();
      decode(gh, gi);
      auto ei = e->second.cbegin();
      decode(eh, ei);
    } catch (const ceph::buffer::error&) {
      return true;                 // the comparison reports it
    }
    for (auto& c : gh.chunk_sets) {
      const auto* w = eh.find(c.id);
      if (!c.publishing || !w || !w->publishing ||
	  c.publishing->olh_epoch == w->publishing->olh_epoch) {
	continue;
      }
      if (!in_call(c.publishing->olh_epoch)) {
	diverge(fmt::format("cluster:  after {}, {}'s OLH epoch {} is not "
			    "the OSD's time during the call [{}, {}]", method,
			    c.id.to_str(), c.publishing->olh_epoch, call_start,
			    call_end));
	return false;
      }
      c.publishing->olh_epoch = w->publishing->olh_epoch;
    }
    g->second.clear();
    encode(gh, g->second);
    return true;
  }

  /* a finer count of a call that succeeded, as fsio_replace_head's modes */
  void count(const std::string& what) {
    ++calls[what];
  }

  std::string reach() const {
    std::string s;
    for (const auto& [k, n] : calls) {
      s += fmt::format("  {:<44} {}\n", k, n);
    }
    return s;
  }

  void reply_mismatch(const char* method, const std::string& what) {
    diverge(fmt::format("cluster:  {}'s reply:  {}", method, what));
  }
};

} // namespace fsio_cluster
