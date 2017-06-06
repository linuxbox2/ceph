// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGWRADOS_OBJ_V2_H
#define CEPH_RGWRADOS_OBJ_V2_H

#include <string>
#include <map>

namespace rgw {
  namespace mv2 {

    static constexpr uint32_t manifest_v2_ver = 1;

    class MFSeg
    {
      int foo;
    };

    class RGWObjManifestV2
    {
    public:
      uint32_t ver;

    RGWObjManifestV2()
      : ver(manifest_v2_ver)
	{}
    };
  }; /* mv2 */

  using RGWObjManifestV2 = mv2::RGWObjManifestV2;

}; /* rgw */

#endif /* CEPH_RGWRADOS_OBJ_V2_H */
