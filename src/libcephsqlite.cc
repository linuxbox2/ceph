#include <iostream>
#include <sstream>
#include <iomanip>

#include <pthread.h>
#include <sqlite3.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>


#include "include/rados/librados.hpp"
#include "include/radosstriper/libradosstriper.hpp"
#include "common/ceph_mutex.h"

extern "C" {
int _cephsqlite3_Close(sqlite3_file *pFile);
int _cephsqlite3_Read(sqlite3_file *pFile, void *zBuf, int iAmt, sqlite_int64 iOfst);
int _cephsqlite3_Write(sqlite3_file *pFile, const void *zBuf, int iAmt, sqlite_int64 iOfst);
int _cephsqlite3_Truncate(sqlite3_file *pFile, sqlite_int64 size);
int _cephsqlite3_Sync(sqlite3_file *pFile, int flags);
int _cephsqlite3_FileSize(sqlite3_file *pFile, sqlite_int64 *pSize);
int _cephsqlite3_Lock(sqlite3_file *pFile, int eLock);
int _cephsqlite3_Unlock(sqlite3_file *pFile, int eLock);
int _cephsqlite3_CheckReservedLock(sqlite3_file *pFile, int *pResOut);
int _cephsqlite3_FileControl(sqlite3_file *pFile, int op, void *pArg);
int _cephsqlite3_SectorSize(sqlite3_file *pFile);
int _cephsqlite3_DeviceCharacteristics(sqlite3_file *pFile);

int _cephsqlite3_Open(sqlite3_vfs *pVfs, const char *zName, sqlite3_file *pFile, int flags, int *pOutFlags);
int _cephsqlite3_Delete(sqlite3_vfs *pVfs, const char *zPath, int dirSync);
int _cephsqlite3_Access(sqlite3_vfs *pVfs, const char *zPath, int flags, int *pResOut);
int _cephsqlite3_FullPathname(sqlite3_vfs *pVfs, const char *zPath, int nPathOut, char *zPathOut);
void *_cephsqlite3_DlOpen(sqlite3_vfs *pVfs, const char *zPath);
void _cephsqlite3_DlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg);
void (*_cephsqlite3_DlSym(sqlite3_vfs *pVfs, void *pH, const char *z))(void);
void _cephsqlite3_DlClose(sqlite3_vfs *pVfs, void *pHandle);
int _cephsqlite3_Randomness(sqlite3_vfs *pVfs, int nByte, char *zByte);
int _cephsqlite3_Sleep(sqlite3_vfs *pVfs, int nMicro);
int _cephsqlite3_CurrentTime(sqlite3_vfs *pVfs, double *pTime);

sqlite3_vfs *_cephsqlite3__vfs(void);
librados::IoCtx *get_io_ctx(const char *zPath);
libradosstriper::RadosStriper *get_radosstriper(const char *zPath);
std::string get_db_name(const char *zName);
int get_lock_type(const std::string &db_name, const std::string &file_name);
void set_lock_type(const std::string &db_name, const std::string &file_name, int eLock);
std::string get_lock_file_name(const std::string &db_name, int eLock);
int create_lock_files(librados::IoCtx *io_ctx, const std::string &db_name, bool must_create);
int remove_lock_files(librados::IoCtx *io_ctx, const std::string &db_file);
int lock_file_in_rados(librados::IoCtx *io_ctx, const std::string &lock_file_name, const std::string &lock_name, bool exclusive);
}

#if 0
static pthread_key_t  ceph_vfs_context_key;
static pthread_once_t ceph_vfs_context_key_once = PTHREAD_ONCE_INIT;
#endif

struct CephVFSContext {
  librados::IoCtx               *io_ctx = nullptr;
  libradosstriper::RadosStriper *rs     = nullptr;
  // file name to exsint lock type map
  std::map<std::string, int>    lock_info;
};

struct CephFile {
  sqlite3_file  base;
  const char   *name                = nullptr;
  int           file_open_flags     = 0;
};

// map to hold pointers to vfs contexts for various databases
ceph::mutex                             vfs_context_map_mutex("vfs_context_map_mutex");
std::map<std::string, CephVFSContext*>  vfs_context_map;

const std::string lock_name = "cephsqlite3_vfs_lock";
const std::string emptystr = "";

#if 0
static void make_key()
{
  (void) pthread_key_create(&ceph_vfs_context_key, NULL);
}
#endif


