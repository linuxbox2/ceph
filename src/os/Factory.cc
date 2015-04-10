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

#include <errno.h>
#include <dlfcn.h>

#include <string>
#include <map>

#include "common/Mutex.h"
#include "Factory.h"
#include "common/debug.h"
#include <boost/filesystem.hpp>

#define dout_subsys ceph_subsys_filestore

using namespace std;
namespace bf = boost::filesystem;

static Mutex mtx("os-factory mutex");
static map<string, objectstore_factory_method> modules;

ObjectStore* ObjectStore::create(CephContext* cct,
				 const string& name,
				 const string& data,
				 const string& journal,
				 osflagbits_t flag)
  {
    Mutex::Locker lock(mtx);

    ObjectStore* os = NULL;
    void* module = NULL;
    objectstore_factory_method factory = NULL;
    map<string, objectstore_factory_method>::iterator iter;

    // cached?
    iter = modules.find(name);
    if (iter != modules.end())
      return iter->second(cct, data, journal, flag);

    // ok, try the file system
    bf::path p(name);
    if (bf::exists(p) && bf::is_regular_file(p)) {
      module = ::dlopen(p.c_str(), RTLD_NOW);
    }
    if (! module) {
      // try relative
      string bname = "libos_" + name + ".so";
      p = cct->_conf->osd_module_dir;
      p /= bname;
      if (bf::exists(p)) {
	module = ::dlopen(p.c_str(), RTLD_NOW);
	if (! module) {
#if 0
	  dout(0) << __func__  << " failed to load ObjectStore module "
		  << bname << " (" << p << ")" << dendl;
#endif
	  return NULL;
	}
#if 0
	dout(11) << "load ObjectStore module " << bname << " (" << p << ")"
		 << dendl;
#endif
      }
    }

    objectstore_dllinit_func dllinit =
      reinterpret_cast<objectstore_dllinit_func>(
	dlsym(module, OBJECTSTORE_INIT_FUNC));

    if (! dllinit) {
#if 0
      dout(0) << __func__ << " " << OBJECTSTORE_INIT_FUNC << " failed "
	      << name << " (" << p << ")" << dendl;
#endif
      goto out;
    }

    factory = reinterpret_cast<objectstore_factory_method>(dllinit());
    if (!! factory) {
      modules[name] = factory;
      os = factory(cct, data, journal, flag);
    } else {
#if 0
      dout(0) << __func__  << " ObjectStore factory failed "
	      << " (" << p << ")" << dendl;
#endif
    }

  out:
    return os;
  } /* ObjectStore::create */
