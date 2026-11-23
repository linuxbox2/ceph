// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* The RADOS FSIO driver's harness (FSIO_RADOS_WORK_PLAN.md, Phase 3, the
 * driver's step 1):  RadosStore brought up from $CEPH_CONF on a RADOS
 * vstart whose zonegroup has the fsio feature, and each FSIO op called
 * through the SAL directly, with no librgw.  Each op's tests land with
 * the op. */

#include <chrono>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "cls/rgw/cls_rgw_fsio_ops.h"
#include "cls/rgw/cls_rgw_types.h"
#include "common/async/blocked_completion.h"
#include "common/async/context_pool.h"
#include "common/common_init.h"
#include "common/error_code.h"
#include "common/random_string.h"
#include "global/global_context.h"
#include "gtest/gtest.h"
#include "include/neorados/RADOS.hpp"
#include "neorados/cls/rgw_fsio.h"
#include "rgw/driver/rados/rgw_fsio_hints.h"
#include "rgw/driver/rados/rgw_fsio_object.h"
#include "rgw/driver/rados/rgw_fsio_watch.h"
#include "rgw/driver/rados/rgw_obj_manifest.h"
#include "rgw/driver/rados/rgw_rados.h"
#include "rgw/driver/rados/rgw_sal_rados.h"
#include "rgw/driver/rados/rgw_tools.h"
#include "rgw/rgw_common.h"
#include "rgw/rgw_req_context.h"
#include "rgw/rgw_sal.h"
#include "rgw/rgw_sal_config.h"
#include "rgw/rgw_zone.h"

#define dout_subsys ceph_subsys_rgw

namespace {

/* the driver, brought up once for the run as a gateway's is, but with
 * every background thread off:  the tests drive it synchronously */
class DriverEnv : public ::testing::Environment {
public:
  boost::intrusive_ptr<CephContext> cct;
  std::unique_ptr<DoutPrefix> dpp;
  std::unique_ptr<ceph::async::io_context_pool> pool;
  std::unique_ptr<rgw::sal::ConfigStore> cfgstore;
  rgw::SiteConfig site;
  rgw::sal::Driver* driver = nullptr;

  void SetUp() override {
    std::vector<const char*> args;	// $CEPH_CONF, $CEPH_ARGS
    cct = rgw_global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
			  CODE_ENVIRONMENT_UTILITY, 0);
    common_init_finish(g_ceph_context);
    dpp = std::make_unique<DoutPrefix>(g_ceph_context, ceph_subsys_rgw,
				       "fsio rados test: ");
    pool = std::make_unique<ceph::async::io_context_pool>(
      cct->_conf->rgw_thread_pool_size);
    auto cfg = DriverManager::get_config(true, g_ceph_context);
    ASSERT_EQ("rados", cfg.store_name);
    cfgstore = DriverManager::create_config_store(
      dpp.get(), g_conf().get_val<std::string>("rgw_config_store"));
    ASSERT_TRUE(cfgstore);
    ASSERT_EQ(0, site.load(dpp.get(), null_yield, cfgstore.get()));
    /* no background threads;  the cache stays on, since RGWSI_Notify,
     * which FSIO's notifies ride, starts only with it (design §4.3) */
    driver = DriverManager::get_storage(dpp.get(), g_ceph_context, cfg,
					*pool, site, false, false, false,
					false, false, false, false, false,
					false, null_yield, cfgstore.get(),
					true /* use_cache */,
					true /* use_gc:  no thread, the tests
						run GC passes */);
    ASSERT_NE(nullptr, driver);
  }

  void TearDown() override {
    if (driver) {
      driver->shutdown();
      DriverManager::close_storage(driver);
      driver = nullptr;
    }
  }
};

DriverEnv* env;

/* each test its own bucket, removed with its objects after */
class FSIORados : public ::testing::Test {
protected:
  std::unique_ptr<rgw::sal::Bucket> bucket;
  ACLOwner owner;

  void SetUp() override {
    ASSERT_NE(nullptr, env->driver);
    owner.id = rgw_user{"", "fsio-rados-test"};
    owner.display_name = "fsio rados test";
    rgw_bucket b;
    b.name = "fsio-test-" + gen_rand_alphanumeric_lower(g_ceph_context, 12);
    env->driver->load_bucket(env->dpp.get(), b, &bucket, null_yield);
    ASSERT_NE(nullptr, bucket);
    rgw::sal::Bucket::CreateParams params;
    params.owner = owner.id;
    params.zonegroup_id = env->site.get_zonegroup().get_id();
    params.placement_rule = env->site.get_zonegroup().default_placement;
    params.zone_placement = rgw::find_zone_placement(
      env->dpp.get(), env->site.get_zone_params(), params.placement_rule);
    ASSERT_EQ(0, bucket->create(env->dpp.get(), params, null_yield));
  }

  void TearDown() override {
    if (bucket) {
      /* a writer a test leaves open holds FSIO state the bucket's removal
       * refuses;  the purge strips it (shadow chunks leak until step 10) */
      static_cast<rgw::sal::RadosStore*>(env->driver)->getRados()->fsio_purge(
	env->dpp.get(), bucket->get_info(), null_yield);
      bucket->remove(env->dpp.get(), true /* purge */, null_yield);
    }
  }

  /* an object as an S3 PUT writes it:  the content an open clones;  in a
   * bucket with versioning enabled, a new version, its instance returned */
  int put(const std::string& key, const std::string& data,
	  std::string* instance = nullptr) {
    auto obj = bucket->get_object(rgw_obj_key(key));
    if (bucket->versioning_enabled()) {
      obj->gen_rand_obj_instance_name();
    }
    if (instance) {
      *instance = obj->get_key().instance;
    }
    auto writer = env->driver->get_atomic_writer(
      env->dpp.get(), null_yield, obj.get(), owner, nullptr, 0,
      gen_rand_alphanumeric(g_ceph_context, 16));
    if (int r = writer->prepare(null_yield); r < 0) {
      return r;
    }
    ceph::buffer::list bl;
    bl.append(data);
    if (int r = writer->process(std::move(bl), 0); r < 0) {
      return r;
    }
    if (int r = writer->process({}, data.size()); r < 0) {
      return r;
    }
    ceph::real_time mtime;
    rgw::sal::Attrs attrs;
    req_context rctx{env->dpp.get(), null_yield, nullptr};
    return writer->complete(data.size(), "", &mtime, ceph::real_time(),
			    attrs, std::nullopt, ceph::real_time(), nullptr,
			    nullptr, nullptr, nullptr, nullptr, rctx, 0);
  }

  /* the store's neorados client, and the context of the pool an
   * object's head is in */
  neorados::RADOS& neo() {
    return static_cast<rgw::sal::RadosStore*>(env->driver)->get_neorados();
  }
  rgw_raw_obj head_of(const std::string& key) {
    auto obj = bucket->get_object(rgw_obj_key(key));
    rgw_raw_obj head;
    static_cast<rgw::sal::RadosObject*>(obj.get())->get_raw_obj(&head);
    return head;
  }
  rgw_raw_obj instance_of(const std::string& key, const std::string& inst) {
    auto obj = bucket->get_object(rgw_obj_key(key, inst));
    rgw_raw_obj raw;
    static_cast<rgw::sal::RadosObject*>(obj.get())->get_raw_obj(&raw);
    return raw;
  }
  /* an instance head's open count (§2.5), if it has one */
  std::optional<cls_rgw_fsio_ocnt> ocnt_of(const rgw_raw_obj& ih) {
    ceph::buffer::list bl;
    boost::system::error_code ec;
    neorados::ReadOp op;
    op.get_xattr("user.rgw.fsio.ocnt", &bl, &ec);
    neo().execute(neorados::Object{ih.oid}, ioc_of(ih.pool), std::move(op),
		  nullptr, ceph::async::use_blocked[ec]);
    if (ec || !bl.length()) {
      return std::nullopt;
    }
    cls_rgw_fsio_ocnt o;
    auto it = bl.cbegin();
    decode(o, it);
    return o;
  }
  void set_versioning(uint32_t flags) {
    bucket->get_info().flags &= ~(BUCKET_VERSIONED | BUCKET_VERSIONS_SUSPENDED);
    bucket->get_info().flags |= flags;
    ASSERT_EQ(0, bucket->put_info(env->dpp.get(), false, ceph::real_time(),
				  null_yield));
  }
  neorados::IOContext ioc_of(const rgw_pool& pool) {
    return rgw::init_iocontext(env->dpp.get(), neo(), pool,
			       ceph::async::use_blocked);
  }

  /* a head's FSIO records, as the class reads them:  its header, if any,
   * and an incarnation's record, if any (§2.2) */
  struct Records {
    std::optional<cls_rgw_fsio_hdr> hdr;
    std::optional<cls_rgw_fsio_opens> opens;
    bool any_fsio = false;
  };
  Records records_of(const rgw_raw_obj& head, uint64_t incarnation) {
    boost::container::flat_map<std::string, ceph::buffer::list> attrs;
    neorados::ReadOp op;
    op.get_xattrs(&attrs, nullptr);
    neo().execute(neorados::Object{head.oid}, ioc_of(head.pool),
		  std::move(op), nullptr, ceph::async::use_blocked);
    Records r;
    const auto mine = fmt::format("user.rgw.fsio.m.{:016x}", incarnation);
    for (auto& [k, v] : attrs) {
      if (!k.starts_with("user.rgw.fsio.")) {
	continue;
      }
      r.any_fsio = true;
      auto it = v.cbegin();
      if (k == "user.rgw.fsio.hdr") {
	decode(r.hdr.emplace(), it);
      } else if (k == mine) {
	decode(r.opens.emplace(), it);
      }
    }
    return r;
  }

  /* an object's bytes, read whole;  nullopt if it does not exist */
  std::optional<std::string> bytes_of(const rgw_raw_obj& raw) {
    auto ioc = ioc_of(raw.pool);
    if (!raw.loc.empty()) {
      ioc.set_key(raw.loc);
    }
    uint64_t size = 0;
    ceph::buffer::list bl;
    neorados::ReadOp op;
    op.stat(&size, nullptr);
    op.read(0, 0, &bl);
    boost::system::error_code ec;
    neo().execute(neorados::Object{raw.oid}, ioc, std::move(op), nullptr,
		  ceph::async::use_blocked[ec]);
    if (ec) {
      return std::nullopt;
    }
    return bl.to_str();
  }

