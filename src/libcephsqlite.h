#ifndef LIBCEPHSQLITE_H
#define LIBCEPHSQLITE_H

#include "rados/librados.hpp"

struct sqlite3;

extern "C"
sqlite3 *ceph_sqlite3_open(
  librados::Rados &cluster,
  const char *dbname,           /* eg. "kvstore" instead of "kvstore.db" */
  const char *rados_namespace,
  int ceph_pool_id,
  int stripe_count,
  int obj_size,
  bool must_create
);

#endif
