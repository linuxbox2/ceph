// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

/*
 * Ceph - scalable distributed file system
 *
 * Copyright contributors to the Ceph project
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#pragma once

#include "rgw_sal_filter.h"
#include "rgw_sal_store.h"
#include "common/dout.h" 

namespace rgw { namespace sal {

class POSIXDriver : public FilterDriver {
private:
  std::string base_path;
  int root_fd;

public:
  POSIXDriver(Driver* _next) : FilterDriver(_next) 
  { }
  virtual ~POSIXDriver() { }

  virtual int initialize(CephContext *cct, const DoutPrefixProvider *dpp) override;
  virtual std::unique_ptr<User> get_user(const rgw_user& u) override;
  virtual int get_user_by_access_key(const DoutPrefixProvider* dpp, const
				     std::string& key, optional_yield y,
				     std::unique_ptr<User>* user) override;
  virtual int get_user_by_email(const DoutPrefixProvider* dpp, const
				std::string& email, optional_yield y,
				std::unique_ptr<User>* user) override;
  virtual int get_user_by_swift(const DoutPrefixProvider* dpp, const
				std::string& user_str, optional_yield y,
				std::unique_ptr<User>* user) override;
  virtual std::unique_ptr<Object> get_object(const rgw_obj_key& k) override;
  virtual int get_bucket(User* u, const RGWBucketInfo& i,
			 std::unique_ptr<Bucket>* bucket) override;
  virtual int get_bucket(const DoutPrefixProvider* dpp, User* u, const
			 rgw_bucket& b, std::unique_ptr<Bucket>* bucket,
			 optional_yield y) override;
  virtual int get_bucket(const DoutPrefixProvider* dpp, User* u, const
			 std::string& tenant, const std::string& name,
			 std::unique_ptr<Bucket>* bucket, optional_yield y) override;

  virtual std::unique_ptr<Writer> get_append_writer(const DoutPrefixProvider *dpp,
				  optional_yield y,
				  std::unique_ptr<rgw::sal::Object> _head_obj,
				  const rgw_user& owner,
				  const rgw_placement_rule
				  *ptail_placement_rule,
				  const std::string& unique_tag,
				  uint64_t position,
				  uint64_t *cur_accounted_size) override;
  virtual std::unique_ptr<Writer> get_atomic_writer(const DoutPrefixProvider *dpp,
				  optional_yield y,
				  std::unique_ptr<rgw::sal::Object> _head_obj,
				  const rgw_user& owner,
				  const rgw_placement_rule *ptail_placement_rule,
				  uint64_t olh_epoch,
				  const std::string& unique_tag) override;

  virtual void finalize(void) override;
  virtual void register_admin_apis(RGWRESTMgr* mgr) override;

  virtual std::unique_ptr<Notification> get_notification(rgw::sal::Object* obj,
				 rgw::sal::Object* src_obj, struct req_state* s,
				 rgw::notify::EventType event_type, optional_yield y,
				 const std::string* object_name=nullptr) override;

  virtual std::unique_ptr<Notification> get_notification(
                                  const DoutPrefixProvider* dpp,
                                  rgw::sal::Object* obj,
                                  rgw::sal::Object* src_obj,
                                  rgw::notify::EventType event_type,
                                  rgw::sal::Bucket* _bucket,
                                  std::string& _user_id,
                                  std::string& _user_tenant,
                                  std::string& _req_id,
                                  optional_yield y) override;

  /* Internal APIs */
  int get_root_fd() { return root_fd; }
  const std::string& get_base_path() const { return base_path; }
};

class POSIXUser : public FilterUser {
private:
  POSIXDriver* driver;

public:
  POSIXUser(std::unique_ptr<User> _next, POSIXDriver* _driver) : 
    FilterUser(std::move(_next)),
    driver(_driver) {}
  virtual ~POSIXUser() = default;

