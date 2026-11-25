// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_fsio_object.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <thread>

#include <boost/system/error_code.hpp>

#include "cls/rgw/cls_rgw_fsio.h"
#include "cls/rgw/cls_rgw_fsio_ops.h"
#include "cls/rgw/cls_rgw_ops.h"
#include "common/async/blocked_completion.h"
#include "common/dout.h"
#include "common/error_code.h"
#include "common/dout_fmt.h"
#include "common/random_string.h"
#include "include/random.h"
#include "neorados/cls/rgw_fsio.h"
#include "rgw_acl_s3.h"
#include "rgw_aio_throttle.h"
#include "rgw_bucket_sync.h"
#include "rgw_fsio_hints.h"
#include "rgw_obj_manifest.h"
#include "rgw_object_ownership.h"
#include "rgw_rados.h"
#include "services/svc_zone.h"
#include "rgw_sal_rados.h"
#include "rgw_tag.h"
#include "rgw_tools.h"

#define dout_subsys ceph_subsys_rgw

namespace sys = boost::system;

namespace rgw::sal {

namespace {
/* the FSIO prefix, RGW's manifest and cls_refcount's tags:  what every
 * FSIO clone drops (§2.4, plan §6.7, X16) */
const std::vector<std::string> CLONE_DROPS{"user.rgw.fsio.", RGW_ATTR_MANIFEST,
					   "refcount"};
bool is_null(const std::string& instance)
{
  return instance.empty() || instance == "null";
}
} // anonymous namespace

int RadosFSIOObject::exec(const std::string& oid,
			  const neorados::IOContext& ioc, neorados::WriteOp op)
{
  sys::error_code ec;
  store->get_neorados().execute(neorados::Object{oid}, ioc, std::move(op),
				ceph::async::use_blocked[ec]);
  return ceph::from_error_code(ec);
}

int RadosFSIOObject::presence_hint(const DoutPrefixProvider* dpp, bool add,
				   bool on_instance)
{
  cls_rgw_fsio_hint_op in;
  in.hint.kind = cls_rgw_fsio_hint_kind::PRESENCE;
  in.hint.target = on_instance ? cls_rgw_fsio_hint_target::INSTANCE_HEAD
			       : cls_rgw_fsio_hint_target::HEAD;
  in.hint.bucket = info.bucket;
  in.hint.key = on_instance ? rgw_obj_key(key.name, inst) : rgw_obj_key(key.name);
  in.hint.incarnation = h.incarnation;
  in.hint.handle = h.handle;
  in.oid = on_instance ? inst_oid : head_oid;
  const auto shards = static_cast<uint32_t>(
    dpp->get_cct()->_conf.get_val<uint64_t>("rgw_fsio_open_hints_num_shards"));
  const auto shard = rgw::fsio::open_hint_shard_oid(
    rgw::fsio::open_hint_shard(in.oid, shards));
  neorados::WriteOp op;
  if (add) {
    op.exec(neorados::cls::rgw_fsio::open_hint_add(in));
  } else {
    op.exec(neorados::cls::rgw_fsio::open_hint_remove(in));
  }
  int r = exec(shard, store->getRados()->fsio_hint_ioc(), std::move(op));
  if (r < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  " << (add ? "adding" : "removing")
		      << " the presence hint of " << h.incarnation << "."
		      << h.handle << " on " << in.oid << ":  "
		      << cpp_strerror(-r) << dendl;
  }
  return r;
}

int RadosFSIOObject::read_source(const DoutPrefixProvider* dpp,
				 const std::string& oid,
				 const neorados::IOContext& ioc, Source* src)
{
  *src = Source{};
  src->oid = oid;
  /* every xattr, not the two by name:  a head without one (a created or
   * tracking-only head, §2.2) would fail the whole op with ENODATA */
  neorados::ReadOp op;
  boost::container::flat_map<std::string, ceph::buffer::list> attrs;
  op.get_xattrs(&attrs);
  op.stat(&src->head_size, &src->mtime);
  sys::error_code ec;
  store->get_neorados().execute(neorados::Object{oid}, ioc, std::move(op),
				nullptr, ceph::async::use_blocked[ec]);
  if (ec) {
    return ceph::from_error_code(ec);
  }
  if (auto i = attrs.find(RGW_ATTR_ID_TAG); i != attrs.end()) {
    src->id_tag = std::move(i->second);
  }
  if (auto i = attrs.find(RGW_ATTR_MANIFEST); i != attrs.end()) {
    src->manifest = std::move(i->second);
  }
  /* the object's size is the manifest's;  an object without one is its
   * head */
  src->size = src->head_size;
  if (src->manifest.length()) {
    try {
      RGWObjManifest m;
      auto it = src->manifest.cbegin();
      decode(m, it);
      src->size = m.get_obj_size();
    } catch (const ceph::buffer::error& e) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  undecodable manifest on " << oid
			<< ":  " << e.what() << dendl;
      return -EIO;
    }
  }
  return 0;
}

int RadosFSIOObject::read_olh(const DoutPrefixProvider* dpp, Olh* olh)
{
  ceph::buffer::list info_bl;
  neorados::ReadOp op;
  sys::error_code info_ec, tag_ec, ver_ec;
  op.get_xattr(RGW_ATTR_OLH_INFO, &info_bl, &info_ec);
  op.get_xattr(RGW_ATTR_OLH_ID_TAG, &olh->id_tag, &tag_ec);
  op.get_xattr(RGW_ATTR_OLH_VER, &olh->ver, &ver_ec);
  sys::error_code ec;
  store->get_neorados().execute(neorados::Object{head_oid}, head_ioc,
				std::move(op), nullptr,
				ceph::async::use_blocked[ec]);
  if (ec) {
    return ceph::from_error_code(ec);
  }
  if (info_ec || !info_bl.length()) {
    return -ENOENT;		// no OLH:  the head is the object's
  }
  try {
    RGWOLHInfo oi;
    auto it = info_bl.cbegin();
    decode(oi, it);
    if (oi.removed) {
      olh->removed = true;	// a delete marker:  no current version
      return 0;
    }
    olh->instance = oi.target.key.instance;
  } catch (const ceph::buffer::error& e) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  undecodable OLH info on " << head_oid
		      << ":  " << e.what() << dendl;
    return -EIO;
  }
  return 0;
}

int RadosFSIOObject::count(const DoutPrefixProvider* dpp)
{
  rgw_raw_obj raw;
  store->getRados()->obj_to_raw(info.placement_rule,
				rgw_obj(info.bucket, rgw_obj_key(key.name, inst)),
				&raw);
  inst_oid = raw.oid;
  try {
    inst_ioc = rgw::init_iocontext(dpp, store->get_neorados(), raw.pool,
				   ceph::async::use_blocked);
  } catch (const sys::system_error& e) {
    return ceph::from_error_code(e.code());
  }
  /* §4.3:  the hint before every ocnt_add */
  if (int r = presence_hint(dpp, true, true); r < 0) {
    return r;
  }
  inst_hinted = true;
  cls_rgw_fsio_ocnt_op in;
  in.holder = h;
  cls_rgw_fsio_ocnt_add_ret ret;
  neorados::WriteOp op;
  op.exec(neorados::cls::rgw_fsio::ocnt_add(in, &ret));
  int r = exec(inst_oid, inst_ioc, std::move(op));
  if (r == 0 && !ret.gone) {
    counted = true;
    /* the content it is counted for, by the tag a deferred record names
     * it with (gc_tag_of(), E5):  its tail tag, else its ID tag */
    ceph::buffer::list idt, tailt;
    neorados::ReadOp rop;
    sys::error_code e1, e2, ec;
    rop.get_xattr(RGW_ATTR_ID_TAG, &idt, &e1);
    rop.get_xattr(RGW_ATTR_TAIL_TAG, &tailt, &e2);
    store->get_neorados().execute(neorados::Object{inst_oid}, inst_ioc,
				  std::move(rop), nullptr,
				  ceph::async::use_blocked[ec]);
    counted_id_tag = idt;
    counted_gc_tag = tailt.length() ? tailt.to_str() : idt.to_str();
    return 0;
  }
  /* §4.3:  a version gone keeps the hint only if the count holds the
   * handle (a publisher's instance write counted it);  the open fails
   * either way */
  if (r < 0 || !ret.held) {
    presence_hint(dpp, false, true);
    inst_hinted = false;
  }
  return r < 0 ? r : -ENOENT;
}

int RadosFSIOObject::uncount(const DoutPrefixProvider* dpp)
{
  if (counted) {
    cls_rgw_fsio_ocnt_op in;
    in.holder = h;
    cls_rgw_fsio_flags_ret ret;
    neorados::WriteOp op;
    op.exec(neorados::cls::rgw_fsio::ocnt_remove(in, &ret));
    if (int r = exec(inst_oid, inst_ioc, std::move(op)); r < 0) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_ocnt_remove on " << inst_oid
			<< ":  " << cpp_strerror(-r) << dendl;
      return r;
    }
    counted = false;
    if (ret.drained) {
      if (int d = drain_count(dpp); d < 0) {
	return d;
      }
    }
  }
  if (inst_hinted) {
    presence_hint(dpp, false, true);
    inst_hinted = false;
  }
  return 0;
}

int RadosFSIOObject::join(const DoutPrefixProvider* dpp, bool writer,
			  Source* src, FSIOGeometry* g,
			  cls_rgw_fsio_join_ret* ret)
{
  /* §4.1, W13:  a join that must wait (§3.1 step 5) is retried with
   * backoff, for at most rgw_fsio_join_wait */
  const auto wait = dpp->get_cct()->_conf.get_val<std::chrono::seconds>(
    "rgw_fsio_join_wait");
  const auto until = ceph::mono_clock::now() + wait;
  auto backoff = std::chrono::milliseconds(50);
  static constexpr int tries = 10;	// of S3 writes landing in between
  for (int t = 0; t < tries; ) {
    neorados::WriteOp op;
    std::string instance;
    bool absent = false;
    Olh olh;
    int r = info.versioned() ? read_olh(dpp, &olh) : -ENOENT;
    if (r < 0 && r != -ENOENT) {
      return r;
    }
    if (r == 0 && olh.removed) {
      /* §3.1 step 0, §4.1, 8i:  a delete marker:  the name has no current
       * version;  only a writer's create joins, absent, guarded by the
       * OLH it read, so a version linked since fails it */
      if (!writer || !create) {
	return -ENOENT;
      }
      if (counted) {
	if (int u = uncount(dpp); u < 0) {
	  return u;
	}
      }
      absent = true;
      *src = Source{};
      src->oid = head_oid;
      op.cmpxattr(RGW_ATTR_OLH_ID_TAG, neorados::cmp_op::eq, olh.id_tag);
      op.cmpxattr(RGW_ATTR_OLH_VER, neorados::cmp_op::eq, olh.ver);
    } else if (r == 0 && !is_null(olh.instance)) {
      /* §2.5:  count on the instance the OLH names;  a relink since the
       * last try moved it, so the old count comes off first */
      if (counted && inst != olh.instance) {
	if (int u = uncount(dpp); u < 0) {
	  return u;
	}
      }
      if (!counted) {
	inst = olh.instance;
	if (int c = count(dpp); c < 0) {
	  return c;
	}
      }
      instance = inst;
      if (int s = read_source(dpp, inst_oid, inst_ioc, src); s < 0) {
	return s;
      }
      /* §6.2:  declined before the join, so it leaves no state */
      if (int e = make_geometry(dpp, src->manifest, g); e < 0) {
	return e;
      }
      /* §3.1:  versioned, the OLH's ID tag and version are the guard */
      op.cmpxattr(RGW_ATTR_OLH_ID_TAG, neorados::cmp_op::eq, olh.id_tag);
      op.cmpxattr(RGW_ATTR_OLH_VER, neorados::cmp_op::eq, olh.ver);
    } else {
      /* unversioned, or the null version (§2.5a), joined as a head is */
      if (r == 0) {
	instance = "null";
      }
      int s = read_source(dpp, head_oid, head_ioc, src);
      if (s < 0 && s != -ENOENT) {
	return s;
      }
      /* §2.2:  no head, or a tracking-only one (no ID tag, no manifest):
       * the name has no S3 object, and only a writer's create joins it,
       * with no guard, since it read no tag (§3.1 step 0, Z24) */
      if (s == -ENOENT || (!src->id_tag.length() && !src->manifest.length())) {
	if (!writer || !create) {
	  return -ENOENT;
	}
	if (s == -ENOENT) {
	  *src = Source{};
	  src->oid = head_oid;
	}
      } else {
	/* §6.2:  declined before the join, so it leaves no state */
	if (int e = make_geometry(dpp, src->manifest, g); e < 0) {
	  return e;
	}
	if (src->id_tag.length()) {
	  op.cmpxattr(RGW_ATTR_ID_TAG, neorados::cmp_op::eq, src->id_tag);
	}
      }
    }
    cls_rgw_fsio_join_op in;
    in.caller = h;
    in.seq = seq + 1;		// taken only if the call applies
    in.writer = writer;
    in.nonce = ceph::util::generate_random_number<uint64_t>();
    /* §6.8:  a new shadow's id sorts after the published one's */
    in.counter_seed = g->max_counter() + 1;
    in.instance = instance;
    in.marker = info.bucket.marker;
    if (writer) {
      in.identity = identity;	// §3.5 item 6, Y16
      in.create = create;
      in.absent = absent;
    }
    op.exec(neorados::cls::rgw_fsio::join(in, ret));
    r = exec(head_oid, head_ioc, std::move(op));
    if (r == -ECANCELED) {
      ++t;
      continue;			// an S3 write or a relink landed:  read again
    }
    if (r == -CLS_RGW_ERR_FSIO_BUSY) {
      /* the shadow is being constructed or published:  the whole wait,
       * a last try at its end */
      const auto now = ceph::mono_clock::now();
      if (now >= until) {
	ldpp_dout(dpp, 1) << "FSIO:  fsio_join on " << head_oid << " waited "
			  << wait.count() << "s:  EAGAIN" << dendl;
	return -EAGAIN;
      }
      std::this_thread::sleep_for(std::min<ceph::timespan>(backoff, until - now));
      backoff = std::min(backoff * 2, std::chrono::milliseconds(1000));
      continue;
    }
    if (r < 0) {
      ldpp_dout(dpp, 1) << "FSIO:  fsio_join as " << (writer ? "writer" : "reader")
			<< " of " << head_oid << " by " << h.incarnation << "."
			<< h.handle << ":  " << cpp_strerror(-r) << dendl;
      return r;
    }
    seq = in.seq;
    return 0;
  }
  return -EAGAIN;
}

