// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* The fsio_* methods on a real object (FSIO_RADOS_DESIGN.md §2.2, §2.5,
 * §2.7, §8):  each reads the object's FSIO records once, runs the pure
 * transition in cls_rgw_fsio.cc, writes the records that changed and
 * removes those that went, applies the effects, and encodes its reply.
 * A method never reads back what it wrote (Z32). */

#include <fmt/format.h>

#include "objclass/objclass.h"
#include "osd/osd_types.h"
#include "cls/rgw/cls_rgw_fsio_ops.h"
#include "cls/rgw/cls_rgw_fsio.h"

namespace fsio = cls_rgw_fsio;

namespace {

static constexpr std::string_view ID_TAG = "user.rgw.idtag";          // RGW_ATTR_ID_TAG
static constexpr std::string_view TAIL_TAG = "user.rgw.tail_tag";     // RGW_ATTR_TAIL_TAG
static constexpr std::string_view MANIFEST = "user.rgw.manifest";     // RGW_ATTR_MANIFEST
static constexpr std::string_view OLH_INFO = "user.rgw.olh.info";     // RGW_ATTR_OLH_INFO
static constexpr std::string_view TAGS = "user.rgw.x-amz-tagging";   // RGW_ATTR_TAGS
static constexpr std::string_view FSIO_PREFIX = "user.rgw.fsio.";
static constexpr std::string_view HDR = "user.rgw.fsio.hdr";
static constexpr std::string_view OPENS = "user.rgw.fsio.m.";         // + the incarnation
static constexpr std::string_view OCNT = "user.rgw.fsio.ocnt";
static constexpr std::string_view GEN = "user.rgw.fsio.gen";
static constexpr std::string_view PM = "user.rgw.fsio.pm";
static constexpr std::string_view REFCOUNT = "refcount";       // cls_refcount's REFCOUNT_ATTR

std::string opens_name(uint64_t incarnation)
{
  return fmt::format("{}{:016x}", OPENS, incarnation);
}

/* the FSIO records as read, so a store writes only what changed */
struct loaded {
  std::map<std::string, ceph::buffer::list> fsio;
  std::map<std::string, ceph::buffer::list> all;    // every xattr read
};

/* one stat and one getxattrs;  an absent object loads as not existing */
int read_attrs(cls_method_context_t hctx, bool& exists,
	       std::map<std::string, ceph::buffer::list>& attrs)
{
  uint64_t size;
  ceph::real_time mtime;
  int r = cls_cxx_stat2(hctx, &size, &mtime);
  if (r == -ENOENT) {
    exists = false;
    return 0;
  }
  if (r < 0) {
    return r;
  }
  exists = true;
  return cls_cxx_getxattrs(hctx, &attrs);
}

int load_head(cls_method_context_t hctx, cls_rgw_fsio_head& h, loaded& l)
{
  auto& attrs = l.all;
  int r = read_attrs(hctx, h.exists, attrs);
  if (r < 0 || !h.exists) {
    return r;
  }
  try {
    for (auto& [k, v] : attrs) {
      if (k == ID_TAG) {
	h.id_tag = v.to_str();
      } else if (k == TAIL_TAG) {
	h.tail_tag = v.to_str();
      } else if (k == MANIFEST) {
	h.manifest = v;
      } else if (k == OLH_INFO) {
	h.olh = true;
      } else if (k.starts_with(FSIO_PREFIX)) {
	l.fsio[k] = v;
	auto it = v.cbegin();
	if (k == HDR) {
	  h.hdr.emplace();
	  decode(*h.hdr, it);
	} else if (k.starts_with(OPENS)) {
	  const auto inc = std::stoull(k.substr(OPENS.size()), nullptr, 16);
	  decode(h.opens[inc], it);
	}
      }
    }
  } catch (const std::exception& e) {
    CLS_ERR("ERROR: %s: decoding the FSIO records: %s", __func__, e.what());
    return -EIO;
  }
  return 0;
}

int set_if_changed(cls_method_context_t hctx, const loaded& before,
		   const std::string& name, ceph::buffer::list& bl)
{
  auto i = before.fsio.find(name);
  if (i != before.fsio.end() && i->second.contents_equal(bl)) {
    return 0;
  }
  return cls_cxx_setxattr(hctx, name.c_str(), &bl);
}

/* writes each record whose encoding changed and removes each that went:
 * an incarnation's, the header.  RGW's own attributes are not written
 * here. */
int store_head(cls_method_context_t hctx, const loaded& before,
	       const cls_rgw_fsio_head& h)
{
  int r = 0;
  if (h.hdr) {
    ceph::buffer::list bl;
    encode(*h.hdr, bl);
    r = set_if_changed(hctx, before, std::string(HDR), bl);
  } else if (before.fsio.count(std::string(HDR))) {
    r = cls_cxx_rmxattr(hctx, HDR.data());
  }
  if (r < 0) {
    return r;
  }
  std::set<std::string> kept;
  for (const auto& [inc, o] : h.opens) {
    auto name = opens_name(inc);
    kept.insert(name);
    ceph::buffer::list bl;
    encode(o, bl);
    if ((r = set_if_changed(hctx, before, name, bl)) < 0) {
      return r;
    }
  }
  for (const auto& [name, v] : before.fsio) {
    if (name.starts_with(OPENS) && !kept.count(name)) {
      if ((r = cls_cxx_rmxattr(hctx, name.c_str())) < 0) {
	return r;
      }
    }
  }
  return 0;
}

int load_instance(cls_method_context_t hctx, cls_rgw_fsio_instance_head& ih,
		  loaded& l)
{
  std::map<std::string, ceph::buffer::list> attrs;
  int r = read_attrs(hctx, ih.exists, attrs);
  if (r < 0 || !ih.exists) {
    return r;
  }
  l.all = attrs;
  try {
    for (auto& [k, v] : attrs) {
      if (k == ID_TAG) {
	ih.id_tag = v.to_str();
      } else if (k == TAIL_TAG) {
	ih.tail_tag = v.to_str();
      } else if (k == MANIFEST) {
	ih.manifest = v;
      } else if (k == OCNT) {
	l.fsio[k] = v;
	auto it = v.cbegin();
	ih.ocnt.emplace();
	decode(*ih.ocnt, it);
      }
    }
  } catch (const std::exception& e) {
    CLS_ERR("ERROR: %s: decoding the open count: %s", __func__, e.what());
    return -EIO;
  }
  return 0;
}

/* the open count, and the ID tag, tail tag and manifest a version
 * removal clears (§2.5) */
int store_instance(cls_method_context_t hctx, const loaded& before,
		   const cls_rgw_fsio_instance_head& was,
		   const cls_rgw_fsio_instance_head& ih)
{
  int r = 0;
  if (ih.ocnt) {
    ceph::buffer::list bl;
    encode(*ih.ocnt, bl);
    r = set_if_changed(hctx, before, std::string(OCNT), bl);
  } else if (before.fsio.count(std::string(OCNT))) {
    r = cls_cxx_rmxattr(hctx, OCNT.data());
  }
  if (r < 0) {
    return r;
  }
  if (!was.id_tag.empty() && ih.id_tag.empty()) {
    if ((r = cls_cxx_rmxattr(hctx, ID_TAG.data())) < 0) {
      return r;
    }
  }
  if (!was.tail_tag.empty() && ih.tail_tag.empty()) {
    if ((r = cls_cxx_rmxattr(hctx, TAIL_TAG.data())) < 0) {
      return r;
    }
  }
  if (was.manifest.length() && !ih.manifest.length()) {
    if ((r = cls_cxx_rmxattr(hctx, MANIFEST.data())) < 0) {
      return r;
    }
  }
  return 0;
}

/* §2.7, E2:  a clone of the head (or instance head) lives after the
 * bucket's heads, <marker>~fsio_<role><id>_<key part>;  the caller names
 * the marker, which the object's name must start with, then "_" */
int head_clone_name(cls_method_context_t hctx, const std::string& marker,
		    char role, const cls_rgw_fsio_id& id, std::string& name)
{
  const auto& oid = cls_get_object_info(hctx).soid.oid.name;
  if (marker.empty() || marker.find('~') != std::string::npos ||
      !oid.starts_with(marker + "_")) {
    CLS_ERR("ERROR: %s: marker %s does not name %s", __func__,
	    marker.c_str(), oid.c_str());
    return -EINVAL;
  }
  name = cls_rgw_fsio_clone_name(marker, role, id, oid);
  return 0;
}

/* §4.2, X31:  the OLH epoch a versioned publish links with, the OSD's
 * time in nanoseconds, as rgw_bucket_link_olh computes it;  never the
 * caller's, which would carry the gateway's clock skew */
uint64_t osd_epoch()
{
  using namespace std::chrono;
  return duration_cast<nanoseconds>(
    ceph::real_clock::now().time_since_epoch()).count();
}

/* §3, Z28:  the caller's incarnation is the op's origin, the RADOS
 * client whose watches decide its liveness;  the incarnations a call
 * names as others' (expired, a hint's, a dropped seq) are not checked */
int check_origin(cls_method_context_t hctx, uint64_t incarnation)
{
  if (!cls_get_config(hctx).get_val<bool>("rgw_fsio_check_origin")) {
    return 0;
  }
  entity_inst_t origin;
  if (int r = cls_get_request_origin(hctx, &origin); r < 0) {
    return r;
  }
  if (!origin.name.is_client() ||
      origin.name.num() != static_cast<int64_t>(incarnation)) {
    CLS_LOG(1, "fsio:  caller incarnation %llu is not the op's origin "
	    "(type %d, num %lld)", (unsigned long long)incarnation,
	    (int)origin.name.type(), (long long)origin.name.num());
    return -EPERM;
  }
  return 0;
}

/* the ops' fields as the core's inputs */
fsio::publish_intent to_core(const cls_rgw_fsio_intent& i)
{
  return {i.instance, i.token, i.now_epoch};
}

fsio::replace_in to_core(const cls_rgw_fsio_replace_op& op)
{
  fsio::replace_in in;
  in.mode = static_cast<fsio::replace_mode>(op.mode);
  in.r = op.r;
  in.expected_id_tag = op.expected_id_tag;
  in.caller = op.caller;
  in.shadow = op.shadow;
  in.versioned = op.versioned;
  in.complete = op.complete;
  in.expired = op.expired;
  in.keep_tail = op.keep_tail;
  return in;
}

/* the effects:  each clone by CLONE_TO, FSIO's records dropped from it
 * and its own record set;  a head with nothing left removed */
int apply(cls_method_context_t hctx, const std::string& marker,
	  const cls_rgw_fsio_effects& fx)
{
  for (const auto& c : fx.clones) {
    std::string dest;
    int r = head_clone_name(hctx, marker, c.role, c.r, dest);
    if (r < 0) {
      return r;
    }
    std::map<std::string, ceph::buffer::list> set;
    if (c.role == 'G') {
      encode(c.record, set[std::string(GEN)]);
    } else if (c.role == 'a' && c.pm.length()) {
      set[std::string(PM)] = c.pm;
    }
    /* §2.3, §2.4, plan §6.7:  a clone drops FSIO's records, the head's
     * manifest (its record carries it) and cls_refcount's tags (X16) */
    r = cls_cxx_clone_to(hctx, dest,
			 {std::string(FSIO_PREFIX), std::string(MANIFEST),
			  std::string(REFCOUNT)}, set);
    if (r < 0 && r != -EEXIST) {   // EEXIST:  a resend's, done (§2.7)
      return r;
    }
  }
  if (fx.remove_head) {
    return cls_cxx_remove(hctx);
  }
  return 0;
}

/* §2.2, Z24:  no method creates a head */
int require(bool exists)
{
  return exists ? 0 : -ENOENT;
}

template <typename T>
int decode_op(ceph::buffer::list* in, T& op, const char* fn)
{
  try {
    auto it = in->cbegin();
    decode(op, it);
  } catch (const ceph::buffer::error&) {
    CLS_ERR("ERROR: %s: failed to decode its input", fn);
    return -EINVAL;
  }
  return 0;
}

/* a head method's frame:  load, the transition, store, apply */
template <typename F>
int on_head(cls_method_context_t hctx, const std::string& marker, F&& f)
{
  cls_rgw_fsio_head h;
  loaded l;
  int r = load_head(hctx, h, l);
  if (r < 0) {
    return r;
  }
  if ((r = require(h.exists)) < 0) {
    return r;
  }
  cls_rgw_fsio_effects fx;
  if ((r = f(h, fx)) < 0) {
    return r;
  }
  if (fx.remove_head) {
    return apply(hctx, marker, fx);   // the head goes, its records with it
  }
  if ((r = store_head(hctx, l, h)) < 0) {
    return r;
  }
  return apply(hctx, marker, fx);
}

template <typename F>
int on_instance(cls_method_context_t hctx, const std::string& marker, F&& f)
{
  cls_rgw_fsio_instance_head ih;
  loaded l;
  int r = load_instance(hctx, ih, l);
  if (r < 0) {
    return r;
  }
  if ((r = require(ih.exists)) < 0) {
    return r;
  }
  const auto was = ih;
  cls_rgw_fsio_effects fx;
  if ((r = f(ih, fx)) < 0) {
    return r;
  }
  if (fx.remove_head) {
    return apply(hctx, marker, fx);
  }
  if ((r = store_instance(hctx, l, was, ih)) < 0) {
    return r;
  }
  return apply(hctx, marker, fx);
}

int rgw_fsio_join(cls_method_context_t hctx, ceph::buffer::list* in,
	      ceph::buffer::list* out)
{
  cls_rgw_fsio_join_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
    return r;
  }
  cls_rgw_fsio_join_ret ret;
  int r = on_head(hctx, op.marker, [&](auto& h, auto& fx) {
    fsio::join_in jin{op.caller, op.seq, op.writer, op.nonce, op.counter_seed,
		      op.expired, op.instance, op.identity};
    fsio::join_out jout;
    int r = fsio::join(h, jin, jout, fx);
    ret.bound = jout.bound;
    ret.construct = jout.construct;
    ret.already_applied = jout.already_applied;
    ret.state = static_cast<uint8_t>(jout.state);
    ret.others = jout.others;
    ret.trunc_epoch = jout.trunc_epoch;
    ret.size_hint = jout.size_hint;
    ret.mtime_hint = jout.mtime_hint;
    ret.last_run = jout.last_run;
    return r;
  });
  if (r < 0) {
    return r;
  }
  encode(ret, *out);
  return 0;
}

