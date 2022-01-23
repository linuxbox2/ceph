// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_xml.h"
#include "rgw_inventory.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <stdexcept>

static const char* inv_xml_1 =
R"(<?xml version="1.0" encoding="UTF-8"?>
<InventoryConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
   <Id>report1</Id>
   <IsEnabled>true</IsEnabled>
   <Filter>
      <Prefix>filterPrefix</Prefix>
   </Filter>
   <Destination>
      <S3BucketDestination>
         <Format>CSV</Format>
         <AccountId>123456789012</AccountId>
         <Bucket>arn:aws:s3:::destination-bucket</Bucket>
         <Prefix>prefix1</Prefix>
         <Encryption>
            <SSE-KMS>
               <KeyId>arn:aws:kms:us-west-2:111122223333:key/1234abcd-12ab-34cd-56ef-1234567890ab</KeyId>
            </SSE-KMS>
         </Encryption>
      </S3BucketDestination>
   </Destination>
   <Schedule>
      <Frequency>Daily</Frequency>
   </Schedule>
   <IncludedObjectVersions>All</IncludedObjectVersions>
   <OptionalFields>
      <Field>Size</Field>
      <Field>LastModifiedDate</Field>
      <Field>ETag</Field>
      <Field>StorageClass</Field>
      <Field>IsMultipartUploaded</Field>
      <Field>ReplicationStatus</Field>
      <Field>EncryptionStatus</Field>
      <Field>ObjectLockRetainUntilDate</Field>
      <Field>ObjectLockMode</Field>
      <Field>ObjectLockLegalHoldStatus</Field>
   </OptionalFields>
</InventoryConfiguration>
)";

TEST(TestLCFilterDecoder, InvXML1)
{
  RGWXMLDecoder::XMLParser parser;
  ASSERT_TRUE(parser.init());
  ASSERT_TRUE(parser.parse(inv_xml_1, strlen(inv_xml_1), 1));
#if 0
  LCFilter_S3 filter;
  auto result = RGWXMLDecoder::decode_xml("Filter", filter, &parser, true);
  ASSERT_TRUE(result);
  /* check repeated Tag element */
  auto tag_map = filter.get_tags().get_tags();
  auto val1 = tag_map.find("key1");
  ASSERT_EQ(val1->second, "value1");
  auto val2 = tag_map.find("key2");
  ASSERT_EQ(val2->second, "value2");
  /* check our flags */
  ASSERT_EQ(filter.get_flags(), 0);
#endif
}