  /* the chunks of an object's current manifest, head first */
  std::vector<rgw_raw_obj> chunks_of(const std::string& key,
				     const std::string& inst = {}) {
    auto head = inst.empty() ? head_of(key) : instance_of(key, inst);
    ceph::buffer::list bl;
    neorados::ReadOp op;
    op.get_xattr(RGW_ATTR_MANIFEST, &bl, nullptr);
    neo().execute(neorados::Object{head.oid}, ioc_of(head.pool),
		  std::move(op), nullptr, ceph::async::use_blocked);
    RGWObjManifest m;
    auto it = bl.cbegin();
    decode(m, it);
    auto* rados = static_cast<rgw::sal::RadosStore*>(env->driver)->getRados();
    std::vector<rgw_raw_obj> v;
    for (auto p = m.obj_begin(env->dpp.get()); p != m.obj_end(env->dpp.get());
	 ++p) {
      v.push_back(p.get_location().get_raw_obj(rados));
    }
    return v;
  }

  /* §2.4:  a chunk's clone in a shadow, f(anchor, id) */
  rgw_raw_obj clone_of(const rgw_raw_obj& chunk, const cls_rgw_fsio_id& id) {
    rgw_raw_obj c = chunk;
    c.oid = cls_rgw_fsio_clone_name(bucket->get_marker(), 'd', id, chunk.oid);
    c.loc = chunk.loc.empty() ? chunk.oid : chunk.loc;
    return c;
  }

  std::unique_ptr<rgw::sal::Object::FSIOObject> open(const std::string& key,
						      bool writer) {
    auto obj = bucket->get_object(rgw_obj_key(key));
    auto [r, fh] = obj->get_fsio_handle(
      env->dpp.get(), writer ? rgw::sal::Object::FSIOObject::OPEN_FLAG_WRITE
			     : rgw::sal::Object::FSIOObject::OPEN_FLAG_NONE);
    EXPECT_EQ(0, r);
    return std::move(fh);
  }
  static rgw::sal::RadosFSIOObject* rados_fsio(
    const std::unique_ptr<rgw::sal::Object::FSIOObject>& fh) {
    return static_cast<rgw::sal::RadosFSIOObject*>(fh.get());
  }
  /* bytes whose value depends on their offset and the seed, so a read from
   * the wrong place or the wrong content shows */
  static std::string pattern(uint64_t size, int seed) {
    std::string s(size, '\0');
    for (uint64_t i = 0; i < size; ++i) {
      s[i] = static_cast<char>((i * 31 + (i >> 12) * 7 + seed * 101) & 0xff);
    }
    return s;
  }
  /* preadv into buffers of the given lengths, concatenated;  its return */
  static int read(const std::unique_ptr<rgw::sal::Object::FSIOObject>& fh,
		  uint64_t ofs, const std::vector<size_t>& lens,
		  std::string* out) {
    std::vector<std::string> bufs;
    std::vector<struct iovec> iov;
    for (auto l : lens) {
      bufs.emplace_back(l, '\xee');
    }
    for (auto& b : bufs) {
      iov.push_back({b.data(), b.size()});
    }
    uint64_t n = 0;
    int r = fh->preadv(iov.data(), iov.size(), ofs, &n, 0);
    out->clear();
    for (auto& b : bufs) {
      out->append(b);
    }
    out->resize(n);
    return r;
  }
  std::string read(const std::unique_ptr<rgw::sal::Object::FSIOObject>& fh,
		   uint64_t ofs, size_t len) {
    std::string out;
    EXPECT_EQ(0, read(fh, ofs, {len}, &out));
    return out;
  }
  /* pwritev from buffers holding the pieces;  its return */
  static int write(const std::unique_ptr<rgw::sal::Object::FSIOObject>& fh,
		   uint64_t ofs, const std::vector<std::string>& pieces,
		   uint64_t* n = nullptr) {
    std::vector<std::string> bufs(pieces);
    std::vector<struct iovec> iov;
    for (auto& b : bufs) {
      iov.push_back({b.data(), b.size()});
    }
    uint64_t done = 0;
    int r = fh->pwritev(iov.data(), iov.size(), ofs, &done, 0);
    if (n) {
      *n = done;
    }
    return r;
  }
  /* the reserved runs of a shadow, as its record on the head holds them */
  std::vector<cls_rgw_fsio_run> extent_of(const std::string& key,
					  const cls_rgw_fsio_id& id) {
    auto rec = records_of(head_of(key), 0);
    if (!rec.hdr) {
      return {};
    }
    auto* e = entry_of(*rec.hdr, id);
    if (!e || !e->shadow) {
      return {};
    }
    return e->shadow->extent;
  }

  /* a shadow's size hint, as its record on the head holds it */
  std::optional<uint64_t> size_hint_of(const std::string& key,
				       const cls_rgw_fsio_id& id) {
    auto rec = records_of(head_of(key), 0);
    if (!rec.hdr) {
      return std::nullopt;
    }
    auto* e = entry_of(*rec.hdr, id);
    if (!e || !e->shadow) {
      return std::nullopt;
    }
    return e->shadow->size_hint;
  }
  /* a writer handle of the test's own on a head, as another gateway's:  a
   * raw fsio_join as writer, its first sequenced call;  its handle */
  cls_rgw_fsio_handle raw_writer(const rgw_raw_obj& head, uint64_t number) {
    const cls_rgw_fsio_handle th{static_cast<rgw::sal::RadosStore*>(
				   env->driver)->getRados()->fsio_incarnation(),
				 number};
    cls_rgw_fsio_join_op in;
    in.caller = th;
    in.seq = 1;
    in.writer = true;
    in.nonce = 1;
    in.counter_seed = 1;
    in.marker = bucket->get_marker();
    cls_rgw_fsio_join_ret ret;
    neorados::WriteOp op;
    op.exec(neorados::cls::rgw_fsio::join(in, &ret));
    neo().execute(neorados::Object{head.oid}, ioc_of(head.pool), std::move(op),
		  ceph::async::use_blocked);
    return th;
  }

  /* fstat's size;  -1 on its error */
  static int64_t size_of(const std::unique_ptr<rgw::sal::Object::FSIOObject>& fh) {
    struct stat st;
    if (fh->fstat(&st, 0) < 0) {
      return -1;
    }
    EXPECT_TRUE(S_ISREG(st.st_mode));
    return st.st_size;
  }
  /* a shadow's truncate epoch, as its record on the head holds it */
  std::optional<uint64_t> epoch_of(const std::string& key,
				   const cls_rgw_fsio_id& id) {
    auto rec = records_of(head_of(key), 0);
    if (!rec.hdr) {
      return std::nullopt;
    }
    auto* e = entry_of(*rec.hdr, id);
    if (!e || !e->shadow) {
      return std::nullopt;
    }
    return e->shadow->trunc_epoch;
  }

  /* where got first differs from want, and whose bytes are there instead */
  static std::string diff(const std::string& got, const std::string& want,
			  const std::string& other) {
    if (got == want) {
      return "same";
    }
    if (got.size() != want.size()) {
      return fmt::format("{} bytes, not {}", got.size(), want.size());
    }
    uint64_t i = 0;
    while (got[i] == want[i]) {
      ++i;
    }
    uint64_t j = i;
    while (j < got.size() && got[j] != want[j]) {
      ++j;
    }
    const bool theirs = got.compare(i, j - i, other, i, j - i) == 0;
    return fmt::format("differs at {} for {} bytes, {}", i, j - i,
		       theirs ? "the other content's" : "neither content's");
  }

  /* a RADOS object's bytes changed or removed directly:  a state the driver
   * must handle, made by the test */
  void write_raw(const rgw_raw_obj& raw, uint64_t off, const std::string& d) {
    auto ioc = ioc_of(raw.pool);
    ioc.set_key(raw.loc.empty() ? raw.oid : raw.loc);
    ceph::buffer::list bl;
    bl.append(d);
    neorados::WriteOp op;
    op.write(off, std::move(bl));
    neo().execute(neorados::Object{raw.oid}, ioc, std::move(op),
		  ceph::async::use_blocked);
  }
  void remove_raw(const rgw_raw_obj& raw) {
    auto ioc = ioc_of(raw.pool);
    if (!raw.loc.empty()) {
      ioc.set_key(raw.loc);
    }
    neorados::WriteOp op;
    op.remove();
    neo().execute(neorados::Object{raw.oid}, ioc, std::move(op),
		  ceph::async::use_blocked);
  }

  static const cls_rgw_fsio_chunk_set* entry_of(const cls_rgw_fsio_hdr& hdr,
						const cls_rgw_fsio_id& id) {
    for (const auto& e : hdr.chunk_sets) {
      if (e.id == id) {
	return &e;
      }
    }
    return nullptr;
  }

  /* the presence hints a handle holds on a head or an instance head,
   * found on the shard of that head's oid (§2.7) */
  int presence_hints(const rgw_raw_obj& head, const cls_rgw_fsio_handle& h,
		     cls_rgw_fsio_hint_target target =
		       cls_rgw_fsio_hint_target::HEAD) {
    auto* rados = static_cast<rgw::sal::RadosStore*>(env->driver)->getRados();
    const auto shards = static_cast<uint32_t>(
      g_conf().get_val<uint64_t>("rgw_fsio_open_hints_num_shards"));
    const auto shard = rgw::fsio::open_hint_shard_oid(
      rgw::fsio::open_hint_shard(head.oid, shards));
    cls_rgw_fsio_hint_list_op in;
    in.bucket = bucket->get_key();
    in.incarnation = h.incarnation;
    cls_rgw_fsio_hint_list_ret ret;
    neorados::ReadOp op;
    op.exec(neorados::cls::rgw_fsio::open_hint_list(in, &ret));
    neo().execute(neorados::Object{shard}, rados->fsio_hint_ioc(),
		  std::move(op), nullptr, ceph::async::use_blocked);
    return std::count_if(ret.hints.begin(), ret.hints.end(),
			 [&](const cls_rgw_fsio_hint& x) {
			   return x.kind == cls_rgw_fsio_hint_kind::PRESENCE &&
			     x.handle == h.handle && x.target == target;
			 });
  }