int rgw_fsio_ready(cls_method_context_t hctx, ceph::buffer::list* in,
	       ceph::buffer::list* out)
{
  cls_rgw_fsio_runs_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
    return r;
  }
  return on_head(hctx, {}, [&](auto& h, auto&) {
    return fsio::ready(h, op.caller, op.cs, op.runs, op.size, op.mtime);
  });
}

int rgw_fsio_hint(cls_method_context_t hctx, ceph::buffer::list* in,
		  ceph::buffer::list* out)
{
  cls_rgw_fsio_hint_size_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
    return r;
  }
  cls_rgw_fsio_hint_size_ret ret;
  int r = on_head(hctx, {}, [&](auto& h, auto&) {
    fsio::hint_out hout;
    int r = fsio::hint(h, op.caller, op.cs, op.trunc_epoch, op.size,
		       op.mtime, hout);
    ret.stale = hout.stale;
    ret.others = hout.others;
    ret.trunc_epoch = hout.trunc_epoch;
    ret.size = hout.size;
    return r;
  });
  if (r < 0) {
    return r;
  }
  encode(ret, *out);
  return 0;
}

int rgw_fsio_truncate(cls_method_context_t hctx, ceph::buffer::list* in,
		      ceph::buffer::list* out)
{
  cls_rgw_fsio_truncate_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
    return r;
  }
  cls_rgw_fsio_truncate_ret ret;
  int r = on_head(hctx, {}, [&](auto& h, auto&) {
    fsio::truncate_out tout;
    int r = fsio::truncate(h, op.caller, op.seq, op.cs, op.size, op.mtime,
			   tout);
    ret.trunc_epoch = tout.trunc_epoch;
    ret.already_applied = tout.already_applied;
    return r;
  });
  if (r < 0) {
    return r;
  }
  encode(ret, *out);
  return 0;
}

