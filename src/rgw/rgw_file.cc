// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/rados/rgw_file.h"

#include <sys/types.h>
#include <sys/stat.h>

#include "rgw_lib.h"
#include "rgw_rados.h"
#include "rgw_resolve.h"
#include "rgw_op.h"
#include "rgw_rest.h"
#include "rgw_acl.h"
#include "rgw_acl_s3.h"
#include "rgw_frontend.h"
#include "rgw_request.h"
#include "rgw_process.h"
#include "rgw_rest_user.h"
#include "rgw_rest_s3.h"
#include "rgw_os_lib.h"
#include "rgw_auth_s3.h"
#include "rgw_user.h"
#include "rgw_bucket.h"

#include "rgw_file.h"
#include "rgw_lib_frontend.h"

#define dout_subsys ceph_subsys_rgw

using namespace rgw;

namespace rgw {

  extern RGWLib librgw;

  const string RGWFileHandle::root_name = "/";

  atomic<uint32_t> RGWLibFS::fs_inst;

  LookupFHResult RGWLibFS::stat_bucket(RGWFileHandle* parent,
				       const char *path, uint32_t flags)
  {
    LookupFHResult fhr{nullptr, 0};
    std::string bucket_name{path};
    RGWStatBucketRequest req(cct, get_user(), bucket_name);

    int rc = librgw.get_fe()->execute_req(&req);
    if ((rc == 0) &&
	(req.get_ret() == 0) &&
	(req.matched())) {
      fhr = lookup_fh(parent, path,
		      RGWFileHandle::FLAG_CREATE|
		      RGWFileHandle::FLAG_BUCKET);
    }
    return fhr;
  }

  LookupFHResult RGWLibFS::stat_leaf(RGWFileHandle* parent,
				     const char *path,
				     uint32_t flags)
  {
    /* find either-of <object_name>, <object_name/>, only one of
     * which should exist;  atomicity? */
    LookupFHResult fhr{nullptr, 0};
    std::string object_name{path};

    for (auto ix : { 0, 1 }) {
      switch (ix) {
      case 0:
      {
	RGWStatObjRequest req(cct, get_user(),
			      parent->bucket_name(), object_name,
			      RGWStatObjRequest::FLAG_NONE);
	int rc = librgw.get_fe()->execute_req(&req);
	if ((rc == 0) &&
	    (req.get_ret() == 0)) {
	  fhr = lookup_fh(parent, path, RGWFileHandle::FLAG_NONE);
	  goto done;
	}
      }
      break;
      case 1:
      {
	RGWStatLeafRequest req(cct, get_user(), parent, object_name);
	int rc = librgw.get_fe()->execute_req(&req);
	if ((rc == 0) &&
	    (req.get_ret() == 0)) {
	  if (req.matched) {
	    fhr = lookup_fh(parent, path,
			    RGWFileHandle::FLAG_CREATE|
			    ((req.is_dir) ?
			     RGWFileHandle::FLAG_DIRECTORY :
			     RGWFileHandle::FLAG_NONE));
	  }
	}
      }
      break;
      default:
	/* not reached */
	break;
      }
    }
  done:
    return fhr;
  } /* RGWLibFS::stat_leaf */

  void RGWLibFS::close()
  {
    flags |= FLAG_CLOSED;

    class ObjUnref
    {
      RGWLibFS* fs;
    public:
      ObjUnref(RGWLibFS* fs) : fs(fs) {}
      void operator()(RGWFileHandle* fh) const {
	lsubdout(fs->get_context(), rgw, 5)
	  << __func__
	  << fh->name
	  << " before ObjUnref refs=" << fh->get_refcnt()
	  << dendl;
	fs->fh_lru.unref(fh, cohort::lru::FLAG_NONE);
      }
    };

    /* force cache drain, forces objects to evict */
    fh_cache.drain(ObjUnref(this),
		  RGWFileHandle::FHCache::FLAG_LOCK);
    librgw.get_fe()->get_process()->unregister_fs(this);
    rele();
  } /* RGWLibFS::close */