/*
** Close a file.
*/
extern "C"
int _cephsqlite3_Close(sqlite3_file *pFile)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;

  CephFile *p = (CephFile*)pFile;

  // if only we are closing the main database
  if (get_db_name(p->name) == p->name) {
    CephVFSContext *cc = nullptr;

    vfs_context_map_mutex.lock();
    cc = vfs_context_map[std::string(p->name)];
    if (cc)
      vfs_context_map.erase(std::string(p->name));
    vfs_context_map_mutex.unlock();

    if (cc) {
      delete cc->rs;
      if (cc->io_ctx) {
        cc->io_ctx->close();
        delete cc->io_ctx;
      }
      delete cc;
    }
    // delete reinterpret_cast<CephVFSContext*>(pthread_getspecific(ceph_vfs_context_key));
    // (void) pthread_setspecific(ceph_vfs_context_key, 0);
  }

  return SQLITE_OK;
}

/*
** Read data from a file.
*/
extern "C"
int _cephsqlite3_Read(sqlite3_file *pFile, void *zBuf, int iAmt, sqlite_int64 iOfst)
{
  CephFile *p = (CephFile*)pFile;
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << "read(" << p->name << ", " << std::dec << iAmt << ", " << std::dec << iOfst << ")" << std::endl;
  libradosstriper::RadosStriper *rs = get_radosstriper(get_db_name(p->name).c_str());

  /* ceph::bufferlist buffer pointers are all char* */
  char *b = static_cast<char*>(zBuf);
  if (rs->read(p->name, b, iAmt, iOfst) < 0) {
    // int e = errno;
    // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::errno:" << e << std::endl;
    return SQLITE_IOERR_READ;
  }

  return SQLITE_OK;
}

/*
** Write data to a crash-file.
*/
extern "C"
int _cephsqlite3_Write(sqlite3_file *pFile, const void *zBuf, int iAmt, sqlite_int64 iOfst)
{
  CephFile *p = (CephFile*)pFile;
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << "write(" << p->name << ", " << std::dec << iAmt << ", " << std::dec << iOfst << ")" << std::endl;
  libradosstriper::RadosStriper *rs = get_radosstriper(get_db_name(p->name).c_str());

  /* ceph::bufferlist buffer pointers are all char* */
  char *b = reinterpret_cast<char*>(const_cast<void*>(zBuf));
  ceph::bufferlist bl = ceph::bufferlist::static_from_mem(b, iAmt);

  if (rs->write(p->name, bl, iAmt, iOfst) < 0)
    return SQLITE_IOERR;

  return SQLITE_OK;
}

/*
** Truncate a file. This is a no-op for this VFS (see header comments at
** the top of the file).
*/
extern "C"
int _cephsqlite3_Truncate(sqlite3_file *pFile, sqlite_int64 size)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  CephFile *p = (CephFile*)pFile;
  libradosstriper::RadosStriper *rs = get_radosstriper(get_db_name(p->name).c_str());

  if (rs->trunc(p->name, size) != 0)
    return SQLITE_IOERR;

  return SQLITE_OK;
}

/*
** Sync the contents of the file to the persistent media.
*/
extern "C"
int _cephsqlite3_Sync(sqlite3_file *pFile, int flags)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  return SQLITE_OK;
}


/*
** Write the size of the file in bytes to *pSize.
*/
extern "C"
int _cephsqlite3_FileSize(sqlite3_file *pFile, sqlite_int64 *pSize)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  CephFile *p = (CephFile*)pFile;
  libradosstriper::RadosStriper *rs = get_radosstriper(get_db_name(p->name).c_str());

  uint64_t  size = 0;
  time_t    mtime = 0;
  int       rc = rs->stat(p->name, &size, &mtime);

  *pSize = 0;

  if (rc == 0)
    *pSize = (sqlite_int64)size;

  return SQLITE_OK;
}

#define CASE(x) case x: return #x

extern "C"
const char *lock_type_str(int eLock)
{
  switch (eLock) {
    CASE(SQLITE_LOCK_NONE);
    CASE(SQLITE_LOCK_SHARED);
    CASE(SQLITE_LOCK_RESERVED);
    CASE(SQLITE_LOCK_PENDING);
    CASE(SQLITE_LOCK_EXCLUSIVE);
  }
  return "UNKNOWN";
}

extern "C"
std::string get_lock_file_name(const std::string &db_name, int eLock)
{
  std::string lock_file_name = db_name;
  lock_file_name += "-";
  lock_file_name += lock_type_str(eLock);
  return lock_file_name;
}