  /* an object's content as an S3 GET reads it */
  int get(const std::string& key, std::string* data) {
    auto obj = bucket->get_object(rgw_obj_key(key));
    auto rop = obj->get_read_op();
    if (int r = rop->prepare(null_yield, env->dpp.get()); r < 0) {
      return r;
    }
    /* a read returns at most one RADOS object's bytes, and their count */
    ceph::buffer::list bl;
    const int64_t size = obj->get_size();
    for (int64_t ofs = 0; ofs < size; ) {
      ceph::buffer::list part;
      int r = rop->read(ofs, size - 1, part, null_yield, env->dpp.get());
      if (r < 0) {
	return r;
      }
      if (r == 0) {
	return -EIO;			// no progress short of the size
      }
      ofs += r;
      bl.claim_append(part);
    }
    *data = bl.to_str();
    return 0;
  }
};

} // anonymous namespace

/* the harness itself:  the zonegroup serves FSIO, and an object put
 * through the SAL reads back whole, across chunk boundaries */
TEST_F(FSIORados, BringUp)
{
  ASSERT_TRUE(env->site.get_zonegroup().supports(rgw::zone_features::fsio));
  const std::string data(9 << 20, 'x');	// three 4 MiB chunks, the last short
  ASSERT_EQ(0, put("obj", data));
  std::string back;
  ASSERT_EQ(0, get("obj", &back));
  ASSERT_EQ(data, back);
}

/* FSIO design §2.2, §4.3, Z28:  the incarnation is the global id of the
 * store's neorados client, the origin its fsio_* calls carry and the
 * watcher its liveness objects name.  Needs the OSDs' origin check on
 * (rgw_fsio_check_origin), which the driver's cluster-tier step turns
 * off after this step */
TEST_F(FSIORados, IncarnationIsTheOriginAndAWatcher)
{
  auto* store = static_cast<rgw::sal::RadosStore*>(env->driver);
  auto* rados = store->getRados();
  auto& neo = store->get_neorados();
  const uint64_t inc = rados->fsio_incarnation();
  ASSERT_EQ(neo.instance_id(), inc);

  /* each liveness object is watched by this incarnation */
  ASSERT_TRUE(rados->fsio_watch);
  auto cioc = rgw::init_iocontext(
    env->dpp.get(), neo, env->site.get_zone_params().control_pool,
    ceph::async::use_blocked);
  for (int i = 0; i < g_conf()->rgw_num_control_oids; ++i) {
    std::vector<neorados::ObjWatcher> watchers;
    neorados::ReadOp op;
    op.list_watchers(&watchers, nullptr);
    neo.execute(neorados::Object{RGWFSIOWatch::oid(i)}, cioc, std::move(op),
		nullptr, ceph::async::use_blocked);
    ASSERT_TRUE(std::any_of(watchers.begin(), watchers.end(),
			    [inc](const auto& w) {
			      return w.watcher_id == static_cast<int64_t>(inc);
			    }))
      << RGWFSIOWatch::oid(i) << " has no watcher client." << inc;
  }
  ASSERT_TRUE(rados->fsio_watch->since_confirmed());

  /* an fsio_* call carrying the incarnation passes the origin check;  one
   * carrying any other id returns EPERM */
  ASSERT_EQ(0, put("obj", "data"));
  auto obj = bucket->get_object(rgw_obj_key("obj"));
  rgw_raw_obj head;
  static_cast<rgw::sal::RadosObject*>(obj.get())->get_raw_obj(&head);
  auto hioc = rgw::init_iocontext(env->dpp.get(), neo, head.pool,
				  ceph::async::use_blocked);
  auto join = [&](uint64_t incarnation, cls_rgw_fsio_join_ret* ret) {
    cls_rgw_fsio_join_op in;
    in.caller = cls_rgw_fsio_handle{incarnation, 1};
    in.seq = 1;
    in.nonce = 1;
    in.counter_seed = 10;
    in.marker = bucket->get_marker();
    neorados::WriteOp op;
    op.exec(neorados::cls::rgw_fsio::join(in, ret));
    boost::system::error_code ec;
    neo.execute(neorados::Object{head.oid}, hioc, std::move(op),
		ceph::async::use_blocked[ec]);
    return ceph::from_error_code(ec);
  };
  ASSERT_EQ(-EPERM, join(inc + 1, nullptr));
  cls_rgw_fsio_join_ret ret;
  ASSERT_EQ(0, join(inc, &ret));
  cls_rgw_fsio_leave_op in;
  in.caller = cls_rgw_fsio_handle{inc, 1};
  in.seq = 2;
  in.cs = ret.bound;
  neorados::WriteOp op;
  op.exec(neorados::cls::rgw_fsio::leave(in));
  boost::system::error_code ec;
  neo.execute(neorados::Object{head.oid}, hioc, std::move(op),
	      ceph::async::use_blocked[ec]);
  ASSERT_EQ(0, ceph::from_error_code(ec));
}

/* the driver's step 3, the open:  a reader's open adds its presence hint
 * and binds its handle to the published chunk set;  its close takes both
 * away, leaving no FSIO state (FSIO design §3.1, §3.4, §4.3) */
TEST_F(FSIORados, ReaderBindsPublished)
{
  ASSERT_EQ(0, put("obj", std::string(9 << 20, 'r')));
  const auto head = head_of("obj");
  auto obj = bucket->get_object(rgw_obj_key("obj"));
  auto [r, fh] = obj->get_fsio_handle(env->dpp.get(),
				      rgw::sal::Object::FSIOObject::OPEN_FLAG_NONE);
  ASSERT_EQ(0, r);
  ASSERT_TRUE(fh);
  auto* hdl = static_cast<rgw::sal::RadosFSIOObject*>(fh.get());
  const auto h = hdl->handle();

  auto rec = records_of(head, h.incarnation);
  ASSERT_TRUE(rec.hdr);
  ASSERT_TRUE(rec.opens);
  auto ho = rec.opens->handles.find(h.handle);
  ASSERT_NE(rec.opens->handles.end(), ho);
  auto c = ho->second.chunk_sets.find(hdl->bound());
  ASSERT_NE(ho->second.chunk_sets.end(), c);
  EXPECT_EQ(1u, c->second.readers);
  EXPECT_EQ(0u, c->second.writers);
  auto e = std::find_if(rec.hdr->chunk_sets.begin(), rec.hdr->chunk_sets.end(),
			[&](const auto& x) { return x.id == hdl->bound(); });
  ASSERT_NE(rec.hdr->chunk_sets.end(), e);
  EXPECT_EQ(cls_rgw_fsio_cs_state::PUBLISHED, e->state);
  EXPECT_FALSE(rec.hdr->cur_shadow);
  EXPECT_EQ(1, presence_hints(head, h));

  ASSERT_EQ(0, fh->close(env->dpp.get(),
			 rgw::sal::Object::FSIOObject::CLOSE_FLAG_NONE));
  EXPECT_FALSE(records_of(head, h.incarnation).any_fsio);
  EXPECT_EQ(0, presence_hints(head, h));
  std::string back;
  ASSERT_EQ(0, get("obj", &back));
  EXPECT_EQ(std::string(9 << 20, 'r'), back);
}

/* the driver's step 3, element 2:  a writer's open constructs a shadow
 * (FSIO design §3.1 step 7, §4.1):  the join clones the head, the gateway
 * every tail chunk, and fsio_ready makes it ready */
TEST_F(FSIORados, WriterConstructsShadow)
{
  const std::string data(9 << 20, 'w');
  ASSERT_EQ(0, put("obj", data));
  const auto head = head_of("obj");
  const auto chunks = chunks_of("obj");
  ASSERT_EQ(3u, chunks.size());
  auto fh = open("obj", true);
  ASSERT_TRUE(fh);
  auto* hdl = rados_fsio(fh);
  const auto id = hdl->writing_to();
  const auto h = hdl->handle();

  auto rec = records_of(head, h.incarnation);
  ASSERT_TRUE(rec.hdr);
  ASSERT_TRUE(rec.hdr->cur_shadow);
  EXPECT_EQ(id, *rec.hdr->cur_shadow);
  const auto* e = entry_of(*rec.hdr, id);
  ASSERT_NE(nullptr, e);
  EXPECT_EQ(cls_rgw_fsio_cs_state::READY, e->state);
  ASSERT_TRUE(e->shadow);
  EXPECT_EQ(uint64_t(9 << 20), e->shadow->size_hint);
  ASSERT_EQ(1u, e->shadow->extent.size());
  EXPECT_EQ(0u, e->shadow->extent[0].first);
  EXPECT_EQ(2u, e->shadow->extent[0].last);
  ASSERT_TRUE(rec.opens);
  EXPECT_EQ(1u, rec.opens->handles.at(h.handle).chunk_sets.at(id).writers);

  for (const auto& c : chunks) {
    auto src = bytes_of(c);
    auto cl = bytes_of(clone_of(c, id));
    ASSERT_TRUE(src) << c.oid;
    ASSERT_TRUE(cl) << clone_of(c, id).oid;
    EXPECT_EQ(*src, *cl) << clone_of(c, id).oid;
  }
  std::string back;
  ASSERT_EQ(0, get("obj", &back));
  EXPECT_EQ(data, back);
}

/* a second writer binds the ready shadow and constructs nothing */
TEST_F(FSIORados, SecondWriterJoinsTheShadow)
{
  ASSERT_EQ(0, put("obj", std::string(9 << 20, 's')));
  const auto head = head_of("obj");
  auto fh1 = open("obj", true);
  auto fh2 = open("obj", true);
  ASSERT_TRUE(fh1);
  ASSERT_TRUE(fh2);
  const auto id = rados_fsio(fh1)->writing_to();
  EXPECT_EQ(id, rados_fsio(fh2)->writing_to());
  auto rec = records_of(head, rados_fsio(fh1)->handle().incarnation);
  ASSERT_TRUE(rec.hdr);
  EXPECT_EQ(id, *rec.hdr->cur_shadow);
  int shadows = 0;
  for (const auto& x : rec.hdr->chunk_sets) {
    shadows += x.shadow ? 1 : 0;
  }
  EXPECT_EQ(1, shadows);
  for (auto* f : {rados_fsio(fh1), rados_fsio(fh2)}) {
    EXPECT_EQ(1u, rec.opens->handles.at(f->handle().handle).chunk_sets.at(id).writers);
  }
}

