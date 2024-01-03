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
 * Foundation.  See file COPYING.
 *
 */

#include "rgw_cksum_pipe.h"
#include <memory>
#include <string>
#include <fmt/format.h>
#include <boost/algorithm/string.hpp>
#include "rgw_common.h"
#include "common/dout.h"
#include "rgw_client_io.h"

namespace rgw::putobj {

  RGWPutObj_Cksum::RGWPutObj_Cksum(rgw::sal::DataProcessor* next,
				   rgw::cksum::Type _type,
				   cksum_hdr_t&& _hdr)
    : Pipe(next),
      dv(rgw::cksum::digest_factory(_type)), cksum_hdr(_hdr),
      _state(State::START)
  {}

  std::unique_ptr<RGWPutObj_Cksum> RGWPutObj_Cksum::Factory(
    rgw::sal::DataProcessor* next, const RGWEnv& env)
  {
    /* look for matching headers */
    const auto algo = env.get("HTTP_X_AMZ_CHECKSUM_ALGORITHM");
    if (algo) {
      std::string ck_key
	= fmt::format("HTTP_X_AMZ_CHECKSUM_{}",
		      boost::to_upper_copy<std::string>(algo));
      auto hdr = cksum_hdr_t{ck_key.c_str(), env.get(ck_key.c_str())};
      return  std::make_unique<RGWPutObj_Cksum>(
          next,
	  rgw::cksum::Type::sha256,
	  std::move(hdr));
    }
    throw rgw::io::Exception(ERR_BAD_DIGEST, std::system_category());
  }

  int RGWPutObj_Cksum::process(ceph::buffer::list &&data, uint64_t logical_offset)
  {
    for (const auto& ptr : data.buffers()) {
      _digest->Update(reinterpret_cast<const unsigned char*>(ptr.c_str()),
                      ptr.length());
    }
    return 0;
  }

  RGWPutObj_Cksum::~RGWPutObj_Cksum()
  {
    if ((_state > State::START) &&
	(_state < State::FINAL))
      (void) finalize();
  }
} // namespace rgw::putobj
