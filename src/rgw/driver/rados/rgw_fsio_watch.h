// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <boost/system/error_code.hpp>
#include <fmt/format.h>

#include "include/neorados/RADOS.hpp"
#include "common/ceph_time.h"
#include "rgw_pool_types.h"

class DoutPrefixProvider;

/* FSIO_RADOS_DESIGN.md §4.3:  this gateway's watches on the zone's FSIO
 * liveness objects, through its neorados client, whose global id is the
 * incarnation.  Nothing notifies the objects;  they exist to be watched,
 * and an incarnation is live while it watches any of them.  Made shared,
 * since its asynchronous handlers hold it. */
class RGWFSIOWatch : public std::enable_shared_from_this<RGWFSIOWatch> {
public:
  explicit RGWFSIOWatch(neorados::RADOS& rados) : rados(rados) {}

  /* creates the n objects if absent (never written:  an exclusive create,
   * EEXIST counted done) and watches each;  0 when at least one watch is
   * registered, a failed one retried in the background */
  int start(const DoutPrefixProvider* dpp, const rgw_pool& control_pool,
	    int n);
  /* unwatches each, and waits for its callbacks:  orderly shutdown */
  void stop();

  /* the lease (§4.3, item 1):  the time since the most recently confirmed
   * of its watches;  nullopt when none is registered */
  std::optional<ceph::timespan> since_confirmed();

  static std::string oid(int i) { return fmt::format("fsio.live.{}", i); }

private:
  /* a watch's callback:  acks a notify, though none is expected;  on an
   * error, unwatches and watches again */
  struct Callback {
    std::shared_ptr<RGWFSIOWatch> w;
    int i;
    void operator()(boost::system::error_code ec, uint64_t notify_id,
		    uint64_t cookie, uint64_t notifier_id,
		    ceph::buffer::list&& bl);
  };

  void watch(int i);			// asynchronous:  records the cookie
  void retry(int i);			// mtx held:  watch(i) after a second

  neorados::RADOS& rados;
  neorados::IOContext ioc;
  std::mutex mtx;
  std::vector<uint64_t> cookies;	// 0:  not registered
  bool stopping = false;
};