int RadosFSIOObject::find_deferred(const DoutPrefixProvider* dpp,
				   cls_rgw_fsio_replaced* rec)
{
  ceph::buffer::list bl;
  neorados::ReadOp op;
  op.get_xattr("user.rgw.fsio.ocnt", &bl, nullptr);
  sys::error_code ec;
  store->get_neorados().execute(neorados::Object{inst_oid}, inst_ioc,
				std::move(op), nullptr,
				ceph::async::use_blocked[ec]);
  if (ec) {
    return ceph::from_error_code(ec);
  }
  cls_rgw_fsio_ocnt oc;
  try {
    auto it = bl.cbegin();
    decode(oc, it);
  } catch (const ceph::buffer::error&) {
    return -EIO;
  }
  for (const auto& d : oc.deferred) {
    if (d.gc_tag == counted_gc_tag) {
      *rec = d;
      return 0;
    }
  }
  ldpp_dout(dpp, 0) << "ERROR: FSIO:  the content " << counted_gc_tag
		    << " counted on " << inst_oid << " is neither the instance "
		    << "head's nor deferred" << dendl;
  return -ENOENT;
}

int RadosFSIOObject::make_geometry(const DoutPrefixProvider* dpp,
				   const ceph::buffer::list& manifest,
				   FSIOGeometry* g)
{
  const auto& zone = *store->svc()->zone;
  return FSIOGeometry::make(dpp, manifest, zone.get_zonegroup(),
			    zone.get_zone_params(), g);
}

int RadosFSIOObject::clone_chunks(const DoutPrefixProvider* dpp,
				  const FSIOGeometry& g,
				  const std::string& skip_oid,
				  const cls_rgw_fsio_id& id, uint64_t* n)
{
  /* §4.1:  the clones in flight together, rgw_fsio_clone_window of them;
   * EEXIST is a resend's, done */
  auto aio = rgw::make_throttle(
    dpp->get_cct()->_conf.get_val<uint64_t>("rgw_fsio_clone_window"),
    null_yield);
  int first = 0;
  auto settle = [&] (rgw::AioResultList&& done) {
    for (auto& d : done) {
      if (d.result < 0 && d.result != -EEXIST && !first) {
	first = d.result;
	ldpp_dout(dpp, 0) << "ERROR: FSIO:  cloning " << d.obj.oid << " for "
			  << id.to_str() << ":  " << cpp_strerror(-first)
			  << dendl;
      }
    }
    done.clear_and_dispose(std::default_delete<rgw::AioResultEntry>{});
  };
  std::map<rgw_pool, neorados::IOContext> iocs;
  /* the head's chunk always, a zero-length object's too */
  uint64_t ofs = 0;
  do {
    const FSIOChunk c = g.locate(dpp, ofs);
    *n = c.index + 1;
    ofs = c.ofs + c.capacity;
    /* the content's chunk, f(anchor, its id) when published (§6.4) */
    const rgw_raw_obj raw = FSIOGeometry::named(c, info.bucket.marker,
						std::nullopt);
    if (raw.oid == skip_oid) {
      continue;
    }
    auto ioc = iocs.find(raw.pool);
    if (ioc == iocs.end()) {
      try {
	ioc = iocs.emplace(raw.pool, rgw::init_iocontext(
			     dpp, store->get_neorados(), raw.pool,
			     ceph::async::use_blocked)).first;
      } catch (const sys::system_error& e) {
	first = ceph::from_error_code(e.code());
	break;
      }
    }
    auto cioc = ioc->second;
    if (!raw.loc.empty()) {
      cioc.set_key(raw.loc);
    }
    neorados::WriteOp op;
    op.clone_to(FSIOGeometry::named(c, info.bucket.marker, id).oid,
		CLONE_DROPS);
    settle(aio->get(raw, rgw::Aio::neorados_op(store->get_neorados(), cioc,
					       std::move(op), null_yield),
		    1, 0));
  } while (ofs < g.size() && !first);
  settle(aio->drain());
  return first;
}

int RadosFSIOObject::construct(const DoutPrefixProvider* dpp,
			       const Source& src, const cls_rgw_fsio_id& id)
{
  const auto& marker = info.bucket.marker;
  const FSIOGeometry* g = &wgeo;
  FSIOGeometry deferred;
  cls_rgw_fsio_replaced rec;
  uint64_t size = src.size;
  if (counted) {
    /* §4.1 step 1:  the join on the OLH only allocated the shadow;  the
     * gateway clones the instance head, guarded by the ID tag read with
     * the count */
    neorados::WriteOp op;
    op.cmpxattr(RGW_ATTR_ID_TAG, neorados::cmp_op::eq, counted_id_tag);
    op.clone_to(cls_rgw_fsio_clone_name(marker, src.head_size ? 'd' : 'a',
					id, inst_oid),
		CLONE_DROPS, {{"user.rgw.fsio.pm", src.manifest}});
    int r = exec(inst_oid, inst_ioc, std::move(op));
    if (r == -ECANCELED) {
      /* the version was rewritten or removed since the count:  its content
       * is the deferred record's, kept on the sibling f(instance, r), which
       * never changes */
      if (int d = find_deferred(dpp, &rec); d < 0) {
	return d;
      }
      auto sioc = inst_ioc;
      sioc.set_key(inst_oid);
      const auto sibling = cls_rgw_fsio_clone_name(marker, 'G', rec.clone,
						   inst_oid);
      uint64_t sib_size = 0;
      {
	neorados::ReadOp sop;
	sop.stat(&sib_size, nullptr);
	sys::error_code ec;
	store->get_neorados().execute(neorados::Object{sibling}, sioc,
				      std::move(sop), nullptr,
				      ceph::async::use_blocked[ec]);
	if (ec) {
	  return ceph::from_error_code(ec);
	}
      }
      neorados::WriteOp sop;
      sop.clone_to(cls_rgw_fsio_clone_name(marker, sib_size ? 'd' : 'a', id,
					   inst_oid),
		   CLONE_DROPS, {{"user.rgw.fsio.pm", rec.manifest}});
      r = exec(sibling, sioc, std::move(sop));
      if (int e = make_geometry(dpp, rec.manifest, &deferred); e < 0) {
	return e;
      }
      g = &deferred;
      size = deferred.size();
    }
    if (r < 0 && r != -EEXIST) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  cloning the instance head "
			<< inst_oid << " for " << id.to_str() << ":  "
			<< cpp_strerror(-r) << dendl;
      return r;
    }
  }
  uint64_t n = 1;
  if (int r = clone_chunks(dpp, *g, counted ? inst_oid : head_oid, id, &n);
      r < 0) {
    return r;
  }
  cls_rgw_fsio_runs_op in;
  in.caller = h;
  in.cs = id;
  in.runs = {cls_rgw_fsio_run{0, n - 1}};
  in.size = size;
  in.mtime = src.mtime;
  neorados::WriteOp op;
  op.exec(neorados::cls::rgw_fsio::ready(in));
  int r = exec(head_oid, head_ioc, std::move(op));
  if (r == 0) {
    wsize = size;		// the join saw the shadow before fsio_ready set it
    know({0, n - 1});
  }
  if (r < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_ready of " << id.to_str()
		      << " on " << head_oid << ":  " << cpp_strerror(-r) << dendl;
  }
  return r;
}

int RadosFSIOObject::ioc_for(const DoutPrefixProvider* dpp,
			     const rgw_raw_obj& raw, neorados::IOContext* ioc)
{
  auto i = iocs.find(raw.pool);
  if (i == iocs.end()) {
    try {
      i = iocs.emplace(raw.pool, rgw::init_iocontext(
			 dpp, store->get_neorados(), raw.pool,
			 ceph::async::use_blocked)).first;
    } catch (const sys::system_error& e) {
      return ceph::from_error_code(e.code());
    }
  }
  *ioc = i->second;
  if (!raw.loc.empty()) {
    ioc->set_key(raw.loc);
  }
  return 0;
}

int RadosFSIOObject::read_header(const DoutPrefixProvider* dpp,
				 cls_rgw_fsio_hdr* hdr)
{
  ceph::buffer::list bl;
  neorados::ReadOp op;
  op.get_xattr("user.rgw.fsio.hdr", &bl, nullptr);
  sys::error_code ec;
  store->get_neorados().execute(neorados::Object{head_oid}, head_ioc,
				std::move(op), nullptr,
				ceph::async::use_blocked[ec]);
  if (ec) {
    return ceph::from_error_code(ec);
  }
  try {
    auto it = bl.cbegin();
    decode(*hdr, it);
  } catch (const ceph::buffer::error&) {
    return -EIO;
  }
  return 0;
}

int RadosFSIOObject::sequenced(const DoutPrefixProvider* dpp,
			       fu2::function_view<void(neorados::WriteOp&)> build)
{
  int r = 0;
  for (int i = 0; i < (twice ? 2 : 1); ++i) {
    neorados::WriteOp op;
    build(op);
    r = exec(head_oid, head_ioc, std::move(op));
    if (r < 0) {
      return r;
    }
  }
  ++seq;
  return r;
}

rgw_raw_obj RadosFSIOObject::shadow_chunk(uint64_t ofs) const
{
  return FSIOGeometry::named(wgeo.locate(&*ndp, ofs), info.bucket.marker,
			     wcs);
}

int RadosFSIOObject::fstat(struct stat* st, uint32_t flags)
{
  if (!reading && !writing) {
    return -EBADF;
  }
  *st = {};
  st->st_mode = S_IFREG;
  st->st_nlink = 1;
  ceph::real_time mtime;
  if (writing) {
    /* §3.3:  never a remembered length across a lost reply */
    if (unsure) {
      if (int r = report_hint(&*ndp); r < 0) {
	return r;
      }
    }
    st->st_size = std::max(wsize, hwm);
    mtime = std::max(wmtime_hint, wmtime);
  } else {
    st->st_size = rgeo.size();
    mtime = rmtime;
  }
  st->st_mtim = ceph::real_clock::to_timespec(mtime);
  st->st_blksize = 4096;
  st->st_blocks = (st->st_size + 511) / 512;
  return 0;
}

int RadosFSIOObject::ftruncate(const DoutPrefixProvider* dpp, uint64_t size,
			       uint32_t flags)
{
  if (!writing) {
    return -EBADF;
  }
  rgw_cls_obj_check_attrs_prefix fence;
  fence.check_prefix = "user.rgw.fsio.fence";
  fence.fail_if_exist = true;
  ceph::buffer::list fence_bl;
  encode(fence, fence_bl);
  auto chunk_op = [&] (const FSIOChunk& c, auto&& body) -> int {
    const rgw_raw_obj raw = FSIOGeometry::named(c, info.bucket.marker, wcs);
    neorados::IOContext ioc;
    if (int r = ioc_for(dpp, raw, &ioc); r < 0) {
      return r;
    }
    neorados::WriteOp op;
    op.exec(::cls::rgw::method::obj_check_attrs_prefix, fence_bl);
    body(op);
    int r = exec(raw.oid, ioc, std::move(op));
    if (r == -ECANCELED) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  truncating chunk " << c.index
			<< " (" << raw.oid << ") of " << wcs.to_str()
			<< ":  it is fenced" << dendl;
      return -EIO;
    }
    return r;
  };
  /* the chunk holding the new end;  of a zero size, the head's */
  const FSIOChunk end = wgeo.locate(dpp, size ? size - 1 : 0);
  if (int r = reserve(dpp, end.index); r < 0) {
    return r;
  }
  /* §3.3:  down, every reserved chunk past the end goes:  the shadow's
   * extent covers every writer's reservations, and no chunk exists
   * outside it (§3.7) */
  cls_rgw_fsio_hdr hdr;
  if (int r = read_header(dpp, &hdr); r < 0) {
    return r;
  }
  std::vector<cls_rgw_fsio_run> extent;
  for (const auto& e : hdr.chunk_sets) {
    if (e.id == wcs && e.shadow) {
      extent = e.shadow->extent;
    }
  }
  uint64_t last = 0;
  for (const auto& run : extent) {
    last = std::max(last, run.last);
  }
  auto in_extent = [&] (uint64_t i) {
    for (const auto& run : extent) {
      if (run.first <= i && i <= run.last) {
	return true;
      }
    }
    return false;
  };
  for (FSIOChunk c = wgeo.locate(dpp, end.ofs + end.capacity);
       c.index <= last; c = wgeo.locate(dpp, c.ofs + c.capacity)) {
    if (!in_extent(c.index)) {
      continue;
    }
    int r = chunk_op(c, [] (neorados::WriteOp& op) { op.remove(); });
    if (r < 0 && r != -ENOENT) {
      return r;
    }
  }
  /* the end's chunk cut, or made and extended (TRUNCATE on an absent
   * object does nothing:  §2.7);  the head clone is never removed */
  if (int r = chunk_op(end, [&] (neorados::WriteOp& op) {
	op.create(false);
	op.truncate(size - std::min(size, end.ofs));
      }); r < 0) {
    return r;
  }
  /* then the head's record:  exactly this size, a new truncate epoch */
  const auto mtime = ceph::real_clock::now();
  cls_rgw_fsio_truncate_ret ret;
  int r = sequenced(dpp, [&] (neorados::WriteOp& op) {
    cls_rgw_fsio_truncate_op in;
    in.caller = h;
    in.seq = seq + 1;
    in.cs = wcs;
    in.size = size;
    in.mtime = mtime;
    op.exec(neorados::cls::rgw_fsio::truncate(in, &ret));
  });
  if (r == -CLS_RGW_ERR_FSIO_BUSY) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_truncate of " << wcs.to_str()
		      << " on " << head_oid << ":  publishing" << dendl;
    return -EIO;
  }
  if (r < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_truncate of " << wcs.to_str()
		      << " on " << head_oid << ":  " << cpp_strerror(-r) << dendl;
    return r;
  }
  hwm = size;
  wmtime = mtime;
  if (ret.already_applied) {
    /* §3.3:  applied, its reply lost:  the epoch it carried cannot be
     * recovered from the head, which may show a later truncate's;  keep
     * the one it knew, owe a report, and rebase before trusting a length */
    ldpp_dout(dpp, 1) << "FSIO:  fsio_truncate of " << wcs.to_str()
		      << " was applied, its reply lost" << dendl;
    unsure = true;
    dirty = true;
    return 0;
  }
  trunc_epoch = ret.trunc_epoch;
  wsize = size;
  dirty = false;
  return 0;
}