  virtual int list_buckets(const DoutPrefixProvider* dpp,
			   const std::string& marker, const std::string& end_marker,
			   uint64_t max, bool need_stats, BucketList& buckets,
			   optional_yield y) override;
  virtual int create_bucket(const DoutPrefixProvider* dpp,
                            const rgw_bucket& b,
                            const std::string& zonegroup_id,
                            rgw_placement_rule& placement_rule,
                            std::string& swift_ver_location,
                            const RGWQuotaInfo* pquota_info,
                            const RGWAccessControlPolicy& policy,
                            Attrs& attrs,
                            RGWBucketInfo& info,
                            obj_version& ep_objv,
                            bool exclusive,
                            bool obj_lock_enabled,
                            bool* existed,
                            req_info& req_info,
                            std::unique_ptr<Bucket>* bucket,
                            optional_yield y) override;
  virtual Attrs& get_attrs() override { return next->get_attrs(); }
  virtual void set_attrs(Attrs& _attrs) override { next->set_attrs(_attrs); }
  virtual int read_attrs(const DoutPrefixProvider* dpp, optional_yield y) override;
  virtual int merge_and_store_attrs(const DoutPrefixProvider* dpp, Attrs&
				    new_attrs, optional_yield y) override;
  virtual int load_user(const DoutPrefixProvider* dpp, optional_yield y) override;
  virtual int store_user(const DoutPrefixProvider* dpp, optional_yield y, bool
			 exclusive, RGWUserInfo* old_info = nullptr) override;
  virtual int remove_user(const DoutPrefixProvider* dpp, optional_yield y) override;
};

class POSIXBucket : public StoreBucket {
private:
  POSIXDriver* driver;
  int dir_fd{-1};
  struct statx stx;
  bool stat_done{false};
  RGWAccessControlPolicy acls;

public:
  POSIXBucket(POSIXDriver *_dr, const rgw_bucket& _b, User* _u)
    : StoreBucket(_b, _u),
    driver(_dr),
    acls()
    { }

  POSIXBucket(POSIXDriver *_dr, const RGWBucketEnt& _e, User* _u)
    : StoreBucket(_e, _u),
    driver(_dr),
    acls()
    { }

  POSIXBucket(POSIXDriver *_dr, const RGWBucketInfo& _i, User* _u)
    : StoreBucket(_i, _u),
    driver(_dr),
    acls()
    { }

  virtual ~POSIXBucket() = default;

  virtual void set_owner(rgw::sal::User* _owner) override {
    StoreBucket::set_owner(_owner);
    info.owner = owner->get_id();
  }
  virtual std::unique_ptr<Object> get_object(const rgw_obj_key& key) override;
  virtual int list(const DoutPrefixProvider* dpp, ListParams&, int,
		   ListResults&, optional_yield y) override;
  virtual int merge_and_store_attrs(const DoutPrefixProvider* dpp,
				    Attrs& new_attrs, optional_yield y) override;
  virtual int remove_bucket(const DoutPrefixProvider* dpp, bool delete_children,
			    bool forward_to_master, req_info* req_info,
			    optional_yield y) override;
  virtual int remove_bucket_bypass_gc(int concurrent_max,
				      bool keep_index_consistent,
				      optional_yield y,
				      const DoutPrefixProvider *dpp) override;
  virtual int load_bucket(const DoutPrefixProvider* dpp, optional_yield y,
			  bool get_stats = false) override;
  virtual RGWAccessControlPolicy& get_acl(void) override { return acls; }
  virtual int set_acl(const DoutPrefixProvider* dpp, RGWAccessControlPolicy& acl,
		      optional_yield y) override;
  virtual int read_stats(const DoutPrefixProvider *dpp,
			 const bucket_index_layout_generation& idx_layout,
			 int shard_id, std::string* bucket_ver, std::string* master_ver,
			 std::map<RGWObjCategory, RGWStorageStats>& stats,
			 std::string* max_marker = nullptr,
			 bool* syncstopped = nullptr) override;
  virtual int read_stats_async(const DoutPrefixProvider *dpp,
			       const bucket_index_layout_generation& idx_layout,
			       int shard_id, RGWGetBucketStats_CB* ctx) override;
  virtual int sync_user_stats(const DoutPrefixProvider *dpp, optional_yield y) override;
  virtual int update_container_stats(const DoutPrefixProvider* dpp) override;
  virtual int check_bucket_shards(const DoutPrefixProvider* dpp) override;
  virtual int chown(const DoutPrefixProvider* dpp, User& new_user, optional_yield y) override;
  virtual int put_info(const DoutPrefixProvider* dpp, bool exclusive,
		       ceph::real_time mtime) override;
  virtual int check_empty(const DoutPrefixProvider* dpp, optional_yield y) override;
  virtual int check_quota(const DoutPrefixProvider *dpp, RGWQuota& quota, uint64_t obj_size, optional_yield y, bool check_size_only = false) override;
  virtual int try_refresh_info(const DoutPrefixProvider* dpp, ceph::real_time* pmtime) override;
  virtual int read_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch,
			 uint64_t end_epoch, uint32_t max_entries, bool* is_truncated,
			 RGWUsageIter& usage_iter, std::map<rgw_user_bucket, rgw_usage_log_entry>& usage) override;
  virtual int trim_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch) override;
  virtual int remove_objs_from_index(const DoutPrefixProvider *dpp, std::list<rgw_obj_index_key>& objs_to_unlink) override;
  virtual int check_index(const DoutPrefixProvider *dpp, std::map<RGWObjCategory, RGWStorageStats>& existing_stats, std::map<RGWObjCategory, RGWStorageStats>& calculated_stats) override;
  virtual int rebuild_index(const DoutPrefixProvider *dpp) override;
  virtual int set_tag_timeout(const DoutPrefixProvider *dpp, uint64_t timeout) override;
  virtual int purge_instance(const DoutPrefixProvider* dpp) override;

  virtual std::unique_ptr<Bucket> clone() override {
    return std::make_unique<POSIXBucket>(*this);
  }

  virtual std::unique_ptr<MultipartUpload> get_multipart_upload(
				const std::string& oid,
				std::optional<std::string> upload_id=std::nullopt,
				ACLOwner owner={}, ceph::real_time mtime=real_clock::now()) override;
  virtual int list_multiparts(const DoutPrefixProvider *dpp,
			      const std::string& prefix,
			      std::string& marker,
			      const std::string& delim,
			      const int& max_uploads,
			      std::vector<std::unique_ptr<MultipartUpload>>& uploads,
			      std::map<std::string, bool> *common_prefixes,
			      bool *is_truncated) override;
  virtual int abort_multiparts(const DoutPrefixProvider* dpp,
			       CephContext* cct) override;

  /* Internal APIs */
  int create(const DoutPrefixProvider *dpp, optional_yield y, bool* existed);
  void set_stat(struct statx _stx) { stx = _stx; stat_done = true; }
  int get_dir_fd(const DoutPrefixProvider *dpp) { open(dpp); return dir_fd; }
  /* TODO dang Escape the bucket name for file use */
  const std::string& get_fname() { return get_name(); }
