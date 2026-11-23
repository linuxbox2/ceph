// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "include/neorados/RADOS.hpp"
#include "cls/rgw/cls_rgw_fsio_ops.h"
#include "cls/rgw/cls_rgw_fsio_types.h"
#include "rgw_fsio_geometry.h"
#include "rgw_sal.h"

namespace rgw::sal {

class RadosStore;

/* FSIO_RADOS_DESIGN.md:  one handle's FSIO view of an object on RADOS.
 * The head's records name it by {incarnation, handle} (§2.2, Z4);  every
 * sequenced call it makes carries the next of its sequence numbers (§3,
 * Z22).  In a versioned bucket the head is the OLH, and the handle counts
 * itself on the instance it binds (§2.5).  Built a step at a time
 * (FSIO_RADOS_WORK_PLAN.md, Phase 3, the driver):  the open, then reads
 * and writes, the close, the publish;  an operation not built yet returns
 * -ENOTSUP. */
class RadosFSIOObject : public Object::FSIOObject {
public:
  RadosFSIOObject(RadosStore* store, const RGWBucketInfo& info,
		  rgw_obj_key key, std::string head_oid,
		  neorados::IOContext head_ioc, cls_rgw_fsio_handle h)
    : store(store), info(info), key(std::move(key)),
      head_oid(std::move(head_oid)), head_ioc(std::move(head_ioc)), h(h) {}

  /* §3.1, §4.1, §4.3:  the presence hints, the instance's open count in a
   * versioned bucket, then fsio_join as a reader or a writer;  a writer
   * that must construct clones the content's chunks and readies the
   * shadow */
  int open(const DoutPrefixProvider* dpp, bool writer);

  /* §2.7:  a writer open reads its shadow, otherwise the bound content;
   * short at the size;  a missing shadow chunk inside it reads as zeros
   * (Z11), a missing published one fails */
  int preadv(const struct iovec* iov, int iovcnt, uint64_t ofs,
	     uint64_t* bytes_read, uint32_t flags) override;
  /* §2.7, §3.7:  into the shadow's chunks, each reserved first and each
   * write carrying the chunk write fence's check;  one that meets the
   * fence returns -EIO */
  int pwritev(const struct iovec* iov, int iovcnt, uint64_t ofs,
	      uint64_t* bytes_written, uint32_t flags) override;
  int commit(const DoutPrefixProvider* dpp, uint32_t flags) override { return 0; }
  /* §3.1:  a reader's handle joins as a writer too */
  int begin_write(const DoutPrefixProvider* dpp, uint32_t flags) override;
  /* §3.4:  the hint reported, then fsio_leave as writer;  the last
   * writer's freezes the shadow, which the publish (plan, driver step 8)
   * completes;  until it lands the freeze is released and -ENOTSUP */
  int end_write(const DoutPrefixProvider* dpp, uint32_t flags) override;
  /* §3.4:  fsio_leave, the instance's count, then the presence hints go;
   * the lost-reply rule and the drain's GC chain are the close's step
   * (plan, driver step 7) */
  int close(const DoutPrefixProvider* dpp, uint32_t flags) override;
  /* §2.7:  the size and mtime this handle sees:  a reader its content's,
   * a writer its shadow's as it knows them, rebased first when a
   * sequenced call's reply was lost (§3.3) */
  int fstat(struct stat* st, uint32_t flags) override;
  /* §3.3:  the chunks first, each with the chunk write fence's check,
   * then fsio_truncate */
  int ftruncate(const DoutPrefixProvider* dpp, uint64_t size,
		uint32_t flags) override;
  int fgetattr(const DoutPrefixProvider* dpp, const std::string& name,
	       buffer::list& dest, uint32_t flags) override { return -ENOTSUP; }
  int fsetattr(const DoutPrefixProvider* dpp, const std::string& name,
	       const buffer::list& val, uint32_t flags) override { return -ENOTSUP; }
  int fgetattrs(const DoutPrefixProvider* dpp, Attrs& attrs,
		uint32_t flags) override { return -ENOTSUP; }
  int fsetattrs(const DoutPrefixProvider* dpp, Attrs& attrs,
		uint32_t flags) override { return -ENOTSUP; }
  int fremovexattr(const DoutPrefixProvider* dpp, const std::string& name,
		   uint32_t flags) override { return -ENOTSUP; }