std::string get_cookie()
{
  std::stringstream ss;

  ss << std::hex << std::setfill('0') << std::setw(16) << "0x" << pthread_self();

  return ss.str();
}

/*
** Locking functions. The xLock() and xUnlock() methods are both no-ops.
** The xCheckReservedLock() always indicates that no other process holds
** a reserved lock on the database file. This ensures that if a hot-journal
** file is found in the file-system it is rolled back.
*/
extern "C"
int _cephsqlite3_Lock(sqlite3_file *pFile, int eLock)
{
  CephFile *p = (CephFile *)pFile;

  assert(p != NULL);

  std::string db_name = get_db_name(p->name);
  std::string db_file = p->name;
  /* Make sure the locking sequence is correct.
  **  (1) We never move from unlocked to anything higher than shared lock.
  **  (2) SQLite never explicitly requests a pendig lock.
  **  (3) A shared lock is always held when a reserve lock is requested.
  */
  int curr_lock_type = get_lock_type(db_name, db_file);

  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::eLock " << p->name << " from:" << lock_type_str(curr_lock_type) << " to:" << lock_type_str(eLock) << std::endl;

  assert(curr_lock_type != SQLITE_LOCK_NONE || eLock == SQLITE_LOCK_SHARED);
  assert(eLock != SQLITE_LOCK_PENDING);
  assert(eLock != SQLITE_LOCK_RESERVED || curr_lock_type == SQLITE_LOCK_SHARED);


  if (curr_lock_type == eLock)
      return SQLITE_OK;

  librados::IoCtx *io_ctx = get_io_ctx(db_name.c_str());
  assert(io_ctx != nullptr);

  std::string lock_file_name;

  if (curr_lock_type == SQLITE_LOCK_NONE) {
    if (eLock == SQLITE_LOCK_SHARED) {
      lock_file_name = get_lock_file_name(db_file, SQLITE_LOCK_SHARED);
      if (lock_file_in_rados(io_ctx, lock_file_name, lock_name, false) != 0)
        return SQLITE_BUSY;
      set_lock_type(db_name, db_file, SQLITE_LOCK_SHARED);
      return SQLITE_OK;
    }
    return SQLITE_IOERR_RDLOCK;
  } else if (curr_lock_type == SQLITE_LOCK_SHARED) {
    if (eLock == SQLITE_LOCK_RESERVED)
      lock_file_name = get_lock_file_name(db_file, SQLITE_LOCK_RESERVED);
    else if (eLock == SQLITE_LOCK_PENDING)
      lock_file_name = get_lock_file_name(db_file, SQLITE_LOCK_PENDING);
    else
      return SQLITE_IOERR_LOCK;

    if (lock_file_in_rados(io_ctx, lock_file_name, lock_name, true) != 0)
      return SQLITE_BUSY;

    set_lock_type(db_name, db_file, eLock);
    lock_file_name = get_lock_file_name(db_file, SQLITE_LOCK_SHARED);
    io_ctx->unlock(lock_file_name, lock_name, get_cookie());
    return SQLITE_OK;
  } else if (curr_lock_type == SQLITE_LOCK_RESERVED) {
    if (eLock == SQLITE_LOCK_PENDING)
      lock_file_name = get_lock_file_name(db_file, SQLITE_LOCK_PENDING);
    else if (eLock == SQLITE_LOCK_EXCLUSIVE)
      lock_file_name = get_lock_file_name(db_file, SQLITE_LOCK_EXCLUSIVE);
    else
      return SQLITE_IOERR_LOCK;

    if (lock_file_in_rados(io_ctx, lock_file_name, lock_name, true) != 0)
      return SQLITE_BUSY;

    set_lock_type(db_name, db_file, eLock);
    lock_file_name = get_lock_file_name(db_file, SQLITE_LOCK_RESERVED);
    io_ctx->unlock(lock_file_name, lock_name, get_cookie());
    return SQLITE_OK;
  } else if (curr_lock_type == SQLITE_LOCK_PENDING) {
    if (eLock == SQLITE_LOCK_EXCLUSIVE) {
      lock_file_name = get_lock_file_name(db_file, SQLITE_LOCK_EXCLUSIVE);
      if (lock_file_in_rados(io_ctx, lock_file_name, lock_name, true) != 0)
        return SQLITE_BUSY;
      set_lock_type(db_name, db_file, SQLITE_LOCK_EXCLUSIVE);
      lock_file_name = get_lock_file_name(db_name, SQLITE_LOCK_PENDING);
      io_ctx->unlock(lock_file_name, lock_name, get_cookie());
      return SQLITE_OK;
    }
    return SQLITE_IOERR_LOCK;
  }

  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::returning SQLITE_IOERR_LOCK" << std::endl;
  return SQLITE_IOERR_LOCK;
}

