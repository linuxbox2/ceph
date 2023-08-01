// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <experimental/propagate_const>

#include "rgw/rgw_sal.h"

namespace rgw::inventory {

  enum class output_format : uint8_t
  {
    parquet,
    orc,
    csv
  };

  class Engine {
  private:
    class EngineImpl;
    std::experimental::propagate_const<
      std::unique_ptr<EngineImpl>> pimpl;
  public:
    Engine();
    void generate(rgw::sal::Bucket* bucket, output_format format);
    
  }; /* Engine */
	
} /* namespace rgw::inventory */
