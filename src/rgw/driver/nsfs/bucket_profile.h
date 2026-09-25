// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab ft=cpp

/*
 * Ceph - scalable distributed file system
 *
 * Copyright contributors to the Ceph project
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#pragma once

#include <cstdint>
#include <string>

namespace rgw { namespace sal { namespace nsfs {

class XattrStrategy;
class PathStrategy;

/* Whether a bucket carries our extensions, and what that resolves to.
 *
 * THE UNMARKED CASE IS NOOBAA'S.  A bucket directory with no marker is
 * read as NooBaa would write it and we add nothing to it:  no shadow
 * subtree, no positional layout, no S3 ACL attribute.  Marking is a
 * declared act -- at creation, or by adopting an existing tree -- and it
 * is what buys the structure our own drivers want.
 *
 * That polarity is the point.  A NooBaa root we are asked to serve is
 * unmarked by construction, because NooBaa never wrote a marker, so the
 * correct reading of it is the default rather than something an operator
 * has to remember to configure.  And a tree we have not extended can go
 * back:  rollback after cutover is refusing to mark, not undoing a
 * conversion.  See project_noobaa_interchange.
 *
 * It also means every nsfs tree written before this existed reads as
 * base, although it was written with our extensions.  Those trees are
 * adopted or wiped;  there is no way to tell them apart from a NooBaa
 * tree by inspection, which is exactly why the marker had to exist.
 */

/* The marker.  A physical xattr name, deliberately NOT routed through
 * XattrStrategy::disk_name().
 *
 * Reading it through a strategy would mean choosing the strategy first,
 * and choosing the strategy is what the marker is for.  Worse, a NooBaa
 * XattrStrategy will not claim a user.nsfs.* name at all -- it returns
 * false from parse_disk_name() and the attribute is dropped as foreign --
 * so the marker would be invisible in precisely the case it decides.
 * It is read with its own fgetxattr, before any strategy is consulted. */
inline constexpr const char* EXTENSIONS_XATTR = "user.nsfs.extensions";

/* The extension set this build implements, as a decimal integer in that
 * attribute.  A bucket carrying a higher number was written by a newer
 * gateway whose additions we do not know how to honour, and is refused
 * rather than served on a guess -- an unrecognised marker is not a
 * fallback and not a warning. */
inline constexpr uint32_t EXTENSIONS_VERSION = 1;

/* 0 is not a version;  it is the absence of the attribute. */
inline constexpr uint32_t EXTENSIONS_NONE = 0;

/* What a bucket's marker resolves to.
 *
 * The strategy pointers are the reason this is a struct rather than a
 * bool.  Both profiles resolve to the same instances today, because the
 * noobaa implementations are S5;  what S4 buys is that S5 can land one
 * strategy at a time without touching the call sites again. */
struct BucketProfile {
  uint32_t extensions{EXTENSIONS_NONE};
  XattrStrategy* xattr_strategy{nullptr};
  PathStrategy* path_strategy{nullptr};

  bool extended() const { return extensions != EXTENSIONS_NONE; }
  const char* name() const { return extended() ? "extended" : "base"; }
};

}}} // namespace rgw::sal::nsfs
