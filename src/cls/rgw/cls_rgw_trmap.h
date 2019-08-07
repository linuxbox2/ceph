// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef CLS_RGW_TRMAP_H
#define CLS_RGW_TRMAP_H


#include <string>

namespace rgw::cls::trmap {

  class TRMap {
    TRMap() = delete;
    
  public:
    static TRMap* get_instance(const std::string path);
    void unref();
 
    // XXX map ops
    
  }; /* TRMap */

} /* namespace rgw::cls::trmap */

#endif /* CLS_RGW_TRMAP_H */
