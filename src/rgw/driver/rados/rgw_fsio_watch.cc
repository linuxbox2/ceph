// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_fsio_watch.h"

#include <boost/asio/steady_timer.hpp>
#include <boost/system/errc.hpp>
#include <boost/system/system_error.hpp>

#include "common/async/blocked_completion.h"
#include "common/dout.h"
#include "common/error_code.h"
#include "rgw_tools.h"

#define dout_subsys ceph_subsys_rgw

namespace sys = boost::system;

int RGWFSIOWatch::start(const DoutPrefixProvider* dpp,
			const rgw_pool& control_pool, int n)
{
  try {
    ioc = rgw::init_iocontext(dpp, rados, control_pool, rgw::create,
			      ceph::async::use_blocked);
  } catch (const sys::system_error& e) {
    ldpp_dout(dpp, 0) << "ERROR: FSIO:  no ioctx for the control pool "
		      << control_pool << ":  " << e.what() << dendl;
    return ceph::from_error_code(e.code());
  }
  {
    std::lock_guard l{mtx};
    cookies.assign(n, 0);
    stopping = false;
  }
  int registered = 0;
  for (int i = 0; i < n; ++i) {
    sys::error_code ec;
    neorados::WriteOp op;
    op.create(true);
    rados.execute(neorados::Object{oid(i)}, ioc, std::move(op),
		  ceph::async::use_blocked[ec]);
    if (ec && ec != sys::errc::file_exists) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  cannot create " << oid(i)
			<< ":  " << ec.message() << dendl;
      std::lock_guard l{mtx};
      retry(i);
      continue;
    }
    ec.clear();
    auto cookie = rados.watch(neorados::Object{oid(i)}, ioc, std::nullopt,
			      Callback{shared_from_this(), i},
			      ceph::async::use_blocked[ec]);
    std::lock_guard l{mtx};
    if (ec) {
      ldpp_dout(dpp, 0) << "ERROR: FSIO:  cannot watch " << oid(i)
			<< ":  " << ec.message() << dendl;
      retry(i);
      continue;
    }
    cookies[i] = cookie;
    ++registered;
  }
  ldpp_dout(dpp, 1) << "FSIO:  incarnation " << rados.instance_id()
		    << " watches " << registered << " of " << n
		    << " liveness objects" << dendl;
  return registered ? 0 : -EIO;
}

void RGWFSIOWatch::stop()
{
  std::vector<uint64_t> registered;
  {
    std::lock_guard l{mtx};
    stopping = true;
    for (auto& c : cookies) {
      if (c) {
	registered.push_back(c);
	c = 0;
      }
    }
  }
  for (auto c : registered) {
    sys::error_code ec;
    rados.unwatch(c, ioc, ceph::async::use_blocked[ec]);
  }
  rados.flush_watch(ceph::async::use_blocked);
}

std::optional<ceph::timespan> RGWFSIOWatch::since_confirmed()
{
  std::lock_guard l{mtx};
  std::optional<ceph::timespan> best;
  for (auto c : cookies) {
    if (!c) {
      continue;
    }
    if (auto t = rados.check_watch(c); t && (!best || *t < *best)) {
      best = *t;
    }
  }
  return best;
}

void RGWFSIOWatch::Callback::operator()(sys::error_code ec,
					uint64_t notify_id, uint64_t cookie,
					uint64_t notifier_id,
					ceph::buffer::list&& bl)
{
  if (!ec) {
    w->rados.notify_ack(neorados::Object{oid(i)}, w->ioc, notify_id, cookie,
			{}, [](sys::error_code) {});
    return;
  }
  /* the watch failed:  unregister it, then watch again */
  {
    std::lock_guard l{w->mtx};
    if (w->stopping) {
      return;
    }
    w->cookies[i] = 0;
  }
  w->rados.unwatch(cookie, w->ioc,
		   [w = w, i = i](sys::error_code) { w->watch(i); });
}

void RGWFSIOWatch::watch(int i)
{
  {
    std::lock_guard l{mtx};
    if (stopping) {
      return;
    }
  }
  rados.watch(neorados::Object{oid(i)}, ioc, std::nullopt,
	      Callback{shared_from_this(), i},
	      [w = shared_from_this(), i](sys::error_code ec, uint64_t cookie) {
		std::lock_guard l{w->mtx};
		if (w->stopping) {
		  if (!ec) {
		    w->rados.unwatch(cookie, w->ioc, [](sys::error_code) {});
		  }
		  return;
		}
		if (ec) {
		  w->retry(i);
		  return;
		}
		w->cookies[i] = cookie;
	      });
}

void RGWFSIOWatch::retry(int i)
{
  if (stopping) {
    return;
  }
  auto t = std::make_shared<boost::asio::steady_timer>(rados.get_executor(),
						       std::chrono::seconds(1));
  t->async_wait([w = shared_from_this(), i, t](sys::error_code ec) {
    if (!ec) {
      w->watch(i);
    }
  });
}