int RadosFSIOObject::find_moved_segment(const DoutPrefixProvider* dpp)
{
  const auto& marker = info.bucket.marker;
  if (counted) {
    /* §4.1 step 1, as construction:  a rewrite deferred it to the sibling */
    cls_rgw_fsio_replaced rec;
    if (int r = find_deferred(dpp, &rec); r < 0) {
      return r;
    }
    seg_moved = cls_rgw_fsio_clone_name(marker, 'G', rec.clone, seg_oid);
    return 0;
  }
  /* §2.3:  an overwrite or a removal made the generation's leaf clone */
  cls_rgw_fsio_hdr hdr;
  if (int r = read_header(dpp, &hdr); r < 0) {
    return r;
  }
  for (const auto& e : hdr.chunk_sets) {
    if (e.id == cs && e.manifest_at == cls_rgw_fsio_manifest_at::CLONE &&
	e.clone) {
      seg_moved = cls_rgw_fsio_clone_name(marker, 'G', *e.clone, seg_oid);
      return 0;
    }
  }
  ldpp_dout(dpp, 0) << "ERROR: FSIO:  the head segment of " << cs.to_str()
		    << " moved from " << seg_oid << ", but its chunk set "
		    << "names no clone" << dendl;
  return -EIO;
}

int RadosFSIOObject::read_piece(const DoutPrefixProvider* dpp,
				const FSIOChunk& c, bool shadow, uint64_t off,
				uint64_t len, ceph::buffer::list* bl)
{
  rgw_raw_obj raw = FSIOGeometry::named(
    c, info.bucket.marker,
    shadow ? std::optional<cls_rgw_fsio_id>{wcs} : std::nullopt);
  const bool segment = !shadow && c.index == 0 && raw.oid == seg_oid;
  for (int t = 0; t < 2; ++t) {
    if (segment && seg_moved) {
      raw.oid = *seg_moved;
      raw.loc = seg_oid;
    }
    neorados::IOContext ioc;
    if (int r = ioc_for(dpp, raw, &ioc); r < 0) {
      return r;
    }
    neorados::ReadOp op;
    if (segment && !seg_moved && seg_tag.length()) {
      op.cmpxattr(RGW_ATTR_ID_TAG, neorados::cmp_op::eq, seg_tag);
    }
    op.read(off, len, bl);
    sys::error_code ec;
    store->get_neorados().execute(neorados::Object{raw.oid}, ioc,
				  std::move(op), nullptr,
				  ceph::async::use_blocked[ec]);
    int r = ceph::from_error_code(ec);
    if (segment && !seg_moved && (r == -ECANCELED || r == -ENOENT)) {
      if (int m = find_moved_segment(dpp); m < 0) {
	return m;
      }
      ldpp_dout(dpp, 1) << "FSIO:  the head segment of " << seg_oid
			<< " moved to " << *seg_moved << " (" << cpp_strerror(-r)
			<< ")" << dendl;
      bl->clear();
      continue;
    }
    return r;
  }
  return -EIO;
}

int RadosFSIOObject::preadv(const struct iovec* iov, int iovcnt, uint64_t ofs,
			    uint64_t* bytes_read, uint32_t flags)
{
  *bytes_read = 0;
  if (!reading && !writing) {
    return -EBADF;
  }
  const DoutPrefixProvider* dpp = &*ndp;
  const bool shadow = writing || reader_on_shadow;
  const FSIOGeometry& g = shadow ? wgeo : rgeo;
  const uint64_t end = shadow ? std::max(wsize, hwm) : g.size();
  uint64_t total = 0;
  for (int i = 0; i < iovcnt; ++i) {
    total += iov[i].iov_len;
  }
  const uint64_t want = ofs >= end ? 0 : std::min(total, end - ofs);
  int vi = 0;
  uint64_t vo = 0;
  auto put = [&] (const char* p, uint64_t n) {	// p null:  zeros
    while (n) {
      const uint64_t k = std::min<uint64_t>(n, iov[vi].iov_len - vo);
      char* dst = static_cast<char*>(iov[vi].iov_base) + vo;
      if (p) {
	memcpy(dst, p, k);
	p += k;
      } else {
	memset(dst, 0, k);
      }
      n -= k;
      vo += k;
      if (vo == iov[vi].iov_len) {
	++vi;
	vo = 0;
      }
    }
  };
  for (uint64_t pos = ofs; pos < ofs + want; ) {
    const FSIOChunk c = g.locate(dpp, pos);
    const uint64_t len = std::min(c.ofs + c.capacity, ofs + want) - pos;
    ceph::buffer::list bl;
    int r = read_piece(dpp, c, shadow, pos - c.ofs, len, &bl);
    if (r == -ENOENT && shadow) {
      bl.clear();		// a hole in the shadow:  zeros (Z11)
    } else if (r < 0 || (!shadow && bl.length() < len)) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  reading chunk " << c.index << " ("
			<< c.anchor.oid << ") of " << head_oid << ":  "
			<< (r < 0 ? cpp_strerror(-r) : "short") << dendl;
      return -EIO;
    }
    for (const auto& b : bl.buffers()) {
      put(b.c_str(), b.length());
    }
    put(nullptr, len - bl.length());	// the rest of a short shadow chunk
    pos += len;
  }
  *bytes_read = want;
  return 0;
}

void RadosFSIOObject::know(const cls_rgw_fsio_run& run)
{
  known.push_back(run);
  std::sort(known.begin(), known.end());
  std::vector<cls_rgw_fsio_run> merged;
  for (const auto& r : known) {
    if (!merged.empty() && r.first <= merged.back().last + 1) {
      merged.back().last = std::max(merged.back().last, r.last);
    } else {
      merged.push_back(r);
    }
  }
  known.swap(merged);
}

int RadosFSIOObject::reserve(const DoutPrefixProvider* dpp, uint64_t index)
{
  for (const auto& r : known) {
    if (r.first <= index && index <= r.last) {
      return 0;
    }
  }
  /* §3.7:  an append extends the highest run by its own length, between
   * rgw_fsio_extent_reserve and extent_reserve_max;  a far write adds a run
   * of rgw_fsio_extent_reserve */
  static constexpr uint64_t extent_reserve_max = 256;
  const uint64_t base = dpp->get_cct()->_conf.get_val<uint64_t>(
    "rgw_fsio_extent_reserve");
  cls_rgw_fsio_run run{index, index + base - 1};
  if (!known.empty() && index == known.back().last + 1) {
    const uint64_t len = known.back().last - known.back().first + 1;
    run.last = index + std::min(std::max(len, base), extent_reserve_max) - 1;
  }
  cls_rgw_fsio_runs_op in;
  in.caller = h;
  in.cs = wcs;
  in.runs = {run};
  neorados::WriteOp op;
  op.exec(neorados::cls::rgw_fsio::extend(in));
  int r = exec(head_oid, head_ioc, std::move(op));
  if (r == -CLS_RGW_ERR_FSIO_BUSY) {
    /* §3.7:  the shadow is publishing:  a fenced finalize froze its extent,
     * this gateway having been presumed dead;  as a fenced chunk, -EIO */
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_extend of " << wcs.to_str()
		      << " on " << head_oid << ":  its extent is frozen" << dendl;
    return -EIO;
  }
  if (r < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_extend of " << wcs.to_str()
		      << " on " << head_oid << ":  " << cpp_strerror(-r) << dendl;
    return r;
  }
  ++n_extends;
  know(run);
  return 0;
}

int RadosFSIOObject::pwritev(const struct iovec* iov, int iovcnt,
			     uint64_t ofs, uint64_t* bytes_written,
			     uint32_t flags)
{
  *bytes_written = 0;
  if (!writing) {
    return -EBADF;
  }
  const DoutPrefixProvider* dpp = &*ndp;
  ceph::buffer::list all;
  for (int i = 0; i < iovcnt; ++i) {
    all.append(static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
  }
  rgw_cls_obj_check_attrs_prefix fence;
  fence.check_prefix = "user.rgw.fsio.fence";
  fence.fail_if_exist = true;
  ceph::buffer::list fence_bl;
  encode(fence, fence_bl);
  const uint64_t end = ofs + all.length();
  for (uint64_t pos = ofs; pos < end; ) {
    const FSIOChunk c = wgeo.locate(dpp, pos);
    const uint64_t len = std::min(c.ofs + c.capacity, end) - pos;
    if (int r = reserve(dpp, c.index); r < 0) {
      return r;
    }
    const rgw_raw_obj raw = FSIOGeometry::named(c, info.bucket.marker, wcs);
    neorados::IOContext ioc;
    if (int r = ioc_for(dpp, raw, &ioc); r < 0) {
      return r;
    }
    ceph::buffer::list piece;
    piece.substr_of(all, pos - ofs, len);
    neorados::WriteOp op;
    op.exec(::cls::rgw::method::obj_check_attrs_prefix, fence_bl);
    op.write(pos - c.ofs, std::move(piece));
    int r = exec(raw.oid, ioc, std::move(op));
    if (r == -ECANCELED) {
      /* §3.7:  the chunk write fence:  a fenced finalize took the shadow
       * over, and no retry can land this write */
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  chunk " << c.index << " ("
			<< raw.oid << ") of " << wcs.to_str()
			<< " is fenced" << dendl;
      return -EIO;
    }
    if (r < 0) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  writing chunk " << c.index << " ("
			<< raw.oid << ") of " << wcs.to_str() << ":  "
			<< cpp_strerror(-r) << dendl;
      return r;
    }
    pos += len;
    hwm = std::max(hwm, pos);
    *bytes_written = pos - ofs;
  }
  dirty = true;
  wmtime = ceph::real_clock::now();
  /* §7:  periodically only while another handle watches */
  const auto interval = dpp->get_cct()->_conf.get_val<std::chrono::seconds>(
    "rgw_fsio_hint_interval");
  if (others && ceph::mono_clock::now() - last_report >= interval) {
    if (int r = report_hint(dpp); r < 0) {
      ldpp_dout(dpp, 1) << "FSIO:  a periodic size report on " << head_oid
			<< " failed:  " << cpp_strerror(-r) << dendl;
    }
  }
  return 0;
}

int RadosFSIOObject::report_hint(const DoutPrefixProvider* dpp)
{
  if (!writing) {
    return -EBADF;
  }
  static constexpr int tries = 3;	// each stale reply needs a truncate between
  for (int t = 0; t < tries; ++t) {
    cls_rgw_fsio_hint_size_op in;
    in.caller = h;
    in.cs = wcs;
    in.trunc_epoch = trunc_epoch;
    in.size = hwm;
    in.mtime = wmtime;
    cls_rgw_fsio_hint_size_ret ret;
    neorados::WriteOp op;
    op.exec(neorados::cls::rgw_fsio::hint(in, &ret));
    if (int r = exec(head_oid, head_ioc, std::move(op)); r < 0) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_hint of " << wcs.to_str()
			<< " on " << head_oid << ":  " << cpp_strerror(-r)
			<< dendl;
      return r;
    }
    trunc_epoch = ret.trunc_epoch;
    others = ret.others;
    if (!ret.stale) {
      wsize = ret.size;
      dirty = false;
      unsure = false;
      last_report = ceph::mono_clock::now();
      return 0;
    }
    /* §2.7:  a truncate landed since its last report, and it cannot tell
     * which of its writes came after it:  the chunk at its high-water mark
     * says;  what the truncate cut is not reported */
    uint64_t end = ret.size;
    if (hwm > 0) {
      const FSIOChunk c = wgeo.locate(dpp, hwm - 1);
      const rgw_raw_obj raw = FSIOGeometry::named(c, info.bucket.marker, wcs);
      neorados::IOContext ioc;
      if (int r = ioc_for(dpp, raw, &ioc); r < 0) {
	return r;
      }
      uint64_t len = 0;
      neorados::ReadOp sop;
      sop.stat(&len, nullptr);
      sys::error_code ec;
      store->get_neorados().execute(neorados::Object{raw.oid}, ioc,
				    std::move(sop), nullptr,
				    ceph::async::use_blocked[ec]);
      if (ec && ec != sys::errc::no_such_file_or_directory) {
	return ceph::from_error_code(ec);
      }
      if (!ec) {
	end = std::max(end, c.ofs + len);
      }
    }
    ldpp_dout(dpp, 10) << "FSIO:  " << wcs.to_str() << " truncated since its "
		       << "last report:  rebased from " << hwm << " to " << end
		       << " at epoch " << trunc_epoch << dendl;
    hwm = end;
    wsize = ret.size;
  }
  return -EAGAIN;
}

int RadosFSIOObject::open(const DoutPrefixProvider* dpp, bool writer,
			  const rgw_owner* who)
{
  if (writer && who) {
    identity = to_string(*who);
  }
  /* §4.3:  the hint first, so the expirer can always reach the state the
   * join records */
  if (!hinted) {
    if (int r = presence_hint(dpp, true, false); r < 0) {
      return r;
    }
    hinted = true;
  }
  if (!ndp) {
    ndp.emplace(dpp->get_cct(), dout_subsys);
  }
  Source src;
  FSIOGeometry g;
  cls_rgw_fsio_join_ret ret;
  if (int r = join(dpp, writer, &src, &g, &ret); r < 0) {
    if (!reading && !writing) {
      uncount(dpp);
      presence_hint(dpp, false, false);
      hinted = false;
    }
    return r;
  }
  if (!writer) {
    cs = ret.bound;
    rgeo = g;
    rmtime = src.mtime;
    seg_oid = src.oid;
    seg_tag = src.id_tag;
    seg_moved.reset();
    reading = true;
    if (!writing) {
      binding = Binding::PUBLISHED;
    }
    return 0;
  }
  wcs = ret.bound;
  wgeo = g;
  wsize = ret.size_hint;
  known.clear();
  if (ret.last_run) {
    know(*ret.last_run);
  }
  trunc_epoch = ret.trunc_epoch;
  others = ret.others;
  dirty = false;
  unsure = false;
  wmtime_hint = ret.mtime_hint;
  writing = true;
  binding = Binding::SHADOW;
  /* Z24:  a writer joined a name with no S3 object:  a created head */
  const bool created = !counted && !src.id_tag.length() &&
    !src.manifest.length();
  if (ret.construct) {
    int r = created ? construct_created(dpp, wcs) : construct(dpp, src, wcs);
    if (r < 0) {
      return abandon(dpp, src);
    }
  } else if (created) {
    /* another create's shadow:  its layout is its attribute clone's */
    ceph::buffer::list pm;
    if (int r = shadow_pm(dpp, wcs, &pm); r < 0) {
      return r;
    }
    if (int r = make_geometry(dpp, pm, &wgeo); r < 0) {
      return r;
    }
  }
  return 0;
}

void RadosFSIOObject::set_create(uint8_t mode, const Object::FSIOCreateSpec* spec)
{
  create = mode;
  if (!spec) {
    return;
  }
  if (spec->attrs) {
    create_attrs.insert(spec->attrs->begin(), spec->attrs->end());
  }
  if (spec->times && spec->times[1].tv_nsec != UTIME_OMIT) {
    wmtime = ceph::real_clock::from_timespec(spec->times[1]);
  }
}

int RadosFSIOObject::fresh_layout(const DoutPrefixProvider* dpp,
				  ceph::buffer::list* pm)
{
  auto* rados = store->getRados();
  const rgw_obj obj(info.bucket, key);
  rgw_pool pool;
  if (!rados->get_obj_data_pool(info.placement_rule, obj, &pool)) {
    return -EIO;
  }
  uint64_t chunk = 0, alignment = 0;
  if (int r = rados->get_max_chunk_size(pool, &chunk, dpp, &alignment); r < 0) {
    return r;
  }
  uint64_t stripe = 0;
  rados->get_max_aligned_size(dpp->get_cct()->_conf->rgw_obj_stripe_size,
			      alignment, &stripe);
  RGWObjManifest m;
  m.set_trivial_rule(0, stripe);
  RGWObjManifest::generator gen;
  if (int r = gen.create_begin(dpp->get_cct(), &m, info.placement_rule,
			       nullptr, info.bucket, obj); r < 0) {
    return r;
  }
  m.set_obj_size(0);
  encode(m, *pm);
  return 0;
}

int RadosFSIOObject::construct_created(const DoutPrefixProvider* dpp,
				       const cls_rgw_fsio_id& id)
{
  ceph::buffer::list pm;
  if (int r = fresh_layout(dpp, &pm); r < 0) {
    return r;
  }
  if (int r = make_geometry(dpp, pm, &wgeo); r < 0) {
    return r;
  }
  /* §3.1 step 7:  role 'a', attributes only;  EEXIST is a resend's */
  neorados::WriteOp op;
  op.create(true);
  op.setxattr("user.rgw.fsio.pm", pm);
  for (const auto& [k, v] : create_attrs) {
    op.setxattr(k, v);
  }
  auto aioc = head_ioc;
  aioc.set_key(head_oid);
  const auto attr_oid = cls_rgw_fsio_clone_name(info.bucket.marker, 'a', id,
						head_oid);
  if (int r = exec(attr_oid, aioc, std::move(op)); r < 0 && r != -EEXIST) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  writing " << attr_oid << ":  "
		      << cpp_strerror(-r) << dendl;
    return r;
  }
  cls_rgw_fsio_runs_op in;
  in.caller = h;
  in.cs = id;
  in.size = 0;
  in.mtime = ceph::real_clock::is_zero(wmtime) ? ceph::real_clock::now()
					       : wmtime;
  neorados::WriteOp rop;
  rop.exec(neorados::cls::rgw_fsio::ready(in));
  if (int r = exec(head_oid, head_ioc, std::move(rop)); r < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_ready of " << id.to_str()
		      << " on " << head_oid << ":  " << cpp_strerror(-r) << dendl;
    return r;
  }
  wsize = 0;
  return 0;
}