extern "C"
int _cephsqlite3_Unlock(sqlite3_file *pFile, int eLock)
{
  CephFile *p = (CephFile *)pFile;

  assert(p != NULL);


  std::string db_name = get_db_name(p->name);
  std::string db_file = p->name;

  librados::IoCtx *io_ctx = get_io_ctx(db_name.c_str());
  assert(io_ctx != nullptr);

  int curr_lock_type = get_lock_type(db_name, db_file);

  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::eLock " << p->name << " from:" << lock_type_str(curr_lock_type) << " to:" << lock_type_str(eLock) << std::endl;

  if (eLock == curr_lock_type)
    return SQLITE_OK;

  assert(eLock < curr_lock_type);

  std::string lock_file_name = get_lock_file_name(db_file, curr_lock_type);
  if (io_ctx->unlock(lock_file_name, lock_name, get_cookie()) != 0)
    return SQLITE_IOERR_UNLOCK;

  lock_file_name = get_lock_file_name(db_file, eLock);
  if (eLock == SQLITE_LOCK_SHARED) {
    if (lock_file_in_rados(io_ctx, lock_file_name, lock_name, false) != 0)
      return SQLITE_BUSY;
  } else if (eLock > SQLITE_LOCK_SHARED) {
    if (lock_file_in_rados(io_ctx, lock_file_name, lock_name, true) != 0)
      return SQLITE_BUSY;
  }

  set_lock_type(db_name, db_file, eLock);

  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << ":: returning SQLITE_OK" << std::endl;
  return SQLITE_OK;
}

extern "C"
int _cephsqlite3_CheckReservedLock(sqlite3_file *pFile, int *pResOut)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  CephFile *p = (CephFile *)pFile;

  assert(p != NULL);

  std::string db_name = get_db_name(p->name);
  std::string db_file = p->name;
  librados::IoCtx *io_ctx = get_io_ctx(db_name.c_str());
  assert(io_ctx != nullptr);
  int curr_lock_type = get_lock_type(db_name, db_file);

  *pResOut = (curr_lock_type > SQLITE_LOCK_SHARED);
  return SQLITE_OK;
}

/*
** No xFileControl() verbs are implemented by this VFS.
*/
extern "C"
int _cephsqlite3_FileControl(sqlite3_file *pFile, int op, void *pArg)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  return SQLITE_NOTFOUND;
}

/*
** The xSectorSize() and xDeviceCharacteristics() methods. These two
** may return special values allowing SQLite to optimize file-system
** access to some extent. But it is also safe to simply return 0.
*/
extern "C"
int _cephsqlite3_SectorSize(sqlite3_file *pFile)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  return 4096;
}

extern "C"
int _cephsqlite3_DeviceCharacteristics(sqlite3_file *pFile)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  return 0;
}

