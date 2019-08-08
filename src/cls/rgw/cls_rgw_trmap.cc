// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <sqlite3.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>

#include "cls_rgw_trmap.h"



namespace rgw::cls::trmap {

  /* SQLite3 VFS */
  class TRMapFile;
  class VFSCtx;

  class TRMapFile
  {
  public:
    sqlite3_file base;
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

  } /* extern "C" */


  /* TRMap Singleton */  
  TRMap* get_instance(const std::string& path) 
  {
    return nullptr;
  }

} /* namespace rgw::cls::trmap */
