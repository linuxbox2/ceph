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

#include "XioMsg.h"
#include "XioConnection.h"
#include "XioMessenger.h"
#include "messages/MDataPing.h"

#include "auth/none/AuthNoneProtocol.h" // XXX

#include "include/assert.h"
#include "common/dout.h"

extern struct xio_mempool *xio_msgr_mpool;
extern struct xio_mempool *xio_msgr_noreg_mpool;

#define dout_subsys ceph_subsys_xio

void print_xio_msg_hdr(CephContext *cct, const char *tag,
		       const XioMsgHdr &hdr, const struct xio_msg *msg)
{
  if (msg) {
    ldout(cct,4) << tag <<
      " xio msg:" <<
      " sn: " << msg->sn <<
      " timestamp: " << msg->timestamp <<
      dendl;
  }

  ldout(cct,4) << tag <<
    " ceph header: " <<
    " front_len: " << hdr.hdr->front_len <<
    " seq: " << hdr.hdr->seq <<
    " tid: " << hdr.hdr->tid <<
    " type: " << hdr.hdr->type <<
    " prio: " << hdr.hdr->priority <<
    " name type: " << (int) hdr.hdr->src.type <<
    " name num: " << (int) hdr.hdr->src.num <<
    " version: " << hdr.hdr->version <<
    " compat_version: " << hdr.hdr->compat_version <<
    " front_len: " << hdr.hdr->front_len <<
    " middle_len: " << hdr.hdr->middle_len <<
    " data_len: " << hdr.hdr->data_len <<
    " xio header: " <<
    " msg_cnt: " << hdr.msg_cnt <<
    dendl;

  ldout(cct,4) << tag <<
    " ceph footer: " <<
    " front_crc: " << hdr.ftr->front_crc <<
    " middle_crc: " << hdr.ftr->middle_crc <<
    " data_crc: " << hdr.ftr->data_crc <<
    " sig: " << hdr.ftr->sig <<
    " flags: " << (uint32_t) hdr.ftr->flags <<
    dendl;
}

void print_ceph_msg(CephContext *cct, const char *tag, Message *m)
{
  if (m->get_magic() & (MSG_MAGIC_XIO & MSG_MAGIC_TRACE_DTOR)) {
    ceph_msg_header& header = m->get_header();
    ldout(cct,4) << tag << " header version " << header.version <<
      " compat version " << header.compat_version <<
      dendl;
  }
}

XioConnection::XioConnection(XioMessenger *m, XioConnection::type _type,
			     const entity_inst_t& _peer) :
  Connection(m->cct, m),
  xio_conn_type(_type),
  portal(m->default_portal()),
  connected(false),
  peer(_peer),
  session(nullptr),
  conn(nullptr),
  magic(m->get_magic()),
  scount(0),
  send_ctr(0),
  in_seq(),
  cstate(this)
{
  pthread_spin_init(&sp, PTHREAD_PROCESS_PRIVATE);
  if (xio_conn_type == XioConnection::ACTIVE)
    peer_addr = peer.addr;
  peer_type = peer.name.type();
  set_peer_addr(peer.addr);

  Messenger::Policy policy;
  int64_t max_msgs = 0, max_bytes = 0, bytes_opt = 0;
  int xopt;

  policy = m->get_policy(peer_type);

  if (policy.throttler_messages) {
    max_msgs = policy.throttler_messages->get_max();
    ldout(m->cct,4) << "XioMessenger throttle_msgs: " << max_msgs << dendl;
  }

  xopt = m->cct->_conf->xio_queue_depth;
  if (max_msgs > xopt)
    xopt = max_msgs;

  /* set high mark for send, reserved 20% for credits */
  q_high_mark = xopt * 4 / 5;
  q_low_mark = q_high_mark/2;

  /* set send & receive msgs queue depth */
  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_SND_QUEUE_DEPTH_MSGS,
	      &xopt, sizeof(xopt));
  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_RCV_QUEUE_DEPTH_MSGS,
	      &xopt, sizeof(xopt));

  if (policy.throttler_bytes) {
    max_bytes = policy.throttler_bytes->get_max();
    ldout(m->cct,4) << "XioMessenger throttle_bytes: " << max_bytes << dendl;
  }

  bytes_opt = (2 << 28); /* default: 512 MB */
  if (max_bytes > bytes_opt)
    bytes_opt = max_bytes;

  /* set send & receive total bytes throttle */
  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_SND_QUEUE_DEPTH_BYTES,
	      &bytes_opt, sizeof(bytes_opt));
  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_RCV_QUEUE_DEPTH_BYTES,
	      &bytes_opt, sizeof(bytes_opt));

  ldout(m->cct,4) << "Peer type: " << peer.name.type_str()
		  << " throttle_msgs: " << xopt
		  << " throttle_bytes: " << bytes_opt
		  << dendl;

  /* start with local_features */
  set_features(m->local_features);
}