  void RGWLibFS::gc()
  {
  } /* RGWLibFS::gc */

  bool RGWFileHandle::reclaim() {
    fs->fh_cache.remove(fh.fh_hk.object, this, cohort::lru::FLAG_NONE);
    return true;
  } /* RGWFileHandle::reclaim */

  int RGWFileHandle::write(uint64_t off, size_t len, size_t *bytes_written,
			   void *buffer)
  {
    using std::get;
    lock_guard guard(mtx);

    int rc = 0;
    buffer::list bl;
    bl.push_back(
      buffer::create_static(len, static_cast<char*>(buffer)));

    file* f = get<file>(&variant_type);
    if (! f)
      return -EISDIR;

    if (! f->write_req) {
      /* start */
      std::string object_name = full_object_name();
      f->write_req =
	new RGWWriteRequest(fs->get_context(), fs->get_user(), this,
			    bucket_name(), object_name);
      rc = librgw.get_fe()->start_req(f->write_req);
    }

    f->write_req->put_data(off, bl);
    rc = f->write_req->exec_continue();

    size_t min_size = off + len;
    if (min_size > get_size())
      set_size(min_size);

    *bytes_written = (rc == 0) ? len : 0;
    return rc;
  } /* RGWFileHandle::write */

  int RGWFileHandle::close()
  {
    lock_guard guard(mtx);

    int rc = 0;
    file* f = get<file>(&variant_type);
    if (f && (f->write_req)) {
      rc = librgw.get_fe()->finish_req(f->write_req);
      if (! rc) {
	rc = f->write_req->get_ret();
      }
      delete f->write_req;
      f->write_req = nullptr;
    }

    flags &= ~FLAG_OPEN;
    return rc;
  } /* RGWFileHandle::close */

  RGWFileHandle::file::~file()
  {
    delete write_req;
  }

  int RGWWriteRequest::exec_start() {
    struct req_state* s = get_state();

    // XXX check this
    need_calc_md5 = (dlo_manifest == NULL) && (slo_info == NULL);

    perfcounter->inc(l_rgw_put);
    op_ret = -EINVAL;

    // XXX check this
    if (s->object.empty()) {
      goto done;
    }

    op_ret = get_params();
    if (op_ret < 0)
      goto done;

    op_ret = get_system_versioning_params(s, &olh_epoch, &version_id);
    if (op_ret < 0) {
      goto done;
    }

    /* user-supplied MD5 check skipped (not supplied) */
    /* early quota check skipped--we don't have size yet */
    /* skipping user-supplied etag--we might have one in future, but
     * like data it and other attrs would arrive after open */
    processor = select_processor(*static_cast<RGWObjectCtx *>(s->obj_ctx),
				 &multipart);
    op_ret = processor->prepare(get_store(), NULL);

  done:
    return op_ret;
  } /* exec_start */

  int RGWWriteRequest::exec_continue()
  {
    struct req_state* s = get_state();
    op_ret = 0;

#if 0 // TODO: check offsets
    if (next_off != last_off)
      return -EIO;
#endif
    size_t len = data.length();
    if (! len)
      return 0;

    /* XXX won't see multipart */
    bool need_to_wait = (ofs == 0) && multipart;
    bufferlist orig_data;

    if (need_to_wait) {
      orig_data = data;
    }

    op_ret = put_data_and_throttle(processor, data, ofs,
				   (need_calc_md5 ? &hash : NULL), need_to_wait);
    if (op_ret < 0) {
      if (!need_to_wait || op_ret != -EEXIST) {
	ldout(s->cct, 20) << "processor->thottle_data() returned ret="
			  << op_ret << dendl;
	goto done;
      }

      ldout(s->cct, 5) << "NOTICE: processor->throttle_data() returned -EEXIST, need to restart write" << dendl;

      /* restore original data */
      data.swap(orig_data);

      /* restart processing with different oid suffix */
      dispose_processor(processor);
      processor = select_processor(*static_cast<RGWObjectCtx *>(s->obj_ctx),
				   &multipart);

      string oid_rand;
      char buf[33];
      gen_rand_alphanumeric(get_store()->ctx(), buf, sizeof(buf) - 1);
      oid_rand.append(buf);

      op_ret = processor->prepare(get_store(), &oid_rand);
      if (op_ret < 0) {
	ldout(s->cct, 0) << "ERROR: processor->prepare() returned "
			 << op_ret << dendl;
	goto done;
      }

      op_ret = put_data_and_throttle(processor, data, ofs, NULL, false);
      if (op_ret < 0) {
	goto done;
      }
    }
    bytes_written += len;

  done:
    return op_ret;
  } /* exec_continue */