/*
** Open a file handle.
*/
extern "C"
int _cephsqlite3_Open(
  sqlite3_vfs *pVfs,              /* VFS */
  const char *zName,              /* File to open, or 0 for a temp file */
  sqlite3_file *pFile,            /* Pointer to DemoFile struct to populate */
  int flags,                      /* Input SQLITE_OPEN_XXX flags */
  int *pOutFlags                  /* Output SQLITE_OPEN_XXX flags (or NULL) */
)
{
  static const sqlite3_io_methods _cephsqlite3_io = {
    1,                                     /* iVersion */
    _cephsqlite3_Close,                    /* xClose */
    _cephsqlite3_Read,                     /* xRead */
    _cephsqlite3_Write,                    /* xWrite */
    _cephsqlite3_Truncate,                 /* xTruncate */
    _cephsqlite3_Sync,                     /* xSync */
    _cephsqlite3_FileSize,                 /* xFileSize */
    _cephsqlite3_Lock,                     /* xLock */
    _cephsqlite3_Unlock,                   /* xUnlock */
    _cephsqlite3_CheckReservedLock,        /* xCheckReservedLock */
    _cephsqlite3_FileControl,              /* xFileControl */
    _cephsqlite3_SectorSize,               /* xSectorSize */
    _cephsqlite3_DeviceCharacteristics     /* xDeviceCharacteristics */
  };

  CephFile *p = (CephFile*)pFile; /* Populate this structure */

  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << "zName:" << zName << std::endl;

  if ((zName == 0) || (strncmp(zName, ":memory:", 8) == 0)) {
    /* we are not going to create temporary files */
    return SQLITE_IOERR;
  }


  p->name = zName; /* save the file name */
  p->file_open_flags = flags;
  libradosstriper::RadosStriper* rs = get_radosstriper(get_db_name(zName).c_str());

  // std::cerr << "rs:" << std::hex << rs << std::endl;

  assert(rs != nullptr);

  /* exclusive create
   * radosstriper doesn't have a create()! ... since the data is striped,
   * radosstriper can't decide beforehand which of the stripe files it needs
   * to create
   * so here, we just force create the first stripe by writing to offset zero
   * so that the rest of the interface functions work as expected
   */
  if (flags & SQLITE_OPEN_CREATE) {
    // std::cerr << __FUNCTION__ << "::creating database" << std::endl;
    char dummy[4096] = {0,};
    ceph::bufferlist bl = ceph::bufferlist::static_from_mem(dummy, sizeof(dummy));
    if (rs->write(p->name, bl, sizeof(dummy), 0) != 0) {
      // int e = errno;
      // std::cerr << __FUNCTION__ << "::error: during write():errno(" << e << ")" << std::endl;
      return SQLITE_IOERR_WRITE;
    }
    if (rs->trunc(p->name, 0) < 0) {
      // int e = errno;
      // std::cerr << __FUNCTION__ << "::error: during trunc():errno(" << e << ")" << std::endl;
      return SQLITE_IOERR_TRUNCATE;
    }

    // we also create the sentinel file which would be for locking operations
    librados::IoCtx *io_ctx = get_io_ctx(get_db_name(zName).c_str());
#if 0
    if (io_ctx->create(p->name, true) != 0) {
      return SQLITE_ERROR;
    }
#endif

    if (create_lock_files(io_ctx, std::string(zName), true) != SQLITE_OK)
      return SQLITE_ERROR;
  }

  if (pOutFlags) {
    *pOutFlags = flags;
  }
  p->base.pMethods = &_cephsqlite3_io;
  return SQLITE_OK;
}

/*
** Delete the file identified by argument zPath. If the dirSync parameter
** is non-zero, then ensure the file-system modification to delete the
** file has been synced to disk before returning.
*/
extern "C"
int _cephsqlite3_Delete(sqlite3_vfs *pVfs, const char *zPath, int dirSync)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  libradosstriper::RadosStriper *rs = get_radosstriper(get_db_name(zPath).c_str());
  librados::IoCtx *io_ctx           = get_io_ctx(get_db_name(zPath).c_str());

  int ret = rs->remove(zPath);
  if (ret == 0) {
    // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::deleted " << zPath << std::endl;
    return SQLITE_OK;
  }

  remove_lock_files(io_ctx, std::string(zPath));

  // int e = errno;
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::error deleting " << zPath << " ret:" << ret << ", errno:" << e << std::endl;
  return SQLITE_IOERR_DELETE;
}

/*
** Query the file-system to see if the named file exists, is readable or
** is both readable and writable.
*/
extern "C"
int _cephsqlite3_Access(sqlite3_vfs *pVfs, const char *zPath, int flags, int *pResOut)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << ":zPath:" << zPath << std::endl;

  assert(flags == SQLITE_ACCESS_EXISTS  ||       /* access(zPath, F_OK) */
         flags == SQLITE_ACCESS_READ    ||       /* access(zPath, R_OK) */
         flags == SQLITE_ACCESS_READWRITE        /* access(zPath, R_OK|W_OK) */
  );

  libradosstriper::RadosStriper *rs = get_radosstriper(get_db_name(zPath).c_str());

  uint64_t  size = 0;
  time_t    mtime = 0;
  int       rc = rs->stat(zPath, &size, &mtime);

  *pResOut = (rc == 0);

  return SQLITE_OK;
}

