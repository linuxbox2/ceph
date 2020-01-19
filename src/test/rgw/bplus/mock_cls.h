// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#ifndef MOCK_CLS_H
#define MOCK_CLS_H

#include "bplus_types.h"
#include <cstdint>
#include <string>
#include <vector>
#include "mio.hpp" // think of the Windows children

namespace rgw::bplus::ondisk {

  class MockHctx
  {
  private:
    mio::mmap_sink mmap;
  public:
    static constexpr uint32_t obj_max_len = 1024 * 1024 * 128;

    static constexpr uint32_t FLAG_NONE =   0x0000;
    static constexpr uint32_t FLAG_CREATE = 0x0001;
    static constexpr uint32_t FLAG_ZERO =   0x0002;
    
    MockHctx(std::string oid,
	    size_t size = obj_max_len,
	    uint32_t flags = FLAG_NONE);

    int read(int ofs, int len, char** outdata, int* outdatalen);
  }; /* MockHctx */

  using cls_method_context_t = void*;

  int cls_cxx_stat2(cls_method_context_t hctx, uint64_t *size,
		    ceph::real_time *mtime);
  int cls_cxx_read2(cls_method_context_t hctx, int ofs, int len,
		    ceph::buffer::list *bl, uint32_t op_flags);
  int cls_cxx_write2(cls_method_context_t hctx, int ofs, int len,
		    ceph::buffer::list *bl, uint32_t op_flags);
  int cls_cxx_write_full(cls_method_context_t hctx, ceph::buffer::list *bl);
  int cls_cxx_replace(cls_method_context_t hctx, int ofs, int len,
			   ceph::buffer::list *bl);
  int cls_cxx_truncate(cls_method_context_t hctx, int ofs);
  int cls_cxx_write_zero(cls_method_context_t hctx, int ofs, int len);

// XXX kill me
  int cls_read(cls_method_context_t hctx, int ofs, int len, char **outdata,
	      int *outdatalen) {
    MockHctx* mhctx = static_cast<MockHctx*>(hctx);
    return mhctx->read(ofs, len, outdata, outdatalen);
  }

} /* namespace */

#endif /* MOCK_CLS_H */