  int RGWWriteRequest::exec_finish()
  {
    bufferlist bl, aclbl;
    map<string, bufferlist> attrs;
    map<string, string>::iterator iter;
    char calc_md5[CEPH_CRYPTO_MD5_DIGESTSIZE * 2 + 1];
    unsigned char m[CEPH_CRYPTO_MD5_DIGESTSIZE];
    struct req_state* s = get_state();

    s->obj_size = ofs; // XXX check ofs
    perfcounter->inc(l_rgw_put_b, s->obj_size);

    op_ret = get_store()->check_quota(s->bucket_owner.get_id(), s->bucket,
				      user_quota, bucket_quota, s->obj_size);
    if (op_ret < 0) {
      goto done;
    }

    if (need_calc_md5) {
      processor->complete_hash(&hash);
    }
    hash.Final(m);

    buf_to_hex(m, CEPH_CRYPTO_MD5_DIGESTSIZE, calc_md5);
    etag = calc_md5;

#if 0 /* XXX only in PostObj currently */
    if (supplied_md5_b64 && strcmp(calc_md5, supplied_md5)) {
      op_ret = -ERR_BAD_DIGEST;
      goto done;
    }
#endif

    policy.encode(aclbl);

    attrs[RGW_ATTR_ACL] = aclbl;

    /* XXX most of the following cases won't currently arise */
    if (unlikely(!! dlo_manifest)) {
      op_ret = encode_dlo_manifest_attr(dlo_manifest, attrs);
      if (op_ret < 0) {
	ldout(s->cct, 0) << "bad user manifest: " << dlo_manifest << dendl;
	goto done;
      }
      complete_etag(hash, &etag);
      ldout(s->cct, 10) << __func__ << ": calculated md5 for user manifest: "
			<< etag << dendl;
    }

    if (unlikely(!! slo_info)) {
      bufferlist manifest_bl;
      ::encode(*slo_info, manifest_bl);
      attrs[RGW_ATTR_SLO_MANIFEST] = manifest_bl;

      hash.Update((byte *)slo_info->raw_data, slo_info->raw_data_len);
      complete_etag(hash, &etag);
      ldout(s->cct, 10) << __func__ << ": calculated md5 for user manifest: "
			<< etag << dendl;
    }

    if (supplied_etag && etag.compare(supplied_etag) != 0) {
      op_ret = -ERR_UNPROCESSABLE_ENTITY;
      goto done;
    }
    bl.append(etag.c_str(), etag.size() + 1);
    attrs[RGW_ATTR_ETAG] = bl;

    for (iter = s->generic_attrs.begin(); iter != s->generic_attrs.end();
	 ++iter) {
      bufferlist& attrbl = attrs[iter->first];
      const string& val = iter->second;
      attrbl.append(val.c_str(), val.size() + 1);
    }

    rgw_get_request_metadata(s->cct, s->info, attrs);
    encode_delete_at_attr(delete_at, attrs);

    /* Add a custom metadata to expose the information whether an object
     * is an SLO or not. Appending the attribute must be performed AFTER
     * processing any input from user in order to prohibit overwriting. */
    if (unlikely(!! slo_info)) {
      bufferlist slo_userindicator_bl;
      ::encode("True", slo_userindicator_bl);
      attrs[RGW_ATTR_SLO_UINDICATOR] = slo_userindicator_bl;
    }

    op_ret = processor->complete(etag, &mtime, 0, attrs, delete_at, if_match,
				 if_nomatch);
    if (! op_ret) {
      /* update stats */
      rgw_fh->set_mtime({mtime, 0});
      rgw_fh->set_size(bytes_written);
    }

  done:
    dispose_processor(processor);
    perfcounter->tinc(l_rgw_put_lat,
		      (ceph_clock_now(s->cct) - s->time));
    return op_ret;
  } /* exec_finish */

} /* namespace rgw */