/*
** Argument zPath points to a nul-terminated string containing a file path.
** If zPath is an absolute path, then it is copied as is into the output
** buffer. Otherwise, if it is a relative path, then the equivalent full
** path is written to the output buffer.
**
** This function assumes that paths are UNIX style. Specifically, that:
**
**   1. Path components are separated by a '/'. and
**   2. Full paths begin with a '/' character.
*/
extern "C"
int _cephsqlite3_FullPathname(
  sqlite3_vfs *pVfs,              /* VFS */
  const char *zPath,              /* Input path (possibly a relative path) */
  int nPathOut,                   /* Size of output buffer in bytes */
  char *zPathOut                  /* Pointer to output buffer */
)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << ":zPath:" << zPath << std::endl;

  /* There are no directories to be searched for RADOS obects.
   * They are always at the root.
   * So, just copy the path if starting with '/' or prefix the path with a '/'
   * and copy to output.
   */
  if (strlen(zPath) >= static_cast<unsigned long>(static_cast<long>(nPathOut)))
    return SQLITE_ERROR;

  strncpy(zPathOut, zPath, strlen(zPath));
  zPathOut[strlen(zPath)] = '\0';

  return SQLITE_OK;
}

/*
** The following four VFS methods:
**
**   xDlOpen
**   xDlError
**   xDlSym
**   xDlClose
**
** are supposed to implement the functionality needed by SQLite to load
** extensions compiled as shared objects. This VFS does not support
** this functionality, so the following functions are no-ops.
*/
extern "C"
void *_cephsqlite3_DlOpen(sqlite3_vfs *pVfs, const char *zPath)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  return 0;
}

extern "C"
void _cephsqlite3_DlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  snprintf(zErrMsg, nByte, "Loadable extensions are not supported");
  zErrMsg[nByte-1] = '\0';
}

extern "C"
void (*_cephsqlite3_DlSym(sqlite3_vfs *pVfs, void *pH, const char *z))(void)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  return 0;
}

extern "C"
void _cephsqlite3_DlClose(sqlite3_vfs *pVfs, void *pHandle)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  return;
}

/*
** Parameter zByte points to a buffer nByte bytes in size. Populate this
** buffer with pseudo-random data.
*/
extern "C"
int _cephsqlite3_Randomness(sqlite3_vfs *pVfs, int nByte, char *zByte)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    if (read(fd, zByte, nByte) == nByte) {
      close(fd);
      return SQLITE_OK;
    }
    close(fd);
  }
  return SQLITE_ERROR;
}

/*
** Sleep for at least nMicro microseconds. Return the (approximate) number
** of microseconds slept for.
*/
extern "C"
int _cephsqlite3_Sleep(sqlite3_vfs *pVfs, int nMicro)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  sleep(nMicro / 1000000);
  usleep(nMicro % 1000000);
  return nMicro;
}

/*
** Set *pTime to the current UTC time expressed as a Julian day. Return
** SQLITE_OK if successful, or an error code otherwise.
**
**   http://en.wikipedia.org/wiki/Julian_day
**
** This implementation is not very good. The current time is rounded to
** an integer number of seconds. Also, assuming time_t is a signed 32-bit
** value, it will stop working some time in the year 2038 AD (the so-called
** "year 2038" problem that afflicts systems that store time this way).
*/
extern "C"
int _cephsqlite3_CurrentTime(sqlite3_vfs *pVfs, double *pTime)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  time_t t = time(0);
  *pTime = t/86400.0 + 2440587.5;
  return SQLITE_OK;
}

extern "C"
sqlite3_vfs *_cephsqlite3__vfs(void)
{
  static sqlite3_vfs _cephsqlite3_vfs = {
    1,                               /* iVersion */
    sizeof(CephFile),                /* szOsFile */
    PATH_MAX,                     /* mxPathname */
    0,                               /* pNext */
    "cephsqlite3",                   /* zName */
    0,                               /* pAppData */
    _cephsqlite3_Open,               /* xOpen */
    _cephsqlite3_Delete,             /* xDelete */
    _cephsqlite3_Access,             /* xAccess */
    _cephsqlite3_FullPathname,       /* xFullPathname */
    _cephsqlite3_DlOpen,             /* xDlOpen */
    _cephsqlite3_DlError,            /* xDlError */
    _cephsqlite3_DlSym,              /* xDlSym */
    _cephsqlite3_DlClose,            /* xDlClose */
    _cephsqlite3_Randomness,         /* xRandomness */
    _cephsqlite3_Sleep,              /* xSleep */
    _cephsqlite3_CurrentTime,        /* xCurrentTime */
  };
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  return &_cephsqlite3_vfs;
}

extern "C"
void __attribute__ ((constructor)) _cephsqlite3__vfs_register()
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  sqlite3_vfs_register(_cephsqlite3__vfs(), 1);
}

extern "C"
void __attribute__ ((destructor)) _cephsqlite3__vfs_unregister()
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << std::endl;
  sqlite3_vfs_unregister(_cephsqlite3__vfs());
}