/* begin_write() on a reader's handle makes it a writer as well */
TEST_F(FSIORados, BeginWriteOnAReader)
{
  ASSERT_EQ(0, put("obj", std::string(9 << 20, 'b')));
  const auto head = head_of("obj");
  auto fh = open("obj", false);
  ASSERT_TRUE(fh);
  ASSERT_EQ(0, fh->begin_write(env->dpp.get(),
			       rgw::sal::Object::FSIOObject::WRITE_FLAG_NONE));
  auto* hdl = rados_fsio(fh);
  auto rec = records_of(head, hdl->handle().incarnation);
  ASSERT_TRUE(rec.opens);
  const auto& cs = rec.opens->handles.at(hdl->handle().handle).chunk_sets;
  EXPECT_EQ(1u, cs.at(hdl->bound()).readers);
  EXPECT_EQ(1u, cs.at(hdl->writing_to()).writers);
  EXPECT_NE(hdl->bound(), hdl->writing_to());
}

/* a writer's join after an S3 PUT builds its shadow from the new content
 * (§3.1, the compound op's ID-tag guard) */
TEST_F(FSIORados, GuardRetriesAfterAPut)
{
  ASSERT_EQ(0, put("obj", std::string(9 << 20, '1')));
  auto fh1 = open("obj", true);
  ASSERT_TRUE(fh1);
  ASSERT_EQ(0, put("obj", std::string(9 << 20, '2')));
  auto fh2 = open("obj", true);
  ASSERT_TRUE(fh2);
  const auto id = rados_fsio(fh2)->writing_to();
  EXPECT_NE(rados_fsio(fh1)->writing_to(), id);
  for (const auto& c : chunks_of("obj")) {
    auto cl = bytes_of(clone_of(c, id));
    ASSERT_TRUE(cl) << clone_of(c, id).oid;
    EXPECT_EQ(*bytes_of(c), *cl) << clone_of(c, id).oid;
    EXPECT_EQ('2', (*cl)[0]);
  }
}

/* the driver's step 4, element 2:  reads (FSIO design §2.7).  Each
 * object's bytes depend on their offset, so a misplaced read shows */
static constexpr uint64_t MiB = 1 << 20;

/* a reader reads the published content:  across the head segment's end,
 * across stripes, into several buffers, and short at its end */
TEST_F(FSIORados, ReaderReadsThePublished)
{
  const auto a = pattern(9 * MiB, 1);
  ASSERT_EQ(0, put("obj", a));
  auto fh = open("obj", false);
  ASSERT_TRUE(fh);
  EXPECT_EQ(a.substr(0, 100), read(fh, 0, 100));
  EXPECT_EQ(a.substr(4 * MiB - 10, 20), read(fh, 4 * MiB - 10, 20));
  EXPECT_EQ(a.substr(5 * MiB, 4 * MiB), read(fh, 5 * MiB, 4 * MiB));
  std::string out;
  ASSERT_EQ(0, read(fh, 3 * MiB + MiB / 2, {MiB, 10, MiB}, &out));
  EXPECT_EQ(a.substr(3 * MiB + MiB / 2, 2 * MiB + 10), out);
  EXPECT_EQ(a.substr(9 * MiB - 10), read(fh, 9 * MiB - 10, 100));
  EXPECT_EQ("", read(fh, 9 * MiB, 10));
}

/* §2.3:  a reader keeps reading the content it opened after an S3 PUT
 * replaces it, the inline head segment from the generation's clone */
TEST_F(FSIORados, ReaderKeepsItsContentAcrossAnOverwrite)
{
  const auto a = pattern(9 * MiB, 1), b = pattern(9 * MiB, 2);
  ASSERT_EQ(0, put("obj", a));
  auto fh = open("obj", false);
  ASSERT_TRUE(fh);
  EXPECT_EQ(a.substr(0, 10), read(fh, 0, 10));
  ASSERT_EQ(0, put("obj", b));
  std::string now;
  ASSERT_EQ(0, get("obj", &now));
  EXPECT_TRUE(now == b) << "the head does not hold the new content";
  EXPECT_FALSE(rados_fsio(fh)->moved_segment());
  EXPECT_EQ("same", diff(read(fh, 0, 9 * MiB), a, b));
  const auto& moved = rados_fsio(fh)->moved_segment();
  ASSERT_TRUE(moved) << "the head segment was read from the new head";
  EXPECT_NE(std::string::npos, moved->find("~fsio_G")) << *moved;
}

/* a writer reads its shadow:  a change made in a shadow chunk is seen by
 * the writer and not by a reader of the published content */
TEST_F(FSIORados, WriterReadsTheShadow)
{
  const auto a = pattern(9 * MiB, 1);
  ASSERT_EQ(0, put("obj", a));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  EXPECT_EQ("same", diff(read(w, 0, 9 * MiB), a, a));
  const auto id = rados_fsio(w)->writing_to();
  const auto chunks = chunks_of("obj");
  ASSERT_GE(chunks.size(), 2u);
  write_raw(clone_of(chunks[1], id), 0, "XYZ");
  EXPECT_EQ("XYZ", read(w, 4 * MiB, 3));
  auto r = open("obj", false);
  ASSERT_TRUE(r);
  EXPECT_EQ(a.substr(4 * MiB, 3), read(r, 4 * MiB, 3));
}

/* Z11:  a shadow chunk missing inside the size reads as zeros */
TEST_F(FSIORados, MissingShadowChunkReadsAsZeros)
{
  const auto a = pattern(9 * MiB, 1);
  ASSERT_EQ(0, put("obj", a));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  const auto chunks = chunks_of("obj");
  ASSERT_GE(chunks.size(), 2u);
  remove_raw(clone_of(chunks[1], rados_fsio(w)->writing_to()));
  EXPECT_EQ(a.substr(4 * MiB - 5, 5), read(w, 4 * MiB - 5, 5));
  EXPECT_EQ(std::string(MiB, '\0'), read(w, 4 * MiB, MiB));
  EXPECT_EQ(a.substr(8 * MiB, 5), read(w, 8 * MiB, 5));
}

/* a published stripe missing is a fault, not a hole:  no published object
 * is sparse until the publish sets RGW_ATTR_SPARSE (design §2.7) */
TEST_F(FSIORados, MissingPublishedStripeFails)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto fh = open("obj", false);
  ASSERT_TRUE(fh);
  const auto chunks = chunks_of("obj");
  ASSERT_GE(chunks.size(), 2u);
  remove_raw(chunks[1]);
  std::string out;
  EXPECT_EQ(-EIO, read(fh, 4 * MiB, {10}, &out));
}

/* the driver's step 4, element 3:  writes (FSIO design §2.7, §3.7) */

/* a write lands in the shadow's chunks, across a chunk boundary and from
 * several buffers, and the writer reads it back;  the published content
 * is unchanged */
TEST_F(FSIORados, WriteWithinTheShadow)
{
  const auto a = pattern(9 * MiB, 1);
  ASSERT_EQ(0, put("obj", a));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  uint64_t n = 0;
  ASSERT_EQ(0, write(w, 4 * MiB - 2, {"hello"}, &n));
  EXPECT_EQ(5u, n);
  ASSERT_EQ(0, write(w, 6 * MiB, {"ab", "", "cde"}, &n));
  EXPECT_EQ(5u, n);
  auto want = a;
  want.replace(4 * MiB - 2, 5, "hello");
  want.replace(6 * MiB, 5, "abcde");
  EXPECT_EQ("same", diff(read(w, 0, 9 * MiB), want, a));
  const auto id = rados_fsio(w)->writing_to();
  const auto chunks = chunks_of("obj");
  ASSERT_GE(chunks.size(), 2u);
  auto c1 = bytes_of(clone_of(chunks[1], id));
  ASSERT_TRUE(c1);
  EXPECT_EQ("llo", c1->substr(0, 3));
  std::string now;
  ASSERT_EQ(0, get("obj", &now));
  EXPECT_TRUE(now == a) << "a shadow write reached the published content";
}

/* §3.7:  an append reserves ahead, the run doubling, so it costs a few
 * fsio_extend calls, not one a chunk;  the shadow keeps one run */
TEST_F(FSIORados, AppendReservesAhead)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  for (uint64_t i = 3; i <= 40; ++i) {
    ASSERT_EQ(0, write(w, i * 4 * MiB, {"x"})) << "chunk " << i;
  }
  EXPECT_LE(rados_fsio(w)->extends(), 3u);
  const auto ext = extent_of("obj", rados_fsio(w)->writing_to());
  ASSERT_EQ(1u, ext.size());
  EXPECT_EQ(0u, ext[0].first);
  EXPECT_GE(ext[0].last, 40u);
}

/* Z11:  a far write adds a run of its own, never the span before it, and
 * the chunks between read as zeros */
TEST_F(FSIORados, FarWriteAddsARun)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  const uint64_t far = 1000 * 4 * MiB;
  ASSERT_EQ(0, write(w, far, {"z"}));
  const auto ext = extent_of("obj", rados_fsio(w)->writing_to());
  ASSERT_EQ(2u, ext.size());
  EXPECT_EQ((cls_rgw_fsio_run{0, 2}), ext[0]);
  EXPECT_EQ(1000u, ext[1].first);
  EXPECT_EQ(1015u, ext[1].last);
  EXPECT_EQ("z", read(w, far, 1));
  EXPECT_EQ(std::string(4096, '\0'), read(w, far - 4096, 4096));
}

/* §3.7:  a write to a chunk carrying the chunk write fence returns -EIO;
 * another chunk still takes writes */
TEST_F(FSIORados, WriteToAFencedChunkFails)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  const auto chunks = chunks_of("obj");
  ASSERT_GE(chunks.size(), 3u);
  const auto c1 = clone_of(chunks[1], rados_fsio(w)->writing_to());
  {
    auto ioc = ioc_of(c1.pool);
    ioc.set_key(c1.loc);
    ceph::buffer::list v;
    v.append("finalizer");
    neorados::WriteOp op;
    op.setxattr("user.rgw.fsio.fence", std::move(v));
    neo().execute(neorados::Object{c1.oid}, ioc, std::move(op),
		  ceph::async::use_blocked);
  }
  EXPECT_EQ(-EIO, write(w, 4 * MiB + 10, {"no"}));
  EXPECT_EQ(0, write(w, 8 * MiB + 10, {"yes"}));
}

