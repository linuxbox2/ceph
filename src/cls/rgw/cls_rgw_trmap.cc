// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "cls_rgw_trmap.h"

namespace rgw::cls::trmap {

  TRMap* get_instance(const std::string& path) 
  {
    return nullptr;
  }
  
  extern "C" {
    void get_muggle(void) 
    {
      int x;
    }

  } /* extern "C" */


} /* */
