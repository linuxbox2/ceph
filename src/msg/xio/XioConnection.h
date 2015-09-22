// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Portions Copyright (C) 2013 CohortFS, LLC
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef XIO_CONNECTION_H
#define XIO_CONNECTION_H

#include <atomic>
#include <boost/intrusive/avl_set.hpp>
#include <boost/intrusive/list.hpp>
extern "C" {
#include "libxio.h"
}
#include "XioInSeq.h"
#include "XioSubmit.h"
#include "msg/Connection.h"
#include "msg/Messenger.h"
#include "messages/MConnect.h"
#include "auth/AuthSessionHandler.h"


/* XXX now used only in xio_client/server */
#define XIO_ALL_FEATURES (CEPH_FEATURES_ALL & \
			  ~CEPH_FEATURE_MSGR_KEEPALIVE2)

#define XIO_NOP_TAG_MARKDOWN 0x0001

namespace bi = boost::intrusive;

class XioPortal;
class XioMessenger;
class XioMsg;

class XioConnection : public Connection
{
public:
  enum type { ACTIVE, PASSIVE };

  enum session_states {
    INIT = 0,
    START,
    UP,
    FLOW_CONTROLLED,
    DISCONNECTED,
    BARRIER
  };

  enum session_startup_states {
    IDLE = 0,
    CONNECTING,
    ACCEPTING,
    READY,
    FAIL
  };

private:
  XioConnection::type xio_conn_type;
  XioPortal *portal;
  std::atomic<bool> connected;
  entity_inst_t peer;
  struct xio_session *session;
  struct xio_connection	*conn;
  pthread_spinlock_t sp;
  std::atomic<uint64_t> send;
  std::atomic<uint64_t> recv;
  uint32_t n_reqs; // Accelio-initiated reqs in progress (!counting partials)
  uint32_t magic;
  uint32_t special_handling;
  uint64_t scount;
  uint32_t send_ctr;
  int q_high_mark;
  int q_low_mark;

  /* batching */
  XioInSeq in_seq;

  class CState
  {
  public:
    static const int FLAG_NONE = 0x0000;
    static const int FLAG_BAD_AUTH = 0x0001;
    static const int FLAG_MAPPED = 0x0002;
    static const int FLAG_RESET = 0x0004;
    static const int FLAG_MARK_DOWN = 0x0008;

    static const int OP_FLAG_NONE = 0x0000;
    static const int OP_FLAG_LOCKED = 0x0001;
    static const int OP_FLAG_LRU = 0x0002;

    XioConnection *xcon;

    uint64_t features;
    Messenger::Policy policy;

    ceph::shared_ptr<AuthSessionHandler> session_security;
    CryptoKey* session_key;
    AuthAuthorizer* authorizer;
    uint32_t protocol_version;

    std::atomic<uint32_t> session_state;
    std::atomic<uint32_t> startup_state;

    uint32_t reconnects;
    uint64_t connect_seq, global_seq, peer_global_seq;
    uint64_t in_seq, out_seq_acked; // atomic<uint64_t>, got receipt
    std::atomic<uint64_t> out_seq;

    uint32_t flags;

    CState(XioConnection* _xcon)
      : xcon(_xcon),
	policy(_xcon->get_default_policy()),
	session_key(nullptr),
	authorizer(nullptr),
	protocol_version(0),
	session_state(INIT),
	startup_state(IDLE),
	in_seq(0),
	out_seq(0),
	flags(FLAG_NONE) {}

    uint64_t get_session_state() {
      return session_state;
    }

    uint64_t get_startup_state() {
      return startup_state;
    }

    void set_in_seq(uint32_t seq) {
      in_seq = seq;
    }

    uint32_t next_out_seq() {
      return ++out_seq;
    };

    // state machine
    int init_active(uint32_t flags);
    int init_passive(uint32_t flags);
    int next_state(Message* m);
    int msg_connect(MConnect *m);
    int msg_connect_reply(MConnectReply *m);
    int msg_connect_reply(MConnectAuthReply *m);
    int msg_connect_auth(MConnectAuth *m);
    int msg_connect_auth_reply(MConnectAuthReply *m);
    int state_up_ready(uint32_t flags);
    int state_flow_controlled(uint32_t flags);
    int state_discon();
    int state_fail(Message* m, uint32_t flags);

  } cstate; /* CState */

  // message submission queue
  struct SendQ {
    Message::Queue mqueue; // deferred
    XioSubmit::Queue requeue;
  } outgoing;

  // conns_entity_map comparison functor
  struct EntityComp
  {
    // for internal ordering
    bool operator()(const XioConnection &lhs,  const XioConnection &rhs) const
      {  return lhs.get_peer() < rhs.get_peer(); }

