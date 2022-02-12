// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#pragma once

#include <string>
#include <string_view>
#include <map>

namespace rgw { namespace lc {

  using namespace std::string_literals;

  class FileEngine {
  public:
    static constexpr std::string_view tempdir = "/tmp/rgwlc";

    bool check();
    bool cleanup();

  };

}} /* namespace rgw::lc */