int rgw_fsio_writer_identity(cls_method_context_t hctx,
			     ceph::buffer::list* in, ceph::buffer::list* out)
{
  cls_rgw_fsio_identity_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
    return r;
  }
  return on_head(hctx, {}, [&](auto& h, auto&) {
    return fsio::writer_identity(h, op.caller, op.cs, op.identity);
  });
}

int rgw_fsio_extend(cls_method_context_t hctx, ceph::buffer::list* in,
		ceph::buffer::list* out)
{
  cls_rgw_fsio_runs_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
    return r;
  }
  if (op.runs.size() != 1) {
    return -EINVAL;
  }
  return on_head(hctx, {}, [&](auto& h, auto&) {
    return fsio::extend(h, op.caller, op.cs, op.runs.front());
  });
}

int rgw_fsio_leave(cls_method_context_t hctx, ceph::buffer::list* in,
	       ceph::buffer::list* out)
{
  cls_rgw_fsio_leave_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
    return r;
  }
  cls_rgw_fsio_leave_ret ret;
  int r = on_head(hctx, {}, [&](auto& h, auto& fx) {
    std::optional<fsio::publish_intent> intent;
    if (op.intent) {
      intent = to_core(*op.intent);
      intent->now_epoch = osd_epoch();
    }
    fsio::leave_out lout;
    int r = fsio::leave(h, op.caller, op.seq, op.cs, op.writer, op.expired,
			lout, fx, intent ? &*intent : nullptr);
    ret.already_applied = lout.already_applied;
    ret.publishing = lout.publishing;
    ret.abandoned = lout.abandoned;
    ret.olh_epoch = lout.olh_epoch;
    ret.instance = lout.instance;
    ret.drained = lout.drained;
    return r;
  });
  if (r < 0) {
    return r;
  }
  encode(ret, *out);
  return 0;
}

