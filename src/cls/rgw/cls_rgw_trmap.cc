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

    /* File/IO Ops */
    static
    int io_close(sqlite3_file *_f)
    {
      auto f = reinterpret_cast<TRMapFile*>(_f);
      /* XXX do something with f */
      return SQLITE_OK;
    } /* io_close */

    static
    int io_read(sqlite3_file *_f, void *_buf, int len,
		sqlite_int64 off)
    {
      auto f = reinterpret_cast<TRMapFile*>(_f);

      buffer::list bl;
      bl.push_back(
	buffer::create_static(len, static_cast<char*>(_buf)));
      auto ret = cls_cxx_read(f->hctx, off, len, &bl);
      if ( ret < 0) {
	return SQLITE_IOERR_READ;
      }

      return SQLITE_OK;
    } /* io_read */

    static
    int io_write(sqlite3_file *_f, const void *_buf, int len,
		 sqlite_int64 off)
    {
      auto f = reinterpret_cast<TRMapFile*>(_f);

      buffer::list bl;
      bl.push_back(
	buffer::create_static(len, static_cast<char*>(
				const_cast<void*>(_buf))));

      auto ret = cls_cxx_write2(f->hctx, off, len, &bl,
				CEPH_OSD_OP_FLAG_FADVISE_WILLNEED);
      if (ret < 0)
	return SQLITE_IOERR;

      return SQLITE_OK;
    } /* io_write */

    static
    int io_truncate(sqlite3_file *_f, sqlite_int64 size)
    {
      auto f = reinterpret_cast<TRMapFile*>(_f);

      /* XXX this suspicious size arises from RADOS object size
       * limits */
      auto ret = cls_cxx_write_zero(f->hctx, size,
				    std::numeric_limits<int>::max());
      if (ret < 0) {
	return SQLITE_IOERR;
      }

      return SQLITE_OK;
    } /* io_truncate */

    static
    int io_sync(sqlite3_file *_f, int flags)
    {
      /* in RADOS all i/o is immediately consistent */
      return SQLITE_OK;
    }

    static
    int io_filesize(sqlite3_file *_f, sqlite_int64 *osz)
    {
      uint64_t size{0};
      time_t mtime{0};

      auto f = reinterpret_cast<TRMapFile*>(_f);

      int ret = cls_cxx_stat(f->hctx, &size, &mtime);
      if (ret < 0) {
	return SQLITE_IOERR; // XXX ok?
      }

      *osz = size;

      return SQLITE_OK;
    } /* io_filesize */

    /* if we need these at all, the semantic is basically
     * that of a shared-exclusive lock with an additional pending
     * state that can seemingly be ignored;  for a single-threaded
     * access model, moreover, the embedded example just does: */

    static
    int io_lock(sqlite3_file *file, int type) {
      return SQLITE_OK;
    } /* io_lock */

    static
    int io_unlock(sqlite3_file *file, int type){
      return SQLITE_OK;
    } /* io_unlock */

    static
    int io_checkreservedlock(sqlite3_file *file, int *result){
      *result = 0;
      return SQLITE_OK;
    } /* io_checkreservedlock */

    /* nothing defined */
    static
    int io_fcntl(sqlite3_file *file, int op, void *arg)
    {
      return SQLITE_NOTFOUND;
    } /* io_fcntl */

    static
    int io_sectorsize(sqlite3_file *file)
    {
      return 4096;
    } /* io_sectorsize */

    static
    int io_devicecharacteristics(sqlite3_file *file)
    {
      return 0;
    } /* io_devicecharacteristics */

    static const sqlite3_io_methods trmap_sqlite_io = {
      1,                           /* iVersion */
      io_close,                    /* xClose */
      io_read,                     /* xRead */
      io_write,                    /* xWrite */
      io_truncate,                 /* xTruncate */
      io_sync,                     /* xSync */
      io_filesize,                 /* xFileSize */
      io_lock,                     /* xLock */
      io_unlock,                   /* xUnlock */
      io_checkreservedlock,        /* xCheckReservedLock */
      io_fcntl,                    /* xFileControl */
      io_sectorsize,               /* xSectorSize */
      io_devicecharacteristics,    /* xDeviceCharacteristics */
      nullptr,                     /* xShmMap */
      nullptr,                     /* xShmLock */
      nullptr,                     /* xShmBarrier */
      nullptr                      /* xShmUnmap */
    }; /* trmap_sqlite_io */

    /* VFS Ops */


  } /* extern "C" */


  /* TRMap Singleton */  
  TRMap* TRMap::get_instance(const std::string& path) 
  {
    return nullptr;
  }

} /* namespace rgw::cls::trmap */
