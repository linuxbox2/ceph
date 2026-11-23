// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <cstdint>
#include <string>

#include "include/neorados/RADOS.hpp"
#include "cls/rgw/cls_rgw_fsio_ops.h"
#include "cls/rgw/cls_rgw_fsio_types.h"
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

  int preadv(const struct iovec* iov, int iovcnt, uint64_t ofs,
	     uint64_t* bytes_read, uint32_t flags) override { return -ENOTSUP; }
  int pwritev(const struct iovec* iov, int iovcnt, uint64_t ofs,
	      uint64_t* bytes_written, uint32_t flags) override { return -ENOTSUP; }
  int commit(const DoutPrefixProvider* dpp, uint32_t flags) override { return 0; }
  /* §3.1:  a reader's handle joins as a writer too */
  int begin_write(const DoutPrefixProvider* dpp, uint32_t flags) override;
  int end_write(const DoutPrefixProvider* dpp, uint32_t flags) override { return -ENOTSUP; }
  /* §3.4:  fsio_leave, the instance's count, then the presence hints go;
   * the lost-reply rule and the drain's GC chain are the close's step
   * (plan, driver step 6) */
  int close(const DoutPrefixProvider* dpp, uint32_t flags) override;
  int fstat(struct stat* st, uint32_t flags) override { return -ENOTSUP; }
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

private:
  /* an object's head as a guard reads it:  its ID tag, its manifest, the
   * head object's own size (inline data), the object's size, its mtime */
  struct Source {
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
	   cls_rgw_fsio_join_ret* ret);
  /* §4.1:  versioned, the instance head's clone first;  then every chunk
   * but the head's cloned to f(anchor, id), then fsio_ready */
  int construct(const DoutPrefixProvider* dpp, const Source& src,
		const cls_rgw_fsio_id& id);
  int clone_chunks(const DoutPrefixProvider* dpp,
		   const ceph::buffer::list& manifest,
		   const std::string& skip_oid, const cls_rgw_fsio_id& id,
		   uint64_t* n);

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
  bool hinted = false;		// on the head
  bool inst_hinted = false;
  bool counted = false;
  bool reading = false;
  bool writing = false;
};

} // namespace rgw::sal