    // for external search by entity_inst_t(peer)
    bool operator()(const entity_inst_t &peer, const XioConnection &c) const
      {  return peer < c.get_peer(); }

    bool operator()(const XioConnection &c, const entity_inst_t &peer) const
      {  return c.get_peer() < peer;  }
  };

  bi::list_member_hook<> conns_hook;
  bi::avl_set_member_hook<> conns_entity_map_hook;

  typedef bi::list< XioConnection,
		    bi::member_hook<XioConnection, bi::list_member_hook<>,
				    &XioConnection::conns_hook > > ConnList;

  typedef bi::member_hook<XioConnection, bi::avl_set_member_hook<>,
			  &XioConnection::conns_entity_map_hook> EntityHook;

  typedef bi::avl_set< XioConnection, EntityHook,
		       bi::compare<EntityComp> > EntitySet;

  friend class XioPortal;
  friend class XioMessenger;
  friend class XioDispatchHook;
  friend class XioMarkDownHook;
  friend class XioMsg;

  int on_disconnect_event() {
    connected.store(false);
    // per interface comment, we only stage a remote reset if the
    // current policy required it
    if (cstate.policy.resetcheck)
      cstate.flags |= CState::FLAG_RESET;
    return 0;
  }

  int on_teardown_event();

  int xio_qdepth_high_mark() {
    return q_high_mark;
  }

  int xio_qdepth_low_mark() {
    return q_low_mark;
  }

public:
  XioConnection(XioMessenger *m, XioConnection::type _type,
		const entity_inst_t& peer);

  ~XioConnection() {
    if (conn)
      xio_connection_destroy(conn);
  }

  const Messenger::Policy get_default_policy() {
    return static_cast<SimplePolicyMessenger*>(
      get_messenger())->get_default_policy();
  }

  bool is_connected() { return connected; }

  int send_message(Message *m);
  void send_keepalive() {}

  virtual void mark_down() {
    _mark_down(XioConnection::CState::OP_FLAG_NONE);
  }

  int _mark_down(uint32_t flags);
  virtual void mark_disposable();
  int _mark_disposable(uint32_t flags);

  const entity_inst_t& get_peer() const { return peer; }

  XioConnection* get() {
#if 0
    int refs = nref.read();
    cout << "XioConnection::get " << this << " " << refs << std::endl;
#endif
    RefCountedObject::get();
    return this;
  }

  void put() {
    RefCountedObject::put();
#if 0
    int refs = nref.read();
    cout << "XioConnection::put " << this << " " << refs << std::endl;
#endif
  }

  void disconnect() {
    if (is_connected()) {
      connected.store(false);
      xio_disconnect(conn); // normal teardown will clean up conn
    }
  }

  uint32_t get_magic() { return magic; }
  void set_magic(int _magic) { magic = _magic; }
  uint32_t get_special_handling() { return special_handling; }
  void set_special_handling(int n) { special_handling = n; }
  uint64_t get_scount() { return scount; }

  int _retire_msg(struct xio_session *session, struct xio_msg *req,
		  void *conn_user_context);
  int on_msg_req(struct xio_session *session, struct xio_msg *req,
		 int more_in_batch, void *cb_user_context);
  int on_msg_delivered(struct xio_session *session, struct xio_msg *msg,
		       void *conn_user_context);
  int on_msg_send_complete(struct xio_session *session, struct xio_msg *msg,
			   void *conn_user_context);
  int on_msg_error(struct xio_session *session, enum xio_status error,
		   struct xio_msg  *msg, void *conn_user_context);
  void msg_send_fail(XioMsg *xmsg, int code);
  void msg_release_fail(struct xio_msg *msg, int code);
  int flush_input_queue(uint32_t flags);
  int discard_input_queue(uint32_t flags);
  int adjust_clru(uint32_t flags);
};

typedef boost::intrusive_ptr<XioConnection> XioConnectionRef;

class XioLoopbackConnection : public Connection
{
private:
  std::atomic<uint64_t> seq;
public:
  XioLoopbackConnection(XioMessenger *m);

  XioLoopbackConnection* get() {
    return static_cast<XioLoopbackConnection*>(RefCountedObject::get());
  }

  virtual bool is_connected() { return true; }

  int send_message(Message *m);
  void send_keepalive() {}
  void mark_down() {}
  void mark_disposable() {}

  uint32_t get_seq() {
    return seq;
  }

  uint32_t next_seq() {
    return ++seq;
  }
};

typedef boost::intrusive_ptr<XioLoopbackConnection> XioLoopbackConnectionRef;

#endif /* XIO_CONNECTION_H */