/* librgw */
extern "C" {

/*
 attach rgw namespace
*/
  int rgw_mount(librgw_t rgw, const char *uid, const char *acc_key,
		const char *sec_key, struct rgw_fs **rgw_fs)
{
  int rc = 0;

  /* stash access data for "mount" */
  RGWLibFS* new_fs = new RGWLibFS(static_cast<CephContext*>(rgw), uid, acc_key,
				  sec_key);
  assert(new_fs);

  rc = new_fs->authorize(librgw.get_store());
  if (rc != 0) {
    delete new_fs;
    return -EINVAL;
  }

  /* register fs for shared gc */
  librgw.get_fe()->get_process()->register_fs(new_fs);

  struct rgw_fs *fs = new_fs->get_fs();
  fs->rgw = rgw;

  /* XXX we no longer assume "/" is unique, but we aren't tracking the
   * roots atm */

  *rgw_fs = fs;

  return 0;
}

/*
 detach rgw namespace
*/
int rgw_umount(struct rgw_fs *rgw_fs)
{
  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);
  fs->close();
  fs->rele();
  return 0;
}

/*
  get filesystem attributes
*/
int rgw_statfs(struct rgw_fs *rgw_fs,
	       struct rgw_file_handle *parent_fh,
	       struct rgw_statvfs *vfs_st)
{
  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);

  /* XXX for now, just publish a huge capacity and
   * limited utiliztion */
  vfs_st->f_bsize = 1024*1024 /* 1M */;
  vfs_st->f_frsize = 1024;    /* minimal allocation unit (who cares) */
  vfs_st->f_blocks = UINT64_MAX;
  vfs_st->f_bfree = UINT64_MAX;
  vfs_st->f_bavail = UINT64_MAX;
  vfs_st->f_files = 1024; /* object count, do we have an est? */
  vfs_st->f_ffree = UINT64_MAX;
  vfs_st->f_fsid[0] = fs->get_inst();
  vfs_st->f_fsid[1] = fs->get_inst();
  vfs_st->f_flag = 0;
  vfs_st->f_namemax = 4096;
  return 0;
}

/*
  generic create -- creates a regular file
*/
int rgw_create(struct rgw_fs *rgw_fs,
	       struct rgw_file_handle *parent_fh,
	       const char *name, mode_t mode, struct stat *st,
	       struct rgw_file_handle **fh)
{
  /* XXX a CREATE operation can be a precursor to the canonical
   * OPEN, WRITE*, CLOSE transaction which writes or overwrites
   * an object in S3 */
  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);

  RGWFileHandle* parent = get_rgwfh(parent_fh);
  if ((! parent) ||
      (parent->is_root()) ||
      (parent->is_file())) {
    /* bad parent */
    return -EINVAL;
  }

  using std::get;

  LookupFHResult fhr = fs->lookup_fh(parent, name);
  RGWFileHandle* rgw_fh = get<0>(fhr);

  if (! rgw_fh)
    return -EINVAL;


  /* mark if we created it */
  if (get<1>(fhr) & RGWFileHandle::FLAG_CREATE) {
        /* fill in stat data */
    time_t now = time(0);
    rgw_fh->set_times(now);
    rgw_fh->open_for_create();
  }

  (void) rgw_fh->stat(st);

  struct rgw_file_handle *rfh = rgw_fh->get_fh();
  *fh = rfh;

  return 0;
}

/*
  create a new directory
*/
int rgw_mkdir(struct rgw_fs *rgw_fs,
	      struct rgw_file_handle *parent_fh,
	      const char *name, mode_t mode, struct stat *st,
	      struct rgw_file_handle **fh)
{
  int rc, rc2;