/* the driver's step 4, element 4:  the size hint (FSIO design §2.7, §3.3,
 * §7) */

/* two writers' reports merge by max, whatever their order */
TEST_F(FSIORados, HintMergesByMax)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w1 = open("obj", true), w2 = open("obj", true);
  ASSERT_TRUE(w1 && w2);
  const auto id = rados_fsio(w1)->writing_to();
  ASSERT_EQ(id, rados_fsio(w2)->writing_to());
  ASSERT_EQ(0, write(w1, 20 * MiB - 1, {"a"}));
  ASSERT_EQ(0, write(w2, 12 * MiB - 1, {"b"}));
  ASSERT_EQ(0, rados_fsio(w1)->report_hint(env->dpp.get()));
  EXPECT_EQ(20 * MiB, size_hint_of("obj", id));
  ASSERT_EQ(0, rados_fsio(w2)->report_hint(env->dpp.get()));
  EXPECT_EQ(20 * MiB, size_hint_of("obj", id));
}

/* §2.7:  a report after another handle's truncate is stale;  the writer
 * rebases from the chunk at its high-water mark, so a write the truncate
 * cut is not reported and one after it is */
TEST_F(FSIORados, StaleHintRebasesFromTheChunk)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  const auto id = rados_fsio(w)->writing_to();
  ASSERT_EQ(0, write(w, 10 * MiB, {"x"}));	// chunk 2, [8, 12) MiB
  /* another handle's ftruncate() to 9.5 MiB:  the chunk first, then
   * fsio_truncate (§3.3) */
  const auto head = head_of("obj");
  const auto t = raw_writer(head, 999998);
  const auto chunks = chunks_of("obj");
  ASSERT_GE(chunks.size(), 3u);
  {
    const auto c2 = clone_of(chunks[2], id);
    auto ioc = ioc_of(c2.pool);
    ioc.set_key(c2.loc);
    neorados::WriteOp op;
    op.truncate(MiB + MiB / 2);
    neo().execute(neorados::Object{c2.oid}, ioc, std::move(op),
		  ceph::async::use_blocked);
    cls_rgw_fsio_truncate_op in;
    in.caller = t;
    in.seq = 2;
    in.cs = id;
    in.size = 9 * MiB + MiB / 2;
    neorados::WriteOp top;
    top.exec(neorados::cls::rgw_fsio::truncate(in));
    neo().execute(neorados::Object{head.oid}, ioc_of(head.pool),
		  std::move(top), ceph::async::use_blocked);
  }
  ASSERT_EQ(9 * MiB + MiB / 2, size_hint_of("obj", id));
  ASSERT_EQ(0, write(w, 9 * MiB + 3 * MiB / 4, {"y"}));
  ASSERT_EQ(0, rados_fsio(w)->report_hint(env->dpp.get()));
  EXPECT_EQ(9 * MiB + 3 * MiB / 4 + 1, size_hint_of("obj", id));
  EXPECT_EQ("", read(w, 10 * MiB, 1));
}

/* §7:  a writer reports as it writes only while another handle is open in
 * the generation */
TEST_F(FSIORados, ReportsOnlyWhileWatched)
{
  g_conf().set_val_or_die("rgw_fsio_hint_interval", "0");
  ASSERT_EQ(0, put("alone", pattern(9 * MiB, 1)));
  auto w = open("alone", true);
  ASSERT_TRUE(w);
  ASSERT_EQ(0, write(w, 9 * MiB, {"q"}));
  EXPECT_EQ(9 * MiB, size_hint_of("alone", rados_fsio(w)->writing_to()));

  ASSERT_EQ(0, put("watched", pattern(9 * MiB, 1)));
  auto r = open("watched", false);
  auto w2 = open("watched", true);
  ASSERT_TRUE(r && w2);
  ASSERT_EQ(0, write(w2, 9 * MiB, {"q"}));
  EXPECT_EQ(9 * MiB + 1,
	    size_hint_of("watched", rados_fsio(w2)->writing_to()));
  g_conf().set_val_or_die("rgw_fsio_hint_interval", "3");
}

/* the driver's step 5:  fstat and ftruncate (FSIO design §2.7, §3.3) */

/* a reader sees its content's size, whatever S3 writes since */
TEST_F(FSIORados, ReaderStatIsTheContent)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto r = open("obj", false);
  ASSERT_TRUE(r);
  EXPECT_EQ(int64_t(9 * MiB), size_of(r));
  ASSERT_EQ(0, put("obj", pattern(5 * MiB, 2)));
  EXPECT_EQ(int64_t(9 * MiB), size_of(r));
}

/* a writer sees its own writes' end */
TEST_F(FSIORados, WriterStatFollowsItsWrites)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  EXPECT_EQ(int64_t(9 * MiB), size_of(w));
  ASSERT_EQ(0, write(w, 20 * MiB - 1, {"e"}));
  EXPECT_EQ(int64_t(20 * MiB), size_of(w));
}

/* §3.3:  a truncate down cuts the end's chunk and removes every reserved
 * chunk past it, then sets the size exactly at a new epoch;  the published
 * content is unchanged */
TEST_F(FSIORados, TruncateDownRemovesChunksPastTheEnd)
{
  const auto a = pattern(9 * MiB, 1);
  ASSERT_EQ(0, put("obj", a));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  auto* hdl = rados_fsio(w);
  ASSERT_EQ(0, write(w, 20 * MiB - 1, {"e"}));	// chunk 4
  const auto c1 = hdl->shadow_chunk(4 * MiB), c2 = hdl->shadow_chunk(8 * MiB),
	     c4 = hdl->shadow_chunk(16 * MiB);
  ASSERT_TRUE(bytes_of(c4));
  ASSERT_EQ(0, w->ftruncate(env->dpp.get(), 5 * MiB, 0));
  auto b1 = bytes_of(c1);
  ASSERT_TRUE(b1);
  EXPECT_EQ(MiB, b1->size());
  EXPECT_FALSE(bytes_of(c2));
  EXPECT_FALSE(bytes_of(c4));
  const auto id = hdl->writing_to();
  EXPECT_EQ(5 * MiB, size_hint_of("obj", id));
  EXPECT_EQ(1u, epoch_of("obj", id));
  EXPECT_EQ(int64_t(5 * MiB), size_of(w));
  EXPECT_EQ(MiB, read(w, 4 * MiB, 2 * MiB).size());
  std::string now;
  ASSERT_EQ(0, get("obj", &now));
  EXPECT_TRUE(now == a);
}

/* §2.7:  a truncate up makes the new end's chunk, the rest reading as
 * zeros */
TEST_F(FSIORados, TruncateUpExtendsTheEndChunk)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  auto* hdl = rados_fsio(w);
  ASSERT_EQ(0, w->ftruncate(env->dpp.get(), 30 * MiB, 0));
  auto end = bytes_of(hdl->shadow_chunk(30 * MiB - 1));
  ASSERT_TRUE(end) << "the new end's chunk was not made";
  EXPECT_EQ(30 * MiB - 28 * MiB, end->size());
  EXPECT_EQ(int64_t(30 * MiB), size_of(w));
  EXPECT_EQ(30 * MiB, size_hint_of("obj", hdl->writing_to()));
  EXPECT_EQ(std::string(4096, '\0'), read(w, 25 * MiB, 4096));
  EXPECT_EQ(std::string(10, '\0'), read(w, 30 * MiB - 10, 100));
}

/* the head clone carries the attributes and the pseudo-manifest:  a
 * truncate to zero empties it and never removes it */
TEST_F(FSIORados, TruncateToZeroKeepsTheHeadClone)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  auto* hdl = rados_fsio(w);
  const auto c0 = hdl->shadow_chunk(0), c1 = hdl->shadow_chunk(4 * MiB);
  ASSERT_EQ(0, w->ftruncate(env->dpp.get(), 0, 0));
  auto b0 = bytes_of(c0);
  ASSERT_TRUE(b0) << "the head clone was removed";
  EXPECT_EQ(0u, b0->size());
  ceph::buffer::list pm;
  boost::system::error_code ec;
  neo().execute(neorados::Object{c0.oid}, [&] {
		  auto x = ioc_of(c0.pool); x.set_key(c0.loc); return x; }(),
		neorados::ReadOp{}.get_xattr("user.rgw.fsio.pm", &pm), nullptr,
		ceph::async::use_blocked[ec]);
  EXPECT_FALSE(ec) << "the head clone lost its pseudo-manifest";
  EXPECT_FALSE(bytes_of(c1));
  EXPECT_EQ(0, size_of(w));
  EXPECT_EQ("", read(w, 0, 10));
}

/* Z16:  each chunk step carries the chunk write fence's check */
TEST_F(FSIORados, TruncateOfAFencedChunkFails)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  const auto c2 = rados_fsio(w)->shadow_chunk(8 * MiB);
  {
    auto ioc = ioc_of(c2.pool);
    ioc.set_key(c2.loc);
    ceph::buffer::list v;
    v.append("finalizer");
    neorados::WriteOp op;
    op.setxattr("user.rgw.fsio.fence", std::move(v));
    neo().execute(neorados::Object{c2.oid}, ioc, std::move(op),
		  ceph::async::use_blocked);
  }
  EXPECT_EQ(-EIO, w->ftruncate(env->dpp.get(), 5 * MiB, 0));
  EXPECT_TRUE(bytes_of(c2)) << "a fenced chunk was removed";
}

/* §3.3:  a truncate applied but its reply lost (sent twice, the second
 * reply kept) leaves the handle unsure of its length;  it rebases from the
 * chunks before it answers fstat or reports, so a write after the
 * truncate is counted and the epoch it never heard is reached */
TEST_F(FSIORados, TruncateHeardTwiceRebases)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  auto* hdl = rados_fsio(w);
  const auto id = hdl->writing_to();
  hdl->send_sequenced_twice(true);
  ASSERT_EQ(0, w->ftruncate(env->dpp.get(), 5 * MiB, 0));
  hdl->send_sequenced_twice(false);
  EXPECT_EQ(1u, epoch_of("obj", id)) << "the truncate applied twice";
  ASSERT_EQ(0, write(w, 6 * MiB, {"z"}));
  EXPECT_EQ(int64_t(6 * MiB + 1), size_of(w));
  EXPECT_EQ(6 * MiB + 1, size_hint_of("obj", id));
}

