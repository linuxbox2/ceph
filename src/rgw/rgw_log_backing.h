// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#ifndef CEPH_RGW_LOGBACKING_H
#define CEPH_RGW_LOGBACKING_H

#include <optional>
#include <iostream>
#include <string>
#include <string_view>

#include <strings.h>

#undef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY 1
#include <fmt/format.h>

#include "include/rados/librados.hpp"
#include "include/encoding.h"
#include "include/expected.hpp"
#include "include/function2.hpp"

#include "cls/version/cls_version_types.h"

#include "common/async/yield_context.h"
#include "common/Formatter.h"
#include "common/strtol.h"

namespace bc = boost::container;
namespace bs = boost::system;

#include "cls_fifo_legacy.h"

/// Type of log backing, stored in the mark used in the quick check,
/// and passed to checking functions.
enum class log_type {
  omap = 0,
  fifo = 1
};

inline void encode(const log_type& type, ceph::buffer::list& bl) {
  auto t = static_cast<uint8_t>(type);
  encode(t, bl);
}

inline void decode(log_type& type, bufferlist::const_iterator& bl) {
  uint8_t t;
  decode(t, bl);
  type = static_cast<log_type>(t);
}

inline std::optional<log_type> to_log_type(std::string_view s) {
  if (strncasecmp(s.data(), "omap", s.length()) == 0) {
    return log_type::omap;
  } else if (strncasecmp(s.data(), "fifo", s.length()) == 0) {
    return log_type::fifo;
  } else {
    return std::nullopt;
  }
}
inline std::ostream& operator <<(std::ostream& m, const log_type& t) {
  switch (t) {
  case log_type::omap:
    return m << "log_type::omap";
  case log_type::fifo:
    return m << "log_type::fifo";
  }

  return m << "log_type::UNKNOWN=" << static_cast<uint32_t>(t);
}

/// Look over the shards in a log and determine the type.
tl::expected<log_type, int>
log_backing_type(librados::IoCtx& ioctx,
		 log_type def,
		 int shards, //< Total number of shards
		 /// A function taking a shard number and
		 /// returning an oid.
		 const fu2::unique_function<std::string(int) const>& get_oid,
		 optional_yield y);

/// Remove all log shards and associated parts of fifos.
int log_remove(librados::IoCtx& ioctx,
	       int shards, //< Total number of shards
	       /// A function taking a shard number and returning an oid.
	       const fu2::unique_function<std::string(int) const>& get_oid,
	       bool leave_zero,
	       optional_yield y);

struct logback_generation {
  uint64_t gen_id = 0;
  log_type type;
  std::optional<ceph::real_time> pruned;

  void encode(ceph::buffer::list& bl) const {
    ENCODE_START(1, 1, bl);
    encode(gen_id, bl);
    encode(type, bl);
    encode(pruned, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(gen_id, bl);
    decode(type, bl);
    decode(pruned, bl);
    DECODE_FINISH(bl);
  }
};
WRITE_CLASS_ENCODER(logback_generation)
inline std::ostream& operator <<(std::ostream& m, const logback_generation& g) {
  return m << "[" << g.gen_id << "," << g.type << ","
	   << (g.pruned ? "PRUNED" : "NOT PRUNED") << "]";
}

class logback_generations : public librados::WatchCtx2 {
public:
  using entries_t = bc::flat_map<uint64_t, logback_generation>;

protected:
  librados::IoCtx& ioctx;
  logback_generations(librados::IoCtx& ioctx,
		      std::string oid,
		      fu2::unique_function<std::string(
			uint64_t, int) const>&& get_oid,
		      int shards) noexcept
    : ioctx(ioctx), oid(oid), get_oid(std::move(get_oid)),
      shards(shards) {}

    uint64_t my_id = ioctx.get_instance_id();

private:
  const std::string oid;
  const fu2::unique_function<std::string(uint64_t, int) const> get_oid;

protected:
  const int shards;

private:

  uint64_t watchcookie = 0;

  obj_version version;
  std::mutex m;
  entries_t entries_;

  tl::expected<std::pair<entries_t, obj_version>, int>
  read(optional_yield y) noexcept;
  int write(entries_t&& e, std::unique_lock<std::mutex>&& l_,
	    optional_yield y) noexcept;
  int setup(log_type def, optional_yield y) noexcept;

  int watch() noexcept;

  auto lowest_nomempty(const entries_t& es) {
    return std::find_if(es.begin(), es.end(),
			[](const auto& e) {
			  return !e.second.pruned;
			});
  }

public:

  /// For the use of watch/notify.

  void handle_notify(uint64_t notify_id,
		     uint64_t cookie,
		     uint64_t notifier_id,
		     bufferlist& bl) override final;

  void handle_error(uint64_t cookie, int err) override final;

  /// Public interface

  virtual ~logback_generations();

  template<typename T, typename... Args>
  static tl::expected<std::unique_ptr<T>, int>
  init(librados::IoCtx& ioctx_, std::string oid_,
       fu2::unique_function<std::string(uint64_t, int) const>&& get_oid_,
       int shards_, log_type def, optional_yield y,
       Args&& ...args) noexcept {
    try {
      T* lgp = new T(ioctx_, std::move(oid_),
		     std::move(get_oid_),
		     shards_, std::forward<Args>(args)...);
      std::unique_ptr<T> lg(lgp);
      lgp = nullptr;
      auto ec = lg->setup(def, y);
      if (ec)
	return tl::unexpected(ec);
      // Obnoxiousness for C++ Compiler in Bionic Beaver
      return tl::expected<std::unique_ptr<T>, int>(std::move(lg));
    } catch (const std::bad_alloc&) {
      return tl::unexpected(-ENOMEM);
    }
  }