int RadosFSIOObject::shadow_pm(const DoutPrefixProvider* dpp,
			       const cls_rgw_fsio_id& id,
			       ceph::buffer::list* pm)
{
  auto aioc = head_ioc;
  aioc.set_key(head_oid);
  for (const char role : {'d', 'a'}) {
    sys::error_code ec;
    store->get_neorados().execute(
      neorados::Object{cls_rgw_fsio_clone_name(info.bucket.marker, role, id,
					       head_oid)},
      aioc, neorados::ReadOp{}.get_xattr("user.rgw.fsio.pm", pm), nullptr,
      ceph::async::use_blocked[ec]);
    if (!ec) {
      return 0;
    }
  }
  ldpp_dout(dpp, 0) << "ERROR: FSIO:  no attribute clone for " << id.to_str()
		    << " on " << head_oid << dendl;
  return -EIO;
}

int RadosFSIOObject::abandon(const DoutPrefixProvider* dpp, const Source& src)
{
  /* §3.4, W15:  what the expirer does for a dead constructor:  every
   * chunk construction would have cloned, the clones made so far among
   * them (ENOENT for the rest), and the attribute clone;  construction's
   * pseudo-manifest is the source's */
  const uint64_t last = wgeo.locate(dpp, std::max<uint64_t>(wgeo.size(), 1) - 1).index;
  collect_shadow(dpp, wcs, {cls_rgw_fsio_run{0, last}}, &src.manifest);
  cls_rgw_fsio_leave_ret ret;
  int r = leave(dpp, wcs, true, &ret);
  writing = false;
  if (r == 0 && !ret.abandoned) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_leave of the constructing "
		      << wcs.to_str() << " did not abandon it" << dendl;
  }
  ldpp_dout(dpp, 1) << "FSIO:  construction of " << wcs.to_str() << " on "
		    << head_oid << " failed;  abandoned" << dendl;
  if (!reading) {
    uncount(dpp);
    presence_hint(dpp, false, false);
    hinted = false;
  }
  return -EAGAIN;
}

int RadosFSIOObject::begin_write(const DoutPrefixProvider* dpp, uint32_t flags,
				 const rgw_owner* writer)
{
  if (writing) {
    /* §3.5 item 6:  another write open, another identity:  recorded at
     * once, not sequenced (idempotent by its effect) */
    if (writer && to_string(*writer) != identity) {
      identity = to_string(*writer);
      neorados::WriteOp op;
      op.exec(neorados::cls::rgw_fsio::writer_identity(
		cls_rgw_fsio_identity_op{h, wcs, identity}));
      if (int r = exec(head_oid, head_ioc, std::move(op)); r < 0) {
	ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_writer_identity on "
			  << head_oid << ":  " << cpp_strerror(-r) << dendl;
	return r;
      }
    }
    return 0;
  }
  if (int r = open(dpp, true, writer); r < 0) {
    return r;
  }
  if (!reading || cs == wcs) {
    return 0;
  }
  /* §3.4, fsio_rebind:  its reader follows its own shadow, so the head
   * knows which chunk set each open reads */
  cls_rgw_fsio_rebind_ret ret;
  int r = sequenced(dpp, [&] (neorados::WriteOp& op) {
    cls_rgw_fsio_rebind_op in;
    in.caller = h;
    in.seq = seq + 1;
    in.from = cs;
    in.to = wcs;
    in.readers = 1;
    op.exec(neorados::cls::rgw_fsio::rebind(in, &ret));
  });
  if (r < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_rebind of " << cs.to_str()
		      << " to " << wcs.to_str() << ":  " << cpp_strerror(-r)
		      << dendl;
    return 0;			// the reader stays where it was
  }
  cs = wcs;
  reader_on_shadow = true;
  if (ret.drained) {
    return drain(dpp, *ret.drained);
  }
  return 0;
}

int RadosFSIOObject::leave(const DoutPrefixProvider* dpp,
			   const cls_rgw_fsio_id& id, bool writer,
			   cls_rgw_fsio_leave_ret* ret,
			   const cls_rgw_fsio_intent* intent, bool handoff,
			   bool discard)
{
  int r = sequenced(dpp, [&] (neorados::WriteOp& op) {
    cls_rgw_fsio_leave_op in;
    in.caller = h;
    in.seq = seq + 1;
    in.cs = id;
    in.writer = writer;
    in.handoff = handoff;
    in.discard = discard;
    if (intent) {
      in.intent = *intent;	// §4.2 step 2:  the claim, the epoch returned
    }
    op.exec(neorados::cls::rgw_fsio::leave(in, ret));
  });
  if (r < 0 && r != -EINVAL) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_leave as "
		      << (writer ? "writer" : "reader") << " of " << head_oid
		      << " by " << h.incarnation << "." << h.handle << ":  "
		      << cpp_strerror(-r) << dendl;
  }
  return r;
}

int RadosFSIOObject::keeps_hint(const DoutPrefixProvider* dpp, bool* keep)
{
  *keep = false;
  ceph::buffer::list bl;
  neorados::ReadOp op;
  op.get_xattr(fmt::format("user.rgw.fsio.m.{:016x}", h.incarnation), &bl,
	       nullptr);
  sys::error_code ec;
  store->get_neorados().execute(neorados::Object{head_oid}, head_ioc,
				std::move(op), nullptr,
				ceph::async::use_blocked[ec]);
  if (ec) {
    /* no record:  nothing of the incarnation's is left on the head */
    return (ec == sys::errc::no_such_file_or_directory ||
	    ec == sys::errc::no_message_available) ? 0
						   : ceph::from_error_code(ec);
  }
  cls_rgw_fsio_opens rec;
  try {
    auto it = bl.cbegin();
    decode(rec, it);
  } catch (const ceph::buffer::error&) {
    return -EIO;
  }
  /* opens it no longer holds still counted, or its last sequence number
   * kept by expiry with none counted:  others are taking its state over
   * while its incarnation is presumed dead */
  if (auto o = rec.handles.find(h.handle); o != rec.handles.end()) {
    for (const auto& [id, c] : o->second.chunk_sets) {
      if ((c.readers && !reading) || (c.writers && !writing)) {
	*keep = true;
      }
    }
  }
  if (rec.seqs.count(h.handle)) {
    *keep = true;
  }
  return 0;
}

int RadosFSIOObject::collect_shadow(const DoutPrefixProvider* dpp,
				    const cls_rgw_fsio_id& id,
				    const std::vector<cls_rgw_fsio_run>& extent,
				    const ceph::buffer::list* given,
				    const std::string& version)
{
  if (given && given->length()) {
    return store->getRados()->send_manifest_to_gc(
      dpp, *given, rgw_obj(info.bucket, key),
      cls_rgw_fsio_gc_shadow{id, extent}, id.to_str(), null_yield);
  }
  /* its pseudo-manifest, on its attribute clone:  the head's data chunk
   * when the head holds data, else the attributes-only clone (§2.4) */
  const auto& marker = info.bucket.marker;
  std::string base = head_oid;
  auto aioc = head_ioc;
  if (!is_null(version)) {
    if (int r = instance_obj(version, &base, &aioc); r < 0) {
      return r;
    }
  }
  aioc.set_key(base);
  ceph::buffer::list pm;
  sys::error_code ec;
  for (const char role : {'d', 'a'}) {
    neorados::ReadOp op;
    op.get_xattr("user.rgw.fsio.pm", &pm, nullptr);
    ec.clear();
    store->get_neorados().execute(
      neorados::Object{cls_rgw_fsio_clone_name(marker, role, id, base)},
      aioc, std::move(op), nullptr, ceph::async::use_blocked[ec]);
    if (!ec) {
      break;
    }
  }
  if (ec || !pm.length()) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  no pseudo-manifest for "
		      << id.to_str() << " on " << base << dendl;
    return -EIO;
  }
  return store->getRados()->send_manifest_to_gc(
    dpp, pm, rgw_obj(info.bucket, key), cls_rgw_fsio_gc_shadow{id, extent},
    id.to_str(), null_yield);
}

int RadosFSIOObject::drain(const DoutPrefixProvider* dpp, cls_rgw_fsio_id id)
{
  const auto& marker = info.bucket.marker;
  for (;;) {
    cls_rgw_fsio_hdr hdr;
    if (int r = read_header(dpp, &hdr); r < 0) {
      return r;
    }
    const cls_rgw_fsio_chunk_set* e = nullptr;
    for (const auto& c : hdr.chunk_sets) {
      if (c.id == id) {
	e = &c;
      }
    }
    if (e && e->manifest_at == cls_rgw_fsio_manifest_at::CLONE && e->clone) {
      /* §2.3:  the generation's record, then its leaf clone */
      if (int r = collect_leaf(dpp, *e->clone); r < 0) {
	return r;
      }
    } else if (e && e->manifest_at == cls_rgw_fsio_manifest_at::SHADOW &&
	       e->shadow) {
      if (int r = collect_shadow(dpp, id, e->shadow->extent, nullptr,
				 e->shadow->base); r < 0) {
	return r;
      }
    }
    /* INSTANCE:  its tail is a live version's (§2.5);  none, nothing */
    cls_rgw_fsio_forget_ret ret;
    neorados::WriteOp op;
    op.exec(neorados::cls::rgw_fsio::forget(cls_rgw_fsio_cs_op{h, id}, &ret));
    if (int r = exec(head_oid, head_ioc, std::move(op)); r < 0) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_forget of " << id.to_str()
			<< " on " << head_oid << ":  " << cpp_strerror(-r)
			<< dendl;
      return r;
    }
    if (!ret.drained) {
      return 0;
    }
    id = *ret.drained;		// §3.6:  a shared tail's holder, drained now
  }
}

int RadosFSIOObject::drain_count(const DoutPrefixProvider* dpp)
{
  ceph::buffer::list bl;
  neorados::ReadOp op;
  op.get_xattr("user.rgw.fsio.ocnt", &bl, nullptr);
  sys::error_code ec;
  store->get_neorados().execute(neorados::Object{inst_oid}, inst_ioc,
				std::move(op), nullptr,
				ceph::async::use_blocked[ec]);
  if (ec) {
    return 0;			// gone with the version:  nothing to send
  }
  cls_rgw_fsio_ocnt oc;
  try {
    auto it = bl.cbegin();
    decode(oc, it);
  } catch (const ceph::buffer::error&) {
    return -EIO;
  }
  const rgw_obj inst_obj(info.bucket, rgw_obj_key(key.name, inst));
  rgw_raw_obj raw;
  store->getRados()->obj_to_raw(info.placement_rule, inst_obj, &raw);
  for (const auto& d : oc.deferred) {
    librados::IoCtx ioctx;
    if (int r = rgw_init_ioctx(dpp, store->getRados()->get_rados_handle(),
			       raw.pool, ioctx); r < 0) {
      return r;
    }
    ioctx.locator_set_key(inst_oid);
    store->getRados()->fsio_collect_replaced(
      dpp, ioctx,
      cls_rgw_fsio_clone_name(info.bucket.marker, 'G', d.clone, inst_oid),
      inst_obj, d.manifest, d.gc_tag, null_yield);
  }
  /* §3.4:  any failure counts as done */
  neorados::WriteOp fop;
  cls_rgw_fsio_ocnt_op in;
  in.holder = h;
  fop.exec(neorados::cls::rgw_fsio::ocnt_forget(in));
  (void)exec(inst_oid, inst_ioc, std::move(fop));
  return 0;
}