int rgw_fsio_rebind(cls_method_context_t hctx, ceph::buffer::list* in,
		ceph::buffer::list* out)
{
  cls_rgw_fsio_rebind_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
    return r;
  }
  cls_rgw_fsio_rebind_ret ret;
  int r = on_head(hctx, {}, [&](auto& h, auto&) {
    fsio::rebind_out rout;
    int r = fsio::rebind(h, op.caller, op.seq, op.from, op.to, op.readers,
			 &rout);
    ret.already_applied = rout.already_applied;
    ret.drained = rout.drained;
    return r;
  });
  if (r < 0) {
    return r;
  }
  encode(ret, *out);
  return 0;
}

int rgw_fsio_publish_release(cls_method_context_t hctx, ceph::buffer::list* in,
			 ceph::buffer::list* out)
{
  cls_rgw_fsio_cs_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
    return r;
  }
  return on_head(hctx, {}, [&](auto& h, auto&) {
    return fsio::publish_release(h, op.caller, op.cs);
  });
}

int rgw_fsio_fenced_finalize(cls_method_context_t hctx, ceph::buffer::list* in,
			 ceph::buffer::list* out)
{
  cls_rgw_fsio_finalize_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.finalizer.incarnation); r < 0) {
    return r;
  }
  cls_rgw_fsio_finalize_ret ret;
  int r = on_head(hctx, {}, [&](auto& h, auto&) {
    std::optional<fsio::publish_intent> intent;
    if (op.intent) {
      intent = to_core(*op.intent);
      intent->now_epoch = osd_epoch();
    }
    return fsio::fenced_finalize(h, op.finalizer, op.from, op.expired, ret.to,
				 intent ? &*intent : nullptr, op.aborted);
  });
  if (r < 0) {
    return r;
  }
  encode(ret, *out);
  return 0;
}

