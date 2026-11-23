// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_fsio_object.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include <boost/system/error_code.hpp>

#include "cls/rgw/cls_rgw_fsio_ops.h"
#include "cls/rgw/cls_rgw_ops.h"
#include "common/async/blocked_completion.h"
#include "common/dout.h"
#include "common/error_code.h"
#include "include/random.h"
#include "neorados/cls/rgw_fsio.h"
#include "rgw_aio_throttle.h"
#include "rgw_fsio_hints.h"
#include "rgw_obj_manifest.h"
#include "rgw_rados.h"
#include "services/svc_zone.h"
#include "rgw_sal_rados.h"
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
  neorados::ReadOp op;
  sys::error_code tag_ec, man_ec;
  op.get_xattr(RGW_ATTR_ID_TAG, &src->id_tag, &tag_ec);
  op.get_xattr(RGW_ATTR_MANIFEST, &src->manifest, &man_ec);
  op.stat(&src->head_size, &src->mtime);
  sys::error_code ec;
  store->get_neorados().execute(neorados::Object{oid}, ioc, std::move(op),
				nullptr, ceph::async::use_blocked[ec]);
  if (ec) {
    return ceph::from_error_code(ec);
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
      return -ENOENT;		// a delete marker, or removed:  no version
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
    /* a drain's GC entries are the close's step (plan, driver step 7) */
    counted = false;
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
    Olh olh;
    int r = info.versioned() ? read_olh(dpp, &olh) : -ENOENT;
    if (r < 0 && r != -ENOENT) {
      return r;
    }
    if (r == 0 && !is_null(olh.instance)) {
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
      if (int s = read_source(dpp, head_oid, head_ioc, src); s < 0) {
	return s;
      }
      /* §6.2:  declined before the join, so it leaves no state */
      if (int e = make_geometry(dpp, src->manifest, g); e < 0) {
	return e;
      }
      if (src->id_tag.length()) {
	op.cmpxattr(RGW_ATTR_ID_TAG, neorados::cmp_op::eq, src->id_tag);
      }
    }
    cls_rgw_fsio_join_op in;
    in.caller = h;
    in.seq = seq + 1;		// taken only if the call applies
    in.writer = writer;
    in.nonce = ceph::util::generate_random_number<uint64_t>();
    in.counter_seed = 1;	// no manifest carries a shadow id until a
				// publish writes one (plan, the driver's step 3)
    in.instance = instance;
    in.marker = info.bucket.marker;
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
    const rgw_raw_obj& raw = c.anchor;
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
  cls_rgw_fsio_hdr hdr;
  try {
    auto it = bl.cbegin();
    decode(hdr, it);
  } catch (const ceph::buffer::error&) {
    return -EIO;
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
  const bool shadow = writing;
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

int RadosFSIOObject::open(const DoutPrefixProvider* dpp, bool writer)
{
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
  writing = true;
  binding = Binding::SHADOW;
  if (ret.construct) {
    return construct(dpp, src, wcs);
  }
  return 0;
}

int RadosFSIOObject::begin_write(const DoutPrefixProvider* dpp, uint32_t flags)
{
  return writing ? 0 : open(dpp, true);
}

int RadosFSIOObject::close(const DoutPrefixProvider* dpp, uint32_t flags)
{
  if (writing) {
    return -ENOTSUP;		// a writer's last close publishes:  step 8
  }
  if (!reading) {
    return 0;
  }
  cls_rgw_fsio_leave_op in;
  in.caller = h;
  in.seq = seq + 1;
  in.cs = cs;
  in.writer = false;
  neorados::WriteOp op;
  op.exec(neorados::cls::rgw_fsio::leave(in));
  if (int r = exec(head_oid, head_ioc, std::move(op)); r < 0) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  fsio_leave as reader of " << head_oid
		      << " by " << h.incarnation << "." << h.handle << ":  "
		      << cpp_strerror(-r) << dendl;
    return r;
  }
  seq = in.seq;
  reading = false;
  /* §2.5:  off the instance's count once it holds no open bound to it */
  if (int r = uncount(dpp); r < 0) {
    return r;
  }
  hinted = false;
  return presence_hint(dpp, false, false);
}

Object::FSIOResult RadosObject::get_fsio_handle(const DoutPrefixProvider* dpp,
						uint32_t flags,
						const FSIOCreateSpec* spec)
{
  /* creating, truncating at the open:  later work elements */
  if (flags & (FSIOObject::OPEN_FLAG_CREATE | FSIOObject::OPEN_FLAG_TRUNC |
	       FSIOObject::OPEN_FLAG_EXCL)) {
    return FSIOResult{-ENOTSUP, nullptr};
  }
  auto* rados = store->getRados();
  const auto& info = bucket->get_info();
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
  if (int r = hdl->open(dpp, flags & FSIOObject::OPEN_FLAG_WRITE); r < 0) {
    return FSIOResult{r, nullptr};
  }
  return FSIOResult{0, std::move(hdl)};
}

} // namespace rgw::sal
