// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "acconfig.h"
#include "common/ceph_mutex.h"
#include "common/ceph_time.h"
#include "include/function2.hpp"
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
  ~RadosFSIOObject() override;

  /* §3.1, §4.1, §4.3:  the presence hints, the instance's open count in a
   * versioned bucket, then fsio_join as a reader or a writer;  a writer
   * that must construct clones the content's chunks and readies the
   * shadow */
  int open(const DoutPrefixProvider* dpp, bool writer,
	   const rgw_owner* identity = nullptr);
  /* §3.1 step 0, Z24:  before open(), a create-open's mode
   * (cls_rgw_fsio::create_mode) and what its create sets:  the attribute
   * clone's attributes, and the file's mtime */
  void set_create(uint8_t mode, const Object::FSIOCreateSpec* spec);

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
  /* §3.6:  an unlinked generation's shadow still publishes, last writer
   * wins:  only local state, of which the handle keeps none to drop */
  int discard(const DoutPrefixProvider* dpp, uint32_t flags) override {
    return 0;
  }
  /* §3.1:  a reader's handle joins as a writer too */
  /* §3.5 item 6:  a writer open records its identity at its join;  a
   * later one with another identity replaces it (fsio_writer_identity) */
  int begin_write(const DoutPrefixProvider* dpp, uint32_t flags,
		  const rgw_owner* writer = nullptr) override;
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
  /* §2.4:  a writer's attributes are its shadow's attribute clone's,
   * which the publish carries to the head;  a reader's, the content it is
   * bound to, and it sets none (-EBADF).  FSIO's own records, under
   * user.rgw.fsio., are neither shown nor set (-EINVAL) */
  int fgetattr(const DoutPrefixProvider* dpp, const std::string& name,
	       buffer::list& dest, uint32_t flags) override;
  int fsetattr(const DoutPrefixProvider* dpp, const std::string& name,
	       const buffer::list& val, uint32_t flags) override;
  int fgetattrs(const DoutPrefixProvider* dpp, Attrs& attrs,
		uint32_t flags) override;
  int fsetattrs(const DoutPrefixProvider* dpp, Attrs& attrs,
		uint32_t flags) override;
  int fremovexattr(const DoutPrefixProvider* dpp, const std::string& name,
		   uint32_t flags) override;

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
  /* test only:  the publish's head write applied, its reply treated as
   * lost, so the publish decides as the expirer would (§4.2, W21) */
  void lose_publish_reply(bool on) { lose_reply = on; }
  /* test only:  the versioned instance write's reply lost:  1 after it
   * applied, 2 without sending it;  the publish decides from the
   * instance head (§4.2) */
  void lose_instance_reply(int how) { lose_instance = how; }
  /* test only:  called between the instance write and the link (Z34) */
  void after_instance_write(fu2::unique_function<void()> f) {
    instance_written = std::move(f);
  }
  /* test only:  called between the link and fsio_publish (§3.1 step 4) */
  void after_link(fu2::unique_function<void()> f) { linked = std::move(f); }
  /* test only:  the instance its last placeholder was for */
  const std::string& placed_instance() const { return placed; }
  /* test only:  the next n publish calls (a head or instance write, a
   * link, the versioned fsio_replace_head) return ERR_BUSY_RESHARDING
   * without being sent (W13) */
  void busy_publish_calls(int n) { busy_calls = n; }
  /* test only:  called on each try of the publish's head write, after
   * its read of the tags it evaluated, which it is given, and before the
   * write, so a test can write the tags in between (C1) */
  void after_tags_read(fu2::unique_function<void(const ceph::buffer::list&)> f) {
    tags_read = std::move(f);
  }
  /* test only:  each sequenced call sent twice with one sequence number,
   * the second reply kept, as the Objecter's resend past the PG log's
   * dup window returns it (Z22) */
  void send_sequenced_twice(bool on) { twice = on; }

  /* the fsio_extend calls this handle has made */
  uint64_t extends() const { return n_extends; }
  /* plan 9g:  where a write session's time goes;  timed only when built
   * WITH_RGW_FSIO_PHASES.  construct excludes its ready, write its
   * extend, when reported */
  enum class Phase : uint8_t { hint_in, join, construct, ready, extend,
			       write, attrs, report, leave, pub_hint, prepare,
			       head_write, cleanup, hint_out, n };
  static const char* phase_name(Phase p);
  struct Phases {
    std::array<ceph::timespan, size_t(Phase::n)> t{};
    std::array<uint32_t, size_t(Phase::n)> calls{};
    uint32_t busy = 0;			// fsio_join BUSY replies
    ceph::timespan slept{};		// its backoff sleeps
    uint32_t publish_retries = 0;	// head writes retried
  };
  /* nullopt when built without the option */
  std::optional<Phases> phases() const;
  /* test only:  ops sent to the hint shard (§4.3) */
  uint64_t hint_ops() const { return n_hint_ops; }
  /* test only:  reads of the attribute clone (plan 9g) */
  uint64_t clone_reads() const { return n_clone_reads; }
  /* test only:  write ops sent to chunks (§4.5) */
  uint64_t chunk_writes() const { return n_chunk_writes; }

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
  /* Z24:  a created name's layout:  a random tail prefix, the
   * placement's stripe size, head size 0 and object size 0 (§3.1 step 7) */
  int fresh_layout(const DoutPrefixProvider* dpp, ceph::buffer::list* pm);
  /* Z24:  a created head's shadow:  its attribute clone written here,
   * the fresh layout as its pseudo-manifest, then fsio_ready */
  int construct_created(const DoutPrefixProvider* dpp, const cls_rgw_fsio_id& id);
  /* §2.4:  a shadow's pseudo-manifest, from its attribute clone */
  int shadow_pm(const DoutPrefixProvider* dpp, const cls_rgw_fsio_id& id,
		ceph::buffer::list* pm);
  /* the OLH as the versioned join's guard reads it (§3.1) */
  struct Olh {
    std::string instance;
    ceph::buffer::list id_tag;
    ceph::buffer::list ver;
    bool removed = false;	// a delete marker is current (§4.1, 8i)
  };
  /* 0 and the OLH, `removed` for a delete marker;  -ENOENT when the head
   * is no OLH */
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
  /* §4.2 steps 1-5:  the last writer's publish of the shadow its leave
   * froze */
  int publish(const DoutPrefixProvider* dpp);
  /* §4.2 steps 2-3, §3.5:  what either publish writes:  the size, the
   * manifest, the attribute set with its ACL and ETag, the clone's tags,
   * the owner, the sync policy */
  struct Prepared {
    RGWObjManifest m;
    uint64_t size = 0;
    std::map<std::string, ceph::buffer::list> attrs;
    ceph::buffer::list clone_tags;
    RGWAccessControlPolicy policy;
    rgw_owner bowner;
    RGWBucketSyncPolicyHandlerRef sync;
    ceph::real_time mtime;
  };
  int prepare_publish(const DoutPrefixProvider* dpp, Prepared* p);
  /* §3.5, C1:  PENDING when the sync policy exports the object with the
   * tags it will keep */
  int replication_status(const DoutPrefixProvider* dpp, const Prepared& p,
			 const ceph::buffer::list& tags,
			 std::map<std::string, ceph::buffer::list>* attrs);
  /* §3.5:  the publish hint naming the leaf clone f(head, r) */
  int publish_hint(const DoutPrefixProvider* dpp, bool add,
		   const cls_rgw_fsio_id& r);
  /* §2.3:  the content replaced under the leaf clone f(head, r):  its
   * tail to GC from the clone's record, then the clone */
  int collect_leaf(const DoutPrefixProvider* dpp, const cls_rgw_fsio_id& r);
  /* §2.7:  the size from the chunks:  the highest existing chunk in the
   * shadow's extent that is not a fenced hole, scanned down */
  int scan_size(const DoutPrefixProvider* dpp, uint64_t* size);

  /* §4.1, W15:  a shadow's chunks and attribute clone to GC;  in a
   * versioned bucket its clones are beside `base`, the version it was
   * built from (§2.2) */
  int collect_shadow(const DoutPrefixProvider* dpp, const cls_rgw_fsio_id& id,
		     const std::vector<cls_rgw_fsio_run>& extent,
		     const ceph::buffer::list* pm = nullptr,
		     const std::string& base = {});
  /* §3.4, W15:  a construction that failed:  its clones to GC, then
   * fsio_leave as writer, which abandons the shadow */
  int abandon(const DoutPrefixProvider* dpp, const Source& src);
  /* §3, Z22:  after a lost reply, whether the head still holds this
   * handle's opens it gave up, or its last sequence number alone */
  int keeps_hint(const DoutPrefixProvider* dpp, bool* keep);
  /* fsio_leave of cs in a role, through sequenced() */
  int leave(const DoutPrefixProvider* dpp, const cls_rgw_fsio_id& cs,
	    bool writer, cls_rgw_fsio_leave_ret* ret,
	    const cls_rgw_fsio_intent* intent = nullptr, bool handoff = false,
	    bool discard = false,
	    const cls_rgw_fsio_size_report* report = nullptr);
  /* §4.2, 8j:  a close whose publish could not complete hands it off:
   * the uncompleted-publish hint, then the hand-off leave;  the handle
   * then holds nothing of the write */
  int handoff(const DoutPrefixProvider* dpp);

  /* §4.2, versioned:  an instance head's object, by its instance */
  int instance_obj(const std::string& instance, std::string* oid,
		   neorados::IOContext* ioc);
  /* §2.7, §4.3:  a hint on an instance head, for `who` */
  int instance_hint(const DoutPrefixProvider* dpp, bool add,
		    cls_rgw_fsio_hint_kind kind, const std::string& instance,
		    const cls_rgw_fsio_handle& who);
  /* §4.2 step 3:  the handles with readers bound to a shadow */
  int shadow_readers(const DoutPrefixProvider* dpp, const cls_rgw_fsio_id& cs,
		     std::set<cls_rgw_fsio_handle>* readers);
  /* §4.2 step 1:  an instance id and a token, the publish hint naming the
   * instance head, and its placeholder holding the claim */
  int place(const DoutPrefixProvider* dpp, cls_rgw_fsio_intent* intent);
  /* the placeholder deleted while its claim is `claim`, then the hint */
  void unplace(const DoutPrefixProvider* dpp, const cls_rgw_fsio_intent& intent,
	       const std::string& claim);
  /* §4.2 steps 3-6:  the versioned publish of the shadow the leave froze
   * under `intent`, linked at `epoch` */
  int publish_versioned(const DoutPrefixProvider* dpp,
			const cls_rgw_fsio_intent& intent, uint64_t epoch);
  /* §4.2 steps 5-6:  the link at `epoch` and fsio_publish;  kept in
   * `linking` for the next end_write() when either does not complete */
  int complete_versioned(const DoutPrefixProvider* dpp,
			 const cls_rgw_fsio_intent& intent, uint64_t epoch,
			 ceph::mono_time until);
  /* W13:  a publish call retried on ERR_BUSY_RESHARDING and EAGAIN, with
   * backoff, until `until` */
  int publish_wait(const DoutPrefixProvider* dpp, ceph::mono_time until,
		   fu2::function_view<int()> call);
  /* §4.2, W10:  before the instance write:  its claim aborted, the shadow
   * released, then the cleanup */
  int release_versioned(const DoutPrefixProvider* dpp,
			const cls_rgw_fsio_intent& intent,
			const std::set<cls_rgw_fsio_handle>& readers);
  /* §4.2, V4, H3f:  once no chunk-set entry names the instance, the
   * readers' hints, the aborted placeholder and the publish hint */
  void cleanup_versioned(const DoutPrefixProvider* dpp,
			 const cls_rgw_fsio_intent& intent,
			 const std::set<cls_rgw_fsio_handle>& readers);
  /* §2.5, W7:  a reader off the count of the version its leave returned,
   * when it is not the one it counted itself on */
  int leave_count(const DoutPrefixProvider* dpp, const std::string& instance);

  /* the head's FSIO header */
  int read_header(const DoutPrefixProvider* dpp, cls_rgw_fsio_hdr* hdr);
  /* a sequenced call on the head:  the op exec, then seq advanced;
   * twice when the test asks */
  int sequenced(const DoutPrefixProvider* dpp,
		fu2::function_view<void(neorados::WriteOp&)> build);

  /* §2.3, §2.5:  where the bound content's head segment went:  the
   * generation's leaf clone f(head, r), or the deferred sibling */
  int find_moved_segment(const DoutPrefixProvider* dpp);

  /* §2.7:  this handle's presence hint on the head or the instance head */
  int presence_hint(const DoutPrefixProvider* dpp, bool add, bool on_instance);
  /* the hint ops, and their shard:  the presence hint, a publish hint */
  cls_rgw_fsio_hint_op presence_op(bool on_instance) const;
  cls_rgw_fsio_hint_op publish_op(const cls_rgw_fsio_id& r) const;
  std::string hint_shard(const DoutPrefixProvider* dpp,
			 const std::string& oid) const;
  /* §4.3:  a publish's hint and the head's presence hint, removed in one
   * op when nothing comes between them */
  int unhint_both(const DoutPrefixProvider* dpp, const cls_rgw_fsio_id& r);
  /* a write op, synchronously;  0 or its error */
  /* §2.4:  the writer's attribute clone f(head_oid, id), role 'd' where
   * the head segment is inline, else 'a', beside the instance head it was
   * cloned from in a versioned bucket (§4.1);  its xattrs, if asked */
  int attr_clone(const DoutPrefixProvider* dpp, std::string* oid,
		 neorados::IOContext* ioc,
		 boost::container::flat_map<std::string, ceph::buffer::list>* xattrs);
  int exec(const std::string& oid, const neorados::IOContext& ioc,
	   neorados::WriteOp op);
  /* §4.5:  one call's chunk operations;  a write piece waiting for its
   * chunk's op in flight;  per chunk, at most one write op in flight, the
   * writes arriving meanwhile waiting in arrival order */
  struct IoBatch;
  struct Pending {
    uint64_t off;			// within the chunk
    ceph::buffer::list data;
    std::shared_ptr<IoBatch> batch;	// its call's
    uint64_t pos;			// in the file
  };
  struct ChunkQueue {
    bool sending = false;
    std::vector<Pending> waiting;
  };
  /* §4.5:  one op for these writes to a chunk, then the next waiting */
  void send_chunk(uint64_t index, const rgw_raw_obj& raw,
		  const neorados::IOContext& ioc, std::vector<Pending> writes);
  void chunk_done(uint64_t index, const rgw_raw_obj& raw,
		  const neorados::IOContext& ioc, std::vector<Pending> writes,
		  int r);
  /* §4.5:  a read or write in flight, which a session change waits for */
  void io_begin();
  void io_end();
  void drain_io();
  /* §2.7:  the size report, its caller holding report_lock */
  int do_report_hint(const DoutPrefixProvider* dpp);

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
  bool lose_reply = false;	// test only:  lose_publish_reply()
  fu2::unique_function<void(const ceph::buffer::list&)> tags_read;	// test only
  int lose_instance = 0;	// test only:  lose_instance_reply()
  int busy_calls = 0;		// test only:  busy_publish_calls()
  /* a versioned publish written and not yet linked:  its intent and
   * epoch, which the next end_write() completes with (W10) */
  std::optional<std::pair<cls_rgw_fsio_intent, uint64_t>> linking;
  fu2::unique_function<void()> instance_written;	// test only
  fu2::unique_function<void()> linked;	// test only
  std::string placed;		// test only:  placed_instance()
  std::string identity;		// the writer open's, as the join records it
  uint8_t create = 0;		// a create-open's cls_rgw_fsio::create_mode
  std::map<std::string, ceph::buffer::list> create_attrs;	// its create's
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
  /* §4.5:  what concurrent reads and writes share (the runs held, hwm,
   * wsize, wmtime, trunc_epoch, others, dirty, unsure, last_report,
   * n_extends, seg_moved, iocs), never held across a RADOS operation */
  mutable ceph::mutex state_lock = ceph::make_mutex("RadosFSIOObject::state");
  ceph::condition_variable io_cond;
  unsigned in_flight = 0;		// reads and writes in progress
  /* §4.5:  fsio_extend calls one at a time;  one size report at a time */
  ceph::mutex extend_lock = ceph::make_mutex("RadosFSIOObject::extend");
  ceph::mutex report_lock = ceph::make_mutex("RadosFSIOObject::report");
  /* §4.5:  the chunks' write queues, never held across a RADOS operation */
  ceph::mutex queue_lock = ceph::make_mutex("RadosFSIOObject::queue");
  std::map<uint64_t, ChunkQueue> chunkq;	// by chunk index
  std::atomic<uint64_t> n_chunk_writes{0};
  /* plan 9g:  the writer's attribute clone's role, 'd' or 'a', once its
   * construction or a probe found it;  0 unknown */
  std::atomic<char> attr_role{0};
  /* §4.3:  a publish hint still to remove, with the presence hint */
  std::optional<cls_rgw_fsio_id> unhint;
  std::atomic<uint64_t> n_hint_ops{0};
  std::atomic<uint64_t> n_clone_reads{0};
#ifdef WITH_RGW_FSIO_PHASES
  Phases phases_;			// under state_lock
  class Timed {
    RadosFSIOObject& o;
    Phase p;
    ceph::mono_time t0;
  public:
    Timed(RadosFSIOObject& o, Phase p)
      : o(o), p(p), t0(ceph::mono_clock::now()) {}
    ~Timed();
  };
  void phase_busy(ceph::timespan slept);
  void phase_retry();
  void log_phases(const DoutPrefixProvider* dpp);
#else
  struct Timed {
    Timed(RadosFSIOObject&, Phase) {}
  };
  void phase_busy(ceph::timespan) {}
  void phase_retry() {}
  void log_phases(const DoutPrefixProvider*) {}
#endif
};

} // namespace rgw::sal
