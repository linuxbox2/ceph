// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab
/*
 * ceph_bench_neorados_clone_to:  the latency of CEPH_OSD_OP_CLONE_TO
 * through the OSD, against writing the same bytes and against copying
 * them (read, then write_full), which is what a gateway would do without
 * the clone.  A measurement, not a test.  Ops are issued one at a time,
 * so the figures are latencies, not throughput.
 *
 * Options of its own:
 *   --objects N   objects per size (default 64)
 *   --pool NAME   use this existing pool instead of a temporary one
 *   --populate    only write N 4 MiB sources and clone each, leaving them
 *                 (for measuring what recovery does to the sharing)
 * Without --pool it creates a temporary pool and deletes it at exit.
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <boost/system/system_error.hpp>
#include <fmt/format.h>

#include "include/neorados/RADOS.hpp"
#include "include/scope_guard.h"
#include "common/async/blocked_completion.h"
#include "common/async/context_pool.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"

namespace ca = ceph::async;
namespace R = neorados;
using clk = std::chrono::steady_clock;

static double usec(clk::duration d) {
  return std::chrono::duration<double, std::micro>(d).count();
}

int main(int argc, char** argv)
{
  auto args = argv_to_vec(argc, argv);
  env_to_vec(args);
  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                         CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(cct.get());

  unsigned objects = 64;
  std::string pool_arg;
  bool populate = false;
  for (auto i = args.begin(); i != args.end(); ++i) {
    std::string_view a{*i};
    if (a == "--objects" && std::next(i) != args.end()) {
      objects = std::atoi(*++i);
    } else if (a == "--pool" && std::next(i) != args.end()) {
      pool_arg = *++i;
    } else if (a == "--populate") {
      populate = true;
    }
  }

  try {
    ca::io_context_pool p(1);
    auto r = R::RADOS::make_with_cct(cct, p, ca::use_blocked);
    const bool temp_pool = pool_arg.empty();
    const auto pool_name = temp_pool
      ? fmt::format("clone_to_bench.{}", ::getpid()) : pool_arg;
    if (temp_pool) {
      r.create_pool(pool_name, std::nullopt, ca::use_blocked);
    }
    auto pd = make_scope_guard([&] {
      if (temp_pool) {
        r.delete_pool(pool_name, ca::use_blocked);
      }
    });
    R::IOContext ioc(r.lookup_pool(pool_name, ca::use_blocked));

    if (populate) {
      bufferlist data;
      data.append(std::string(uint64_t{4} << 20, 'x'));
      for (unsigned i = 0; i < objects; ++i) {
        const auto s = fmt::format("p{}", i);
        r.execute(s, ioc, R::WriteOp{}.write_full(bufferlist{data}),
                  ca::use_blocked);
        r.execute(s, ioc,
                  R::WriteOp{}.clone_to(fmt::format("{}~{:016x}{:016x}", s, 1,
                                                    uint64_t{0xfeedfacecafebeef})),
                  ca::use_blocked);
      }
      std::cout << fmt::format("populated {} pairs of 4 MiB in {}\n", objects,
                               pool_name);
      return 0;
    }

    std::cout << fmt::format("# objects/size={}  ops one at a time\n", objects);
    std::cout << "# size clone_to_us write_full_us copy_us\n";

    for (uint64_t size : {uint64_t{64} << 10, uint64_t{1} << 20,
                          uint64_t{4} << 20}) {
      bufferlist data;
      data.append(std::string(size, 'x'));
      std::vector<std::string> src;
      for (unsigned i = 0; i < objects; ++i) {
        src.push_back(fmt::format("s{}.{}", size, i));
        r.execute(src.back(), ioc, R::WriteOp{}.write_full(bufferlist{data}),
                  ca::use_blocked);
      }

      /* clone_to into <anchor>~<id> */
      auto t0 = clk::now();
      for (const auto& s : src) {
        r.execute(s, ioc,
                  R::WriteOp{}.clone_to(fmt::format("{}~{:016x}{:016x}", s, 1,
                                                    uint64_t{0xfeedfacecafebeef})),
                  ca::use_blocked);
      }
      const double clone_us = usec(clk::now() - t0) / objects;

      /* write_full of the same size to a new object */
      t0 = clk::now();
      for (unsigned i = 0; i < objects; ++i) {
        r.execute(fmt::format("w{}.{}", size, i), ioc,
                  R::WriteOp{}.write_full(bufferlist{data}), ca::use_blocked);
      }
      const double write_us = usec(clk::now() - t0) / objects;

      /* copy:  read the source, write it to a new object */
      t0 = clk::now();
      for (unsigned i = 0; i < objects; ++i) {
        bufferlist got;
        r.execute(src[i], ioc, R::ReadOp{}.read(0, 0, &got), nullptr,
                  ca::use_blocked);
        r.execute(fmt::format("c{}.{}", size, i), ioc,
                  R::WriteOp{}.write_full(std::move(got)), ca::use_blocked);
      }
      const double copy_us = usec(clk::now() - t0) / objects;

      std::cout << fmt::format("{} {:.1f} {:.1f} {:.1f}\n", size, clone_us,
                               write_us, copy_us);
    }
  } catch (const boost::system::system_error& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