int rgw_fsio_expire(cls_method_context_t hctx, ceph::buffer::list* in,
		ceph::buffer::list* out)
{
  cls_rgw_fsio_expire_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
    return r;
  }
  cls_rgw_fsio_expire_ret ret;
  int r = on_head(hctx, {}, [&](auto& h, auto& fx) {
    fsio::expire_out eout;
    int r = fsio::expire(h, op.caller, op.expired, op.clear, eout, fx);
    ret.drained = !eout.drained.empty();
    ret.finalize = eout.finalize;
    return r;
  });
  if (r < 0) {
    return r;
  }
  encode(ret, *out);
  return 0;
}

int rgw_fsio_forget(cls_method_context_t hctx, ceph::buffer::list* in,
		ceph::buffer::list* out)
{
  cls_rgw_fsio_cs_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
    return r;
  }
  return on_head(hctx, {}, [&](auto& h, auto& fx) {
    return fsio::forget(h, op.cs, fx);
  });
}

int rgw_fsio_drop_seqs(cls_method_context_t hctx, ceph::buffer::list* in,
		   ceph::buffer::list* out)
{
  cls_rgw_fsio_drop_seqs_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  return on_head(hctx, {}, [&](auto& h, auto& fx) {
    return fsio::drop_seqs(h, op.incarnation, fx);
  });
}

int ocnt_reply(const fsio::ocnt_drain& d, ceph::buffer::list* out)
{
  cls_rgw_fsio_flags_ret ret;
  ret.drained = d.drained;
  ret.removed = d.removed;
  encode(ret, *out);
  return 0;
}

int rgw_fsio_ocnt_add(cls_method_context_t hctx, ceph::buffer::list* in,
		  ceph::buffer::list* out)
{
  cls_rgw_fsio_ocnt_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.holder.incarnation); r < 0) {
    return r;
  }
  return on_instance(hctx, {}, [&](auto& ih, auto&) {
    return fsio::ocnt_add(ih, op.holder);
  });
}

int rgw_fsio_ocnt_remove(cls_method_context_t hctx, ceph::buffer::list* in,
		     ceph::buffer::list* out)
{
  cls_rgw_fsio_ocnt_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (int r = check_origin(hctx, op.holder.incarnation); r < 0) {
    return r;
  }
  fsio::ocnt_drain d;
  int r = on_instance(hctx, {}, [&](auto& ih, auto&) {
    return fsio::ocnt_remove(ih, op.holder, d);
  });
  return r < 0 ? r : ocnt_reply(d, out);
}

int rgw_fsio_ocnt_expire(cls_method_context_t hctx, ceph::buffer::list* in,
		     ceph::buffer::list* out)
{
  cls_rgw_fsio_ocnt_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  fsio::ocnt_drain d;
  int r = on_instance(hctx, {}, [&](auto& ih, auto&) {
    return fsio::ocnt_expire(ih, op.expired, d);
  });
  return r < 0 ? r : ocnt_reply(d, out);
}