int XioConnection::send_message(Message *m)
{
  XioMessenger *ms = static_cast<XioMessenger*>(get_messenger());
  return ms->_send_message(m, this);
}

static inline XioDispatchHook* pool_alloc_xio_dispatch_hook(
  XioConnection *xcon, Message *m, XioInSeq& msg_seq)
{
  struct xio_reg_mem mp_mem;
  int e = xpool_alloc(xio_msgr_noreg_mpool,
		      sizeof(XioDispatchHook), &mp_mem);
  if (!!e)
    return NULL;
  XioDispatchHook *xhook = static_cast<XioDispatchHook*>(mp_mem.addr);
  new (xhook) XioDispatchHook(xcon, m, msg_seq, mp_mem);
  return xhook;
}

int XioConnection::on_msg_req(struct xio_session *session,
			      struct xio_msg *req,
			      int more_in_batch,
			      void *cb_user_context)
{
  struct xio_msg *treq = req;

  /* XXX Accelio guarantees message ordering at
   * xio_session */
  if (! in_seq.p()) {
    if (!treq->in.header.iov_len) {
	ldout(msgr->cct,0) << __func__
			   << " empty header: packet out of sequence?"
			   << dendl;
	xio_release_msg(req);
	return 0;
    }

    XioMsgCnt msg_cnt(
      buffer::create_static(treq->in.header.iov_len,
			    (char*) treq->in.header.iov_base));

    ldout(msgr->cct,10) << __func__ << " receive req " << "treq " << treq
			<< " msg_cnt " << msg_cnt.msg_cnt
			<< " iov_base " << treq->in.header.iov_base
			<< " iov_len " << (int) treq->in.header.iov_len
			<< " nents " << treq->in.pdata_iov.nents
			<< " conn " << conn << " sess " << session
			<< " sn " << treq->sn << dendl;
    assert(session == this->session);
    in_seq.set_count(msg_cnt.msg_cnt);
  } else {
    /* XXX major sequence error */
    assert(! treq->in.header.iov_len);
  }

  in_seq.append(req);
  if (in_seq.count() > 0) {
    return 0;
  }

  XioMessenger *msgr = static_cast<XioMessenger*>(get_messenger());
  XioDispatchHook *m_hook =
    pool_alloc_xio_dispatch_hook(this, NULL /* msg */, in_seq);
  XioInSeq& msg_seq = m_hook->msg_seq;
  in_seq.clear();

  ceph_msg_header header;
  ceph_msg_footer footer;
  buffer::list payload, middle, data;

  const utime_t recv_stamp = ceph_clock_now(msgr->cct);

  ldout(msgr->cct,4) << __func__ << " " << "msg_seq.size()="
		     << msg_seq.size() << dendl;

  struct xio_msg* msg_iter = msg_seq.begin();
  treq = msg_iter;
  XioMsgHdr hdr(header, footer,
		buffer::create_static(treq->in.header.iov_len,
				      (char*) treq->in.header.iov_base));

  if (magic & (MSG_MAGIC_TRACE_XCON)) {
    if (hdr.hdr->type == 43) {
      print_xio_msg_hdr(msgr->cct, "on_msg_req", hdr, NULL);
    }
  }

  unsigned int ix, blen, iov_len;
  struct xio_iovec_ex *msg_iov, *iovs;
  uint32_t take_len, left_len = 0;
  char *left_base = NULL;

  ix = 0;
  blen = header.front_len;

  while (blen && (msg_iter != msg_seq.end())) {
    treq = msg_iter;
    iov_len = vmsg_sglist_nents(&treq->in);
    iovs = vmsg_sglist(&treq->in);
    for (; blen && (ix < iov_len); ++ix) {
      msg_iov = &iovs[ix];
      /* XXX need to detect any buffer which needs to be
       * split due to coalescing of a segment (front, middle,
       * data) boundary */
      take_len = MIN(blen, msg_iov->iov_len);
      payload.append(
	buffer::create_msg(
	  take_len, (char*) msg_iov->iov_base, m_hook));
      blen -= take_len;
      if (! blen) {
	left_len = msg_iov->iov_len - take_len;
	if (left_len) {
	  left_base = ((char*) msg_iov->iov_base) + take_len;
	}
      }
    }
    /* XXX as above, if a buffer is split, then we needed to track
     * the new start (carry) and not advance */
    if (ix == iov_len) {
      msg_seq.next(&msg_iter);
      ix = 0;
    }
  }

  if (magic & (MSG_MAGIC_TRACE_XCON)) {
    if (hdr.hdr->type == 43) {
      ldout(msgr->cct,4) << "front (payload) dump:";
      payload.hexdump( *_dout );
      *_dout << dendl;
    }
  }

  blen = header.middle_len;

  if (blen && left_len) {
    middle.append(
      buffer::create_msg(left_len, left_base, m_hook));
    left_len = 0;
  }

  while (blen && (msg_iter != msg_seq.end())) {
    treq = msg_iter;
    iov_len = vmsg_sglist_nents(&treq->in);
    iovs = vmsg_sglist(&treq->in);
    for (; blen && (ix < iov_len); ++ix) {
      msg_iov = &iovs[ix];
      take_len = MIN(blen, msg_iov->iov_len);
      middle.append(
	buffer::create_msg(
	  take_len, (char*) msg_iov->iov_base, m_hook));
      blen -= take_len;
      if (! blen) {
	left_len = msg_iov->iov_len - take_len;
	if (left_len) {
	  left_base = ((char*) msg_iov->iov_base) + take_len;
	}
      }
    }
    if (ix == iov_len) {
      msg_seq.next(&msg_iter);
      ix = 0;
    }
  }

  blen = header.data_len;

  if (blen && left_len) {
    data.append(
      buffer::create_msg(left_len, left_base, m_hook));
    left_len = 0;
  }

  while (blen && (msg_iter != msg_seq.end())) {
    treq = msg_iter;
    iov_len = vmsg_sglist_nents(&treq->in);
    iovs = vmsg_sglist(&treq->in);
    for (; blen && (ix < iov_len); ++ix) {
      msg_iov = &iovs[ix];
      data.append(
	buffer::create_msg(
	  msg_iov->iov_len, (char*) msg_iov->iov_base, m_hook));
      blen -= msg_iov->iov_len;
    }
    if (ix == iov_len) {
      msg_seq.next(&msg_iter);
      ix = 0;
    }
  }

  /* update connection timestamp */
  recv.store(treq->timestamp);

  Message *m =
    decode_message(msgr->cct, msgr->crcflags, header, footer, payload, middle,
		   data);

  if (m) {
    /* completion */
    m->set_connection(this);

    /* reply hook */
    m_hook->set_message(m);
    m->set_completion_hook(m_hook);

    /* trace flag */
    m->set_magic(magic);

    /* update timestamps */
    m->set_recv_stamp(recv_stamp);
    m->set_recv_complete_stamp(ceph_clock_now(msgr->cct));
    m->set_seq(header.seq);

    /* MP-SAFE */
    cstate.set_in_seq(header.seq);

    /* handle connect negotiation */
    if (unlikely(cstate.get_session_state() == XioConnection::START))
      return cstate.next_state(m);

    if (magic & (MSG_MAGIC_TRACE_XCON)) {
      ldout(msgr->cct,4) << "decode m is " << m->get_type() << dendl;
    }

    /* dispatch it */
    msgr->ds_dispatch(m);
  } else {
    /* responds for undecoded messages and frees hook */
    ldout(msgr->cct,4) << "decode m failed" << dendl;
    m_hook->on_err_finalize(this);
  }

  return 0;
} /* XioConnection::on_msg_req */