  /* XXXX remove uri, deal with bucket names */
  string uri;

  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);
  CephContext* cct = static_cast<CephContext*>(rgw_fs->rgw);

  RGWFileHandle* parent = get_rgwfh(parent_fh);
  if (! parent) {
    /* bad parent */
    return -EINVAL;
  }

  LookupFHResult fhr;
  RGWFileHandle* rgw_fh = nullptr;

  if (parent->is_root()) {
    /* bucket */
    uri += "/"; /* XXX */
    uri += name;
    RGWCreateBucketRequest req(cct, fs->get_user(), uri);
    rc = librgw.get_fe()->execute_req(&req);
    rc2 = req.get_ret();
  } else {
    /* create an object representing the directory (naive version) */
    buffer::list bl;
    fhr = fs->lookup_fh(parent, name, RGWFileHandle::FLAG_PSEUDO);
    rgw_fh = get<0>(fhr);
    string dir_name = rgw_fh->full_object_name() + "/";
    RGWPutObjRequest req(cct, fs->get_user(), rgw_fh->bucket_name(),
			 dir_name, bl);
    rc = librgw.get_fe()->execute_req(&req);
    rc2 = req.get_ret();
  }

  if ((rc == 0) &&
      (rc2 == 0)) {
    fhr = fs->lookup_fh(parent, name, RGWFileHandle::FLAG_CREATE);
    rgw_fh = get<0>(fhr);
    if (rgw_fh) {
      struct rgw_file_handle *rfh = rgw_fh->get_fh();
      *fh = rfh;
    } else
      rc = -EIO;
  }

  return rc;
}

/*
  rename object
*/
int rgw_rename(struct rgw_fs *rgw_fs,
	       struct rgw_file_handle *olddir, const char* old_name,
	       struct rgw_file_handle *newdir, const char* new_name)
{
  /* -ENOTSUP */
  return -EINVAL;
}

/*
  remove file or directory
*/
int rgw_unlink(struct rgw_fs *rgw_fs, struct rgw_file_handle *parent_fh,
	      const char *name)
{
  int rc = 0;

  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);
  CephContext* cct = static_cast<CephContext*>(rgw_fs->rgw);

  RGWFileHandle* parent = get_rgwfh(parent_fh);

  if (parent->is_root()) {
    /* XXXX remove uri and deal with bucket and object names */
    string uri = "/";
    uri += name;
    RGWDeleteBucketRequest req(cct, fs->get_user(), uri);
    rc = librgw.get_fe()->execute_req(&req);
  } else {
    /*
     * leaf object
     */
    /* XXX we must peform a hard lookup to deduce the type of
     * object to be deleted ("foo" vs. "foo/"), and as a side effect
     * can do no further work if no object existed */
    struct rgw_file_handle *fh;
    rc = rgw_lookup(rgw_fs, parent_fh, name, &fh, RGW_LOOKUP_FLAG_NONE);
    if (! rc)  {
      /* rgw_fh ref+ */
      RGWFileHandle* rgw_fh = get_rgwfh(fh);
      std::string oname = rgw_fh->full_object_name();
      RGWDeleteObjRequest req(cct, fs->get_user(), parent->bucket_name(),
			      oname);
      rc = librgw.get_fe()->execute_req(&req);
      /* release */
      (void) rgw_fh_rele(rgw_fs, fh, 0 /* flags */);
    }
  }

  return rc;
}

void dump_buckets(void) {
  /* get the bucket list */
  string marker; // XXX need to match offset
  string end_marker;
  uint64_t bucket_count, bucket_objcount;

  RGWUserBuckets buckets;
  uint64_t max_buckets = g_ceph_context->_conf->rgw_list_buckets_max_chunk;

  RGWRados* store = librgw.get_store();

  /* XXX check offsets */
  uint64_t ix = 3;
  rgw_user uid("testuser");
  int rc = rgw_read_user_buckets(store, uid, buckets, marker, end_marker,
				 max_buckets, true);
  if (rc == 0) {
    bucket_count = 0;
    bucket_objcount = 0;
    map<string, RGWBucketEnt>& m = buckets.get_buckets();
    for (auto& ib : m) {
      RGWBucketEnt& bent = ib.second;
      bucket_objcount += bent.count;
      marker = ib.first;
      std::cout << bent.bucket.name.c_str() << " ix: " << ix++ << std::endl;
    }
    bucket_count += m.size();
  }
} /* dump_buckets */