RadosFSIOObject::~RadosFSIOObject()
{
  if (!reading && !writing) {
    return;
  }
  const DoutPrefix dp(store->ctx(), dout_subsys, "rgw fsio: ");
  ldpp_dout_fmt(&dp, 0, "ERROR: {}: destroyed while open, closing {}",
		__func__, key);
  close(&dp, CLOSE_FLAG_NONE);
}

int RadosFSIOObject::close(const DoutPrefixProvider* dpp, uint32_t flags)
{
  int uncompleted = 0;
  if (writing) {
    const uint32_t wflags = (flags & CLOSE_FLAG_DISCARD) ? WRITE_FLAG_DISCARD
							  : WRITE_FLAG_NONE;
    if (int r = end_write(dpp, wflags); r == -EAGAIN) {
      /* §4.2, 8j:  nobody is left to retry this publish:  handed off */
      if (int e = handoff(dpp); e < 0) {
	return e;
      }
      uncompleted = r;
    } else if (r < 0) {
      return r;
    }
  }
  if (!reading) {
    return uncompleted;
  }
  cls_rgw_fsio_leave_ret ret;
  int r = leave(dpp, cs, false, &ret);
  if (r < 0 && r != -EINVAL) {
    return r;
  }
  reading = false;
  const bool lost = r == -EINVAL || ret.already_applied;
  /* §2.5, W7:  the version its content became, which a publisher's
   * instance write counted it on */
  if (r == 0 && !ret.instance.empty() && !is_null(ret.instance) &&
      ret.instance != inst) {
    if (int c = leave_count(dpp, ret.instance); c < 0) {
      return c;
    }
  }
  if (r == 0 && ret.drained) {
    if (int d = drain(dpp, *ret.drained); d < 0) {
      return d;
    }
  }
  /* §2.5:  off the instance's count once it holds no open bound to it */
  if (!writing) {
    if (int u = uncount(dpp); u < 0) {
      return u;
    }
  }
  bool keep = false;
  if (lost) {
    /* §3, Z22:  any work the head hands back first, then the hint goes
     * only if the head holds nothing of this handle's */
    cls_rgw_fsio_hdr hdr;
    if (read_header(dpp, &hdr) == 0) {
      for (const auto& c : hdr.chunk_sets) {
	if (c.drained) {
	  drain(dpp, c.id);
	}
      }
    }
    if (int k = keeps_hint(dpp, &keep); k < 0) {
      return k;
    }
  }
  if (keep || writing) {
    return uncompleted;
  }
  hinted = false;
  if (int p = presence_hint(dpp, false, false); p < 0) {
    return p;
  }
  return uncompleted;
}

int RadosFSIOObject::publish_hint(const DoutPrefixProvider* dpp, bool add,
				  const cls_rgw_fsio_id& r)
{
  cls_rgw_fsio_hint_op in;
  in.hint.kind = cls_rgw_fsio_hint_kind::PUBLISH;
  in.hint.target = cls_rgw_fsio_hint_target::HEAD;
  in.hint.bucket = info.bucket;
  in.hint.key = rgw_obj_key(key.name);
  in.hint.incarnation = h.incarnation;
  in.hint.handle = h.handle;
  in.hint.r = r;
  in.oid = head_oid;
  const auto shards = static_cast<uint32_t>(
    dpp->get_cct()->_conf.get_val<uint64_t>("rgw_fsio_open_hints_num_shards"));
  neorados::WriteOp op;
  if (add) {
    op.exec(neorados::cls::rgw_fsio::open_hint_add(in));
  } else {
    op.exec(neorados::cls::rgw_fsio::open_hint_remove(in));
  }
  int e = exec(rgw::fsio::open_hint_shard_oid(
		 rgw::fsio::open_hint_shard(in.oid, shards)),
	       store->getRados()->fsio_hint_ioc(), std::move(op));
  if (e < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  " << (add ? "adding" : "removing")
		      << " the publish hint for " << r.to_str() << " on "
		      << head_oid << ":  " << cpp_strerror(-e) << dendl;
  }
  return e;
}

int RadosFSIOObject::collect_leaf(const DoutPrefixProvider* dpp,
				  const cls_rgw_fsio_id& r)
{
  const auto leaf = cls_rgw_fsio_clone_name(info.bucket.marker, 'G', r,
					     head_oid);
  auto lioc = head_ioc;
  lioc.set_key(head_oid);
  ceph::buffer::list bl;
  neorados::ReadOp op;
  op.get_xattr("user.rgw.fsio.gen", &bl, nullptr);
  sys::error_code ec;
  store->get_neorados().execute(neorados::Object{leaf}, lioc, std::move(op),
				nullptr, ceph::async::use_blocked[ec]);
  cls_rgw_fsio_gen gen;
  if (!ec) {
    try {
      auto it = bl.cbegin();
      decode(gen, it);
    } catch (const ceph::buffer::error&) {
      return -EIO;
    }
  }
  rgw_raw_obj raw;
  store->getRados()->obj_to_raw(info.placement_rule,
				rgw_obj(info.bucket, key), &raw);
  librados::IoCtx ioctx;
  if (int e = rgw_init_ioctx(dpp, store->getRados()->get_rados_handle(),
			     raw.pool, ioctx); e < 0) {
    return e;
  }
  ioctx.locator_set_key(head_oid);
  return store->getRados()->fsio_collect_replaced(
    dpp, ioctx, leaf, rgw_obj(info.bucket, key), gen.manifest, gen.gc_tag,
    null_yield);
}

int RadosFSIOObject::scan_size(const DoutPrefixProvider* dpp, uint64_t* size)
{
  *size = 0;
  cls_rgw_fsio_hdr hdr;
  if (int r = read_header(dpp, &hdr); r < 0) {
    return r;
  }
  std::vector<cls_rgw_fsio_run> extent;
  for (const auto& e : hdr.chunk_sets) {
    if (e.id == wcs && e.shadow) {
      extent = e.shadow->extent;
    }
  }
  uint64_t last = 0;
  for (const auto& run : extent) {
    last = std::max(last, run.last);
  }
  std::vector<FSIOChunk> chunks;
  for (FSIOChunk c = wgeo.locate(dpp, 0); c.index <= last;
       c = wgeo.locate(dpp, c.ofs + c.capacity)) {
    for (const auto& run : extent) {
      if (run.first <= c.index && c.index <= run.last) {
	chunks.push_back(c);
	break;
      }
    }
  }
  for (auto c = chunks.rbegin(); c != chunks.rend(); ++c) {
    const rgw_raw_obj raw = FSIOGeometry::named(*c, info.bucket.marker, wcs);
    neorados::IOContext ioc;
    if (int r = ioc_for(dpp, raw, &ioc); r < 0) {
      return r;
    }
    uint64_t len = 0;
    sys::error_code ec;
    store->get_neorados().execute(neorados::Object{raw.oid}, ioc,
				  neorados::ReadOp{}.stat(&len, nullptr),
				  nullptr, ceph::async::use_blocked[ec]);
    if (ec == sys::errc::no_such_file_or_directory) {
      continue;			// never written, or truncated away
    }
    if (ec) {
      return ceph::from_error_code(ec);
    }
    if (len == 0) {
      /* an empty chunk may be a fenced hole (§3.7);  a separate read,
       * since a missing attribute fails the whole op (ENODATA) */
      ceph::buffer::list fence;
      sys::error_code fec;
      store->get_neorados().execute(
	neorados::Object{raw.oid}, ioc,
	neorados::ReadOp{}.get_xattr("user.rgw.fsio.fence", &fence), nullptr,
	ceph::async::use_blocked[fec]);
      if (!fec && fence.length()) {
	continue;
      }
    }
    *size = c->ofs + len;
    return 0;
  }
  return 0;
}

int RadosFSIOObject::attr_clone(
  const DoutPrefixProvider* dpp, std::string* oid, neorados::IOContext* ioc,
  boost::container::flat_map<std::string, ceph::buffer::list>* xattrs)
{
  const auto& marker = info.bucket.marker;
  const auto& base = counted ? inst_oid : head_oid;
  auto aioc = counted ? inst_ioc : head_ioc;
  aioc.set_key(base);
  for (const char role : {'d', 'a'}) {
    boost::container::flat_map<std::string, ceph::buffer::list> x;
    const auto name = cls_rgw_fsio_clone_name(marker, role, wcs, base);
    sys::error_code ec;
    store->get_neorados().execute(neorados::Object{name}, aioc,
				  neorados::ReadOp{}.get_xattrs(&x), nullptr,
				  ceph::async::use_blocked[ec]);
    if (!ec && x.count("user.rgw.fsio.pm")) {
      if (oid) {
	*oid = name;
      }
      if (ioc) {
	*ioc = aioc;
      }
      if (xattrs) {
	*xattrs = std::move(x);
      }
      return 0;
    }
  }
  ldpp_dout(dpp, 0) << "ERROR: FSIO:  no attribute clone for "
		    << wcs.to_str() << " on " << base << dendl;
  return -EIO;
}

namespace {
/* FSIO's own records, never a client's attribute (§2.6) */
bool fsio_record(const std::string& name)
{
  return name.starts_with("user.rgw.fsio.");
}
} // anonymous namespace

int RadosFSIOObject::fgetattrs(const DoutPrefixProvider* dpp, Attrs& attrs,
			       uint32_t flags)
{
  boost::container::flat_map<std::string, ceph::buffer::list> x;
  if (writing) {
    if (int r = attr_clone(dpp, nullptr, nullptr, &x); r < 0) {
      return r;
    }
  } else {
    /* the content it is bound to:  the version it counted, else the head */
    sys::error_code ec;
    store->get_neorados().execute(
      neorados::Object{counted ? inst_oid : head_oid},
      counted ? inst_ioc : head_ioc, neorados::ReadOp{}.get_xattrs(&x),
      nullptr, ceph::async::use_blocked[ec]);
    if (ec) {
      return ceph::from_error_code(ec);
    }
  }
  attrs.clear();
  for (auto& [k, v] : x) {
    if (!fsio_record(k)) {
      attrs[k] = std::move(v);
    }
  }
  return 0;
}

int RadosFSIOObject::fgetattr(const DoutPrefixProvider* dpp,
			      const std::string& name, buffer::list& dest,
			      uint32_t flags)
{
  if (fsio_record(name)) {
    return -ENODATA;
  }
  Attrs attrs;
  if (int r = fgetattrs(dpp, attrs, flags); r < 0) {
    return r;
  }
  auto i = attrs.find(name);
  if (i == attrs.end()) {
    return -ENODATA;
  }
  dest = i->second;
  return 0;
}

int RadosFSIOObject::fsetattrs(const DoutPrefixProvider* dpp, Attrs& attrs,
			       uint32_t flags)
{
  if (!writing) {
    return -EBADF;
  }
  for (const auto& [k, v] : attrs) {
    if (fsio_record(k)) {
      return -EINVAL;
    }
  }
  std::string oid;
  neorados::IOContext ioc;
  if (int r = attr_clone(dpp, &oid, &ioc, nullptr); r < 0) {
    return r;
  }
  neorados::WriteOp op;
  for (const auto& [k, v] : attrs) {
    op.setxattr(k, ceph::buffer::list(v));
  }
  return exec(oid, ioc, std::move(op));
}

int RadosFSIOObject::fsetattr(const DoutPrefixProvider* dpp,
			      const std::string& name,
			      const buffer::list& val, uint32_t flags)
{
  Attrs attrs;
  attrs[name] = val;
  return fsetattrs(dpp, attrs, flags);
}

int RadosFSIOObject::fremovexattr(const DoutPrefixProvider* dpp,
				  const std::string& name, uint32_t flags)
{
  if (!writing) {
    return -EBADF;
  }
  if (fsio_record(name)) {
    return -EINVAL;
  }
  std::string oid;
  neorados::IOContext ioc;
  if (int r = attr_clone(dpp, &oid, &ioc, nullptr); r < 0) {
    return r;
  }
  neorados::WriteOp op;
  op.rmxattr(name);
  return exec(oid, ioc, std::move(op));
}