/* the driver's step 6:  manifest-form GC entries (FSIO design §8).  The
 * in-process store reads the zonegroup once, so the tests stand in for
 * the gc-manifest feature's answer (gc_manifest_override());  both forms
 * and the switch between them run in one bring-up */
class FSIORadosGC : public FSIORados {
protected:
  RGWRados* rados() {
    return static_cast<rgw::sal::RadosStore*>(env->driver)->getRados();
  }
  int64_t min_wait = 0;		// rgw_gc_obj_min_wait is an int option
  void SetUp() override {
    FSIORados::SetUp();
    min_wait = g_conf().get_val<int64_t>("rgw_gc_obj_min_wait");
    g_conf().set_val_or_die("rgw_gc_obj_min_wait", "0");
  }
  void TearDown() override {
    rados()->gc_manifest_override(std::nullopt);
    g_conf().set_val_or_die("rgw_gc_obj_min_wait", std::to_string(min_wait));
    FSIORados::TearDown();
  }
  /* every GC entry carrying tag, in every shard */
  std::vector<cls_rgw_gc_obj_info> gc_entries(const std::string& tag) {
    std::vector<cls_rgw_gc_obj_info> v;
    int index = 0;
    std::string marker;
    bool truncated = false, processing_queue = false;
    do {
      std::list<cls_rgw_gc_obj_info> result;
      EXPECT_EQ(0, rados()->list_gc_objs(index, marker, 1000, false, result,
					 truncated, processing_queue));
      for (auto& i : result) {
	if (i.tag == tag) {
	  v.push_back(std::move(i));
	}
      }
    } while (truncated);
    return v;
  }
  /* the head's manifest, its rgw_obj, and the tag its tail is collected by */
  struct Head {
    ceph::buffer::list manifest;
    rgw_obj obj;
    std::string tag;
  };
  Head head_info(const std::string& key) {
    Head hd;
    const auto raw = head_of(key);
    ceph::buffer::list idt, tailt;
    neorados::ReadOp op;
    op.get_xattr(RGW_ATTR_MANIFEST, &hd.manifest, nullptr);
    op.get_xattr(RGW_ATTR_ID_TAG, &idt, nullptr);
    op.get_xattr(RGW_ATTR_TAIL_TAG, &tailt, nullptr);
    neo().execute(neorados::Object{raw.oid}, ioc_of(raw.pool), std::move(op),
		  nullptr, ceph::async::use_blocked);
    hd.obj = rgw_obj(bucket->get_key(), rgw_obj_key(key));
    hd.tag = (tailt.length() ? tailt : idt).to_str();
    return hd;
  }
  std::vector<rgw_raw_obj> tail_of(const std::string& key) {
    auto c = chunks_of(key);
    return {c.begin() + 1, c.end()};
  }
  int remove(const std::string& key) {
    auto obj = bucket->get_object(rgw_obj_key(key));
    auto del = obj->get_delete_op();
    del->params.bucket_owner = bucket->get_owner();
    return del->delete_obj(env->dpp.get(), null_yield, 0);
  }

  /* the overwrite or DELETE of a two-stripe object, and its old tail's
   * GC entries in the form the feature picks */
  void old_tail_collected(bool on, bool overwrite) {
    rados()->gc_manifest_override(on);
    ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
    const auto hd = head_info("obj");
    const auto old_tail = tail_of("obj");
    ASSERT_EQ(2u, old_tail.size());
    std::vector<rgw_raw_obj> new_tail;
    if (overwrite) {
      ASSERT_EQ(0, put("obj", pattern(9 * MiB, 2)));
      new_tail = tail_of("obj");
    } else {
      ASSERT_EQ(0, remove("obj"));
    }
    auto es = gc_entries(hd.tag);
    ASSERT_FALSE(es.empty()) << "the old tail was not sent to GC";
    if (on) {
      ASSERT_EQ(1u, es.size());
      EXPECT_TRUE(es[0].is_manifest());
      EXPECT_TRUE(es[0].chain.objs.empty());
    } else {
      size_t objs = 0;
      for (const auto& e : es) {
        EXPECT_FALSE(e.is_manifest());
        objs += e.chain.objs.size();
      }
      EXPECT_EQ(old_tail.size(), objs);
    }
    ASSERT_EQ(0, rados()->process_gc(false, null_yield));
    for (const auto& o : old_tail) {
      EXPECT_FALSE(bytes_of(o)) << o.oid;
    }
    for (const auto& o : new_tail) {
      EXPECT_TRUE(bytes_of(o)) << "the new tail was collected:  " << o.oid;
    }
    EXPECT_TRUE(gc_entries(hd.tag).empty());
  }
};


/* with the feature off, the objects a manifest names go to GC as today's
 * expanded chains, and GC deletes them */
TEST_F(FSIORadosGC, ExpandedWhenTheFeatureIsOff)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  const auto hd = head_info("obj");
  const auto tail = tail_of("obj");
  ASSERT_EQ(2u, tail.size());
  rados()->gc_manifest_override(false);
  ASSERT_EQ(0, rados()->send_manifest_to_gc(env->dpp.get(), hd.manifest,
					    hd.obj, std::nullopt, hd.tag,
					    null_yield));
  auto es = gc_entries(hd.tag);
  ASSERT_FALSE(es.empty());
  size_t objs = 0;
  for (const auto& e : es) {
    EXPECT_FALSE(e.is_manifest());
    objs += e.chain.objs.size();
  }
  EXPECT_EQ(tail.size(), objs);
  ASSERT_EQ(0, rados()->process_gc(false, null_yield));
  for (const auto& t : tail) {
    EXPECT_FALSE(bytes_of(t)) << t.oid;
  }
  EXPECT_TRUE(gc_entries(hd.tag).empty());
}

/* with the feature on, one manifest-form entry;  GC expands it while it
 * deletes:  an FSIO shadow's chunks over its extent, past the manifest's
 * end too, and its attribute clone, the published content untouched */
TEST_F(FSIORadosGC, ManifestEntryWhenTheFeatureIsOn)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  auto* hdl = rados_fsio(w);
  ASSERT_EQ(0, write(w, 1000 * 4 * MiB, {"f"}));
  const auto id = hdl->writing_to();
  const std::vector<rgw_raw_obj> shadow{
    hdl->shadow_chunk(0), hdl->shadow_chunk(4 * MiB),
    hdl->shadow_chunk(8 * MiB), hdl->shadow_chunk(1000 * 4 * MiB)};
  for (const auto& c : shadow) {
    ASSERT_TRUE(bytes_of(c)) << c.oid;
  }
  const auto hd = head_info("obj");
  cls_rgw_fsio_gc_shadow sh{id, extent_of("obj", id)};
  rados()->gc_manifest_override(true);
  const std::string tag = "fsio-gc-test";
  ASSERT_EQ(0, rados()->send_manifest_to_gc(env->dpp.get(), hd.manifest,
					    hd.obj, sh, tag, null_yield));
  auto es = gc_entries(tag);
  ASSERT_EQ(1u, es.size());
  EXPECT_TRUE(es[0].is_manifest());
  EXPECT_TRUE(es[0].chain.objs.empty());
  ASSERT_EQ(0, rados()->process_gc(false, null_yield));
  for (const auto& c : shadow) {
    EXPECT_FALSE(bytes_of(c)) << c.oid;
  }
  for (const auto& t : tail_of("obj")) {
    EXPECT_TRUE(bytes_of(t)) << "a published stripe was collected:  " << t.oid;
  }
  EXPECT_TRUE(gc_entries(tag).empty());
}

/* §8:  what a manifest-form entry gains:  a 1 TiB object's tail is one
 * small entry, where today's form lists every stripe.  Measured on the
 * encoding and the expansion, not sent:  collecting 262,143 absent
 * stripes would hold the GC queue for every later test */
TEST_F(FSIORadosGC, LargeManifestEntryStaysSmall)
{
  ASSERT_EQ(0, put("obj", pattern(1 * MiB, 1)));
  const auto hd = head_info("obj");
  RGWObjManifest m;
  RGWObjManifest::generator gen;
  const uint64_t size = uint64_t(1) << 40;
  m.set_trivial_rule(4 * MiB, 4 * MiB);
  ASSERT_EQ(0, gen.create_begin(g_ceph_context, &m,
				bucket->get_info().placement_rule, nullptr,
				bucket->get_key(), hd.obj));
  for (uint64_t ofs = 0; ofs < size; ) {
    ofs = std::min(ofs + gen.cur_stripe_max_size(), size);
    gen.create_next(ofs);
  }
  cls_rgw_gc_obj_info info;
  info.tag = "large";
  encode(m, info.manifest);
  encode(hd.obj, info.head);
  ceph::buffer::list bl;
  encode(info, bl);
  EXPECT_LT(bl.length(), 4096u);
  uint64_t n = 0;
  ASSERT_EQ(0, rados()->expand_gc_entry(env->dpp.get(), info,
					[&n] (const rgw_raw_obj&) {
					  ++n; return 0; }));
  EXPECT_EQ(size / (4 * MiB) - 1, n);
}

/* §8:  with the feature off, no version 2 entry is ever written:  an
 * older OSD would fail to list it */
TEST_F(FSIORadosGC, NoVersion2WhileOff)
{
  ASSERT_EQ(0, put("obj", pattern(9 * MiB, 1)));
  auto w = open("obj", true);
  ASSERT_TRUE(w);
  const auto id = rados_fsio(w)->writing_to();
  const auto hd = head_info("obj");
  rados()->gc_manifest_override(false);
  const std::string tag = "fsio-gc-off";
  ASSERT_EQ(0, rados()->send_manifest_to_gc(
	      env->dpp.get(), hd.manifest, hd.obj,
	      cls_rgw_fsio_gc_shadow{id, extent_of("obj", id)}, tag,
	      null_yield));
  auto es = gc_entries(tag);
  ASSERT_FALSE(es.empty());
  for (const auto& e : es) {
    EXPECT_FALSE(e.is_manifest());
    ceph::buffer::list bl;
    encode(e, bl);
    EXPECT_EQ(1, bl[0]) << "encoded as version " << int(bl[0]);
  }
  ASSERT_EQ(0, rados()->process_gc(false, null_yield));
}

