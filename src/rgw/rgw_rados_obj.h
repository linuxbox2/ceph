// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGWRADOS_OBJ_H
#define CEPH_RGWRADOS_OBJ_H

#include <boost/variant.hpp>

#include "rgw_rados_obj_v1.h"
#include "rgw_rados_obj_v2.h"

namespace rgw {

  typedef boost::variant <RGWObjManifest,
			  RGWObjManifestV2> RGWObjManifestV; /* XXX rename */
};

#endif /* CEPH_RGWRADOS_OBJ_H */
