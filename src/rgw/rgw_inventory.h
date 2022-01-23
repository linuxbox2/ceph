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
    CSV = 0,
    ORC,
    Parquet,
  };

  enum class Schedule : uint8_t
  {
    Daily = 0,
    Weekly,
  };

  enum class ObjectVersions : uint8_t
  {
    All = 0,
    Current,
  };

  enum class Field : uint8_t
  {
    Size = 0,
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

  class Configuration
  {
  public:
    std::string id; // unique identifier

    class Filter
    {
    public:
      std::string filter_prefix; // the only defined filter, as yet
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
	std::string key_id; // for SSE-KMS; SSE-S3 exists but is undefined
      };
    };

    Schedule schedule;
    ObjectVersions versions;
    uint32_t optional_fields; // bitmap

    void dump_xml(Formatter *f) const;
    void decode_xml(XMLObj *obj);
  };

  class InventoryConfigurations
  {
  public:
    std::map<std::string, Configuration> id_mapping;

    void dump_xml(Formatter *f) const;
    void decode_xml(XMLObj *obj);
  };

}} /* namespace rgw::inv */