int RadosFSIOObject::prepare_publish(const DoutPrefixProvider* dpp,
				     Prepared* p)
{
  const auto& marker = info.bucket.marker;
  /* 2:  the size from the chunks, final while the shadow is publishing;
   * the attributes and the pseudo-manifest from its attribute clone, the
   * source instance's in a versioned bucket (§4.1) */
  uint64_t size = 0;
  if (int e = scan_size(dpp, &size); e < 0) {
    return e;
  }
  boost::container::flat_map<std::string, ceph::buffer::list> xattrs;
  if (int r = attr_clone(dpp, nullptr, nullptr, &xattrs); r < 0) {
    return r;
  }
  /* 3:  the manifest:  the pseudo-manifest under the shadow's id */
  RGWObjManifest& m = p->m;
  try {
    auto it = xattrs["user.rgw.fsio.pm"].cbegin();
    decode(m, it);
  } catch (const ceph::buffer::error&) {
    return -EIO;
  }
  m.set_fsio(wcs);
  m.set_obj_size(size);
  p->size = size;
  /* the publishing identity:  the publishing record's, which the freeze
   * copied from the freezing handle's write open (§3.5, Z14, Y16) */
  std::string who;
  {
    cls_rgw_fsio_hdr hdr;
    if (int e = read_header(dpp, &hdr); e < 0) {
      return e;
    }
    for (const auto& e : hdr.chunk_sets) {
      if (e.id == wcs && e.publishing) {
	who = e.publishing->identity;
      }
    }
  }
  /* the bucket as it is now:  its owner and its ownership setting */
  std::unique_ptr<rgw::sal::Bucket> bucket;
  if (int e = store->load_bucket(dpp, info.bucket, &bucket, null_yield);
      e < 0) {
    return e;
  }
  p->bowner = bucket->get_info().owner;
  /* the owner and ACL a PUT by the publishing identity without ACL
   * headers gets:  the bucket's ownership setting decides (§3.5, X14) */
  ACLOwner writer, bowner;
  writer.id = who.empty() ? p->bowner : parse_owner(who);
  bowner.id = p->bowner;
  std::string why;
  if (int e = rgw::s3::create_canned_acl(
	writer, bowner, rgw::s3::get_object_ownership(bucket->get_attrs()),
	"", p->policy, why); e < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  the publish's ACL for " << head_oid
		      << ":  " << why << dendl;
    return e;
  }
  /* the attribute set:  the clone's, as copy_obj() handles a source's,
   * less FSIO's records, the tags (fsio_replace_head or the instance
   * write writes them, C1, Z15), and what names or derives from the
   * replaced content (Z26;  §6.1:  a publish is uncompressed) */
  static const std::set<std::string> dropped{
    RGW_ATTR_MANIFEST, RGW_ATTR_ID_TAG, RGW_ATTR_TAIL_TAG, "refcount",
    RGW_ATTR_PG_VER, RGW_ATTR_SOURCE_ZONE, RGW_ATTR_TAGS,
    RGW_ATTR_ETAG, RGW_ATTR_CKSUM, RGW_ATTR_BLAKE3, RGW_ATTR_SHARE_MANIFEST,
    RGW_ATTR_COMPRESSION,
    RGW_ATTR_CLOUD_TIER_TYPE, RGW_ATTR_RESTORE_STATUS, RGW_ATTR_RESTORE_TYPE,
    RGW_ATTR_RESTORE_TIME, RGW_ATTR_RESTORE_EXPIRY_DATE,
    RGW_ATTR_RESTORE_VERSIONED_EPOCH};
  auto& attrs = p->attrs;
  for (auto& [k, v] : xattrs) {
    if (k.starts_with("user.rgw.fsio.") || dropped.count(k)) {
      continue;
    }
    attrs[k] = v;
  }
  RGWRados::erase_copy_source_attrs(attrs,
				    bucket->get_info().versioning_enabled());
  /* the tags the content had, for a tracking-only head (Z15) */
  if (auto t = xattrs.find(RGW_ATTR_TAGS); t != xattrs.end()) {
    p->clone_tags = t->second;
  }
  if (int e = store->get_sync_policy_handler(dpp, std::nullopt, info.bucket,
					     &p->sync, null_yield); e < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  the sync policy for " << info.bucket
		      << ":  " << cpp_strerror(-e) << dendl;
    return e;
  }
  p->policy.encode(attrs[RGW_ATTR_ACL]);
  p->mtime = std::max(wmtime_hint, wmtime);
  if (ceph::real_clock::is_zero(p->mtime)) {
    p->mtime = ceph::real_clock::now();
  }
  /* the ETag by the SAL's rule (§3.5, E1):  the content's MD5, read
   * through this handle up to the scanned size;  under rgw_non_md5_etag,
   * RGW's opaque form with the shadow's id */
  std::string etag;
  if (dpp->get_cct()->_conf->rgw_non_md5_etag) {
    const auto ts = ceph::real_clock::to_timespec(p->mtime);
    etag = rgw_make_opaque_etag(
      wcs.to_str(), uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec);
  } else {
    wsize = size;
    if (int e = rgw::sal::fsio_content_md5(dpp, *this, size, &etag); e < 0) {
      return e;
    }
  }
  attrs[RGW_ATTR_ETAG].append(etag);
  return 0;
}

/* the sync policy's decision for tags as they will be kept (§3.5, C1) */
int RadosFSIOObject::replication_status(const DoutPrefixProvider* dpp,
					const Prepared& p,
					const ceph::buffer::list& tags,
					std::map<std::string, ceph::buffer::list>* attrs)
{
  RGWObjTags obj_tags;
  if (tags.length()) {
    try {
      auto ti = tags.cbegin();
      obj_tags.decode(ti);
    } catch (const ceph::buffer::error&) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  undecodable tags on " << head_oid
			<< dendl;
      return -EIO;
    }
  }
  attrs->erase(RGW_ATTR_OBJ_REPLICATION_STATUS);
  if (p.sync && p.sync->bucket_exports_object(key.name, obj_tags)) {
    (*attrs)[RGW_ATTR_OBJ_REPLICATION_STATUS].append("PENDING");
  }
  return 0;
}

int RadosFSIOObject::publish(const DoutPrefixProvider* dpp)
{
  /* 1:  the hint first, naming the leaf clone the replaced content will
   * be kept under (§3.5) */
  const cls_rgw_fsio_id r{ceph::util::generate_random_number<uint64_t>(),
			  ceph::util::generate_random_number<uint64_t>()};
  if (int e = publish_hint(dpp, true, r); e < 0) {
    return e;
  }
  Prepared p;
  if (int e = prepare_publish(dpp, &p); e < 0) {
    publish_hint(dpp, false, r);
    return e;
  }
  auto& m = p.m;
  auto& attrs = p.attrs;
  const auto size = p.size;
  auto mtime = p.mtime;
  const auto& tags = p.clone_tags;
  const auto& policy = p.policy;
  const auto& bowner_id = p.bowner;
  const auto& marker = info.bucket.marker;
  auto aioc = head_ioc;
  aioc.set_key(head_oid);
  const auto until = ceph::mono_clock::now() +
    dpp->get_cct()->_conf.get_val<std::chrono::seconds>("rgw_fsio_publish_wait");
  /* 4:  the head write, atomic so it sets the ID tag and carries the
   * guard;  ECANCELED is an S3 write since RGW read the head, or a tag
   * write since this try read the tags:  read again and publish over it,
   * last writer wins (§3.5) */
  fsio_publish_spec spec{h, wcs, tags, r};
  cls_rgw_fsio_replace_ret ret;
  int w = -ECANCELED;
  for (int t = 0; t < 3 && w == -ECANCELED; ++t) {
    /* §3.5, C1:  the tags the publish will keep, by the CLS's rule, so
     * the replication status is decided on them;  the head write fails
     * with ECANCELED if they changed */
    {
      boost::container::flat_map<std::string, ceph::buffer::list> ha;
      sys::error_code ec;
      store->get_neorados().execute(
	neorados::Object{head_oid}, head_ioc,
	neorados::ReadOp{}.get_xattrs(&ha), nullptr,
	ceph::async::use_blocked[ec]);
      if (ec) {
	publish_hint(dpp, false, r);
	return ceph::from_error_code(ec);
      }
      const auto it = ha.find(RGW_ATTR_ID_TAG);
      const auto ht = ha.find(RGW_ATTR_TAGS);
      spec.expected_tags = fsio_publish_tags(
	it != ha.end() && it->second.length(),
	ht != ha.end() ? ht->second : ceph::buffer::list{}, tags);
    }
    if (int e = replication_status(dpp, p, *spec.expected_tags, &attrs);
	e < 0) {
      publish_hint(dpp, false, r);
      return e;
    }
    if (tags_read) {
      tags_read(*spec.expected_tags);
    }
    w = publish_wait(dpp, until, [&] {
      RGWObjectCtx octx(store);
      const rgw_obj obj(info.bucket, key);
      octx.set_atomic(obj, true);
      RGWRados::Object target(store->getRados(), info, octx, obj);
      RGWRados::Object::Write wr(&target);
      const std::string tag = gen_rand_alphanumeric(dpp->get_cct(), 32);
      wr.meta.manifest = &m;
      wr.meta.flags = PUT_OBJ_CREATE;
      /* as RGW's PUT:  in a suspended bucket the head write links the
       * null version, over a delete marker too (§2.5a, 8i);  in an
       * unversioned one it is no versioned op */
      wr.meta.olh_epoch = 0;
      wr.meta.mtime = &mtime;
      wr.meta.ptag = &tag;
      wr.meta.owner = policy.get_owner();
      wr.meta.bucket_owner = bowner_id;
      wr.meta.fsio_publish = &spec;
      wr.meta.fsio_published = &ret;
      req_context rctx{dpp, null_yield, nullptr};
      jspan_context trace(false, false);
      int r = wr.write_meta(size, size, attrs, rctx, trace);
      if (r == 0 && wr.meta.canceled) {
	/* RGW returns a head write its op failed with ECANCELED as a lost
	 * race, 0;  for the publish it did not apply */
	r = -ECANCELED;
      }
      return r;
    });
  }
  if (w == 0 && lose_reply) {
    w = -ETIMEDOUT;		// test:  applied, its reply lost
  }
  if (w == -ETIMEDOUT || w == -ECONNRESET) {
    /* 5, W21:  no reply to read:  decide as the expirer would for this
     * hint.  A chunk-set entry naming r owns the replaced content;  else
     * a leaf clone f(head, r) that exists was born drained */
    cls_rgw_fsio_hdr hdr;
    bool owned = false;
    if (read_header(dpp, &hdr) == 0) {
      for (const auto& e : hdr.chunk_sets) {
	owned = owned || (e.clone && *e.clone == r);
      }
    }
    if (!owned) {
      uint64_t len = 0;
      sys::error_code ec;
      store->get_neorados().execute(
	neorados::Object{cls_rgw_fsio_clone_name(marker, 'G', r, head_oid)},
	aioc, neorados::ReadOp{}.stat(&len, nullptr), nullptr,
	ceph::async::use_blocked[ec]);
      if (!ec) {
	collect_leaf(dpp, r);
      }
    }
    /* whether it published:  the head's manifest names this shadow */
    ceph::buffer::list mbl;
    sys::error_code ec;
    store->get_neorados().execute(
      neorados::Object{head_oid}, head_ioc,
      neorados::ReadOp{}.get_xattr(RGW_ATTR_MANIFEST, &mbl), nullptr,
      ceph::async::use_blocked[ec]);
    bool published = false;
    if (!ec) {
      try {
	RGWObjManifest hm;
	auto it = mbl.cbegin();
	decode(hm, it);
	const auto& rules = hm.get_rules();
	published = !rules.empty() && rules.begin()->second.fsio == wcs;
      } catch (const ceph::buffer::error&) {}
    }
    publish_hint(dpp, false, r);
    ldpp_dout(dpp, 1) << "FSIO:  the publish of " << wcs.to_str() << " on "
		      << head_oid << " had no reply:  "
		      << (published ? "published" : "not published") << dendl;
    return published ? 0 : -EAGAIN;
  }
  if (w < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  the publish of " << wcs.to_str()
		      << " on " << head_oid << ":  " << cpp_strerror(-w)
		      << dendl;
    publish_hint(dpp, false, r);
    /* the tries or the wait spent:  the caller may try again (W13) */
    return (w == -ECANCELED || w == -ERR_BUSY_RESHARDING || w == -EAGAIN)
      ? -EAGAIN : w;
  }
  /* 5:  the replaced content's GC after the reply, when it was born
   * drained;  a retired chunk-set entry owns it otherwise (§2.2) */
  if (ret.born_drained) {
    collect_leaf(dpp, r);
  }
  if (ret.drained) {
    drain(dpp, *ret.drained);
  }
  publish_hint(dpp, false, r);
  return 0;
}

int RadosFSIOObject::instance_obj(const std::string& instance,
				  std::string* oid, neorados::IOContext* ioc)
{
  rgw_raw_obj raw;
  store->getRados()->obj_to_raw(info.placement_rule,
				rgw_obj(info.bucket, rgw_obj_key(key.name, instance)),
				&raw);
  *oid = raw.oid;
  try {
    *ioc = rgw::init_iocontext(&*ndp, store->get_neorados(), raw.pool,
			       ceph::async::use_blocked);
  } catch (const sys::system_error& e) {
    return ceph::from_error_code(e.code());
  }
  return 0;
}

int RadosFSIOObject::instance_hint(const DoutPrefixProvider* dpp, bool add,
				   cls_rgw_fsio_hint_kind kind,
				   const std::string& instance,
				   const cls_rgw_fsio_handle& who)
{
  cls_rgw_fsio_hint_op in;
  in.hint.kind = kind;
  in.hint.target = cls_rgw_fsio_hint_target::INSTANCE_HEAD;
  in.hint.bucket = info.bucket;
  in.hint.key = rgw_obj_key(key.name, instance);
  in.hint.incarnation = who.incarnation;
  in.hint.handle = who.handle;
  neorados::IOContext ioc;
  if (int r = instance_obj(instance, &in.oid, &ioc); r < 0) {
    return r;
  }
  const auto shards = static_cast<uint32_t>(
    dpp->get_cct()->_conf.get_val<uint64_t>("rgw_fsio_open_hints_num_shards"));
  neorados::WriteOp op;
  if (add) {
    op.exec(neorados::cls::rgw_fsio::open_hint_add(in));
  } else {
    op.exec(neorados::cls::rgw_fsio::open_hint_remove(in));
  }
  int r = exec(rgw::fsio::open_hint_shard_oid(
		 rgw::fsio::open_hint_shard(in.oid, shards)),
	       store->getRados()->fsio_hint_ioc(), std::move(op));
  if (r < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  " << (add ? "adding" : "removing")
		      << " a hint of " << who.incarnation << "." << who.handle
		      << " on " << in.oid << ":  " << cpp_strerror(-r) << dendl;
  }
  return r;
}

int RadosFSIOObject::shadow_readers(const DoutPrefixProvider* dpp,
				    const cls_rgw_fsio_id& id,
				    std::set<cls_rgw_fsio_handle>* readers)
{
  boost::container::flat_map<std::string, ceph::buffer::list> attrs;
  sys::error_code ec;
  store->get_neorados().execute(neorados::Object{head_oid}, head_ioc,
				neorados::ReadOp{}.get_xattrs(&attrs), nullptr,
				ceph::async::use_blocked[ec]);
  if (ec) {
    return ceph::from_error_code(ec);
  }
  static const std::string opens = "user.rgw.fsio.m.";
  for (const auto& [k, v] : attrs) {
    if (!k.starts_with(opens)) {
      continue;
    }
    cls_rgw_fsio_opens o;
    try {
      auto it = v.cbegin();
      decode(o, it);
    } catch (const ceph::buffer::error&) {
      return -EIO;
    }
    const auto inc = std::stoull(k.substr(opens.size()), nullptr, 16);
    for (const auto& [hdl, ho] : o.handles) {
      auto c = ho.chunk_sets.find(id);
      if (c != ho.chunk_sets.end() && c->second.readers) {
	readers->insert(cls_rgw_fsio_handle{inc, hdl});	// readers only (W7)
      }
    }
  }
  return 0;
}