extern "C"
sqlite3 *ceph_sqlite3_open(
  librados::Rados &cluster,
  const char *dbname,           /* eg. "kvstore" instead of "kvstore.db" */
  const char *rados_namespace,
  int ceph_pool_id,
  int stripe_count,
  int obj_size,
  bool must_create
)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::must_create:" << must_create << std::endl;
  int ret = -1;
  /* FIXME
   * how long should the io_ctx and cluster objects exist for radosstriper
   * to be functional ?
   */
  librados::IoCtx *io_ctx = new librados::IoCtx;
  libradosstriper::RadosStriper *rs = new libradosstriper::RadosStriper;

  ret = cluster.ioctx_create2(ceph_pool_id, *io_ctx);
  if (ret < 0) {
    /* unable to create the IO Context */
    return NULL;
  }

  io_ctx->set_namespace(rados_namespace);

  ret = libradosstriper::RadosStriper::striper_create(*io_ctx, rs);
  if (ret < 0) {
    /* unable to create the striper */
    return NULL;
  }

  uint64_t alignment = 0;
  ret = io_ctx->pool_required_alignment2(&alignment);
  if (ret < 0) {
    /* no alignment retrieved */
    return NULL;
  }

  rs->set_object_layout_stripe_unit(alignment);
  rs->set_object_layout_stripe_count(stripe_count);
  rs->set_object_layout_object_size(obj_size);

  sqlite3 *db = NULL;
  const int db_open_flags = SQLITE_OPEN_NOMUTEX       | /* single client access */
                            SQLITE_OPEN_PRIVATECACHE  |
                            SQLITE_OPEN_READWRITE     |
                            SQLITE_OPEN_URI           |
                            (must_create ? SQLITE_OPEN_CREATE : 0);

  std::stringstream ss;

  /* pass the address of the RadosStriper C++ object in the URI so that the VFS
   * methods can access it
   */
  std::string mode = (must_create ? "rwc" : "rw");
  /* create a URI based file name */
  ss << "file:" << dbname << ".db?mode=" << mode << "&cache=private&vfs=cephsqlite3";
  // ss << "&radosstriper=0x"  << std::hex << reinterpret_cast<long>(rs);
  // ss << "&io_ctx=0x"        << std::hex << reinterpret_cast<long>(io_ctx);
  std::string filename = ss.str();

  // (void) pthread_once(&ceph_vfs_context_key_once, make_key);
  CephVFSContext *cc = NULL;
#if 0
  if ((cc = reinterpret_cast<CephVFSContext*>(pthread_getspecific(ceph_vfs_context_key))) == nullptr) {
    cc = new CephVFSContext;
    cc->io_ctx = io_ctx;
    cc->rs     = rs;
    (void) pthread_setspecific(ceph_vfs_context_key, cc);
  }
#else
  vfs_context_map_mutex.lock();
  if (vfs_context_map[std::string(dbname) + ".db"] == nullptr) {
    cc = new CephVFSContext;
    cc->io_ctx = io_ctx;
    cc->rs     = rs;
    vfs_context_map[std::string(dbname) + ".db"] = cc;
  }
  vfs_context_map_mutex.unlock();
#endif

  ret = sqlite3_open_v2(filename.c_str(), &db, db_open_flags, "cephsqlite3");
  if (ret < 0) {
    /* db creation failed */
    return NULL;
  }

  return db;
}

extern "C"
int create_lock_files(librados::IoCtx *io_ctx, const std::string &db_file, bool must_create)
{
  int ret = io_ctx->create(get_lock_file_name(db_file, SQLITE_LOCK_SHARED), must_create);
  if (ret < 0 && ret != -EEXIST) {
    // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::error: while creating:" << get_lock_file_name(db_file, SQLITE_LOCK_SHARED) << std::endl;
    return SQLITE_ERROR;
  }

  ret = io_ctx->create(get_lock_file_name(db_file, SQLITE_LOCK_RESERVED), must_create);
  if (ret < 0 && ret != -EEXIST) {
    // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::error: while creating:" << get_lock_file_name(db_file, SQLITE_LOCK_RESERVED) << std::endl;
    return SQLITE_ERROR;
  }

  ret = io_ctx->create(get_lock_file_name(db_file, SQLITE_LOCK_PENDING), must_create);
  if (ret < 0 && ret != -EEXIST) {
    // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::error: while creating:" << get_lock_file_name(db_file, SQLITE_LOCK_PENDING) << std::endl;
    return SQLITE_ERROR;
  }

  ret = io_ctx->create(get_lock_file_name(db_file, SQLITE_LOCK_EXCLUSIVE), must_create);
  if (ret < 0 && ret != -EEXIST) {
    // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::error: while creating:" << get_lock_file_name(db_file, SQLITE_LOCK_EXCLUSIVE) << std::endl;
    return SQLITE_ERROR;
  }

  return SQLITE_OK;
}