int rgw_fsio_ocnt_forget(cls_method_context_t hctx, ceph::buffer::list* in,
		     ceph::buffer::list* out)
{
  cls_rgw_fsio_ocnt_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  return on_instance(hctx, {}, [&](auto& ih, auto& fx) {
    return fsio::ocnt_forget(ih, fx);
  });
}

int rgw_fsio_remove_version(cls_method_context_t hctx, ceph::buffer::list* in,
			ceph::buffer::list* out)
{
  cls_rgw_fsio_remove_version_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  cls_rgw_fsio_instance_head ih;
  loaded l;
  int r = load_instance(hctx, ih, l);
  if (r < 0) {
    return r;
  }
  if ((r = require(ih.exists)) < 0) {
    return r;
  }
  fsio::remove_out rout;
  cls_rgw_fsio_effects fx;
  if ((r = fsio::remove_version(ih, op.r, rout, fx)) < 0) {
    return r;
  }
  /* §2.5:  the clone to the sibling first (CLONE_TO clones the pre-op
   * object), then the instance head reset:  removed, and recreated with
   * its open count alone while the count keeps it */
  if ((r = apply(hctx, op.marker, fx)) < 0) {
    return r;
  }
  if ((r = cls_cxx_remove(hctx)) < 0) {
    CLS_ERR("ERROR: %s: cls_cxx_remove returned %d", __func__, r);
    return r;
  }
  if (ih.exists && ih.ocnt) {
    if ((r = cls_cxx_create(hctx, false)) < 0) {
      return r;
    }
    ceph::buffer::list bl;
    encode(*ih.ocnt, bl);
    if ((r = cls_cxx_setxattr(hctx, OCNT.data(), &bl)) < 0) {
      return r;
    }
  }
  cls_rgw_fsio_remove_version_ret ret{rout.keep_tail};
  encode(ret, *out);
  return 0;
}

/* every FSIO record of a head, encoded:  what a recreated head is given */
std::map<std::string, ceph::buffer::list> records_of(const cls_rgw_fsio_head& h)
{
  std::map<std::string, ceph::buffer::list> m;
  if (h.hdr) {
    encode(*h.hdr, m[std::string(HDR)]);
  }
  for (const auto& [inc, o] : h.opens) {
    encode(o, m[opens_name(inc)]);
  }
  return m;
}

/* design §3:  in one call, the FSIO state for the mode, the clones first
 * (CLONE_TO clones the pre-op object wherever the op places it), then,
 * for every mode but the versioned publish, the head removed and, if
 * anything stays, recreated with the RGW attributes the caller keeps
 * and the new FSIO records, as rgw_obj_remove() removes a head.  The
 * caller's own writes come after this call in its op (§3, the ordering
 * rule). */
/* §2.5, Y6:  fsio_replace_head's supersede on an instance head:  the
 * clone to the sibling first, then the head recreated with the caller's
 * prefixes and the open count, RGW's write following */
int rewrite_instance(cls_method_context_t hctx,
		     const cls_rgw_fsio_replace_op& op, ceph::buffer::list* out)
{
  cls_rgw_fsio_instance_head ih;
  loaded l;
  int r = load_instance(hctx, ih, l);
  if (r < 0) {
    return r;
  }
  const auto& all = l.all;
  fsio::remove_out rout;
  cls_rgw_fsio_effects fx;
  if ((r = fsio::rewrite_version(ih, op.r, op.keep_tail, rout, fx)) < 0) {
    return r;
  }
  if ((r = apply(hctx, op.marker, fx)) < 0) {
    return r;
  }
  std::map<std::string, ceph::buffer::list> kept;
  for (const auto& prefix : op.keep_attr_prefixes) {
    for (auto i = all.lower_bound(prefix);
	 i != all.end() && i->first.starts_with(prefix); ++i) {
      if (!i->first.starts_with(FSIO_PREFIX)) {
	kept[i->first] = i->second;
      }
    }
  }
  if (ih.ocnt) {
    encode(*ih.ocnt, kept[std::string(OCNT)]);
  }
  if ((r = cls_cxx_remove(hctx)) < 0) {
    CLS_ERR("ERROR: %s: cls_cxx_remove returned %d", __func__, r);
    return r;
  }
  if ((r = cls_cxx_create(hctx, false)) < 0) {
    return r;
  }
  for (auto& [name, v] : kept) {
    if ((r = cls_cxx_setxattr(hctx, name.c_str(), &v)) < 0) {
      CLS_ERR("ERROR: %s: setting %s returned %d", __func__, name.c_str(), r);
      return r;
    }
  }
  cls_rgw_fsio_replace_ret ret{rout.keep_tail, false, std::nullopt};
  encode(ret, *out);
  return 0;
}

