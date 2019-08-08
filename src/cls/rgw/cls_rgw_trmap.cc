// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/types.h"

#include <sqlite3.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>

#include "objclass/objclass.h"
#include "cls_rgw_trmap.h"


namespace rgw::cls::trmap {

  /* SQLite3 VFS */
  class TRMapFile;
  class VFSCtx;

  class TRMapFile
  {
  public:
    sqlite3_file base;
    cls_method_context_t hctx;
    std::string name;
    /* XXX open flags? */

  }; /* TRMapFile */


  class VFSCtx
  {
  public:
    
  }; /* VFSCtx */

  static VFSCtx vCtx;

  extern "C" {

    /* VFS Ops */
    static
    int trmap_sqlite_close(sqlite3_file *_f)
    {
      auto f = reinterpret_cast<TRMapFile*>(_f);
      /* XXX do something with f */
      return SQLITE_OK;
    } /* trmap_sqlite_close */

    static
    int trmap_sqlite_read(sqlite3_file *_f, void *_buf, int len,
			  sqlite_int64 off)
    {
      auto f = reinterpret_cast<TRMapFile*>(_f);

      buffer::list bl;
      bl.push_back(
	buffer::create_static(len, static_cast<char*>(_buf)));
      int ret = cls_cxx_read(f->hctx, off, len, &bl);
      if ( ret < 0) {
	return SQLITE_IOERR_READ;
      }

      return SQLITE_OK;
    } /* trmap_sqlite_read */

    static
    int trmap_sqlite_write(sqlite3_file *_f, const void *_buf, int len,
			   sqlite_int64 off)
    {
      auto f = reinterpret_cast<TRMapFile*>(_f);

      buffer::list bl;
      bl.push_back(
	buffer::create_static(len, static_cast<char*>(
				const_cast<void*>(_buf))));

      int ret = cls_cxx_write2(f->hctx, off, len, &bl,
			       CEPH_OSD_OP_FLAG_FADVISE_WILLNEED);
      if (ret < 0)
	return SQLITE_IOERR;

      return SQLITE_OK;
    } /* trmap_sqlite_write */

    
  } /* extern "C" */


  /* TRMap Singleton */  
  TRMap* get_instance(const std::string& path) 
  {
    return nullptr;
  }

} /* namespace rgw::cls::trmap */
