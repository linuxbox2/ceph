// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef RGW_LIB_H
#define RGW_LIB_H

#include "include/unordered_map.h"
#include "rgw_common.h"
#include "rgw_client_io.h"
#include "rgw_rest.h"
#include "rgw_request.h"
#include "rgw_frontend.h"
#include "rgw_process.h"
#include "rgw_rest_s3.h" // RGW_Auth_S3


class RGWLibFrontendConfig;
class RGWLibFrontend;
class OpsLogSocket;

class RGWLib {
  RGWFrontendConfig* fec;
  RGWLibFrontend* fe;
  OpsLogSocket* olog;
  RGWREST rest; // XXX needed for RGWProcessEnv
  RGWProcessEnv env;
  RGWRados* store;

public:
  RGWLib() {}
  ~RGWLib() {}

  RGWRados* get_store() { return store; }

  RGWLibFrontend* get_fe() { return fe; }

  int init();
  int init(vector<const char *>& args);
  int stop();
};

extern RGWLib librgw;

/* request interface */

class RGWLibIO : public RGWClientIO
{
  RGWUserInfo user_info;
public:
  RGWLibIO() {}
  RGWLibIO(const RGWUserInfo &_user_info)
    : user_info(_user_info) {}

  virtual void init_env(CephContext *cct) {}

  int set_uid(RGWRados* store, string& uid);

  const RGWUserInfo& get_user() {
    return user_info;
  }

  int complete_request() { /* XXX */
    return 0;
  };

}; /* RGWLibIO */

/* XXX */
class RGWRESTMgr_Lib : public RGWRESTMgr {
public:
  RGWRESTMgr_Lib() {}
  virtual ~RGWRESTMgr_Lib() {}
}; /* RGWRESTMgr_Lib */

/* XXX */
class RGWHandler_Lib : public RGWHandler {
  friend class RGWRESTMgr_Lib;
public:

  virtual int authorize();

  RGWHandler_Lib() {}
  virtual ~RGWHandler_Lib() {}
  static int init_from_header(struct req_state *s);
}; /* RGWHandler_Lib */

class RGWLibRequest : public RGWRequest,
		      public RGWHandler_Lib {
public:
  CephContext* cct;
  RGWUserInfo* user;

  /* unambiguiously return req_state */
  inline struct req_state* get_state() { return this->RGWRequest::s; }

  RGWLibRequest(CephContext* _cct, RGWUserInfo* _user)
    :  RGWRequest(0), cct(_cct), user(_user)
    {}

  RGWUserInfo* get_user() { return user; }

  /* descendant equivalent of *REST*::init_from_header(...):
   * prepare request for execute()--should mean, fixup URI-alikes
   * and any other expected stat vars in local req_state, for
   * now */
  virtual int header_init() = 0;

  /* descendant initializer responsible to call RGWOp::init()--which
   * descendants are required to inherit */
  virtual int op_init() = 0;

  int init(const RGWEnv& rgw_env, RGWObjectCtx* rados_ctx,
	  RGWLibIO* io, struct req_state* _s) {

    RGWRequest::init_state(_s);
    RGWHandler::init(rados_ctx->store, _s, io);

    /* fixup _s->req */
    _s->req = this;

    log_init();

    get_state()->obj_ctx = rados_ctx;
    get_state()->req_id = store->unique_id(id);
    get_state()->trans_id = store->unique_trans_id(id);

    log_format(_s, "initializing for trans_id = %s",
	      get_state()->trans_id.c_str());

    int ret = header_init();
    if (ret == 0) {
      ret = init_from_header(_s);
    }
    return ret;
  }

  virtual bool only_bucket() = 0;

  virtual int read_permissions(RGWOp *op);

}; /* RGWLibRequest */

class RGWLibContinuedReq : public RGWLibRequest {
  RGWLibIO io_ctx;
  struct req_state rstate;
  RGWObjectCtx rados_ctx;
public:

RGWLibContinuedReq(CephContext* _cct, RGWUserInfo* _user)
  :  RGWLibRequest(_cct, _user), rstate(_cct, &io_ctx.get_env(), _user),
     rados_ctx(librgw.get_store(), &rstate)
    {
      io_ctx.init(_cct);

      /* XXX for now, use "";  could be a legit hostname, or, in future,
       * perhaps a tenant (Yehuda) */
      io_ctx.get_env().set("HTTP_HOST", "");
    }

  inline RGWRados* get_store() { return store; }

  virtual int execute() final { abort(); }
  virtual int exec_start() = 0;
  virtual int exec_continue() = 0;
  virtual int exec_finish() = 0;

}; /* RGWLibContinuedReq */

class RGWLibProcess : public RGWProcess {
    RGWAccessKey access_key;
public:
  RGWLibProcess(CephContext* cct, RGWProcessEnv* pe, int num_threads,
		RGWFrontendConfig* _conf) :
    RGWProcess(cct, pe, num_threads, _conf) {}

  void run();
  void checkpoint();

  void enqueue_req(RGWLibRequest* req) {

    lsubdout(g_ceph_context, rgw, 10)
      << __func__ << " enqueue request req=" << hex << req << dec << dendl;

    req_throttle.get(1);
    req_wq.queue(req);
  } /* enqueue_req */

  /* "regular" requests */
  void handle_request(RGWRequest* req); // async handler, deletes req
  int process_request(RGWLibRequest* req);
  int process_request(RGWLibRequest* req, RGWLibIO* io);
  void set_access_key(RGWAccessKey& key) { access_key = key; }

  /* requests w/continue semantics */
  int start_request(RGWLibContinuedReq* req);
  int finish_request(RGWLibContinuedReq* req);
}; /* RGWLibProcess */

class RGWLibFrontend : public RGWProcessFrontend {
public:
  RGWLibFrontend(RGWProcessEnv& pe, RGWFrontendConfig *_conf)
    : RGWProcessFrontend(pe, _conf) {}

  int init();

  inline void enqueue_req(RGWLibRequest* req) {
    static_cast<RGWLibProcess*>(pprocess)->enqueue_req(req); // async
  }

  inline int execute_req(RGWLibRequest* req) {
    return static_cast<RGWLibProcess*>(pprocess)->process_request(req); // !async
  }

  inline int start_req(RGWLibContinuedReq* req) {
    return static_cast<RGWLibProcess*>(pprocess)->start_request(req);
  }

  inline int finish_req(RGWLibContinuedReq* req) {
    return static_cast<RGWLibProcess*>(pprocess)->finish_request(req);
  }

}; /* RGWLibFrontend */

#endif /* RGW_LIB_H */