/*
  lookup object by name (POSIX style)
*/
int rgw_lookup(struct rgw_fs *rgw_fs,
	      struct rgw_file_handle *parent_fh, const char* path,
	      struct rgw_file_handle **fh, uint32_t flags)
{
  //CephContext* cct = static_cast<CephContext*>(rgw_fs->rgw);
  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);

  RGWFileHandle* parent = get_rgwfh(parent_fh);
  if ((! parent) ||
      (! parent->is_dir())) {
    /* bad parent */
    return -EINVAL;
  }

  RGWFileHandle* rgw_fh;
  LookupFHResult fhr;

  if (parent->is_root()) {
    /* special: lookup on root itself */
    if (strcmp(path, "/") == 0) {
      rgw_fh = fs->ref(parent);
    } else {
      fhr = fs->stat_bucket(parent, path, RGWFileHandle::FLAG_NONE);
      rgw_fh = get<0>(fhr);
      if (! rgw_fh)
	return -ENOENT;
    }
  } else {
    fhr = fs->stat_leaf(parent, path, RGWFileHandle::FLAG_NONE);
    if (! get<0>(fhr)) {
      if (! (flags & RGW_LOOKUP_FLAG_CREATE))
	return -ENOENT;
      else
	fhr = fs->lookup_fh(parent, path, RGWFileHandle::FLAG_CREATE);
    }
    rgw_fh = get<0>(fhr);
  } /* !root */

  struct rgw_file_handle *rfh = rgw_fh->get_fh();
  *fh = rfh;

  return 0;
} /* rgw_lookup */

/*
  lookup object by handle (NFS style)
*/
int rgw_lookup_handle(struct rgw_fs *rgw_fs, struct rgw_fh_hk *fh_hk,
		      struct rgw_file_handle **fh, uint32_t flags)
{
  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);

  RGWFileHandle* rgw_fh = fs->lookup_handle(*fh_hk);
  if (! rgw_fh) {
    /* not found */
    return -ENOENT;
  }

  struct rgw_file_handle *rfh = rgw_fh->get_fh();
  *fh = rfh;

  return 0;
}

/*
 * release file handle
 */
int rgw_fh_rele(struct rgw_fs *rgw_fs, struct rgw_file_handle *fh,
		uint32_t flags)
{
  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);
  RGWFileHandle* rgw_fh = get_rgwfh(fh);
  fs->unref(rgw_fh);

  return 0;
}

/*
   get unix attributes for object
*/
int rgw_getattr(struct rgw_fs *rgw_fs,
		struct rgw_file_handle *fh, struct stat *st)
{
  CephContext* cct = static_cast<CephContext*>(rgw_fs->rgw);
  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);

  RGWFileHandle* rgw_fh = get_rgwfh(fh);

  if (rgw_fh->is_root() ||
      rgw_fh->is_bucket()) {
    /* XXX nothing */
  } else {
    /* object */

    /* an object being created isn't expected to exist (if it does,
     * we'll detect the conflict later */
    if (rgw_fh->creating())
      goto done;

    const std::string& bname = rgw_fh->bucket_name();
    const std::string& oname = rgw_fh->object_name();

    RGWStatObjRequest req(cct, fs->get_user(), bname, oname,
			  RGWStatObjRequest::FLAG_NONE);

    int rc = librgw.get_fe()->execute_req(&req);
    if ((rc != 0) ||
	(req.get_ret() != 0)) {
      /* XXX EINVAL is likely illegal protocol return--if the object
       * should but doesn't exist, it should probably be ENOENT */
      return -EINVAL;
    }

    /* fill in stat data */
    rgw_fh->set_size(req.size());
    rgw_fh->set_ctime({req.ctime(), 0});
    rgw_fh->set_mtime({req.mtime(), 0});
    rgw_fh->set_atime({req.mtime(), 0});
  }