private:
  int open(const DoutPrefixProvider *dpp);
  int close(const DoutPrefixProvider* dpp);
  int stat(const DoutPrefixProvider *dpp);
};

class POSIXObject : public StoreObject {
private:
  POSIXDriver* driver;
  RGWAccessControlPolicy acls;
  int obj_fd{-1};
  struct statx stx;
  bool stat_done{false};
  std::string temp_fname;

public:
  struct POSIXReadOp : StoreReadOp {
    POSIXObject* source;

    POSIXReadOp(POSIXObject* _source) :
      source(_source) {}
    virtual ~POSIXReadOp() = default;

    virtual int prepare(optional_yield y, const DoutPrefixProvider* dpp) override;
    virtual int read(int64_t ofs, int64_t end, bufferlist& bl, optional_yield y,
		     const DoutPrefixProvider* dpp) override;
    virtual int iterate(const DoutPrefixProvider* dpp, int64_t ofs, int64_t end,
			RGWGetDataCB* cb, optional_yield y) override;
    virtual int get_attr(const DoutPrefixProvider* dpp, const char* name,
			 bufferlist& dest, optional_yield y) override;
  };

  struct POSIXDeleteOp : StoreDeleteOp {
    POSIXObject* source;

    POSIXDeleteOp(POSIXObject* _source) :
      source(_source) {}
    virtual ~POSIXDeleteOp() = default;

    virtual int delete_obj(const DoutPrefixProvider* dpp, optional_yield y) override;
  };

  POSIXObject(POSIXDriver *_dr, const rgw_obj_key& _k)
    : StoreObject(_k),
    driver(_dr),
    acls() {}

  POSIXObject(POSIXDriver* _driver, const rgw_obj_key& _k, Bucket* _b) :
    StoreObject(_k, _b),
    driver(_driver),
    acls() {}

  POSIXObject(POSIXObject& _o) :
    StoreObject(_o),
    driver(_o.driver) {}

  virtual ~POSIXObject() { close(); }

