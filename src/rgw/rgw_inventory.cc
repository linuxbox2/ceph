// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#include <tuple>
#include <iostream>
#include "rgw_inventory.h"
#include "simple_match.hpp"

namespace rgw { namespace inv {

namespace sm = simple_match;
namespace smp = simple_match::placeholders;
namespace xd = RGWXMLDecoder;


bool Configuration::operator==(const Configuration &rhs) const
{
  return
    std::tie(id, filter.prefix, destination.format, destination.account_id,
	     destination.bucket_arn, destination.prefix, destination.encryption.kms.key_id,
	     schedule.frequency, versions, optional_fields) ==
    std::tie(rhs.id, rhs.filter.prefix, rhs.destination.format, rhs.destination.account_id,
	     rhs.destination.bucket_arn, rhs.destination.prefix, rhs.destination.encryption.kms.key_id,
	     rhs.schedule.frequency, rhs.versions, rhs.optional_fields);
} /* operator== */

bool Configuration::operator<(const Configuration &rhs) const
{
  return
    std::tie(id, filter.prefix, destination.format, destination.account_id,
	     destination.bucket_arn, destination.prefix, destination.encryption.kms.key_id,
	     schedule.frequency, versions, optional_fields) <
    std::tie(rhs.id, rhs.filter.prefix, rhs.destination.format, rhs.destination.account_id,
	     rhs.destination.bucket_arn, rhs.destination.prefix, rhs.destination.encryption.kms.key_id,
	     rhs.schedule.frequency, rhs.versions, rhs.optional_fields);
} /* operator< */

void Configuration::decode_xml(XMLObj* obj)
{
  xd::decode_xml("Id", id, obj);
  // optional Filter
  if (auto o = obj->find_first("Filter"); o) {
    xd::decode_xml("Prefix", filter.prefix, o, true /* required */);
  }
  // required Destination
  if (auto o = obj->find_first("Destination"); o) {
    if (auto o2 = o->find_first("S3BucketDestination"); o2) {
      std::string sfmt;
      xd::decode_xml("Format", sfmt, o2, true);
      // take that! (Rust and SML/NJ)
      sm::match(sfmt,
		"CSV", [this]() { destination.format = Format::CSV; },
		"ORC", [this]() { destination.format = Format::ORC; },
		"Parquet", [this]() { destination.format = Format::Parquet; },
		smp::_, [this]() { destination.format = Format::None; }
	);
      xd::decode_xml("AccountId", destination.account_id, o2);
      xd::decode_xml("Bucket", destination.bucket_arn, o2);
      if (o2->find_first("Prefix")) {
	xd::decode_xml("Prefix", destination.prefix, o2);
      }
      if (auto o3 = o2->find_first("Encryption"); o3) {
	/* Per AWS doc, an SSES3 configuration object exists but its
	 * structure isn't documented:
	 * https://docs.aws.amazon.com/AmazonS3/latest/API/API_SSES3.html,
	 * so I think all we can do is look for SSE-KMS */
	if (auto o4 = o3->find_first("SSE-KMS"); o4) {
	  xd::decode_xml("KeyId", destination.encryption.kms.key_id, o4);
	}
      } // Encryption
      // required Schedule
      if (auto o = obj->find_first("Schedule"); o) {
	  std::string sfreq;
	  xd::decode_xml("Frequency", sfreq, o, true);
	  sm::match(sfreq,
		    "Daily", [this]() { schedule.frequency = Frequency::Daily; },
		    "Weekly", [this]() { schedule.frequency = Frequency::Weekly; },
		    smp::_, [this]() { schedule.frequency = Frequency::None; }
	    );
      } // Schedule
      // treat IncludedObjectVersions as optional, defaults to Current
      {
	std::string sver;
	xd::decode_xml("IncludedObjectVersions", sver, obj, false);
	sm::match(sver, "All", [this]() { versions = ObjectVersions::All; },
		  smp::_, [this]() { versions = ObjectVersions::Current; }
	  );
      }
      if (auto o = obj->find_first("OptionalFields"); o) {
	auto fields_iter = o->find("Field");
	while (auto field_xml = fields_iter.get_next()) {
	  const auto& sfield = field_xml->get_data();
	  if (auto& field = find_field(sfield); field.ord != FieldType::None) {
	    optional_fields |= shift_field(field.ord);
	  }
	} // each field
      } // OptionalFields
    } // S3BucketDestination
  } // Destination
}

}} /* namespace rgw::inv */