done:
  (void) rgw_fh->stat(st);
  return 0;
}

/*
  set unix attributes for object
*/
int rgw_setattr(struct rgw_fs *rgw_fs,
		struct rgw_file_handle *fh, struct stat *st,
		uint32_t mask)
{
  /* XXX no-op */
  return 0;
}

/*
   truncate file
*/
int rgw_truncate(struct rgw_fs *rgw_fs,
		 struct rgw_file_handle *fh, uint64_t size)
{
  return 0;
}

/*
   open file
*/
int rgw_open(struct rgw_fs *rgw_fs,
	     struct rgw_file_handle *fh, uint32_t flags)
{
  RGWFileHandle* rgw_fh = get_rgwfh(fh);

  /* XXX 
   * need to track specific opens--at least read opens and
   * a write open;  we need to know when a write open is returned,
   * that closes a write transaction
   *
   * for now, we will support single-open only, it's preferable to
   * anything we can otherwise do without access to the NFS state
   */
  if (! rgw_fh->is_file())
    return -EISDIR;

  // convert flags
  uint32_t oflags = 0;
  return rgw_fh->open(oflags);
}

/*
   close file
*/
int rgw_close(struct rgw_fs *rgw_fs,
	      struct rgw_file_handle *fh, uint32_t flags)
{
  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);
  RGWFileHandle* rgw_fh = get_rgwfh(fh);
  int rc = rgw_fh->close(/* XXX */);

  if (flags & RGW_CLOSE_FLAG_RELE)
    fs->unref(rgw_fh);

  return rc;
}

int rgw_readdir(struct rgw_fs *rgw_fs,
		struct rgw_file_handle *parent_fh, uint64_t *offset,
		rgw_readdir_cb rcb, void *cb_arg, bool *eof)
{
  int rc;

  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);
  CephContext* cct = static_cast<CephContext*>(rgw_fs->rgw);

  /* TODO:
   * deal with markers (continuation)
   * deal with authorization
   * consider non-default tenancy/user and bucket layouts
   */
  RGWFileHandle* parent = get_rgwfh(parent_fh);
  if (! parent) {
    /* bad parent */
    return -EINVAL;
  }

  if (parent->is_root()) {
    RGWListBucketsRequest req(cct, fs->get_user(), parent, rcb, cb_arg, offset);
    rc = librgw.get_fe()->execute_req(&req);
  } else {
    // XXX finish marker handling
    rgw_obj_key marker{"", ""};
    RGWReaddirRequest req(cct, fs->get_user(), parent, rcb, cb_arg, offset);
    rc = librgw.get_fe()->execute_req(&req);

    /* XXX update link count (incorrectly) */
    parent->set_nlink(3 + *offset);
  }

  /* XXXX request MUST set this */
  *eof = true; // XXX move into RGGWListBucket(s)Request

  return rc;
}

/*
   read data from file
*/
int rgw_read(struct rgw_fs *rgw_fs,
	    struct rgw_file_handle *fh, uint64_t offset,
	    size_t length, size_t *bytes_read, void *buffer)
{
  CephContext* cct = static_cast<CephContext*>(rgw_fs->rgw);
  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);
  RGWFileHandle* rgw_fh = get_rgwfh(fh);

  if (! rgw_fh->is_file())
    return -EINVAL;

  RGWReadRequest req(cct, fs->get_user(), rgw_fh->bucket_name(),
		    rgw_fh->object_name(), offset, length,
		    buffer);

  int rc = librgw.get_fe()->execute_req(&req);
  if ((rc == 0) &&
      (req.get_ret() == 0)) {
    *bytes_read = req.nread;
  }

  return rc;
}