int RadosFSIOObject::place(const DoutPrefixProvider* dpp,
			   cls_rgw_fsio_intent* intent)
{
  rgw_obj_key k(key.name);
  store->getRados()->gen_rand_obj_instance_name(&k);
  intent->instance = k.instance;
  intent->token = gen_rand_alphanumeric(dpp->get_cct(), 24);
  placed = intent->instance;
  if (int r = instance_hint(dpp, true, cls_rgw_fsio_hint_kind::PUBLISH,
			    intent->instance, h); r < 0) {
    return r;
  }
  std::string oid;
  neorados::IOContext ioc;
  int r = instance_obj(intent->instance, &oid, &ioc);
  if (r == 0) {
    /* §4.2 step 1, Z9:  absent to S3 (tracking-only), not in the index */
    ceph::buffer::list claim;
    claim.append(intent->token);
    neorados::WriteOp op;
    op.create(true);
    op.setxattr("user.rgw.fsio.claim", std::move(claim));
    r = exec(oid, ioc, std::move(op));
  }
  if (r < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  the placeholder for "
		      << intent->instance << " of " << head_oid << ":  "
		      << cpp_strerror(-r) << dendl;
    instance_hint(dpp, false, cls_rgw_fsio_hint_kind::PUBLISH,
		  intent->instance, h);
  }
  return r;
}

void RadosFSIOObject::unplace(const DoutPrefixProvider* dpp,
			      const cls_rgw_fsio_intent& intent,
			      const std::string& claim)
{
  std::string oid;
  neorados::IOContext ioc;
  if (instance_obj(intent.instance, &oid, &ioc) == 0) {
    ceph::buffer::list c;
    c.append(claim);
    neorados::WriteOp op;
    op.cmpxattr("user.rgw.fsio.claim", neorados::cmp_op::eq, c);
    op.remove();
    exec(oid, ioc, std::move(op));	// gone or not held:  nothing to do
  }
  instance_hint(dpp, false, cls_rgw_fsio_hint_kind::PUBLISH, intent.instance,
		h);
}

void RadosFSIOObject::cleanup_versioned(const DoutPrefixProvider* dpp,
					const cls_rgw_fsio_intent& intent,
					const std::set<cls_rgw_fsio_handle>& readers)
{
  /* V4:  while a chunk-set entry names the instance the placeholder is
   * the expirer's to resolve, and the publish hint stays (H1) */
  cls_rgw_fsio_hdr hdr;
  if (read_header(dpp, &hdr) == 0) {
    for (const auto& e : hdr.chunk_sets) {
      if ((e.instance && *e.instance == intent.instance) ||
	  (e.publishing && e.publishing->instance == intent.instance)) {
	return;
      }
    }
  }
  for (const auto& r : readers) {		// H3f:  first its readers' hints
    instance_hint(dpp, false, cls_rgw_fsio_hint_kind::PRESENCE,
		  intent.instance, r);
  }
  unplace(dpp, intent, "aborted");
}

int RadosFSIOObject::release_versioned(const DoutPrefixProvider* dpp,
				       const cls_rgw_fsio_intent& intent,
				       const std::set<cls_rgw_fsio_handle>& readers)
{
  std::string oid;
  neorados::IOContext ioc;
  if (int r = instance_obj(intent.instance, &oid, &ioc); r < 0) {
    return r;
  }
  /* W10:  its own claim aborted first, so no instance write can follow */
  ceph::buffer::list token, aborted;
  token.append(intent.token);
  aborted.append("aborted");
  neorados::WriteOp op;
  op.cmpxattr("user.rgw.fsio.claim", neorados::cmp_op::eq, token);
  op.setxattr("user.rgw.fsio.claim", std::move(aborted));
  if (int r = exec(oid, ioc, std::move(op)); r == 0) {
    neorados::WriteOp rop;
    rop.exec(neorados::cls::rgw_fsio::publish_release(
	       cls_rgw_fsio_cs_op{h, wcs}));
    if (int e = exec(head_oid, head_ioc, std::move(rop)); e < 0) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_publish_release of "
			<< wcs.to_str() << ":  " << cpp_strerror(-e) << dendl;
    }
  }
  /* aborted already:  the expirer owns the shadow, and this cleans up */
  cleanup_versioned(dpp, intent, readers);
  return 0;
}

int RadosFSIOObject::publish_versioned(const DoutPrefixProvider* dpp,
				       const cls_rgw_fsio_intent& intent,
				       uint64_t epoch)
{
  std::string ioid;
  neorados::IOContext iioc;
  if (int r = instance_obj(intent.instance, &ioid, &iioc); r < 0) {
    return r;
  }
  /* 3:  the readers bound to the shadow, each hinted on the new instance
   * head before the open count names it (H3) */
  std::set<cls_rgw_fsio_handle> readers;
  int e = shadow_readers(dpp, wcs, &readers);
  for (auto i = readers.begin(); e == 0 && i != readers.end(); ++i) {
    e = instance_hint(dpp, true, cls_rgw_fsio_hint_kind::PRESENCE,
		      intent.instance, *i);
  }
  Prepared p;
  if (e == 0) {
    e = prepare_publish(dpp, &p);
  }
  /* Z15:  the source version's tags at publish, else the clone's */
  ceph::buffer::list tags = p.clone_tags;
  if (e == 0 && counted) {
    boost::container::flat_map<std::string, ceph::buffer::list> sa;
    sys::error_code ec;
    store->get_neorados().execute(neorados::Object{inst_oid}, inst_ioc,
				  neorados::ReadOp{}.get_xattrs(&sa), nullptr,
				  ceph::async::use_blocked[ec]);
    if (!ec && sa.count(RGW_ATTR_ID_TAG)) {
      auto t = sa.find(RGW_ATTR_TAGS);
      tags = t == sa.end() ? ceph::buffer::list{} : t->second;
    }
  }
  if (e == 0) {
    e = replication_status(dpp, p, tags, &p.attrs);
  }
  if (e < 0) {
    release_versioned(dpp, intent, readers);
    return e;
  }
  /* 4:  the instance write is the publish:  guarded by the claim, its post
   * ops the claim's removal, the open count and the tags (§3, Z6) */
  fsio_instance_spec ispec;
  ispec.token = intent.token;
  ispec.tags = tags;
  if (!readers.empty()) {
    cls_rgw_fsio_ocnt n;
    n.holders = readers;
    encode(n, ispec.ocnt.emplace());
  }
  const auto until = ceph::mono_clock::now() +
    dpp->get_cct()->_conf.get_val<std::chrono::seconds>("rgw_fsio_publish_wait");
  const rgw_obj obj(info.bucket, rgw_obj_key(key.name, intent.instance));
  int w = 0;
  if (lose_instance != 2) {
    w = publish_wait(dpp, until, [&] {
      RGWObjectCtx octx(store);
      octx.set_atomic(obj, true);
      RGWRados::Object target(store->getRados(), info, octx, obj);
      RGWRados::Object::Write wr(&target);
      const std::string tag = gen_rand_alphanumeric(dpp->get_cct(), 32);
      wr.meta.manifest = &p.m;
      wr.meta.flags = PUT_OBJ_CREATE;
      wr.meta.mtime = &p.mtime;
      wr.meta.ptag = &tag;
      wr.meta.owner = p.policy.get_owner();
      wr.meta.bucket_owner = p.bowner;
      wr.meta.fsio_instance = &ispec;
      req_context rctx{dpp, null_yield, nullptr};
      jspan_context trace(false, false);
      int r = wr.write_meta(p.size, p.size, p.attrs, rctx, trace);
      if (r == 0 && wr.meta.canceled) {
	r = -ECANCELED;		// the claim not held, or no placeholder
      }
      return r;
    });
  }
  if (lose_instance) {
    w = -ETIMEDOUT;		// test:  its reply lost
  }
  if (w < 0) {
    /* the instance head decides:  the claim still the token, not written,
     * so it releases;  an ID tag, written, so it completes;  otherwise
     * the claim was aborted and the expirer owns the shadow (§4.2) */
    boost::container::flat_map<std::string, ceph::buffer::list> ia;
    sys::error_code ec;
    store->get_neorados().execute(neorados::Object{ioid}, iioc,
				  neorados::ReadOp{}.get_xattrs(&ia), nullptr,
				  ceph::async::use_blocked[ec]);
    const auto c = ia.find("user.rgw.fsio.claim");
    if (!ec && c != ia.end() && c->second.to_str() == intent.token) {
      release_versioned(dpp, intent, readers);
      /* not written:  the caller may try again (W13) */
      return (w == -ERR_BUSY_RESHARDING || w == -EAGAIN || w == -ETIMEDOUT)
	? -EAGAIN : w;
    }
    if (ec || !ia.count(RGW_ATTR_ID_TAG)) {
      cleanup_versioned(dpp, intent, readers);
      return w;
    }
  }
  if (instance_written) {
    instance_written();
  }
  return complete_versioned(dpp, intent, epoch, until);
}

int RadosFSIOObject::complete_versioned(const DoutPrefixProvider* dpp,
					const cls_rgw_fsio_intent& intent,
					uint64_t epoch, ceph::mono_time until)
{
  std::string ioid;
  neorados::IOContext iioc;
  if (int r = instance_obj(intent.instance, &ioid, &iioc); r < 0) {
    linking.emplace(intent, epoch);
    return r;
  }
  const rgw_obj obj(info.bucket, rgw_obj_key(key.name, intent.instance));
  /* 5:  the link at the recorded epoch, retried, never released;  a
   * version removed before it leaves a phantom entry, unlinked (Z34) */
  /* whether the instance head is still the version it wrote */
  auto written = [&] {
    ceph::buffer::list idt;
    sys::error_code ec;
    store->get_neorados().execute(
      neorados::Object{ioid}, iioc,
      neorados::ReadOp{}.get_xattr(RGW_ATTR_ID_TAG, &idt), nullptr,
      ceph::async::use_blocked[ec]);
    return !ec;
  };
  int l = publish_wait(dpp, until, [&] {
    RGWObjectCtx lctx(store);
    return store->getRados()->set_olh(dpp, lctx, info, obj, false, nullptr,
				      epoch, ceph::real_time(), false,
				      null_yield);
  });
  /* Z34:  removed since its write, the index entry with it:  link_olh
   * returns ENOENT;  nothing to link, the publish completes */
  if ((l < 0 && l != -ENOENT) || (l == -ENOENT && written())) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  linking " << intent.instance << " of "
		      << head_oid << ":  " << cpp_strerror(-l) << dendl;
    linking.emplace(intent, epoch);	// written:  never released (W10)
    return (l == -ERR_BUSY_RESHARDING || l == -EAGAIN) ? -EAGAIN : l;
  }
  if (l == 0 && !written()) {
    /* Z34:  a link that succeeded over a version removed since:  its
     * entry goes, as the removal's did */
    RGWObjectCtx uctx(store);
    if (int u = store->getRados()->unlink_obj_instance(
	  dpp, uctx, info, obj, 0, null_yield, 0, false); u < 0) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  unlinking the removed "
			<< intent.instance << ":  " << cpp_strerror(-u) << dendl;
    }
  }
  if (linked) {
    linked();
  }
  /* 6:  fsio_publish on the OLH head:  the shadow published at INSTANCE */
  cls_rgw_fsio_replace_op in;
  in.mode = static_cast<uint8_t>(cls_rgw_fsio::replace_mode::PUBLISH);
  in.caller = h;
  in.shadow = wcs;
  in.versioned = true;
  in.marker = info.bucket.marker;
  cls_rgw_fsio_replace_ret ret;
  if (int r = publish_wait(dpp, until, [&] {
	neorados::WriteOp op;
	op.exec(neorados::cls::rgw_fsio::replace_head(in, &ret));
	return exec(head_oid, head_ioc, std::move(op));
      }); r < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_publish of " << wcs.to_str()
		      << " on " << head_oid << ":  " << cpp_strerror(-r)
		      << dendl;
    linking.emplace(intent, epoch);	// keeps its hint (H1)
    return (r == -ERR_BUSY_RESHARDING || r == -EAGAIN) ? -EAGAIN : r;
  }
  instance_hint(dpp, false, cls_rgw_fsio_hint_kind::PUBLISH, intent.instance,
		h);
  if (ret.drained) {
    drain(dpp, *ret.drained);
  }
  return 0;
}

int RadosFSIOObject::handoff(const DoutPrefixProvider* dpp)
{
  /* §4.3:  the hint first, keyed by the head and the shadow, so the
   * expirer always reaches the uncompleted publish */
  cls_rgw_fsio_hint_op in;
  in.hint.kind = cls_rgw_fsio_hint_kind::UNCOMPLETED_PUBLISH;
  in.hint.target = cls_rgw_fsio_hint_target::HEAD;
  in.hint.bucket = info.bucket;
  in.hint.key = rgw_obj_key(key.name);
  in.hint.incarnation = h.incarnation;
  in.hint.handle = h.handle;
  in.hint.r = wcs;
  in.oid = head_oid;
  const auto shards = static_cast<uint32_t>(
    dpp->get_cct()->_conf.get_val<uint64_t>("rgw_fsio_open_hints_num_shards"));
  neorados::WriteOp op;
  op.exec(neorados::cls::rgw_fsio::open_hint_add(in));
  if (int r = exec(rgw::fsio::open_hint_shard_oid(
		     rgw::fsio::open_hint_shard(in.oid, shards)),
		   store->getRados()->fsio_hint_ioc(), std::move(op)); r < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  the uncompleted-publish hint for "
		      << wcs.to_str() << " on " << head_oid << ":  "
		      << cpp_strerror(-r) << dendl;
    return r;
  }
  cls_rgw_fsio_leave_ret ret;
  if (int r = leave(dpp, wcs, true, &ret, nullptr, true); r < 0) {
    return r;			// the hint stays, as a failed release's (H1)
  }
  ldpp_dout(dpp, 1) << "FSIO:  the publish of " << wcs.to_str() << " on "
		    << head_oid << " handed off" << dendl;
  /* a written version's publish hint:  the uncompleted-publish hint
   * leads the expirer to it now */
  if (linking) {
    instance_hint(dpp, false, cls_rgw_fsio_hint_kind::PUBLISH,
		  linking->first.instance, h);
    linking.reset();
  }
  writing = false;
  if (ret.drained) {
    drain(dpp, *ret.drained);
  }
  if (!reading) {
    uncount(dpp);
    hinted = false;
    return presence_hint(dpp, false, false);
  }
  return 0;
}

int RadosFSIOObject::publish_wait(const DoutPrefixProvider* dpp,
				  ceph::mono_time until,
				  fu2::function_view<int()> call)
{
  auto backoff = std::chrono::milliseconds(50);
  for (;;) {
    int r;
    if (busy_calls > 0) {
      --busy_calls;
      r = -ERR_BUSY_RESHARDING;	// test:  busy_publish_calls()
    } else {
      r = call();
    }
    if (r != -ERR_BUSY_RESHARDING && r != -EAGAIN) {
      return r;
    }
    const auto now = ceph::mono_clock::now();
    if (now >= until) {
      ldpp_dout(dpp, 1) << "FSIO:  a publish call on " << head_oid
			<< " still busy at the end of rgw_fsio_publish_wait"
			<< dendl;
      return r;
    }
    std::this_thread::sleep_for(std::min<ceph::timespan>(backoff, until - now));
    backoff = std::min(backoff * 2, std::chrono::milliseconds(1000));
  }
}