int XioConnection::_retire_msg(struct xio_session *session,
			       struct xio_msg *req,
			       void *conn_user_context)
{
  /* requester send complete (one-way) */
  uint64_t rc = ++scount;

  XioMsg* xmsg = static_cast<XioMsg*>(req->user_context);
  if (unlikely(magic & MSG_MAGIC_TRACE_CTR)) {
    if (unlikely((rc % 1000000) == 0)) {
      std::cout << "xio finished " << rc << " " << time(0) << std::endl;
    }
  } /* trace ctr */

  ldout(msgr->cct,11) << __func__ << " xcon: " << xmsg->xcon
		      << " session: " << session
		      << " msg: " << req
		      << " sn: " << req->sn
		      << " type: " << xmsg->m->get_type()
		      << " tid: " << xmsg->m->get_tid()
		      << " seq: " << xmsg->m->get_seq()
		      << dendl;

  --send_ctr; /* atomic, because portal thread */

  /* unblock flow-controlled connections, avoid oscillation */
  if (unlikely(cstate.session_state == XioConnection::FLOW_CONTROLLED)) {
    if ((send_ctr <= uint32_t(xio_qdepth_low_mark())) &&
	(1 /* XXX memory <= memory low-water mark */))  {
      cstate.state_up_ready(XioConnection::CState::OP_FLAG_NONE);
      ldout(msgr->cct,2) << __func__ << " xcon: " << xmsg->xcon
			 << " session: " << session
			 << " up_ready from flow_controlled"
			 << dendl;
    }
  }

  xmsg->put();

  return 0;
} /* XioConnection::_retire_msg */

int XioConnection::on_msg_delivered(struct xio_session *session,
				    struct xio_msg *req,
				    void *conn_user_context)
{
  /* if delivery policy is lossy, messages can be retired immediately--
   * if lossless, when delivery is acknowledged */
  assert(!cstate.policy.lossy);
  return _retire_msg(session, req, conn_user_context);
} /* XioConnection::on_msg_delivered */

