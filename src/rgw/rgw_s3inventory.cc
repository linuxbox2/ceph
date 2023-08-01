// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#include "rgw_s3inventory.h"

#include <filesystem>

#include "arrow/status.h"
#include "arrow/type.h"
#include "arrow/io/api.h"
#include "arrow/io/file.h"
#include "parquet/arrow/schema.h"
#include "parquet/arrow/writer.h"

#include "common/errno.h"
#include "common/debug.h"


namespace rgw::inventory {

  class Engine::EngineImpl
  {
  public:
    EngineImpl()
      {}

    void generate(rgw::sal::Bucket* bucket, output_format format) {
    } /* generate */

  }; /* EngineImpl */

  Engine::Engine() : pimpl(new Engine::EngineImpl())
  {}

  void Engine::generate(rgw::sal::Bucket* bucket, output_format format)
  {
    return pimpl->generate(bucket, format);
  } /* generate */

	
} /* namespace rgw::inventory */