int rgw_fsio_replace_head(cls_method_context_t hctx, ceph::buffer::list* in,
			  ceph::buffer::list* out)
{
  cls_rgw_fsio_replace_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (op.mode == static_cast<uint8_t>(fsio::replace_mode::PUBLISH)) {
    if (int r = check_origin(hctx, op.caller.incarnation); r < 0) {
      return r;
    }
  }
  cls_rgw_fsio_head h;
  loaded l;
  int r = load_head(hctx, h, l);
  if (r < 0) {
    return r;
  }
  if ((r = require(h.exists)) < 0) {
    return r;
  }
  const auto in_core = to_core(op);
  /* §2.5, Y6:  a head write on an instance head with an open count, a
   * version rewritten in place:  its content deferred in the count */
  if (in_core.mode == fsio::replace_mode::SUPERSEDE &&
      l.all.count(std::string(OCNT))) {
    return rewrite_instance(hctx, op, out);
  }
  fsio::replace_out rout;
  cls_rgw_fsio_effects fx;
  if ((r = fsio::replace_head(h, in_core, rout, fx)) < 0) {
    return r;
  }
  cls_rgw_fsio_replace_ret ret{rout.keep_tail, rout.born_drained, rout.drained};
  /* the clones first;  apply() would also remove a head with nothing
   * left, which the replacement below decides itself */
  auto clones = fx;
  clones.remove_head = false;
  if ((r = apply(hctx, op.marker, clones)) < 0) {
    return r;
  }
  if (in_core.mode == fsio::replace_mode::PUBLISH && in_core.versioned) {
    if ((r = store_head(hctx, l, h)) < 0) {   // the OLH head stays
      return r;
    }
    encode(ret, *out);
    return 0;
  }
  /* what the recreated head keeps:  the RGW attributes under the
   * caller's prefixes, read before the op, and the new FSIO records */
  std::map<std::string, ceph::buffer::list> kept;
  for (const auto& prefix : op.keep_attr_prefixes) {
    for (auto i = l.all.lower_bound(prefix);
	 i != l.all.end() && i->first.starts_with(prefix); ++i) {
      if (!i->first.starts_with(FSIO_PREFIX)) {
	kept[i->first] = i->second;
      }
    }
  }
  /* §3.5, Y18, Z15, C1:  the publish's only writer of tags:  the
   * committed head's when it is an S3 object, else the tags the content
   * had, which the publisher passes */
  if (in_core.mode == fsio::replace_mode::PUBLISH) {
    if (!h.id_tag.empty()) {
      if (auto t = l.all.find(std::string(TAGS)); t != l.all.end()) {
	kept[t->first] = t->second;
      }
    } else if (op.tags.length()) {
      kept[std::string(TAGS)] = op.tags;
    }
  }
  if (!fx.remove_head) {
    kept.merge(records_of(h));
  }
  if ((r = cls_cxx_remove(hctx)) < 0) {
    CLS_ERR("ERROR: %s: cls_cxx_remove returned %d", __func__, r);
    return r;
  }
  if (!kept.empty()) {
    if ((r = cls_cxx_create(hctx, false)) < 0) {
      CLS_ERR("ERROR: %s: cls_cxx_create returned %d", __func__, r);
      return r;
    }
    for (auto& [name, v] : kept) {
      if ((r = cls_cxx_setxattr(hctx, name.c_str(), &v)) < 0) {
	CLS_ERR("ERROR: %s: setting %s returned %d", __func__, name.c_str(), r);
	return r;
      }
    }
  }
  encode(ret, *out);
  return 0;
}

/* the FSIO open hints (§2.7):  the key leads with the incarnation, so
 * the expirer range-lists a dead incarnation's hints */
std::string incarnation_prefix(uint64_t incarnation)
{
  return fmt::format("{:016x}_", incarnation);
}

std::string hint_key(const cls_rgw_fsio_hint& h, const std::string& oid)
{
  auto k = fmt::format("{}{:016x}_{}_{}", incarnation_prefix(h.incarnation),
		       h.handle, static_cast<unsigned>(h.kind), oid);
  if (h.r) {
    k += "_" + h.r->to_str();
  }
  return k;
}

int rgw_fsio_open_hint_add(cls_method_context_t hctx, ceph::buffer::list* in,
			   ceph::buffer::list* out)
{
  cls_rgw_fsio_hint_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (op.oid.empty()) {
    return -EINVAL;
  }
  ceph::buffer::list bl;
  encode(op.hint, bl);
  return cls_cxx_map_set_val(hctx, hint_key(op.hint, op.oid), &bl);
}

