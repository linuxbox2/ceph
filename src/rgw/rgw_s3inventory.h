// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#pragma once

#include <cstdint>
#include <string>

namespace rgw::inventory {

  enum class output_format : uint8_t
  {
    parquet,
    orc,
    csv
  };
    
  
  class Engine {
  public:
    void generate(output_format format);
    
  }; /* Engine */
	
} /* namespace rgw::inventory */