/*
   write data to file
*/
int rgw_write(struct rgw_fs *rgw_fs,
	      struct rgw_file_handle *fh, uint64_t offset,
	      size_t length, size_t *bytes_written, void *buffer)
{
  RGWFileHandle* rgw_fh = get_rgwfh(fh);

  if (! rgw_fh->is_file())
    return -EISDIR;

  if (! rgw_fh->is_open())
    return -EPERM;

  return rgw_fh->write(offset, length, bytes_written, buffer);
}

/*
   read data from file (vector)
*/
class RGWReadV
{
  buffer::list bl;
  struct rgw_vio* vio;

public:
  RGWReadV(buffer::list& _bl, rgw_vio* _vio) : vio(_vio) {
    bl.claim(_bl);
  }

  struct rgw_vio* get_vio() { return vio; }

  const std::list<buffer::ptr>& buffers() { return bl.buffers(); }

  unsigned /* XXX */ length() { return bl.length(); }

};

void rgw_readv_rele(struct rgw_uio *uio, uint32_t flags)
{
  RGWReadV* rdv = static_cast<RGWReadV*>(uio->uio_p1);
  rdv->~RGWReadV();
  ::operator delete(rdv);
}

int rgw_readv(struct rgw_fs *rgw_fs,
	      struct rgw_file_handle *fh, rgw_uio *uio)
{
#if 0 /* XXX */
  CephContext* cct = static_cast<CephContext*>(rgw_fs->rgw);
  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);
  RGWFileHandle* rgw_fh = get_rgwfh(fh);

  if (! rgw_fh->is_file())
    return -EINVAL;

  int rc = 0;

  buffer::list bl;
  RGWGetObjRequest req(cct, fs->get_user(), rgw_fh->bucket_name(),
		      rgw_fh->object_name(), uio->uio_offset, uio->uio_resid,
		      bl);
  req.do_hexdump = false;

  rc = librgw.get_fe()->execute_req(&req);

  if (! rc) {
    RGWReadV* rdv = static_cast<RGWReadV*>(
      ::operator new(sizeof(RGWReadV) +
		    (bl.buffers().size() * sizeof(struct rgw_vio))));

    (void) new (rdv)
      RGWReadV(bl, reinterpret_cast<rgw_vio*>(rdv+sizeof(RGWReadV)));

    uio->uio_p1 = rdv;
    uio->uio_cnt = rdv->buffers().size();
    uio->uio_resid = rdv->length();
    uio->uio_vio = rdv->get_vio();
    uio->uio_rele = rgw_readv_rele;

    int ix = 0;
    auto& buffers = rdv->buffers();
    for (auto& bp : buffers) {
      rgw_vio *vio = &(uio->uio_vio[ix]);
      vio->vio_base = const_cast<char*>(bp.c_str());
      vio->vio_len = bp.length();
      vio->vio_u1 = nullptr;
      vio->vio_p1 = nullptr;
      ++ix;
    }
  }

  return rc;
#else
  return 0;
#endif
}

/*
   write data to file (vector)
*/
  int rgw_writev(struct rgw_fs *rgw_fs, struct rgw_file_handle *fh,
		rgw_uio *uio)
{
  CephContext* cct = static_cast<CephContext*>(rgw_fs->rgw);
  RGWLibFS *fs = static_cast<RGWLibFS*>(rgw_fs->fs_private);
  RGWFileHandle* rgw_fh = get_rgwfh(fh);

  if (! rgw_fh->is_file())
    return -EINVAL;

  buffer::list bl;
  for (unsigned int ix = 0; ix < uio->uio_cnt; ++ix) {
    rgw_vio *vio = &(uio->uio_vio[ix]);
    bl.push_back(
      buffer::create_static(vio->vio_len,
			    static_cast<char*>(vio->vio_base)));
  }

  std::string oname = rgw_fh->full_object_name();
  RGWPutObjRequest req(cct, fs->get_user(), rgw_fh->bucket_name(),
		       oname, bl);

  int rc = librgw.get_fe()->execute_req(&req);

  /* XXX update size (in request) */

  return rc;
}

/*
   sync written data
*/
int rgw_fsync(struct rgw_fs *rgw_fs, struct rgw_file_handle *handle)
{
  return 0;
}

} /* extern "C" */