int XioConnection::on_msg_send_complete(struct xio_session *session,
					struct xio_msg *req,
					void *conn_user_context)
{
  if (!cstate.policy.lossy)
    return 0;

  return _retire_msg(session, req, conn_user_context);
}  /* XioConnection::on_msg_send_complete */

void XioConnection::msg_send_fail(XioMsg *xmsg, int code)
{
  ldout(msgr->cct,2) << "xio_send_msg FAILED xcon: " << this <<
    " xmsg: " << &xmsg->req_0.msg << " code=" << code <<
    " (" << xio_strerror(code) << ")" << dendl;
  /* return refs taken for each xio_msg */
  xmsg->put_msg_refs();
} /* msg_send_fail */

void XioConnection::msg_release_fail(struct xio_msg *msg, int code)
{
  ldout(msgr->cct,2) << "xio_release_msg FAILED xcon: " << this <<
    " xmsg: " << msg <<  "code=" << code <<
    " (" << xio_strerror(code) << ")" << dendl;
} /* msg_release_fail */

int XioConnection::flush_input_queue(uint32_t flags) {
  XioMessenger* msgr = static_cast<XioMessenger*>(get_messenger());
  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_lock(&sp);

  // send deferred 1 (direct backpresssure)
  if (outgoing.requeue.size() > 0)
    portal->requeue(this, outgoing.requeue);

  // send deferred 2 (sent while deferred)
  int ix, q_size = outgoing.mqueue.size();
  for (ix = 0; ix < q_size; ++ix) {
    Message::Queue::iterator q_iter = outgoing.mqueue.begin();
    Message* m = &(*q_iter);
    outgoing.mqueue.erase(q_iter);
    msgr->_send_message_impl(m, this);
  }
  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&sp);
  return 0;
}

int XioConnection::discard_input_queue(uint32_t flags)
{
  Message::Queue disc_q;
  XioSubmit::Queue deferred_q;

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_lock(&sp);

  /* the two send queues contain different objects:
   * - anything on the mqueue is a Message
   * - anything on the requeue is an XioMsg
   */
  Message::Queue::const_iterator i1 = disc_q.end();
  disc_q.splice(i1, outgoing.mqueue);

  XioSubmit::Queue::const_iterator i2 = deferred_q.end();
  deferred_q.splice(i2, outgoing.requeue);

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&sp);

  // mqueue
  while (!disc_q.empty()) {
    Message::Queue::iterator q_iter = disc_q.begin();
    Message* m = &(*q_iter);
    disc_q.erase(q_iter);
    m->put();
  }

  // requeue
  while (!deferred_q.empty()) {
    XioSubmit::Queue::iterator q_iter = deferred_q.begin();
    XioSubmit* xs = &(*q_iter);
    XioMsg* xmsg;
    switch (xs->type) {
      case XioSubmit::OUTGOING_MSG:
	xmsg = static_cast<XioMsg*>(xs);
	deferred_q.erase(q_iter);
	// release once for each chained xio_msg
	xmsg->put(xmsg->hdr.msg_cnt);
	break;
      case XioSubmit::INCOMING_MSG_RELEASE:
	deferred_q.erase(q_iter);
	portal->release_xio_rsp(static_cast<XioRsp*>(xs));
	break;
      default:
	ldout(msgr->cct,0) << __func__ << ": Unknown Msg type " << xs->type
			   << dendl;
	break;
    }
  }

  return 0;
} /* XioConnection::discard_input_queue */

int XioConnection::adjust_clru(uint32_t flags)
{
  if (flags & CState::OP_FLAG_LOCKED)
    pthread_spin_unlock(&sp);

  XioMessenger* msgr = static_cast<XioMessenger*>(get_messenger());
  msgr->conns_sp.lock();
  pthread_spin_lock(&sp);

  if (cstate.flags & CState::FLAG_MAPPED) {
    XioConnection::ConnList::iterator citer =
      XioConnection::ConnList::s_iterator_to(*this);
    msgr->conns_list.erase(citer);
    msgr->conns_list.push_front(*this); // LRU
  }

  msgr->conns_sp.unlock();

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&sp);

  return 0;
} /* XioConnection::adjust_clru */

int XioConnection::on_teardown_event() {
  pthread_spin_lock(&sp);
  if (conn)
    xio_connection_destroy(conn);
  conn = nullptr;
  pthread_spin_unlock(&sp);
  /* if connection policy lossy or we were marked down, discard */
  if (cstate.policy.lossy ||
      (cstate.flags & CState::FLAG_MARK_DOWN)) {
    static_cast<XioMessenger*>(
      this->get_messenger())->unmap_connection(this); /* ref -1 */
    /* XXX no call path ref, can't touch 'this' */
    goto out;
  }
  /* do nothing */
out:
  return 0;
} /* XioConnection::on_teardown_event() */