  int update(optional_yield y) noexcept;

  entries_t entries() const {
    return entries_;
  }

  int new_backing(log_type type, optional_yield y) noexcept;

  int empty_to(uint64_t gen_id, optional_yield y) noexcept;

  int remove_empty(optional_yield y) noexcept;

  // Callbacks, to be defined by descendant.

  /// Handle initialization on startup
  ///
  /// @param e All non-empty generations
  virtual int handle_init(entries_t e) noexcept = 0;

  /// Handle new generations.
  ///
  /// @param e Map of generations added since last update
  virtual int handle_new_gens(entries_t e) noexcept = 0;

  /// Handle generations being marked empty
  ///
  /// @param new_tail Lowest non-empty generation
  virtual int handle_empty_to(uint64_t new_tail) noexcept = 0;
};

inline std::string gencursor(uint64_t gen_id, std::string_view cursor) {
  return (gen_id > 0 ?
	  fmt::format("G{:0>20}@{}", gen_id, cursor) :
	  std::string(cursor));
}

inline std::pair<uint64_t, std::string_view>
cursorgen(std::string_view cursor_) {
  std::string_view cursor = cursor_;
  if (cursor[0] != 'G') {
    return { 0, cursor };
  }
  cursor.remove_prefix(1);
  auto gen_id = ceph::consume<uint64_t>(cursor);
  if (!gen_id || cursor[0] != '@') {
    return { 0, cursor_ };
  }
  cursor.remove_prefix(1);
  return { *gen_id, cursor };
}

inline std::pair<uint64_t, std::string_view>
cursorgeno(std::optional<std::string_view> cursor) {
  if (cursor && !cursor->empty()) {
    return cursorgen(*cursor);
  } else {
    return { 0, ""s };
  }
}

class LazyFIFO {
  librados::IoCtx& ioctx;
  std::string oid;
  std::mutex m;
  std::unique_ptr<rgw::cls::fifo::FIFO> fifo;

  int lazy_init(optional_yield y) {
    std::unique_lock l(m);
    if (fifo) return 0;
    auto r = rgw::cls::fifo::FIFO::create(ioctx, oid, &fifo, y);
    if (r) {
      fifo.reset();
    }
    return r;
  }

public:

  LazyFIFO(librados::IoCtx& ioctx, std::string oid)
    : ioctx(ioctx), oid(std::move(oid)) {}

  int read_meta(optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    return fifo->read_meta(y);
  }

  int meta(rados::cls::fifo::info& info, optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    info = fifo->meta();
    return 0;
  }

  int get_part_layout_info(std::uint32_t& part_header_size,
			   std::uint32_t& part_entry_overhead,
			   optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    std::tie(part_header_size, part_entry_overhead)
      = fifo->get_part_layout_info();
    return 0;
  }

  int push(const ceph::buffer::list& bl,
	   optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    return fifo->push(bl, y);
  }

  int push(ceph::buffer::list& bl,
	   librados::AioCompletion* c,
	   optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    fifo->push(bl, c);
    return 0;
  }

  int push(const std::vector<ceph::buffer::list>& data_bufs,
	   optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    return fifo->push(data_bufs, y);
  }

  int push(const std::vector<ceph::buffer::list>& data_bufs,
	    librados::AioCompletion* c,
	    optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    fifo->push(data_bufs, c);
    return 0;
  }

  int list(int max_entries, std::optional<std::string_view> markstr,
	   std::vector<rgw::cls::fifo::list_entry>* out,
	   bool* more, optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    return fifo->list(max_entries, markstr, out, more, y);
  }

  int list(int max_entries, std::optional<std::string_view> markstr,
	   std::vector<rgw::cls::fifo::list_entry>* out, bool* more,
	   librados::AioCompletion* c, optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    fifo->list(max_entries, markstr, out, more, c);
    return 0;
  }

  int trim(std::string_view markstr, bool exclusive, optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    return fifo->trim(markstr, exclusive, y);
  }

  int trim(std::string_view markstr, bool exclusive, librados::AioCompletion* c,
	   optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    fifo->trim(markstr, exclusive, c);
    return 0;
  }

  int get_part_info(int64_t part_num, rados::cls::fifo::part_header* header,
		    optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    return fifo->get_part_info(part_num, header, y);
  }

  int get_part_info(int64_t part_num, rados::cls::fifo::part_header* header,
		    librados::AioCompletion* c, optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    fifo->get_part_info(part_num, header, c);
    return 0;
  }

  int get_head_info(fu2::unique_function<
		      void(int r, rados::cls::fifo::part_header&&)>&& f,
		    librados::AioCompletion* c,
		    optional_yield y) {
    auto r = lazy_init(y);
    if (r < 0) return r;
    fifo->get_head_info(std::move(f), c);
    return 0;
  }
};

#endif
