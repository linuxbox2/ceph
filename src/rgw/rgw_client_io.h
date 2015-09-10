// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_CLIENT_IO_H
#define CEPH_RGW_CLIENT_IO_H

#include <stdlib.h>

#include "include/types.h"

#include "rgw_common.h"

class RGWClientIO {
  bool _account;

protected:
  RGWEnv env;

  virtual void init_env(CephContext *cct) = 0;

public:
  virtual ~RGWClientIO() {}
  RGWClientIO() : _account(false) {}

  void init(CephContext *cct);
  RGWEnv& get_env() { return env; }

  bool account() { return _account; }
  void set_account(bool _accnt) {
    _account = _accnt;
  }

  virtual int complete_request() = 0; /* XXX signature likely changing */

  virtual uint64_t get_bytes_sent() { return 0; }
  virtual uint64_t get_bytes_received() { return 0; }
}; /* RGWClient IO */

/* HTTP IO */
class RGWStreamIO : public RGWClientIO {

  size_t bytes_sent;
  size_t bytes_received;

protected:
  virtual int write_data(const char *buf, int len) = 0;
  virtual int read_data(char *buf, int max) = 0;

public:
  virtual ~RGWStreamIO() {}
  RGWStreamIO() : bytes_sent(0), bytes_received(0) {}

  int print(const char *format, ...);
  int write(const char *buf, int len);
  virtual void flush() = 0;
  int read(char *buf, int max, int *actual);

  virtual int send_status(const char *status, const char *status_name) = 0;
  virtual int send_100_continue() = 0;
  virtual int complete_header() = 0;
  virtual int send_content_length(uint64_t len) = 0;

  uint64_t get_bytes_sent() { return bytes_sent; }
  uint64_t get_bytes_received() { return bytes_received; }
}; /* RGWStreamIO */

#endif
