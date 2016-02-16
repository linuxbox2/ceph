// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef RGW_B64_H
#define RGW_B64_H

#include <boost/utility/string_ref.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/insert_linebreaks.hpp>
#include <boost/archive/iterators/transform_width.hpp>

namespace rgw {

  std::string to_base64(boost::string_ref sref)
  {
    using namespace boost::archive::iterators;
    std::string ostr;
    
    // output must be =padded modulo 3
    auto psize = sref.size();
    while ((psize % 3) != 0) {
      ++psize;
    }

    /* use public-domain formula for making a transforming iterator
     * from boost serialization primitives.
     *
     * RFC 2045 requires linebreaks to be present in the output
     * sequence every at-most 76 characters. */
    typedef
      insert_linebreaks<
        base64_from_binary<
          transform_width<
            const unsigned char *
            ,6
            ,8
            >
          >
          ,76
        > b64_iter;

    std::string outstr(b64_iter(sref.data()),
		       b64_iter(sref.data() + sref.size()));

    // pad ostr with '=' to a length that is a multiple of 3
    for (size_t ix = 0; ix < (psize-sref.size()); ++ix)
      outstr.push_back('=');

    return std::move(outstr);
  }

  std::string from_base64(boost::string_ref sref)
  {
    using namespace boost::archive::iterators;

    typedef
      transform_width<
	binary_from_base64<const unsigned char *>
      ,8
      ,6
      > b64_iter;

    while (sref.back() == '=')
      sref.remove_suffix(1);

    std::string outstr(b64_iter(sref.data()),
		      b64_iter(sref.data() + sref.size()));

    return std::move(outstr);
  }

} /* namespace */

#endif /* RGW_B64_H */
