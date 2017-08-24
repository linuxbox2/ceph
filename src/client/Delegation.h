// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef _CEPH_CLIENT_DELEGATION_H
#define _CEPH_CLIENT_DELEGATION_H

#include "include/xlist.h"
#include "common/Clock.h"

#include "Fh.h"

/*
 * This is the callback that the client application will receive when the
 * delegation is being recalled.
 *
 * FIXME: specify the context in which the callbacks are run. Cap revocation
 * for sure, plus other situations like local write opens. Do we need a
 * dedicated thread or is there a generic workqueue of sorts?
 *
 * I'm inclined to punt on the whole thing and offer no thread safety
 * guarantees. This should work something like a signal handler. You want to
 * do as little as possible in the callback, and it should have zero chance of
 * failure. Any major work should be deferred in some fashion as it's
 * difficult to predict the context in which this function will be called.
 */
typedef void (*ceph_deleg_cb_t)(Fh *fh, void *priv);

/*
 * A delegation is a container for holding caps on behalf of a client that
 * wants to be able to rely on them until recalled.
 */
class Delegation {
protected:
  // per-inode linkage
  xlist<Delegation*>::item	inode_item;

  friend class Inode;

private:
  // Filehandle against which it was acquired
  Fh				*fh;

  // opaque token that will be passed to the callback
  void				*priv;

  // Delegation mode (CEPH_FILE_MODE_RD, CEPH_FILE_MODE_RDWR) (other flags later?)
  unsigned			mode;

  // callback into application to recall delegation
  ceph_deleg_cb_t		recall_cb;

  // time of first recall
  utime_t			recall_time;

public:
  Delegation(Fh *_fh, unsigned _mode, ceph_deleg_cb_t _cb, void *_priv)
	: inode_item(this), fh(_fh), priv(_priv), mode(_mode),
	  recall_cb(_cb), recall_time(utime_t()) {};

  Fh *get_fh() { return fh; }
  unsigned get_mode() { return mode; }

  /* Update existing deleg with new mode, cb, and priv value */
  void reinit(unsigned _mode, ceph_deleg_cb_t _recall_cb, void *_priv)
  {
    mode = _mode;
    recall_cb = _recall_cb;
    priv = _priv;
  }

  bool is_recalled() { return !recall_time.is_zero(); }

  void recall(bool skip_read)
  {
    /* If ro is true, don't break read delegations */
    if (skip_read && mode == CEPH_FILE_MODE_RD)
      return;

    if (!is_recalled()) {
      recall_cb(fh, priv);
      recall_time = ceph_clock_now();
    }
  }
};
#endif /* _CEPH_CLIENT_DELEGATION_H */