/* entries of both forms queued across the feature's enabling, all
 * collected by one pass */
TEST_F(FSIORadosGC, BothFormsAcrossEnabling)
{
  ASSERT_EQ(0, put("a", pattern(9 * MiB, 1)));
  ASSERT_EQ(0, put("b", pattern(9 * MiB, 2)));
  const auto ha = head_info("a"), hb = head_info("b");
  const auto ta = tail_of("a"), tb = tail_of("b");
  rados()->gc_manifest_override(false);
  ASSERT_EQ(0, rados()->send_manifest_to_gc(env->dpp.get(), ha.manifest,
					    ha.obj, std::nullopt, ha.tag,
					    null_yield));
  rados()->gc_manifest_override(true);
  ASSERT_EQ(0, rados()->send_manifest_to_gc(env->dpp.get(), hb.manifest,
					    hb.obj, std::nullopt, hb.tag,
					    null_yield));
  ASSERT_FALSE(gc_entries(ha.tag).at(0).is_manifest());
  ASSERT_TRUE(gc_entries(hb.tag).at(0).is_manifest());
  ASSERT_EQ(0, rados()->process_gc(false, null_yield));
  for (const auto& t : ta) {
    EXPECT_FALSE(bytes_of(t)) << t.oid;
  }
  for (const auto& t : tb) {
    EXPECT_FALSE(bytes_of(t)) << t.oid;
  }
  EXPECT_TRUE(gc_entries(ha.tag).empty());
  EXPECT_TRUE(gc_entries(hb.tag).empty());
}

/* RGW's own overwrite and DELETE send the replaced tail through the same
 * path:  one manifest-form entry with the feature on, today's expanded
 * chains with it off;  a GC pass collects the old tail and leaves the new */
TEST_F(FSIORadosGC, OverwriteSendsOneManifestEntry)
{
  old_tail_collected(true, true);
}

TEST_F(FSIORadosGC, OverwriteExpandsWhileOff)
{
  old_tail_collected(false, true);
}

TEST_F(FSIORadosGC, DeleteSendsOneManifestEntry)
{
  old_tail_collected(true, false);
}

TEST_F(FSIORadosGC, DeleteExpandsWhileOff)
{
  old_tail_collected(false, false);
}

/* with no override, the store reads the feature from the zonegroup, which
 * fsio_validate.rs's rados-fsio step enables */
TEST_F(FSIORadosGC, FeatureFollowsTheZonegroup)
{
  EXPECT_TRUE(rados()->gc_manifest_enabled());
}

/* the driver's step 3, element 4:  a join that must wait (FSIO design
 * §3.1 step 5, §4.1, W13).  The test holds a shadow constructing itself:
 * a raw fsio_join as writer under a handle number of its own, its
 * fsio_ready sent or not */
class FSIORadosWait : public FSIORados {
protected:
  static constexpr uint64_t CONSTRUCTOR = 999999;
  cls_rgw_fsio_id constructing;

  void SetUp() override {
    FSIORados::SetUp();
    g_conf().set_val_or_die("rgw_fsio_join_wait", "2");
  }
  void TearDown() override {
    g_conf().set_val_or_die("rgw_fsio_join_wait", "5");
    FSIORados::TearDown();
  }
  cls_rgw_fsio_handle constructor() {
    return {static_cast<rgw::sal::RadosStore*>(env->driver)->getRados()
	      ->fsio_incarnation(), CONSTRUCTOR};
  }
  void hold_constructing(const rgw_raw_obj& head) {
    cls_rgw_fsio_join_op in;
    in.caller = constructor();
    in.seq = 1;
    in.writer = true;
    in.nonce = 1;
    in.counter_seed = 1;
    in.marker = bucket->get_marker();
    cls_rgw_fsio_join_ret ret;
    neorados::WriteOp op;
    op.exec(neorados::cls::rgw_fsio::join(in, &ret));
    neo().execute(neorados::Object{head.oid}, ioc_of(head.pool), std::move(op),
		  ceph::async::use_blocked);
    ASSERT_TRUE(ret.construct);
    constructing = ret.bound;
  }
  void ready(const rgw_raw_obj& head) {
    cls_rgw_fsio_runs_op in;
    in.caller = constructor();
    in.cs = constructing;
    in.runs = {cls_rgw_fsio_run{0, 2}};
    in.size = 9 << 20;
    neorados::WriteOp op;
    op.exec(neorados::cls::rgw_fsio::ready(in));
    neo().execute(neorados::Object{head.oid}, ioc_of(head.pool), std::move(op),
		  ceph::async::use_blocked);
  }
  rgw::sal::Object::FSIOResult open_writer() {
    return bucket->get_object(rgw_obj_key("obj"))->get_fsio_handle(
      env->dpp.get(), rgw::sal::Object::FSIOObject::OPEN_FLAG_WRITE);
  }
};

/* a writer's open waits rgw_fsio_join_wait for the constructing shadow,
 * then returns EAGAIN, leaving nothing of its own on the head */
TEST_F(FSIORadosWait, JoinWaitsThenEAGAIN)
{
  ASSERT_EQ(0, put("obj", std::string(9 << 20, 'a')));
  const auto head = head_of("obj");
  hold_constructing(head);
  const auto start = std::chrono::steady_clock::now();
  auto [r, fh] = open_writer();
  const auto waited = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(-EAGAIN, r);
  EXPECT_FALSE(fh);
  EXPECT_GE(waited, std::chrono::seconds(2));
  EXPECT_LT(waited, std::chrono::seconds(4));
  /* only the constructor's handle has state on the head, and no hint */
  auto rec = records_of(head, constructor().incarnation);
  ASSERT_TRUE(rec.opens);
  EXPECT_EQ(1u, rec.opens->handles.size());
  EXPECT_EQ(1u, rec.opens->handles.count(CONSTRUCTOR));
}

/* a writer's open waiting on a constructing shadow binds it once it is
 * ready, within the wait, and constructs nothing itself */
TEST_F(FSIORadosWait, JoinWaitsForReady)
{
  ASSERT_EQ(0, put("obj", std::string(9 << 20, 'r')));
  const auto head = head_of("obj");
  hold_constructing(head);
  std::thread readier([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ready(head);
  });
  const auto start = std::chrono::steady_clock::now();
  auto [r, fh] = open_writer();
  const auto waited = std::chrono::steady_clock::now() - start;
  readier.join();
  ASSERT_EQ(0, r);
  ASSERT_TRUE(fh);
  EXPECT_LT(waited, std::chrono::seconds(2));
  EXPECT_EQ(constructing, rados_fsio(fh)->writing_to());
  /* the constructor cloned no tail chunk, and neither did this open */
  for (const auto& c : chunks_of("obj")) {
    if (c.oid != head.oid) {
      EXPECT_FALSE(bytes_of(clone_of(c, constructing))) << c.oid;
    }
  }
}

/* the driver's step 3, element 5:  how long a writer's open takes to
 * construct objects of growing chunk counts, against rgw_fsio_join_wait
 * (plan §6.13).  A measurement, not a check:  disabled in ordinary runs,
 * run by fsio_validate.rs --clone-latency.  The time is the whole open,
 * its fsio_join and fsio_ready as well as the clones;  per chunk, the
 * clones'.  Once one at a time, once at rgw_fsio_clone_window's default */
TEST_F(FSIORados, DISABLED_CloneLatency)
{
  for (const char* window : {"1", "10"})
  for (const int chunks : {64, 256, 512}) {
    g_conf().set_val_or_die("rgw_fsio_clone_window", window);
    const std::string key = fmt::format("clone-latency-{}-{}", window, chunks);
    const uint64_t size = uint64_t(chunks) << 22;	// 4 MiB chunks
    {
      auto obj = bucket->get_object(rgw_obj_key(key));
      auto writer = env->driver->get_atomic_writer(
	env->dpp.get(), null_yield, obj.get(), owner, nullptr, 0,
	gen_rand_alphanumeric(g_ceph_context, 16));
      ASSERT_EQ(0, writer->prepare(null_yield));
      const std::string piece(4 << 20, 'c');
      for (uint64_t ofs = 0; ofs < size; ofs += piece.size()) {
	ceph::buffer::list bl;
	bl.append(piece);
	ASSERT_EQ(0, writer->process(std::move(bl), ofs));
      }
      ASSERT_EQ(0, writer->process({}, size));
      ceph::real_time mtime;
      rgw::sal::Attrs attrs;
      req_context rctx{env->dpp.get(), null_yield, nullptr};
      ASSERT_EQ(0, writer->complete(size, "", &mtime, ceph::real_time(), attrs,
				    std::nullopt, ceph::real_time(), nullptr,
				    nullptr, nullptr, nullptr, nullptr, rctx, 0));
    }
    const auto start = std::chrono::steady_clock::now();
    auto fh = open(key, true);
    const auto ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(fh);
    std::cout << fmt::format("CLONE_LATENCY window={} chunks={} ms={:.0f} "
			     "per_chunk_ms={:.2f}", window, chunks, ms,
			     ms / chunks)
	      << std::endl;
  }
  g_conf().set_val_or_die("rgw_fsio_clone_window", "10");
}

/* the driver's step 3, element 3:  the versioned open (FSIO design §2.5,
 * §3.1, §4.1, §4.3) */
class FSIORadosVersioned : public FSIORados {
protected:
  void SetUp() override {
    FSIORados::SetUp();
    set_versioning(BUCKET_VERSIONED);
  }
};

/* a reader counts itself on the instance the OLH names, hinted there and
 * on the OLH, and joins the OLH naming the instance;  its close takes all
 * of it away */