  const cls_rgw_fsio_handle& handle() const { return h; }
  const cls_rgw_fsio_id& bound() const { return cs; }
  const cls_rgw_fsio_id& writing_to() const { return wcs; }
  /* the version it counts itself on;  empty when unversioned, or the null
   * version */
  const std::string& instance() const { return inst; }
  const std::string& instance_oid() const { return inst_oid; }
  /* where the bound content's inline head segment is read from, once an
   * overwrite or a rewrite moved it */
  const std::optional<std::string>& moved_segment() const { return seg_moved; }
  /* §2.7, §3.3:  fsio_hint with this writer's epoch, high-water mark and
   * mtime;  a stale epoch rebases from the chunk at the high-water mark
   * and reports again.  end_write() calls it (the close, plan step 7) */
  int report_hint(const DoutPrefixProvider* dpp);

  /* the shadow's chunk holding ofs, as named on RADOS */
  rgw_raw_obj shadow_chunk(uint64_t ofs) const;
  /* test only:  each sequenced call sent twice with one sequence number,
   * the second reply kept, as the Objecter's resend past the PG log's
   * dup window returns it (Z22) */
  void send_sequenced_twice(bool on) { twice = on; }

  /* the fsio_extend calls this handle has made */
  uint64_t extends() const { return n_extends; }

private:
  /* an object's head as a guard reads it:  its ID tag, its manifest, the
   * head object's own size (inline data), the object's size, its mtime */
  struct Source {
    std::string oid;		// the head it was read from, or the instance's
    ceph::buffer::list id_tag;
    ceph::buffer::list manifest;
    uint64_t head_size = 0;
    uint64_t size = 0;
    ceph::real_time mtime;
  };
  int read_source(const DoutPrefixProvider* dpp, const std::string& oid,
		  const neorados::IOContext& ioc, Source* src);
  /* the OLH as the versioned join's guard reads it (§3.1) */
  struct Olh {
    std::string instance;
    ceph::buffer::list id_tag;
    ceph::buffer::list ver;
  };
  /* 0 and the OLH;  -ENOENT when the head is no OLH */
  int read_olh(const DoutPrefixProvider* dpp, Olh* olh);
  /* §2.5, §4.3:  the presence hint on the instance head, then ocnt_add;
   * and the reverse */
  int count(const DoutPrefixProvider* dpp);
  int uncount(const DoutPrefixProvider* dpp);
  /* §4.1 step 1:  the content this handle counted itself for, deferred by a
   * rewrite or removal since:  its record in the count's deferred list */
  int find_deferred(const DoutPrefixProvider* dpp, cls_rgw_fsio_replaced* rec);
  /* §3.1:  the guard, then fsio_join, in one op, retried while an S3
   * write or a relink lands in between */
  int join(const DoutPrefixProvider* dpp, bool writer, Source* src,
	   FSIOGeometry* g, cls_rgw_fsio_join_ret* ret);
  /* §4.1:  versioned, the instance head's clone first;  then every chunk
   * but the head's cloned to f(anchor, id), then fsio_ready */
  int construct(const DoutPrefixProvider* dpp, const Source& src,
		const cls_rgw_fsio_id& id);
  /* every chunk of g's source but skip_oid, its head's, cloned to
   * f(anchor, id);  n, the source's chunk count */
  int clone_chunks(const DoutPrefixProvider* dpp, const FSIOGeometry& g,
		   const std::string& skip_oid, const cls_rgw_fsio_id& id,
		   uint64_t* n);
  /* the geometry of a manifest (§2.7);  -ENXIO when it has none (§6.2) */
  int make_geometry(const DoutPrefixProvider* dpp,
		    const ceph::buffer::list& manifest, FSIOGeometry* g);

  /* §3.7:  the chunk at index is in a run this writer knows is reserved,
   * or fsio_extend reserves ahead to cover it */
  int reserve(const DoutPrefixProvider* dpp, uint64_t index);
  void know(const cls_rgw_fsio_run& run);