int RadosFSIOObject::leave_count(const DoutPrefixProvider* dpp,
				 const std::string& instance)
{
  std::string oid;
  neorados::IOContext ioc;
  if (int r = instance_obj(instance, &oid, &ioc); r < 0) {
    return r;
  }
  cls_rgw_fsio_ocnt_op in;
  in.holder = h;
  cls_rgw_fsio_flags_ret ret;
  neorados::WriteOp op;
  op.exec(neorados::cls::rgw_fsio::ocnt_remove(in, &ret));
  if (int r = exec(oid, ioc, std::move(op)); r < 0 && r != -ENOENT) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_ocnt_remove on " << oid << ":  "
		      << cpp_strerror(-r) << dendl;
    return r;
  }
  /* its hint goes once it is off the count (§4.3) */
  return instance_hint(dpp, false, cls_rgw_fsio_hint_kind::PRESENCE, instance,
		       h);
}

int RadosFSIOObject::end_write(const DoutPrefixProvider* dpp, uint32_t flags)
{
  if (!writing) {
    return 0;
  }
  /* the end of a write session, once its publish completed */
  auto finished = [&] (const std::optional<cls_rgw_fsio_id>& drained) {
    writing = false;
    if (drained) {
      if (int d = drain(dpp, *drained); d < 0) {
	return d;
      }
    }
    if (!reading) {
      uncount(dpp);
      hinted = false;
      return presence_hint(dpp, false, false);
    }
    return 0;
  };
  if (linking) {
    /* W10:  written, not linked:  steps 5-6 again, never a second
     * instance;  kept again if they do not complete */
    const auto [i, e] = std::move(*linking);
    linking.reset();
    const auto until = ceph::mono_clock::now() +
      dpp->get_cct()->_conf.get_val<std::chrono::seconds>("rgw_fsio_publish_wait");
    if (int p = complete_versioned(dpp, i, e, until); p < 0) {
      return p;
    }
    return finished(std::nullopt);
  }
  if (flags & WRITE_FLAG_DISCARD) {
    /* §3.4, §3.5 item 4, 8h:  its publish refused before its leave:  the
     * shadow retired if this was its last writer, else left to the
     * writers still bound;  no size report, claim or publish hint.  A
     * version already written was completed above, never discarded */
    cls_rgw_fsio_leave_ret ret;
    if (int r = leave(dpp, wcs, true, &ret, nullptr, false, true);
	r < 0 && r != -EINVAL) {
      return r;
    }
    return finished(ret.drained);
  }
  if (dirty || unsure) {
    if (int r = report_hint(dpp); r < 0) {
      return r;
    }
  }
  /* §4.2:  versioning enabled at the close makes the publish a new
   * version, its claim placed before the leave;  suspended, the null
   * version is written in place (§2.5a) */
  std::unique_ptr<rgw::sal::Bucket> bucket;
  if (int r = store->load_bucket(dpp, info.bucket, &bucket, null_yield);
      r < 0) {
    return r;
  }
  std::optional<cls_rgw_fsio_intent> intent;
  if (bucket->get_info().versioning_enabled()) {
    intent.emplace();
    if (int r = place(dpp, &*intent); r < 0) {
      return r;
    }
  }
  cls_rgw_fsio_leave_ret ret;
  if (int r = leave(dpp, wcs, true, &ret, intent ? &*intent : nullptr);
      r < 0) {
    if (intent) {
      unplace(dpp, *intent, intent->token);
    }
    return r;
  }
  if (intent && !ret.publishing) {
    unplace(dpp, *intent, intent->token);	// another writer remains
  }
  if (intent && ret.publishing) {
    /* §4.2:  the versioned publish releases or completes itself */
    if (int p = publish_versioned(dpp, *intent, ret.olh_epoch); p < 0) {
      return p;
    }
  } else if (ret.publishing) {
    /* §3.4, §4.2:  the last writer froze the shadow, and publishes it */
    if (int p = publish(dpp); p < 0) {
      /* a publisher that cannot proceed releases:  the shadow ready
       * again, its open kept, for a later try */
      neorados::WriteOp op;
      op.exec(neorados::cls::rgw_fsio::publish_release(
		cls_rgw_fsio_cs_op{h, wcs}));
      if (int r = exec(head_oid, head_ioc, std::move(op)); r < 0) {
	ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_publish_release of "
			  << wcs.to_str() << ":  " << cpp_strerror(-r) << dendl;
      }
      return p;
    }
  }
  return finished(ret.drained);
}

Object::FSIOResult RadosObject::get_fsio_handle(const DoutPrefixProvider* dpp,
						uint32_t flags,
						const FSIOCreateSpec* spec,
						const rgw_owner* writer)
{
  auto* rados = store->getRados();
  const auto& info = bucket->get_info();
  /* §3.1 step 0, Z24:  a create is a writer's;  in a versioned bucket,
   * of a name with no OLH or a delete marker (8i) */
  const bool create = flags & FSIOObject::OPEN_FLAG_CREATE;
  if ((create || (flags & FSIOObject::OPEN_FLAG_TRUNC)) &&
      !(flags & FSIOObject::OPEN_FLAG_WRITE)) {
    return FSIOResult{-EINVAL, nullptr};
  }
  /* the head, by name:  the OLH in a versioned bucket */
  rgw_raw_obj raw;
  rados->obj_to_raw(info.placement_rule,
		    rgw_obj(info.bucket, rgw_obj_key(get_key().name)), &raw);
  neorados::IOContext ioc;
  try {
    ioc = rgw::init_iocontext(dpp, store->get_neorados(), raw.pool,
			      ceph::async::use_blocked);
  } catch (const sys::system_error& e) {
    return FSIOResult{ceph::from_error_code(e.code()), nullptr};
  }
  const cls_rgw_fsio_handle h{rados->fsio_incarnation(),
			      rados->fsio_next_handle++};
  auto hdl = std::make_unique<RadosFSIOObject>(
    store, info, rgw_obj_key(get_key().name), raw.oid, std::move(ioc), h);
  if (create) {
    hdl->set_create(static_cast<uint8_t>(
		      (flags & FSIOObject::OPEN_FLAG_EXCL)
		      ? cls_rgw_fsio::create_mode::EXCLUSIVE
		      : cls_rgw_fsio::create_mode::UNCHECKED), spec);
  }
  if (int r = hdl->open(dpp, flags & FSIOObject::OPEN_FLAG_WRITE, writer);
      r < 0) {
    return FSIOResult{r, nullptr};
  }
  /* the create's size, else an O_TRUNC:  the writer's ftruncate (§3.3) */
  int64_t size = -1;
  if (create && spec && spec->size >= 0) {
    size = spec->size;
  } else if (flags & FSIOObject::OPEN_FLAG_TRUNC) {
    size = 0;
  }
  if (size >= 0) {
    if (int r = hdl->ftruncate(dpp, size, 0); r < 0) {
      hdl->close(dpp, FSIOObject::CLOSE_FLAG_DISCARD);
      return FSIOResult{r, nullptr};
    }
  }
  return FSIOResult{0, std::move(hdl)};
}

/* "fsio-inspect", {bucket, name}:  the name's FSIO state on its head (the
 * OLH in a versioned bucket), for tests.  out["shadow"] is the current
 * shadow's state, "none", "constructing", "ready" or "publishing";
 * out["held"] is "1" while an open record counts an open on the head or
 * an open hint names the name in the head's hint shard, else "0".  An
 * instance head's own counts and hints are not shown.  -ENOTSUP where the
 * bucket's gate declines:  it has no FSIO state to show */
int RadosStore::driver_hint(const DoutPrefixProvider* dpp,
			    const std::string& hint,
			    const std::map<std::string, std::string>& params,
			    std::map<std::string, std::string>* out)
{
  if (hint != "fsio-inspect") {
    return -ENOTSUP;
  }
  const auto b = params.find("bucket");
  const auto n = params.find("name");
  if (!out || b == params.end() || n == params.end()) {
    return -EINVAL;
  }
  std::unique_ptr<Bucket> bucket;
  if (int r = load_bucket(dpp, rgw_bucket(std::string(), b->second), &bucket,
			  null_yield); r < 0) {
    return r;
  }
  if (!bucket->have_fsio()) {
    return -ENOTSUP;
  }
  const auto& info = bucket->get_info();
  rgw_raw_obj raw;
  rados->obj_to_raw(info.placement_rule,
		    rgw_obj(info.bucket, rgw_obj_key(n->second)), &raw);
  boost::container::flat_map<std::string, ceph::buffer::list> attrs;
  sys::error_code ec;
  try {
    auto ioc = rgw::init_iocontext(dpp, neorados, raw.pool,
				   ceph::async::use_blocked);
    neorados.execute(neorados::Object{raw.oid}, ioc,
		     neorados::ReadOp{}.get_xattrs(&attrs, nullptr), nullptr,
		     ceph::async::use_blocked[ec]);
  } catch (const sys::system_error& e) {
    return ceph::from_error_code(e.code());
  }
  if (ec && ec != sys::errc::no_such_file_or_directory) {
    return ceph::from_error_code(ec);
  }
  std::string shadow = "none";
  bool held = false;
  for (const auto& [k, v] : attrs) {
    try {
      auto it = v.cbegin();
      if (k == "user.rgw.fsio.hdr") {
	cls_rgw_fsio_hdr hdr;
	decode(hdr, it);
	for (const auto& e : hdr.chunk_sets) {
	  if (!hdr.cur_shadow || e.id != *hdr.cur_shadow) {
	    continue;
	  }
	  switch (e.state) {
	  case cls_rgw_fsio_cs_state::CONSTRUCTING: shadow = "constructing"; break;
	  case cls_rgw_fsio_cs_state::READY:        shadow = "ready"; break;
	  case cls_rgw_fsio_cs_state::PUBLISHING:   shadow = "publishing"; break;
	  default: break;
	  }
	}
      } else if (k.starts_with("user.rgw.fsio.m.")) {
	cls_rgw_fsio_opens rec;
	decode(rec, it);
	for (const auto& [h, o] : rec.handles) {
	  for (const auto& [cs, cnt] : o.chunk_sets) {
	    held |= cnt.readers > 0 || cnt.writers > 0;
	  }
	}
      }
    } catch (const ceph::buffer::error&) {
      return -EIO;
    }
  }
  if (!held) {
    const auto shards = static_cast<uint32_t>(
      ctx()->_conf.get_val<uint64_t>("rgw_fsio_open_hints_num_shards"));
    cls_rgw_fsio_hint_list_op in;
    in.bucket = info.bucket;
    cls_rgw_fsio_hint_list_ret ret;
    neorados::ReadOp op;
    op.exec(neorados::cls::rgw_fsio::open_hint_list(in, &ret));
    neorados.execute(
      neorados::Object{rgw::fsio::open_hint_shard_oid(
	  rgw::fsio::open_hint_shard(raw.oid, shards))},
      rados->fsio_hint_ioc(), std::move(op), nullptr,
      ceph::async::use_blocked[ec]);
    if (ec && ec != sys::errc::no_such_file_or_directory) {
      return ceph::from_error_code(ec);
    }
    for (const auto& h : ret.hints) {
      held |= h.key.name == n->second;
    }
  }
  (*out)["shadow"] = shadow;
  (*out)["held"] = held ? "1" : "0";
  return 0;
}

int RadosObject::stat_fsio_view(const DoutPrefixProvider* dpp,
				struct stat* st, Attrs* attrs, uint32_t flags)
{
  if (!bucket->have_fsio()) {
    return -ENOTSUP;
  }
  /* the head by name, the OLH in a versioned bucket:  its FSIO header */
  auto* rados = store->getRados();
  const auto& info = bucket->get_info();
  rgw_raw_obj raw;
  rados->obj_to_raw(info.placement_rule,
		    rgw_obj(info.bucket, rgw_obj_key(get_key().name)), &raw);
  std::optional<cls_rgw_fsio_hdr> hdr;
  ceph::buffer::list bl;
  sys::error_code ec;
  try {
    auto ioc = rgw::init_iocontext(dpp, store->get_neorados(), raw.pool,
				   ceph::async::use_blocked);
    store->get_neorados().execute(
      neorados::Object{raw.oid}, ioc,
      neorados::ReadOp{}.get_xattr("user.rgw.fsio.hdr", &bl), nullptr,
      ceph::async::use_blocked[ec]);
  } catch (const sys::system_error& e) {
    return ceph::from_error_code(e.code());
  }
  if (!ec) {
    try {
      auto it = bl.cbegin();
      decode(hdr.emplace(), it);
    } catch (const ceph::buffer::error&) {
      return -EIO;
    }
  } else if (ec != sys::errc::no_such_file_or_directory &&
	     ec != sys::errc::no_message_available) {
    return ceph::from_error_code(ec);
  }
  /* a name being created:  no lookup finds it until its publish (§3.1) */
  if (hdr && hdr->created) {
    return -ENOTSUP;
  }
  /* no head, a tracking-only one, or a prefix:  the S3 op path's */
  if (int r = load_obj_state(dpp, null_yield, true); r < 0) {
    return r == -ENOENT ? -ENOTSUP : r;
  }
  if (!state.exists) {
    return -ENOTSUP;
  }
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFREG;
  st->st_nlink = 1;
  st->st_size = get_size();
  st->st_mtim = ceph::real_clock::to_timespec(get_mtime());
  /* §3.3:  a current shadow, ready or publishing:  its hints */
  if (hdr && hdr->cur_shadow) {
    for (const auto& e : hdr->chunk_sets) {
      if (e.id != *hdr->cur_shadow || !e.shadow ||
	  (e.state != cls_rgw_fsio_cs_state::READY &&
	   e.state != cls_rgw_fsio_cs_state::PUBLISHING)) {
	continue;
      }
      st->st_size = e.shadow->size_hint;
      if (!ceph::real_clock::is_zero(e.shadow->mtime_hint)) {
	st->st_mtim = ceph::real_clock::to_timespec(e.shadow->mtime_hint);
      }
    }
  }
  if (attrs) {
    *attrs = get_attrs();
  }
  return 0;
}

} // namespace rgw::sal
