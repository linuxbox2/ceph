// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

/// \file neorados/cls/rgw_fsio.h
///
/// \brief NeoRADOS interface to FSIO's head methods in the rgw class
///
/// The calls a gateway makes on its own (fsio_join, fsio_leave,
/// fsio_rebind, ...), and
/// the FSIO open hints' add, remove and list on a hint shard.  The
/// calls that ride in RGW's head writes (the head replacement, the
/// publish) stay on librados beside RGW's own ops
/// (FSIO_RADOS_DESIGN.md §3).  Each sets RETURNVEC, so a write op
/// returns its reply, and decodes the reply into `ret` when given.

#include <boost/system/error_code.hpp>
#include <boost/system/errc.hpp>

#include "include/neorados/RADOS.hpp"
#include "cls/rgw/cls_rgw_fsio_ops.h"
#include "neorados/cls/common.h"

namespace neorados::cls::rgw_fsio {
namespace detail {
template <typename Method, typename In, typename Ret>
[[nodiscard]] auto call(const Method& method, const In& in, Ret* ret,
                        boost::system::error_code* ec)
{
  buffer::list bl;
  encode(in, bl);
  return ClsWriteOp{[method, bl = std::move(bl), ret, ec](WriteOp& op) {
    op.returnvec();
    op.exec(method, bl,
            [ret, ec](boost::system::error_code e, const buffer::list& out) {
              if (!e && ret) {
                try {
                  auto it = out.cbegin();
                  decode(*ret, it);
                } catch (const ceph::buffer::error&) {
                  e = make_error_code(boost::system::errc::bad_message);
                }
              }
              if (ec) {
                *ec = e;
              }
            });
  }};
}

template <typename Method, typename In>
[[nodiscard]] auto call(const Method& method, const In& in)
{
  buffer::list bl;
  encode(in, bl);
  return ClsWriteOp{[method, bl = std::move(bl)](WriteOp& op) {
    op.exec(method, bl);
  }};
}
} // namespace detail

namespace m = ::cls::rgw::method;

[[nodiscard]] inline auto join(const cls_rgw_fsio_join_op& in,
                               cls_rgw_fsio_join_ret* ret = nullptr,
                               boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_join, in, ret, ec);
}

[[nodiscard]] inline auto ready(const cls_rgw_fsio_runs_op& in)
{
  return detail::call(m::fsio_ready, in);
}

[[nodiscard]] inline auto extend(const cls_rgw_fsio_runs_op& in)
{
  return detail::call(m::fsio_extend, in);
}

/* §3.3:  a writer's size and mtime hint, and ftruncate()'s exact set */
[[nodiscard]] inline auto hint(const cls_rgw_fsio_hint_size_op& in,
                               cls_rgw_fsio_hint_size_ret* ret = nullptr,
                               boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_hint, in, ret, ec);
}

[[nodiscard]] inline auto truncate(const cls_rgw_fsio_truncate_op& in,
                                   cls_rgw_fsio_truncate_ret* ret = nullptr,
                                   boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_truncate, in, ret, ec);
}

[[nodiscard]] inline auto leave(const cls_rgw_fsio_leave_op& in,
                                cls_rgw_fsio_leave_ret* ret = nullptr,
                                boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_leave, in, ret, ec);
}

[[nodiscard]] inline auto rebind(const cls_rgw_fsio_rebind_op& in,
                                 cls_rgw_fsio_rebind_ret* ret = nullptr,
                                 boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_rebind, in, ret, ec);
}

[[nodiscard]] inline auto publish_release(const cls_rgw_fsio_cs_op& in)
{
  return detail::call(m::fsio_publish_release, in);
}

[[nodiscard]] inline auto fenced_finalize(const cls_rgw_fsio_finalize_op& in,
                                          cls_rgw_fsio_finalize_ret* ret = nullptr,
                                          boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_fenced_finalize, in, ret, ec);
}

[[nodiscard]] inline auto expire(const cls_rgw_fsio_expire_op& in,
                                 cls_rgw_fsio_expire_ret* ret = nullptr,
                                 boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_expire, in, ret, ec);
}

/* §3.5 item 6, Y16:  a later write open's identity, not sequenced */
[[nodiscard]] inline auto writer_identity(const cls_rgw_fsio_identity_op& in)
{
  return detail::call(m::fsio_writer_identity, in);
}

/* §3.4 step 3:  the reply's drained is a holder the forget let drain */
[[nodiscard]] inline auto forget(const cls_rgw_fsio_cs_op& in,
                                 cls_rgw_fsio_forget_ret* ret = nullptr,
                                 boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_forget, in, ret, ec);
}

[[nodiscard]] inline auto drop_seqs(const cls_rgw_fsio_drop_seqs_op& in)
{
  return detail::call(m::fsio_drop_seqs, in);
}

/* §2.5:  gone, and held when gone, are what a gateway needs to settle
 * its presence hint (§4.3) */
[[nodiscard]] inline auto ocnt_add(const cls_rgw_fsio_ocnt_op& in,
                                   cls_rgw_fsio_ocnt_add_ret* ret = nullptr,
                                   boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_ocnt_add, in, ret, ec);
}

[[nodiscard]] inline auto ocnt_remove(const cls_rgw_fsio_ocnt_op& in,
                                      cls_rgw_fsio_flags_ret* ret = nullptr,
                                      boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_ocnt_remove, in, ret, ec);
}

[[nodiscard]] inline auto ocnt_expire(const cls_rgw_fsio_ocnt_op& in,
                                      cls_rgw_fsio_flags_ret* ret = nullptr,
                                      boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_ocnt_expire, in, ret, ec);
}

[[nodiscard]] inline auto ocnt_forget(const cls_rgw_fsio_ocnt_op& in)
{
  return detail::call(m::fsio_ocnt_forget, in);
}

[[nodiscard]] inline auto remove_version(const cls_rgw_fsio_remove_version_op& in,
                                         cls_rgw_fsio_remove_version_ret* ret = nullptr,
                                         boost::system::error_code* ec = nullptr)
{
  return detail::call(m::fsio_remove_version, in, ret, ec);
}

/* the FSIO open hints, on a hint shard (§2.7) */
[[nodiscard]] inline auto open_hint_add(const cls_rgw_fsio_hint_op& in)
{
  return detail::call(m::fsio_open_hint_add, in);
}

[[nodiscard]] inline auto open_hint_remove(const cls_rgw_fsio_hint_op& in)
{
  return detail::call(m::fsio_open_hint_remove, in);
}

[[nodiscard]] inline auto open_hint_list(const cls_rgw_fsio_hint_list_op& in,
                                         cls_rgw_fsio_hint_list_ret* ret,
                                         boost::system::error_code* ec = nullptr)
{
  buffer::list bl;
  encode(in, bl);
  return ClsReadOp{[bl = std::move(bl), ret, ec](ReadOp& op) {
    op.exec(m::fsio_open_hint_list, bl,
            [ret, ec](boost::system::error_code e, const buffer::list& out) {
              if (!e && ret) {
                try {
                  auto it = out.cbegin();
                  decode(*ret, it);
                } catch (const ceph::buffer::error&) {
                  e = make_error_code(boost::system::errc::bad_message);
                }
              }
              if (ec) {
                *ec = e;
              }
            });
  }};
}
} // namespace neorados::cls::rgw_fsio