  virtual int delete_object(const DoutPrefixProvider* dpp,
			    optional_yield y,
			    bool prevent_versioning = false) override;
  virtual int delete_obj_aio(const DoutPrefixProvider* dpp, RGWObjState* astate,
			     Completions* aio, bool keep_index_consistent,
			     optional_yield y) override;
  virtual int copy_object(User* user,
               req_info* info, const rgw_zone_id& source_zone,
               rgw::sal::Object* dest_object, rgw::sal::Bucket* dest_bucket,
               rgw::sal::Bucket* src_bucket,
               const rgw_placement_rule& dest_placement,
               ceph::real_time* src_mtime, ceph::real_time* mtime,
               const ceph::real_time* mod_ptr, const ceph::real_time* unmod_ptr,
               bool high_precision_time,
               const char* if_match, const char* if_nomatch,
               AttrsMod attrs_mod, bool copy_if_newer, Attrs& attrs,
               RGWObjCategory category, uint64_t olh_epoch,
               boost::optional<ceph::real_time> delete_at,
               std::string* version_id, std::string* tag, std::string* etag,
               void (*progress_cb)(off_t, void *), void* progress_data,
               const DoutPrefixProvider* dpp, optional_yield y) override;
  virtual RGWAccessControlPolicy& get_acl(void) override { return acls; }
  virtual int set_acl(const RGWAccessControlPolicy& acl) override { acls = acl; return 0; }
  virtual int get_obj_state(const DoutPrefixProvider* dpp, RGWObjState **state, optional_yield y, bool follow_olh = true) override;
  virtual int set_obj_attrs(const DoutPrefixProvider* dpp, Attrs* setattrs,
			    Attrs* delattrs, optional_yield y) override;
  virtual int get_obj_attrs(optional_yield y, const DoutPrefixProvider* dpp,
			    rgw_obj* target_obj = NULL) override;
  virtual int modify_obj_attrs(const char* attr_name, bufferlist& attr_val,
			       optional_yield y, const DoutPrefixProvider* dpp) override;
  virtual int delete_obj_attrs(const DoutPrefixProvider* dpp, const char* attr_name,
			       optional_yield y) override;
  virtual bool is_expired() override;
  virtual void gen_rand_obj_instance_name() override;
  virtual std::unique_ptr<MPSerializer> get_serializer(const DoutPrefixProvider *dpp,
						       const std::string& lock_name) override;
  virtual int transition(Bucket* bucket,
			 const rgw_placement_rule& placement_rule,
			 const real_time& mtime,
			 uint64_t olh_epoch,
			 const DoutPrefixProvider* dpp,
			 optional_yield y) override;
  virtual int transition_to_cloud(Bucket* bucket,
			 rgw::sal::PlacementTier* tier,
			 rgw_bucket_dir_entry& o,
			 std::set<std::string>& cloud_targets,
			 CephContext* cct,
			 bool update_object,
			 const DoutPrefixProvider* dpp,
			 optional_yield y) override;
  virtual bool placement_rules_match(rgw_placement_rule& r1, rgw_placement_rule& r2) override;
  virtual int dump_obj_layout(const DoutPrefixProvider *dpp, optional_yield y, Formatter* f) override;
  virtual int swift_versioning_restore(bool& restored,
				       const DoutPrefixProvider* dpp) override;
  virtual int swift_versioning_copy(const DoutPrefixProvider* dpp,
				    optional_yield y) override;
  virtual std::unique_ptr<ReadOp> get_read_op() override;
  virtual std::unique_ptr<DeleteOp> get_delete_op() override;
  virtual int omap_get_vals(const DoutPrefixProvider *dpp, const std::string& marker,
			    uint64_t count, std::map<std::string, bufferlist> *m,
			    bool* pmore, optional_yield y) override;
  virtual int omap_get_all(const DoutPrefixProvider *dpp, std::map<std::string,
			   bufferlist> *m, optional_yield y) override;
  virtual int omap_get_vals_by_keys(const DoutPrefixProvider *dpp, const std::string& oid,
				    const std::set<std::string>& keys,
				    Attrs* vals) override;
  virtual int omap_set_val_by_key(const DoutPrefixProvider *dpp, const std::string& key,
				  bufferlist& val, bool must_exist, optional_yield y) override;
  virtual int chown(User& new_user, const DoutPrefixProvider* dpp, optional_yield y) override;
  virtual std::unique_ptr<Object> clone() override {
    return std::unique_ptr<Object>(new POSIXObject(*this));
  }

  int open(const DoutPrefixProvider *dpp, bool temp_file = false);
  int close();
  int write(int64_t ofs, bufferlist& bl, const DoutPrefixProvider* dpp, optional_yield y);
  int write_attr(const DoutPrefixProvider* dpp, optional_yield y, const std::string& key, bufferlist& value);
  int write_temp_file(const DoutPrefixProvider* dpp);
  void gen_temp_fname();
protected:
  int read(int64_t ofs, int64_t end, bufferlist& bl, const DoutPrefixProvider* dpp, optional_yield y);
  int generate_attrs(const DoutPrefixProvider* dpp, optional_yield y);
private:
  /* TODO dang Escape the object name for file use */
  const std::string get_fname();
  const std::string get_temp_fname();
  int stat(const DoutPrefixProvider *dpp);
};

