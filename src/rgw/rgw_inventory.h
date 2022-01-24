// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#pragma once

#include <map>
#include <string>
#include <iostream>
#include <include/types.h>

#include "include/str_list.h"
#include "rgw_lc.h"
#include "rgw_xml.h"


namespace rgw { namespace inv {

  enum class Format : uint8_t
  {
    None = 0,
    CSV,
    ORC,
    Parquet,
  };

  enum class Frequency : uint8_t
  {
    None = 0,
    Daily,
    Weekly,
  };

  enum class ObjectVersions : uint8_t
  {
    None = 0,
    All,
    Current,
  };

  enum class FieldType : uint8_t
  {
    None = 0,
    Size,
    LastModifiedDate,
    StorageClass,
    ETag,
    IsMultipartUploaded,
    ReplicationStatus,
    EncryptionStatus,
    ObjectLockRetainUntilDate,
    ObjectLockMode,
    ObjectLockLegalHoldStatus,
    IntelligentTieringAccessTier,
    BucketKeyStatus,
  };

  class Field {
  public:
    FieldType ord;
    const char* name;

    constexpr Field(FieldType ord, const char* name) : ord(ord), name(name)
      {}
  };

  static constexpr std::array<Field, 13> field_table =
  {
    Field(FieldType::None, "None"),
    Field(FieldType::Size, "Size"),
    Field(FieldType::LastModifiedDate, "LastModifiedDate"),
    Field(FieldType::StorageClass, "StorageClass"),
    Field(FieldType::ETag, "ETag"),
    Field(FieldType::IsMultipartUploaded, "IsMultipartUploaded"),
    Field(FieldType::ReplicationStatus, "ReplicationStatus"),
    Field(FieldType::EncryptionStatus, "EncryptionStatus"),
    Field(FieldType::ObjectLockRetainUntilDate, "ObjectLockRetainUntilDate"),
    Field(FieldType::ObjectLockMode, "ObjectLockMode"),
    Field(FieldType::ObjectLockLegalHoldStatus, "ObjectLockLegalHoldStatus"),
    Field(FieldType::IntelligentTieringAccessTier,
	  "IntelligentTieringAccessTier"),
    Field(FieldType::BucketKeyStatus, "BucketKeyStatus"),
  };

  static constexpr uint32_t shift_field(FieldType type) {
    switch (type) {
    case FieldType::None:
      return 0;
      break;
    default:
      return 1 << (uint32_t(type) - 1);
    }
   }

  static const Field& find_field(const std::string& fs) {
    for (const auto& field : field_table) {
      if (fs == field.name) {
	return field_table[uint8_t(field.ord)];
      }
    }
    // ok, so the None field
    return field_table[0];
  }

  class Configuration
  {
  public:
    std::string id; // unique identifier

    class Filter
    {
    public:
      std::string prefix; // the only defined filter, as yet
    } filter;

    class Destination
    {
    public:
      Format format;
      std::string account_id;
      std::string bucket_arn;
      std::string prefix;

      class Encryption
      {
      public:
	class KMS
	{
	public:
	  std::string key_id; // for SSE-KMS; SSE-S3 exists but is
			      // undefined
	} kms;
      } encryption;
    } destination;

    class Schedule
    {
    public:
      Frequency frequency;
    } schedule;

    ObjectVersions versions;
    uint32_t optional_fields; // bitmap

    Configuration() :
      optional_fields(uint32_t(FieldType::None))
      {}

    void dump_xml(Formatter *f) const;
    void decode_xml(XMLObj *obj);
  }; /* Configuration */

  class InventoryConfigurations
  {
  public:
    std::map<std::string, Configuration> id_mapping;

    void dump_xml(Formatter *f) const;
    void decode_xml(XMLObj *obj);
  };

}} /* namespace rgw::inv */
