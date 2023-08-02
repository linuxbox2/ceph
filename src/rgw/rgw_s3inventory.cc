// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#include "rgw_s3inventory.h"

#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/util/macros.h>
#include <filesystem>
#include <vector>
#include <atomic>
#include <string_view>

#include "arrow/status.h"
#include "arrow/type.h"
#include "arrow/io/api.h"
#include "arrow/io/file.h"
#include "common/dout.h"
#include "parquet/arrow/schema.h"
#include "parquet/arrow/writer.h"

#include "fmt/format.h"
#include "include/function2.hpp"
#include "common/errno.h"
#include "common/debug.h"


namespace rgw::inventory {

  namespace sf = std::filesystem;

#define ASSERT_OK(expr)							\
  for (arrow::Status _st = arrow::internal::GenericToStatus((expr)); !_st.ok();) \
    abort();

  static inline arrow::Result<std::shared_ptr<arrow::Schema>> s3_inventory_schema()
  {
    /*
      "type": "record",
      "name": "inventory",
      "namespace": "s3",
      "fields": */
    arrow::SchemaBuilder sb;
    ASSERT_OK(sb.AddField(arrow::field("bucket", arrow::utf8(), false /* nullable */)));
    ASSERT_OK(sb.AddField(arrow::field("key", arrow::utf8())));
    ASSERT_OK(sb.AddField(arrow::field("version_id", arrow::utf8())));
    ASSERT_OK(sb.AddField(arrow::field("is_latest", arrow::boolean())));
    ASSERT_OK(sb.AddField(arrow::field("is_delete_marker", arrow::boolean())));
    ASSERT_OK(sb.AddField(arrow::field("size", arrow::int64())));

    // TODO: finish

    return sb.Finish();
  }

  class Engine::EngineImpl
  {
    static constexpr std::string_view work_path_prefix = "rgw-inventory-";

    DoutPrefixProvider* dpp;
    CephContext* cct;
    arrow::MemoryPool* mempool;

    sf::path base_work_path;
    std::shared_ptr<arrow::Schema> schema;

    void init_work_path() {
      base_work_path = cct->_conf.get_val<std::string>("rgw_inventory_work_path");

      /* clear out stale work */
      sf::remove_all(base_work_path);
      sf::create_directory(base_work_path);
    }

    int list_bucket(rgw::sal::Bucket* bucket,
		    const fu2::unique_function<void(const rgw_bucket_dir_entry&) const>& func)
      {
	int ret{0};
	rgw::sal::Bucket::ListParams list_params{};
	rgw::sal::Bucket::ListResults list_results;
	std::vector<rgw_bucket_dir_entry>::iterator obj_iter;

	bool first = true;
	list_params.allow_unordered = false;

      next_page:
	/* XXX entering bucket->list() with list_results.is_truncated ==
	 * true causes a non-terminating loop (at least w/dbstore?)--seems broken */
	ret = bucket->list(dpp, list_params, 1000, list_results, null_yield);
	if (ret < 0) {
	  return ret;
	}

	obj_iter = list_results.objs.begin();
	while (obj_iter != list_results.objs.end()) {
	  /* XXX without this check, re-entering bucket->list() with the
	   * last key as marker presents that key again.  I thought that
	   * searching from K returned a result list starting at K+1? */
	  if ((! first) &&
	      (obj_iter == list_results.objs.begin())) {
	    ++obj_iter;
	  }
	  auto& obj = *obj_iter;
	  list_params.marker = obj.key;
	  func(obj);
	  ++obj_iter;
	}
	if (list_results.is_truncated) {
	  first = false;
	  /* XXX entering bucket->list() with list_results.is_truncated ==
	   * true causes a non-terminating loop--seems broken */
	  list_results.is_truncated = false;
	  goto next_page;
	}

	return 0;
      } /* list_bucket */

  public:
    EngineImpl(DoutPrefixProvider* dpp)
      : dpp(dpp), cct(dpp->get_cct()), mempool(arrow::default_memory_pool())
      {
	auto _schema = s3_inventory_schema();
	if (ARROW_PREDICT_TRUE(_schema.ok())) {
	  schema = _schema.ValueOrDie();
	}
	init_work_path();
      }

    void generate(rgw::sal::Bucket* bucket, output_format format) {
    } /* generate */

  }; /* EngineImpl */

  Engine::Engine(DoutPrefixProvider* dpp) : pimpl(new Engine::EngineImpl(dpp))
  {}

  void Engine::generate(rgw::sal::Bucket* bucket, output_format format)
  {
    return pimpl->generate(bucket, format);
  } /* generate */

	
} /* namespace rgw::inventory */