/* removing an absent hint succeeds:  a release after a resend */
int rgw_fsio_open_hint_remove(cls_method_context_t hctx, ceph::buffer::list* in,
			      ceph::buffer::list* out)
{
  cls_rgw_fsio_hint_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  if (op.oid.empty()) {
    return -EINVAL;
  }
  int r = cls_cxx_map_remove_key(hctx, hint_key(op.hint, op.oid));
  return r == -ENOENT ? 0 : r;
}

/* the most keys one call reads, filtered or not */
constexpr uint32_t HINT_LIST_READ_MAX = 1000;

int rgw_fsio_open_hint_list(cls_method_context_t hctx, ceph::buffer::list* in,
			    ceph::buffer::list* out)
{
  cls_rgw_fsio_hint_list_op op;
  if (int r = decode_op(in, op, __func__); r < 0) {
    return r;
  }
  const uint32_t max = std::min(op.max ? op.max : HINT_LIST_READ_MAX,
				HINT_LIST_READ_MAX);
  /* the OSD seeks to the prefix when it sorts after the marker */
  const std::string prefix =
    op.incarnation ? incarnation_prefix(*op.incarnation) : std::string{};
  std::map<std::string, ceph::buffer::list> vals;
  bool more = false;
  int r = cls_cxx_map_get_vals(hctx, op.marker, prefix, HINT_LIST_READ_MAX,
			       &vals, &more);
  if (r < 0) {
    return r;
  }
  cls_rgw_fsio_hint_list_ret ret;
  ret.marker = op.marker;
  bool stopped = false;
  for (auto& [k, v] : vals) {
    if (ret.hints.size() == max) {
      stopped = true;
      break;
    }
    ret.marker = k;
    cls_rgw_fsio_hint h;
    try {
      auto it = v.cbegin();
      decode(h, it);
    } catch (const ceph::buffer::error&) {
      CLS_ERR("ERROR: %s: failed to decode hint %s", __func__, k.c_str());
      return -EIO;
    }
    if (op.bucket && !(h.bucket == *op.bucket)) {
      continue;
    }
    ret.hints.push_back(std::move(h));
  }
  ret.truncated = stopped || more;
  encode(ret, *out);
  return 0;
}

} // anonymous namespace

void cls_rgw_fsio_register(cls_handle_t h_class)
{
  using namespace cls::rgw::method;
  ClassRegistrar<cls::rgw::ClassId> cls(h_class);
  static cls_method_handle_t h[22];
  cls.register_cxx_method(fsio_join, rgw_fsio_join, &h[0]);
  cls.register_cxx_method(fsio_ready, rgw_fsio_ready, &h[1]);
  cls.register_cxx_method(fsio_extend, rgw_fsio_extend, &h[2]);
  cls.register_cxx_method(fsio_leave, rgw_fsio_leave, &h[3]);
  cls.register_cxx_method(fsio_rebind, rgw_fsio_rebind, &h[4]);
  cls.register_cxx_method(fsio_publish_release, rgw_fsio_publish_release, &h[5]);
  cls.register_cxx_method(fsio_fenced_finalize, rgw_fsio_fenced_finalize, &h[6]);
  cls.register_cxx_method(fsio_expire, rgw_fsio_expire, &h[7]);
  cls.register_cxx_method(fsio_forget, rgw_fsio_forget, &h[8]);
  cls.register_cxx_method(fsio_drop_seqs, rgw_fsio_drop_seqs, &h[9]);
  cls.register_cxx_method(fsio_ocnt_add, rgw_fsio_ocnt_add, &h[10]);
  cls.register_cxx_method(fsio_ocnt_remove, rgw_fsio_ocnt_remove, &h[11]);
  cls.register_cxx_method(fsio_ocnt_expire, rgw_fsio_ocnt_expire, &h[12]);
  cls.register_cxx_method(fsio_ocnt_forget, rgw_fsio_ocnt_forget, &h[13]);
  cls.register_cxx_method(fsio_remove_version, rgw_fsio_remove_version, &h[14]);
  cls.register_cxx_method(fsio_replace_head, rgw_fsio_replace_head, &h[15]);
  cls.register_cxx_method(fsio_open_hint_add, rgw_fsio_open_hint_add, &h[16]);
  cls.register_cxx_method(fsio_open_hint_remove, rgw_fsio_open_hint_remove,
			  &h[17]);
  cls.register_cxx_method(fsio_open_hint_list, rgw_fsio_open_hint_list, &h[18]);
  cls.register_cxx_method(fsio_hint, rgw_fsio_hint, &h[19]);
  cls.register_cxx_method(fsio_truncate, rgw_fsio_truncate, &h[20]);
  cls.register_cxx_method(fsio_writer_identity, rgw_fsio_writer_identity,
			  &h[21]);
}