int XioConnection::on_msg_error(struct xio_session *session,
				enum xio_status error,
				struct xio_msg  *msg,
				void *conn_user_context)
{
  /* XXX if an outgoing msg has errored and we are not
   * shutting down, requeue it for delivery at the next
   * reconnection */
  XioMsg *xmsg = static_cast<XioMsg*>(msg->user_context);
  if (xmsg) {
    if (static_cast<XioMessenger*>(get_messenger())->shutdown_called)
      xmsg->put();
    else {
      pthread_spin_lock(&sp);
      outgoing.requeue.push_back(*xmsg);
      pthread_spin_unlock(&sp);
    }
  }
  --send_ctr; /* atomic, because portal thread */
  return 0;
} /* on_msg_error */

int XioConnection::_mark_down(uint32_t flags)
{
  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_lock(&sp);

  cstate.flags |= CState::FLAG_MARK_DOWN;

  /* XXX mark_down is precisely when reconnect state, and any
   * queued outgoing messages, may be discarded */
  discard_input_queue(flags|CState::OP_FLAG_LOCKED);

  // Accelio disconnect
  if (connected)
    xio_disconnect(conn);
  else
    static_cast<XioMessenger*>(
      this->get_messenger())->unmap_connection(this); /* ref -1 */

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&sp);

  return 0;
} /* XioConnection::_mark_down */

void XioConnection::mark_disposable()
{
  _mark_disposable(XioConnection::CState::OP_FLAG_NONE);
}

int XioConnection::_mark_disposable(uint32_t flags)
{
  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_lock(&sp);

  cstate.policy.lossy = true;

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&sp);

  return 0;
}

int XioConnection::CState::init_active(uint32_t flags)
{
  dout(11) << __func__ << " ENTER " << dendl;

  assert(xcon->xio_conn_type==XioConnection::ACTIVE);
  session_state.store(XioConnection::START);
  startup_state.store(XioConnection::CONNECTING);
  XioMessenger* msgr
    = static_cast<XioMessenger*>(xcon->get_messenger());
  MConnect* m = new MConnect();
  m->addr = msgr->get_myinst().addr;
  m->name = msgr->get_myinst().name;
  m->flags = 0;

  // XXX check
  m->connect_seq = ++connect_seq;
  m->last_in_seq = in_seq;
  m->last_out_seq = out_seq;

  // send it
  msgr->_send_message_impl(m, xcon);

  return 0;
}

int XioConnection::CState::init_passive(uint32_t flags)
{
  dout(11) << __func__ << " ENTER " << dendl;

  assert(xcon->xio_conn_type==XioConnection::PASSIVE);
  session_state.store(START);
  startup_state.store(ACCEPTING);

  return 0;
}

int XioConnection::CState::next_state(Message* m)
{
  dout(11) << __func__ << " ENTER " << dendl;

  switch (m->get_type()) {
  case MSG_CONNECT:
    return msg_connect(static_cast<MConnect*>(m));
  break;
  case MSG_CONNECT_REPLY:
    return msg_connect_reply(static_cast<MConnectReply*>(m));
    break;
  case MSG_CONNECT_AUTH:
    return msg_connect_auth(static_cast<MConnectAuth*>(m));
  break;
  case MSG_CONNECT_AUTH_REPLY:
    return msg_connect_auth_reply(static_cast<MConnectAuthReply*>(m));
    break;
  default:
    abort();
  };

  m->put();
  return 0;
} /* next_state */

int XioConnection::CState::state_up_ready(uint32_t flags)
{
  ldout(xcon->get_messenger()->cct,10) << "xcon " << xcon
				       << " up_ready on session "
				       << xcon->session
				       << dendl;

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_lock(&xcon->sp);

  session_state.store(UP);
  startup_state.store(READY);

  xcon->flush_input_queue(flags|CState::OP_FLAG_LOCKED);

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&xcon->sp);

  return 0;
}

int XioConnection::CState::state_discon()
{
  session_state.store(DISCONNECTED);
  startup_state.store(IDLE);

  return 0;
}

int XioConnection::CState::state_flow_controlled(uint32_t flags)
{
  if (! (flags & OP_FLAG_LOCKED))
    pthread_spin_lock(&xcon->sp);

  session_state.store(FLOW_CONTROLLED);

  if (! (flags & OP_FLAG_LOCKED))
    pthread_spin_unlock(&xcon->sp);

  return (0);
}

