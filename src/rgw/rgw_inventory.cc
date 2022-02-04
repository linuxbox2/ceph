// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#include <tuple>
#include <iostream>
#include <boost/range/irange.hpp>
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
} /* Configuration::decode_xml(...) */

void Configuration::dump_xml(Formatter* f) const
{
  std::string sfmt{};
  encode_xml("Id", id, f);
  if (! filter.prefix.empty()) {
    f->open_object_section("Filter");
    encode_xml("Prefix", filter.prefix, f);
    f->close_section();
  } // optional prefix
  f->open_object_section("Destination");
  f->open_object_section("S3BucketDestination");
  sm::match(destination.format,
	    Format::CSV, [&]() { sfmt = "CSV"; },
	    Format::ORC, [&]() { sfmt = "ORC"; },
	    Format::Parquet, [&]() { sfmt = "Parquet"; });
  f->dump_string("Format", sfmt);
  encode_xml("AccountId", destination.account_id, f);
  encode_xml("Bucket", destination.bucket_arn, f);
  encode_xml("Prefix", destination.prefix, f);
  if (! destination.encryption.kms.key_id.empty()) {
    f->open_object_section("Encryption");
    f->open_object_section("SSE-KMS");
    encode_xml("KeyId", destination.encryption.kms.key_id, f);
    f->close_section();
    f->close_section();
  } // optional encryption
  f->close_section();
  f->close_section(); // required destination
  f->open_object_section("Schedule");
  sm::match(schedule.frequency,
	    Frequency::Daily, [&]() { sfmt = "Daily"; },
	    Frequency::Weekly, [&]() { sfmt = "Weekly"; },
	    smp::_, [&]() {sfmt = "UnknownFrequency"; });
  f->dump_string("Frequency", sfmt);
  f->close_section(); // required schedule
    sm::match(versions,
	      ObjectVersions::Current, [&]() { sfmt = "Current"; },
	      ObjectVersions::All, [&]() {sfmt = "All"; },
	      smp::_, [&]() {sfmt = "_"; });
  f->dump_string("IncludedObjectVersions", sfmt);
  if (optional_fields > 0) {
    f->open_object_section("OptionalFields");
    for (auto ord : boost::irange(1, int(FieldType::Last))) {
      FieldType ft{FieldType(ord)};
      if (optional_fields & shift_field(ft)) {
	f->dump_string("Field", find_field(ft).name);
      }
    }
    f->close_section();
  } // optional fields
} /* Configuration::dump_xml(...) */

void InventoryConfigurations::emplace(std::string&& key, Configuration&& config){
  id_mapping.emplace(std::make_pair(key, config));
}

bool InventoryConfigurations::operator==(const InventoryConfigurations &rhs)
  const
{
  return (id_mapping == rhs.id_mapping);
}

}} /* namespace rgw::inv */