  /* a pool's IOContext, cached, keyed with the object's locator */
  int ioc_for(const DoutPrefixProvider* dpp, const rgw_raw_obj& raw,
	      neorados::IOContext* ioc);
  /* len bytes at off in chunk c of the content (no id) or the shadow;  the
   * inline head segment of the bound content guarded by its ID tag, and
   * read from where an overwrite or a rewrite moved it once it fails */
  int read_piece(const DoutPrefixProvider* dpp, const FSIOChunk& c,
		 bool shadow, uint64_t off, uint64_t len,
		 ceph::buffer::list* bl);
  /* §3.4 steps 2-3:  a drained chunk set's GC entry, then fsio_forget,
   * and the same for any holder that hands back */
  int drain(const DoutPrefixProvider* dpp, cls_rgw_fsio_id id);
  /* §2.5:  a drained instance count's deferred contents, then
   * fsio_ocnt_forget, any failure of which counts as done */
  int drain_count(const DoutPrefixProvider* dpp);
  /* §4.1, W15:  a shadow's chunks and attribute clone to GC */
  int collect_shadow(const DoutPrefixProvider* dpp, const cls_rgw_fsio_id& id,
		     const std::vector<cls_rgw_fsio_run>& extent,
		     const ceph::buffer::list* pm = nullptr);
  /* §3.4, W15:  a construction that failed:  its clones to GC, then
   * fsio_leave as writer, which abandons the shadow */
  int abandon(const DoutPrefixProvider* dpp, const Source& src);
  /* §3, Z22:  after a lost reply, whether the head still holds this
   * handle's opens it gave up, or its last sequence number alone */
  int keeps_hint(const DoutPrefixProvider* dpp, bool* keep);
  /* fsio_leave of cs in a role, through sequenced() */
  int leave(const DoutPrefixProvider* dpp, const cls_rgw_fsio_id& cs,
	    bool writer, cls_rgw_fsio_leave_ret* ret);

  /* the head's FSIO header */
  int read_header(const DoutPrefixProvider* dpp, cls_rgw_fsio_hdr* hdr);
  /* a sequenced call on the head:  the op exec, then seq advanced;
   * twice when the test asks */
  int sequenced(const DoutPrefixProvider* dpp,
		const std::function<void(neorados::WriteOp&)>& build);

  /* §2.3, §2.5:  where the bound content's head segment went:  the
   * generation's leaf clone f(head, r), or the deferred sibling */
  int find_moved_segment(const DoutPrefixProvider* dpp);

  /* §2.7:  this handle's presence hint on the head or the instance head */
  int presence_hint(const DoutPrefixProvider* dpp, bool add, bool on_instance);
  /* a write op, synchronously;  0 or its error */
  int exec(const std::string& oid, const neorados::IOContext& ioc,
	   neorados::WriteOp op);

  RadosStore* store;
  RGWBucketInfo info;
  rgw_obj_key key;
  std::string head_oid;
  neorados::IOContext head_ioc;
  cls_rgw_fsio_handle h;
  uint64_t seq = 0;		// its last sequenced call's
  cls_rgw_fsio_id cs;		// the chunk set its reader is bound to
  cls_rgw_fsio_id wcs;		// the shadow its writer is bound to
  std::string inst;		// the instance it counts itself on
  ceph::buffer::list counted_id_tag;	// the content it counted itself for:
  std::string counted_gc_tag;	// its ID tag, and its tail tag else its ID tag
  std::string inst_oid;
  neorados::IOContext inst_ioc;
  FSIOGeometry rgeo;		// the bound content's, read under the join's guard
  FSIOGeometry wgeo;		// the shadow's:  its source's
  std::string seg_oid;		// the bound content's inline head segment,
  ceph::buffer::list seg_tag;	//   guarded by its ID tag
  std::optional<std::string> seg_moved;	// where it went, once it moved
  uint64_t wsize = 0;		// the shadow's size hint, as the join saw it
  uint64_t hwm = 0;		// the end of this handle's writes
  std::vector<cls_rgw_fsio_run> known;	// runs it knows are reserved, merged
  uint64_t trunc_epoch = 0;	// the shadow's, as last heard
  bool others = false;		// another handle is open in the generation
  bool dirty = false;		// written since its last report
  bool unsure = false;		// a sequenced call's reply was lost (§3.3)
  bool twice = false;		// test only:  send_sequenced_twice()
  ceph::real_time wmtime;	// its last write's
  ceph::real_time rmtime;	// the bound content's
  ceph::real_time wmtime_hint;	// the shadow's, as the join saw it
  ceph::mono_time last_report;
  uint64_t n_extends = 0;
  std::map<rgw_pool, neorados::IOContext> iocs;
  std::optional<NoDoutPrefix> ndp;	// preadv and pwritev take no dpp
  bool hinted = false;		// on the head
  bool inst_hinted = false;
  bool counted = false;
  bool reading = false;
  bool writing = false;
  bool reader_on_shadow = false;	// its reader followed its shadow (rebind)
};

} // namespace rgw::sal