int XioConnection::CState::msg_connect(MConnect *m)
{
  if (xcon->xio_conn_type != XioConnection::PASSIVE) {
    m->put();
    return -EINVAL;
  }

  dout(11) << __func__ << " ENTER " << dendl;

  xcon->peer.name = m->name;
  xcon->peer.addr = xcon->peer_addr = m->addr;
  xcon->peer_type = m->name.type();
  xcon->peer_addr = m->addr;

  XioMessenger* msgr = static_cast<XioMessenger*>(xcon->get_messenger());
  policy = msgr->get_policy(xcon->peer_type);

  dout(11) << "accept of host_type " << xcon->peer_type
	   << ", policy.lossy=" << policy.lossy
	   << " policy.server=" << policy.server
	   << " policy.standby=" << policy.standby
	   << " policy.resetcheck=" << policy.resetcheck
	   << dendl;

  MConnectReply* m2 = new MConnectReply();
  m2->addr = msgr->get_myinst().addr;
  m2->name = msgr->get_myinst().name;
  m2->flags = 0;

  // XXX check
  m2->last_in_seq = in_seq;
  m2->last_out_seq = out_seq;

  // send m2
  msgr->_send_message_impl(m2, xcon);

  // dispose m
  m->put();

  return 0;
} /* msg_connect */

int XioConnection::CState::msg_connect_reply(MConnectReply *m)
{
  if (xcon->xio_conn_type != XioConnection::ACTIVE) {
    m->put();
    return -EINVAL;
  }

  dout(11) << __func__ << " ENTER " << dendl;

  // XXX do we need any data from this phase?
  XioMessenger* msgr = static_cast<XioMessenger*>(xcon->get_messenger());
  authorizer =
    msgr->ms_deliver_get_authorizer(xcon->peer_type, false /* force_new */);

  MConnectAuth* m2 = new MConnectAuth();
  m2->features = policy.features_supported;
  m2->flags = 0;
  if (policy.lossy)
    m2->flags |= CEPH_MSG_CONNECT_LOSSY; // fyi, actually, server decides

  // XXX move seq capture to init_state()?
  m2->global_seq = global_seq = msgr->get_global_seq(); // msgr-wide seq
  m2->connect_seq = connect_seq; // semantics?

  // serialize authorizer in data[0]
  m2->authorizer_protocol = authorizer ? authorizer->protocol : 0;
  m2->authorizer_len = authorizer ? authorizer->bl.length() : 0;
  if (m2->authorizer_len) {
    buffer::ptr bp = buffer::create(m2->authorizer_len);
    bp.copy_in(0 /* off */, m2->authorizer_len, authorizer->bl.c_str());
    m2->get_data().append(bp);
  }

  // send m2
  msgr->_send_message_impl(m2, xcon);

  // dispose m
  m->put();

  return 0;
} /* msg_connect_reply 1 */

int XioConnection::CState::msg_connect_reply(MConnectAuthReply *m)
{
  if (xcon->xio_conn_type != XioConnection::ACTIVE) {
    m->put();
    return -EINVAL;
  }

  dout(11) << __func__ << " ENTER " << dendl;

 // XXX do we need any data from this phase?
  XioMessenger* msgr = static_cast<XioMessenger*>(xcon->get_messenger());
  authorizer =
    msgr->ms_deliver_get_authorizer(xcon->peer_type, true /* force_new */);

  MConnectAuth* m2 = new MConnectAuth();
  m2->features = policy.features_supported;
  m2->flags = 0;
  if (policy.lossy)
    m2->flags |= CEPH_MSG_CONNECT_LOSSY; // fyi, actually, server decides

  // XXX move seq capture to init_state()?
  m2->global_seq = global_seq = msgr->get_global_seq(); // msgr-wide seq
  m2->connect_seq = connect_seq; // semantics?

  // serialize authorizer in data[0]
  m2->authorizer_protocol = authorizer ? authorizer->protocol : 0;
  m2->authorizer_len = authorizer ? authorizer->bl.length() : 0;
  if (m2->authorizer_len) {
    buffer::ptr bp = buffer::create(m2->authorizer_len);
    bp.copy_in(0 /* off */, m2->authorizer_len, authorizer->bl.c_str());
    m2->get_data().append(bp);
  }

  // send m2
  msgr->_send_message_impl(m2, xcon);

  // dispose m
  m->put();

  return 0;
} /* msg_connect_reply 2 */