TEST_F(FSIORadosVersioned, VersionedReaderCountsOnTheInstance)
{
  std::string inst;
  ASSERT_EQ(0, put("obj", std::string(9 << 20, 'v'), &inst));
  ASSERT_FALSE(inst.empty());
  const auto olh = head_of("obj");
  const auto ih = instance_of("obj", inst);
  auto fh = open("obj", false);
  ASSERT_TRUE(fh);
  auto* hdl = rados_fsio(fh);
  const auto h = hdl->handle();
  EXPECT_EQ(inst, hdl->instance());

  auto oc = ocnt_of(ih);
  ASSERT_TRUE(oc);
  EXPECT_EQ(1u, oc->holders.count(h));
  EXPECT_EQ(1, presence_hints(ih, h, cls_rgw_fsio_hint_target::INSTANCE_HEAD));
  EXPECT_EQ(1, presence_hints(olh, h));
  auto rec = records_of(olh, h.incarnation);
  ASSERT_TRUE(rec.hdr);
  EXPECT_EQ(inst, rec.hdr->cur_instance);
  ASSERT_TRUE(rec.opens);
  EXPECT_EQ(1u, rec.opens->handles.at(h.handle).chunk_sets.at(hdl->bound()).readers);

  ASSERT_EQ(0, fh->close(env->dpp.get(),
			 rgw::sal::Object::FSIOObject::CLOSE_FLAG_NONE));
  oc = ocnt_of(ih);
  EXPECT_TRUE(!oc || !oc->holders.count(h));
  EXPECT_EQ(0, presence_hints(ih, h, cls_rgw_fsio_hint_target::INSTANCE_HEAD));
  EXPECT_EQ(0, presence_hints(olh, h));
  EXPECT_FALSE(records_of(olh, h.incarnation).any_fsio);
}

/* a writer's construction clones the instance head itself (§4.1 step 1),
 * with the instance's manifest as the pseudo-manifest, then its tail */
TEST_F(FSIORadosVersioned, VersionedWriterClonesTheInstance)
{
  const std::string data(9 << 20, 'W');
  std::string inst;
  ASSERT_EQ(0, put("obj", data, &inst));
  const auto olh = head_of("obj");
  const auto ih = instance_of("obj", inst);
  auto fh = open("obj", true);
  ASSERT_TRUE(fh);
  auto* hdl = rados_fsio(fh);
  const auto id = hdl->writing_to();
  auto rec = records_of(olh, hdl->handle().incarnation);
  ASSERT_TRUE(rec.hdr);
  const auto* e = entry_of(*rec.hdr, id);
  ASSERT_NE(nullptr, e);
  EXPECT_EQ(cls_rgw_fsio_cs_state::READY, e->state);

  /* the instance head holds the first chunk's data:  its clone is 'd' */
  auto ic = clone_of(ih, id);
  auto ic_bytes = bytes_of(ic);
  ASSERT_TRUE(ic_bytes) << ic.oid;
  EXPECT_EQ(*bytes_of(ih), *ic_bytes);
  ceph::buffer::list pm, man;
  neo().execute(neorados::Object{ic.oid}, [&] {
		  auto x = ioc_of(ic.pool); x.set_key(ic.loc); return x; }(),
		neorados::ReadOp{}.get_xattr("user.rgw.fsio.pm", &pm), nullptr,
		ceph::async::use_blocked);
  neo().execute(neorados::Object{ih.oid}, ioc_of(ih.pool),
		neorados::ReadOp{}.get_xattr(RGW_ATTR_MANIFEST, &man), nullptr,
		ceph::async::use_blocked);
  EXPECT_TRUE(pm.contents_equal(man));
  for (const auto& c : chunks_of("obj", inst)) {
    auto cl = bytes_of(clone_of(c, id));
    ASSERT_TRUE(cl) << clone_of(c, id).oid;
    EXPECT_EQ(*bytes_of(c), *cl) << clone_of(c, id).oid;
  }
  std::string back;
  ASSERT_EQ(0, get("obj", &back));
  EXPECT_EQ(data, back);
}

/* an open after a new version counts itself on the new instance */
TEST_F(FSIORadosVersioned, VersionedOpenAfterAPutBindsTheNewVersion)
{
  std::string i1, i2;
  ASSERT_EQ(0, put("obj", std::string(1 << 20, '1'), &i1));
  ASSERT_EQ(0, put("obj", std::string(1 << 20, '2'), &i2));
  ASSERT_NE(i1, i2);
  auto fh = open("obj", false);
  ASSERT_TRUE(fh);
  auto* hdl = rados_fsio(fh);
  EXPECT_EQ(i2, hdl->instance());
  EXPECT_FALSE(ocnt_of(instance_of("obj", i1)).has_value() &&
	       ocnt_of(instance_of("obj", i1))->holders.count(hdl->handle()));
  auto oc = ocnt_of(instance_of("obj", i2));
  ASSERT_TRUE(oc);
  EXPECT_EQ(1u, oc->holders.count(hdl->handle()));
}

/* §2.5a:  the OLH naming the null version, the open joins the head as an
 * unversioned one does:  no instance count */
TEST_F(FSIORadosVersioned, NullVersionOpensAsUnversioned)
{
  std::string i1;
  ASSERT_EQ(0, put("obj", std::string(1 << 20, '1'), &i1));
  set_versioning(BUCKET_VERSIONED | BUCKET_VERSIONS_SUSPENDED);
  ASSERT_EQ(0, put("obj", std::string(1 << 20, 'n')));
  auto fh = open("obj", false);
  ASSERT_TRUE(fh);
  auto* hdl = rados_fsio(fh);
  EXPECT_TRUE(hdl->instance().empty());
  auto oc = ocnt_of(instance_of("obj", i1));
  EXPECT_TRUE(!oc || !oc->holders.count(hdl->handle()));
  auto rec = records_of(head_of("obj"), hdl->handle().incarnation);
  ASSERT_TRUE(rec.opens);
  EXPECT_EQ(1u, rec.opens->handles.at(hdl->handle().handle)
		  .chunk_sets.at(hdl->bound()).readers);
}

/* §4.1 step 1:  a handle counted on a version that was rewritten in place
 * since (A13) constructs from the content it counted for:  the deferred
 * record's sibling and manifest, not the rewritten instance head's */
TEST_F(FSIORadosVersioned, WriterConstructsFromTheDeferredSibling)
{
  std::string inst;
  ASSERT_EQ(0, put("obj", std::string(9 << 20, 'o'), &inst));
  const auto ih = instance_of("obj", inst);
  const auto old_chunks = chunks_of("obj", inst);
  ceph::buffer::list old_manifest;
  neo().execute(neorados::Object{ih.oid}, ioc_of(ih.pool),
		neorados::ReadOp{}.get_xattr(RGW_ATTR_MANIFEST, &old_manifest),
		nullptr, ceph::async::use_blocked);

  auto fh = open("obj", false);		// counted on the old content
  ASSERT_TRUE(fh);
  auto* rados = static_cast<rgw::sal::RadosStore*>(env->driver)->getRados();
  ASSERT_EQ(0, rados->rewrite_obj(bucket->get_info(),
				  rgw_obj(bucket->get_key(), rgw_obj_key("obj", inst)),
				  env->dpp.get(), null_yield));
  auto oc = ocnt_of(ih);
  ASSERT_TRUE(oc);
  ASSERT_EQ(1u, oc->deferred.size()) << "the rewrite deferred nothing";
  const auto new_chunks = chunks_of("obj", inst);

  ASSERT_EQ(0, fh->begin_write(env->dpp.get(),
			       rgw::sal::Object::FSIOObject::WRITE_FLAG_NONE));
  const auto id = rados_fsio(fh)->writing_to();
  /* the instance head's clone carries the old manifest */
  auto ic = clone_of(ih, id);
  ceph::buffer::list pm;
  boost::system::error_code ec;
  neo().execute(neorados::Object{ic.oid}, [&] {
		  auto x = ioc_of(ic.pool); x.set_key(ic.loc); return x; }(),
		neorados::ReadOp{}.get_xattr("user.rgw.fsio.pm", &pm), nullptr,
		ceph::async::use_blocked[ec]);
  ASSERT_FALSE(ec) << ic.oid << ":  " << ec.message();
  EXPECT_TRUE(pm.contents_equal(old_manifest));
  /* its tail is the old manifest's chunks, not the rewrite's */
  for (const auto& c : old_chunks) {
    if (c.oid != ih.oid) {
      EXPECT_TRUE(bytes_of(clone_of(c, id))) << "no clone of old " << c.oid;
    }
  }
  for (const auto& c : new_chunks) {
    if (c.oid != ih.oid) {
      EXPECT_FALSE(bytes_of(clone_of(c, id))) << "a clone of new " << c.oid;
    }
  }
}

/* §2.5:  a reader on a version keeps reading it after a put makes a newer
 * one, and after the version is rewritten in place, its head segment then
 * from the deferred sibling */
TEST_F(FSIORadosVersioned, VersionedReaderReadsTheInstance)
{
  const auto a = pattern(9 * MiB, 1), b = pattern(9 * MiB, 2);
  std::string inst;
  ASSERT_EQ(0, put("obj", a, &inst));
  auto fh = open("obj", false);
  ASSERT_TRUE(fh);
  ASSERT_EQ(0, put("obj", b));
  EXPECT_EQ("same", diff(read(fh, 0, 9 * MiB), a, b));
  auto* rados = static_cast<rgw::sal::RadosStore*>(env->driver)->getRados();
  ASSERT_EQ(0, rados->rewrite_obj(bucket->get_info(),
				  rgw_obj(bucket->get_key(), rgw_obj_key("obj", inst)),
				  env->dpp.get(), null_yield));
  auto oc = ocnt_of(instance_of("obj", inst));
  ASSERT_TRUE(oc);
  ASSERT_EQ(1u, oc->deferred.size()) << "the rewrite deferred nothing";
  EXPECT_FALSE(rados_fsio(fh)->moved_segment());
  /* the rewrite copied the bytes, so only where they were read shows it */
  EXPECT_EQ("same", diff(read(fh, 0, 9 * MiB), a, b));
  const auto& moved = rados_fsio(fh)->moved_segment();
  ASSERT_TRUE(moved) << "the head segment was read from the rewritten head";
  EXPECT_NE(std::string::npos, moved->find("~fsio_G")) << *moved;
}

/* §3.1 step 4, first case:  a reader joining with the instance a publish
 * in flight has linked binds that publishing shadow, and reads it through
 * the instance.  Written with the publish (the driver's step 8):  a
 * versioned publish in flight is its precondition.  It will check that
 * the reader counted itself on the linked instance, that its binding is
 * the publishing shadow, not the OLH's previous content, and that once
 * the publish completes the binding is the published instance's. */
TEST_F(FSIORadosVersioned, DISABLED_LinkedPublishReadsThroughTheInstance)
{
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  env = new DriverEnv;
  ::testing::AddGlobalTestEnvironment(env);	// gtest owns it
  return RUN_ALL_TESTS();
}