extern "C"
int remove_lock_files(librados::IoCtx *io_ctx, const std::string &db_file)
{
  io_ctx->remove(get_lock_file_name(db_file, SQLITE_LOCK_SHARED));
  io_ctx->remove(get_lock_file_name(db_file, SQLITE_LOCK_RESERVED));
  io_ctx->remove(get_lock_file_name(db_file, SQLITE_LOCK_PENDING));
  io_ctx->remove(get_lock_file_name(db_file, SQLITE_LOCK_EXCLUSIVE));

  return SQLITE_OK;
}

/* input is the full URI file name*/
extern "C"
librados::IoCtx *get_io_ctx(const char *zPath)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << "zPath:" << zPath << std::endl;
  // sqlite3_int64 io_ctx_addr = sqlite3_uri_int64(get_db_name(zPath).c_str(), "io_ctx", 0);
  // return reinterpret_cast<librados::IoCtx*>(io_ctx_addr);
  // return reinterpret_cast<CephVFSContext*>(pthread_getspecific(ceph_vfs_context_key))->io_ctx;

  librados::IoCtx *io_ctx = nullptr;

  vfs_context_map_mutex.lock();
  if (vfs_context_map[std::string(zPath)])
    io_ctx = vfs_context_map[std::string(zPath)]->io_ctx;
  vfs_context_map_mutex.unlock();

  return io_ctx;
}

/* input is the full URI file name*/
extern "C"
libradosstriper::RadosStriper *get_radosstriper(const char *zPath)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << "zPath:" << zPath << std::endl;
  // sqlite3_int64 rs_addr = sqlite3_uri_int64(zPath, "radosstriper", 0);
  // return reinterpret_cast<libradosstriper::RadosStriper*>(rs_addr);
  // return reinterpret_cast<CephVFSContext*>(pthread_getspecific(ceph_vfs_context_key))->rs;

  libradosstriper::RadosStriper *rs = nullptr;

  vfs_context_map_mutex.lock();
  if (vfs_context_map[std::string(zPath)])
    rs = vfs_context_map[std::string(zPath)]->rs;
  vfs_context_map_mutex.unlock();

  return rs;
}

extern "C"
std::string get_db_name(const char *zName)
{
  // std::cerr << std::hex << pthread_self() << ":" << __FUNCTION__ << "::" << "zName:" << zName << std::endl;
  /* eg. "accounts.db-journal"
   * so filename should be extracted as "accounts.db"
   * NOTE main database file name will always have a .db extension
   *      see ceph_sqlite3_open()
   */
  const char *e = strstr(zName, ".db-");
  if (!e)
      return std::string(zName);

  e += 3; // point to the '-' after .db

  return std::string(zName, e - zName);
}

extern "C"
int get_lock_type(const std::string &db_name, const std::string &file_name)
{
  int eLock = SQLITE_LOCK_NONE;

  vfs_context_map_mutex.lock();
  CephVFSContext *cc = vfs_context_map[db_name];
  assert(cc);

  eLock = cc->lock_info[file_name];
  vfs_context_map_mutex.unlock();

  return eLock;
}

extern "C"
void set_lock_type(const std::string &db_name, const std::string &file_name, int eLock)
{
  vfs_context_map_mutex.lock();
  CephVFSContext *cc = vfs_context_map[db_name];
  assert(cc != NULL);

  cc->lock_info[file_name] = eLock;
  vfs_context_map_mutex.unlock();
}

extern "C"
int lock_file_in_rados(librados::IoCtx *io_ctx, const std::string &lock_file_name, const std::string &lock_name, bool exclusive)
{
  int ret = -1;
  int retries = 1;

  while (retries) {
    if (exclusive)
      ret = io_ctx->lock_exclusive(lock_file_name, lock_name, get_cookie(), emptystr, NULL, 0);
    else
      ret = io_ctx->lock_shared(lock_file_name, lock_name, get_cookie(), emptystr, emptystr, NULL, 0);
    
    if (ret == 0)
      return ret;

    if (ret == -EBUSY && --retries)
      usleep(1000000); // sleep for 1 second
  }
  return ret;
}