int XioConnection::CState::msg_connect_auth(MConnectAuth *m)
{
  if (xcon->xio_conn_type != XioConnection::PASSIVE) {
    m->put();
    return -EINVAL;
  }

  dout(11) << __func__ << " ENTER " << dendl;

  bool auth_valid;
  uint64_t fdelta;
  buffer::list auth_bl, auth_reply_bl;
  buffer::ptr bp;

  XioMessenger* msgr = static_cast<XioMessenger*>(xcon->get_messenger());
  int peer_type = xcon->peer.name.type();

  MConnectAuthReply* m2 = new MConnectAuthReply();

  m2->protocol_version = protocol_version =
    msgr->get_proto_version(peer_type, false);
  if (m->protocol_version != m2->protocol_version) {
    m2->tag = CEPH_MSGR_TAG_BADPROTOVER;
    goto send_m2;
  }

  // required CephX features
  if (m->authorizer_protocol == CEPH_AUTH_CEPHX) {
    if (peer_type == CEPH_ENTITY_TYPE_OSD ||
	peer_type == CEPH_ENTITY_TYPE_MDS) {
      if (msgr->cct->_conf->cephx_require_signatures ||
	  msgr->cct->_conf->cephx_cluster_require_signatures) {
	policy.features_required |= CEPH_FEATURE_MSG_AUTH;
	}
    } else {
      if (msgr->cct->_conf->cephx_require_signatures ||
	  msgr->cct->_conf->cephx_service_require_signatures) {
	policy.features_required |= CEPH_FEATURE_MSG_AUTH;
      }
    }
  }

  fdelta = policy.features_required & ~(uint64_t(m->features));
  if (fdelta) {
    m2->tag = CEPH_MSGR_TAG_FEATURES;
    goto send_m2;
  }

  // decode authorizer
  if (m->authorizer_len) {
    bp = buffer::create(m->authorizer_len);
    bp.copy_in(0 /* off */, m->authorizer_len, m->get_data().c_str());
    auth_bl.push_back(bp);
  }

  if (!msgr->ms_deliver_verify_authorizer(
	xcon->get(), peer_type, m->authorizer_protocol, auth_bl,
	auth_reply_bl, auth_valid, *session_key) || !auth_valid) {
    m2->tag = CEPH_MSGR_TAG_BADAUTHORIZER;
    session_security.reset();
    goto send_m2;
  }

  // RESET check
  if (policy.resetcheck) {
    pthread_spin_lock(&xcon->sp);
    if (xcon->cstate.flags & FLAG_RESET) {
      m2->tag = CEPH_MSGR_TAG_RESETSESSION;
      // XXX need completion functor (XioMsg::on_msg_delivered)
      pthread_spin_unlock(&xcon->sp);
      goto send_m2;
    }
    pthread_spin_unlock(&xcon->sp);
  }

  // XXX sequence checks

  // ready
  m2->tag = CEPH_MSGR_TAG_READY;
  m2->features = policy.features_supported;
  m2->global_seq = msgr->get_global_seq();
  m2->connect_seq = connect_seq;
  m2->flags = 0;
  m2->authorizer_len = auth_reply_bl.length();
  if (m2->authorizer_len) {
    buffer::ptr bp = buffer::create(m2->authorizer_len);
    bp.copy_in(0 /* off */, m2->authorizer_len, auth_reply_bl.c_str());
    m2->get_data().append(bp);
  }

  if (policy.lossy)
    m2->flags |= CEPH_MSG_CONNECT_LOSSY;

  features = m2->features;

  session_security.reset(
    get_auth_session_handler(msgr->cct, m2->authorizer_protocol,
			    *session_key, features));

  /* XXX can flush msgs, should precede hook */
  state_up_ready(XioConnection::CState::OP_FLAG_NONE);

  /* notify hook */
  msgr->ms_deliver_handle_accept(xcon);
  msgr->ms_deliver_handle_fast_accept(xcon);

send_m2:
  msgr->_send_message_impl(m2, xcon);

  // dispose m
  m->put();

  return 0;
} /* msg_connect_auth */

