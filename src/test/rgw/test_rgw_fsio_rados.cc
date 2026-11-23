// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/* The RADOS FSIO driver's harness (FSIO_RADOS_WORK_PLAN.md, Phase 3, the
 * driver's step 1):  RadosStore brought up from $CEPH_CONF on a RADOS
 * vstart whose zonegroup has the fsio feature, and each FSIO op called
 * through the SAL directly, with no librgw.  Each op's tests land with
 * the op. */

#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "cls/rgw/cls_rgw_fsio_ops.h"
#include "common/async/blocked_completion.h"
#include "common/async/context_pool.h"
#include "common/common_init.h"
#include "common/error_code.h"
#include "common/random_string.h"
#include "global/global_context.h"
#include "gtest/gtest.h"
#include "include/neorados/RADOS.hpp"
#include "neorados/cls/rgw_fsio.h"
#include "rgw/driver/rados/rgw_fsio_watch.h"
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
					false /* use_gc */);
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
      bucket->remove(env->dpp.get(), true /* purge */, null_yield);
    }
  }

  /* an object as an S3 PUT writes it:  the content an open clones */
  int put(const std::string& key, const std::string& data) {
    auto obj = bucket->get_object(rgw_obj_key(key));
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

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  env = new DriverEnv;
  ::testing::AddGlobalTestEnvironment(env);	// gtest owns it
  return RUN_ALL_TESTS();
}