class POSIXMultipartUpload : public FilterMultipartUpload {
protected:
  POSIXDriver* driver; 

public:
  POSIXMultipartUpload(std::unique_ptr<MultipartUpload> _next,
		       Bucket* _b,
		       POSIXDriver* _driver) :
    FilterMultipartUpload(std::move(_next), _b),
    driver(_driver) {}
  virtual ~POSIXMultipartUpload() = default;

  virtual int init(const DoutPrefixProvider* dpp, optional_yield y, ACLOwner& owner, rgw_placement_rule& dest_placement, rgw::sal::Attrs& attrs) override;
  virtual int list_parts(const DoutPrefixProvider* dpp, CephContext* cct,
			 int num_parts, int marker,
			 int* next_marker, bool* truncated,
			 bool assume_unsorted = false) override;
  virtual int abort(const DoutPrefixProvider* dpp, CephContext* cct) override;
  virtual int complete(const DoutPrefixProvider* dpp,
		       optional_yield y, CephContext* cct,
		       std::map<int, std::string>& part_etags,
		       std::list<rgw_obj_index_key>& remove_objs,
		       uint64_t& accounted_size, bool& compressed,
		       RGWCompressionInfo& cs_info, off_t& ofs,
		       std::string& tag, ACLOwner& owner,
		       uint64_t olh_epoch,
		       rgw::sal::Object* target_obj) override;

  virtual std::unique_ptr<Writer> get_writer(const DoutPrefixProvider *dpp,
			  optional_yield y,
			  std::unique_ptr<rgw::sal::Object> _head_obj,
			  const rgw_user& owner,
			  const rgw_placement_rule *ptail_placement_rule,
			  uint64_t part_num,
			  const std::string& part_num_str) override;
};

class POSIXAtomicWriter : public StoreWriter {
private:
  POSIXDriver* driver;
  const rgw_user& owner;
  const rgw_placement_rule *ptail_placement_rule;
  uint64_t olh_epoch;
  const std::string& unique_tag;
  POSIXObject obj;

public:
  POSIXAtomicWriter(const DoutPrefixProvider *dpp,
                    optional_yield y,
                    std::unique_ptr<rgw::sal::Object> _head_obj,
                    POSIXDriver* _driver,
                    const rgw_user& _owner,
                    const rgw_placement_rule *_ptail_placement_rule,
                    uint64_t _olh_epoch,
                    const std::string& _unique_tag) :
    StoreWriter(dpp, y),
    driver(_driver),
    owner(_owner),
    ptail_placement_rule(_ptail_placement_rule),
    olh_epoch(_olh_epoch),
    unique_tag(_unique_tag),
    obj(_driver, _head_obj->get_key(), _head_obj->get_bucket()) {}
  virtual ~POSIXAtomicWriter() = default;

  virtual int prepare(optional_yield y);
  virtual int process(bufferlist&& data, uint64_t offset) override;
  virtual int complete(size_t accounted_size, const std::string& etag,
                       ceph::real_time *mtime, ceph::real_time set_mtime,
		       std::map<std::string, bufferlist>& attrs,
		       ceph::real_time delete_at,
		       const char *if_match, const char *if_nomatch,
		       const std::string *user_data,
		       rgw_zone_set *zones_trace, bool *canceled,
		       optional_yield y) override;
};

class POSIXWriter : public FilterWriter {
private:
  POSIXDriver* driver; 

public:
  POSIXWriter(std::unique_ptr<Writer> _next,
		    std::unique_ptr<Object> _head_obj, 
		    POSIXDriver* _driver) :
    FilterWriter(std::move(_next),
                 std::move(_head_obj)),
    driver(_driver) {}
  virtual ~POSIXWriter() = default;

  virtual int prepare(optional_yield y);
  virtual int process(bufferlist&& data, uint64_t offset) override;
  virtual int complete(size_t accounted_size, const std::string& etag,
                       ceph::real_time *mtime, ceph::real_time set_mtime,
		       std::map<std::string, bufferlist>& attrs,
		       ceph::real_time delete_at,
		       const char *if_match, const char *if_nomatch,
		       const std::string *user_data,
		       rgw_zone_set *zones_trace, bool *canceled,
		       optional_yield y) override;
};

class MPPOSIXSerializer : public StoreMPSerializer {

public:
  MPPOSIXSerializer(const DoutPrefixProvider *dpp, POSIXDriver* driver, POSIXObject* obj, const std::string& lock_name) {}

  virtual int try_lock(const DoutPrefixProvider *dpp, utime_t dur, optional_yield y) override { return 0; }
  virtual int unlock() override { return 0; }
};

} } // namespace rgw::sal