int XioConnection::CState::msg_connect_auth_reply(MConnectAuthReply *m)
{
  if (xcon->xio_conn_type != XioConnection::ACTIVE) {
    m->put();
    return -EINVAL;
  }

  dout(11) << __func__ << " ENTER " << dendl;

  buffer::list auth_bl;
  buffer::ptr bp;

  XioMessenger* msgr = static_cast<XioMessenger*>(xcon->get_messenger());

  m->features = ceph_sanitize_features(m->features);

  if (m->tag == CEPH_MSGR_TAG_FEATURES) {
    dout(4)  << "connect protocol feature mismatch, my " << std::hex
	     << features << " < peer " << m->features
	     << " missing " << (m->features & ~policy.features_supported)
	     << std::dec << dendl;
    state_fail(m, OP_FLAG_NONE); // XXX correct?
  }

  if (m->tag == CEPH_MSGR_TAG_BADPROTOVER) {
    dout(4) << "connect protocol version mismatch, my " << protocol_version
	    << " != " << m->protocol_version << dendl;
    state_fail(m, OP_FLAG_NONE); // XXX correct?
    goto dispose_m;
  }

  if (m->tag == CEPH_MSGR_TAG_BADAUTHORIZER) {
    dout(4) << "connect got BADAUTHORIZER" << dendl;
    if (flags & FLAG_BAD_AUTH) {
      // prevent oscillation
      flags &= ~FLAG_BAD_AUTH; // XXX terminal state could reset flags?
      state_fail(m, OP_FLAG_NONE); // XXX correct?
      goto dispose_m;
    } else {
      flags |= FLAG_BAD_AUTH;
      return msg_connect_reply(m);
    }
  }

  if (m->tag == CEPH_MSGR_TAG_RESETSESSION) {
    dout(4) << "connect got RESETSESSION" << dendl;

    xcon->discard_input_queue(OP_FLAG_NONE);
    in_seq = 0;
    if (features & CEPH_FEATURE_MSG_AUTH) {
      (void) get_random_bytes((char *)&out_seq, sizeof(out_seq));
      out_seq = (out_seq & 0x7fffffff);
    } else {
      out_seq = 0;
    }
    connect_seq = 0;
    // notify ULP
    msgr->ms_deliver_handle_reset(xcon);
    /* restart negotiation */
    init_active(XioConnection::CState::OP_FLAG_NONE);
    goto dispose_m;
  }

  /* XXX can we remove global_seq? */
  if (m->tag == CEPH_MSGR_TAG_RETRY_GLOBAL) {
    global_seq = msgr->get_global_seq(m->global_seq);
    dout(4) << "connect got RETRY_GLOBAL " << m->global_seq
	    << " chose new " << global_seq << dendl;
    /* restart negotiation */
    init_active(XioConnection::CState::OP_FLAG_NONE);
    goto dispose_m;
  }

  if (!! authorizer) {
    if (m->authorizer_len) {
      bp = buffer::create(m->authorizer_len);
      bp.copy_in(0 /* off */, m->authorizer_len, m->get_data().c_str());
      auth_bl.push_back(bp);
      bufferlist::iterator iter = auth_bl.begin();
      if (!authorizer->verify_reply(iter)) {
	return state_fail(m, OP_FLAG_NONE);
      }
    } else {
      return state_fail(m, OP_FLAG_NONE);
    }
  }

  if (m->tag == CEPH_MSGR_TAG_READY) {
    uint64_t fdelta = policy.features_required & ~((uint64_t) m->features);
    if (fdelta) {
      dout(4) << "missing required features " << std::hex << fdelta
	      << std::dec << dendl;
      // XXX don't understand intended behavior
      return state_fail(m, OP_FLAG_NONE);
    }
  }

  // TODO check sequence updates commented here

  // hooray!
  //peer_global_seq = m->global_seq;
  policy.lossy = m->flags & CEPH_MSG_CONNECT_LOSSY;
  //connect_seq = cseq + 1;
  //assert(connect_seq == reply.connect_seq);
  features = ((uint64_t) m->features & (uint64_t) features);

  if (!! authorizer) {
    session_security.reset(
      get_auth_session_handler(
	msgr->cct, authorizer->protocol, authorizer->session_key, features));
    delete authorizer; authorizer = NULL;
      }  else {
      // no authorizer, so we shouldn't be applying security to messages
      session_security.reset();
    }

  state_up_ready(XioConnection::CState::OP_FLAG_NONE);

  // notify ULP
  msgr->ms_deliver_handle_connect(xcon);

dispose_m:
  m->put();

  return 0;
} /* msg_connect_reply */

int XioConnection::CState::state_fail(Message* m, uint32_t flags)
{
  if (! (flags & OP_FLAG_LOCKED))
    pthread_spin_lock(&xcon->sp);

  // advance to state FAIL, drop queued, msgs, adjust LRU
  session_state.store(DISCONNECTED);
  startup_state.store(FAIL);

  xcon->discard_input_queue(flags|OP_FLAG_LOCKED);
  xcon->adjust_clru(flags|OP_FLAG_LOCKED|OP_FLAG_LRU);

  // Accelio disconnect
  xio_disconnect(xcon->conn);

  if (! (flags & OP_FLAG_LOCKED))
    pthread_spin_unlock(&xcon->sp);

  // notify ULP
  XioMessenger* msgr = static_cast<XioMessenger*>(xcon->get_messenger());
  msgr->ms_deliver_handle_reset(xcon);
  m->put();

  return 0;
}

XioLoopbackConnection::XioLoopbackConnection(XioMessenger *m)
  : Connection(m->cct, m), seq(0)
{
  const entity_inst_t& m_inst = m->get_myinst();
  peer_addr = m_inst.addr;
  peer_type = m_inst.name.type();
  set_features(m->local_features);
}

int XioLoopbackConnection::send_message(Message *m)
{
  XioMessenger *ms = static_cast<XioMessenger*>(get_messenger());
  m->set_connection(this);
  m->set_seq(next_seq());
  m->set_src(ms->get_myinst().name);
  ms->ds_dispatch(m);
  return 0;
}
