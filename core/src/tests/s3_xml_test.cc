/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026-2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

/* ListObjectsV2 and S3 error body readers, on bodies in the AWS S3 API
 * reference formats. */

#include "gtest/gtest.h"

#include "stored/backends/s3_xml.h"

#include <random>
#include <string>

using namespace storagedaemon::s3;

namespace {

const char kXmlDecl[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";

const char kListing[]
    = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
      "  <Name>bucket-a</Name>\n"
      "  <Prefix>Vol-1/</Prefix>\n"
      "  <KeyCount>3</KeyCount>\n"
      "  <MaxKeys>1000</MaxKeys>\n"
      "  <IsTruncated>false</IsTruncated>\n"
      "  <Contents>\n"
      "    <Key>Vol-1/0000</Key>\n"
      "    <LastModified>2026-01-06T08:15:30.000Z</LastModified>\n"
      "    <ETag>&quot;d41d8cd98f00b204e9800998ecf8427e&quot;</ETag>\n"
      "    <Size>10485760</Size>\n"
      "    <Owner><ID>u1</ID><DisplayName>user</DisplayName></Owner>\n"
      "    <StorageClass>STANDARD</StorageClass>\n"
      "  </Contents>\n"
      "  <Contents>\n"
      "    <Key>Vol-1/0001</Key>\n"
      "    <Size>10485760</Size>\n"
      "  </Contents>\n"
      "  <Contents>\n"
      "    <Key>Vol-1/0002</Key>\n"
      "    <Size>123</Size>\n"
      "  </Contents>\n"
      "</ListBucketResult>\n";

}  // namespace

TEST(s3_xml_list, CompleteListing)
{
  auto r = ParseListObjectsV2(kListing);
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_FALSE(r->is_truncated);
  EXPECT_TRUE(r->next_continuation_token.empty());
  ASSERT_EQ(r->objects.size(), 3u);
  EXPECT_EQ(r->objects[0].key, "Vol-1/0000");
  EXPECT_EQ(r->objects[0].size, 10485760u);
  EXPECT_EQ(r->objects[1].key, "Vol-1/0001");
  EXPECT_EQ(r->objects[2].key, "Vol-1/0002");
  EXPECT_EQ(r->objects[2].size, 123u);
}

TEST(s3_xml_list, TruncatedListingWithToken)
{
  std::string xml = std::string(kXmlDecl)
      + "<ListBucketResult><IsTruncated>true</IsTruncated>"
        "<NextContinuationToken>1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM="
        "</NextContinuationToken>"
        "<Contents><Key>a</Key><Size>1</Size></Contents></ListBucketResult>";
  auto r = ParseListObjectsV2(xml);
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_TRUE(r->is_truncated);
  EXPECT_EQ(r->next_continuation_token,
            "1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=");
  EXPECT_EQ(r->objects.size(), 1u);
}

TEST(s3_xml_list, TruncatedWithoutTokenIsAnError)
{
  EXPECT_FALSE(
      ParseListObjectsV2("<ListBucketResult><IsTruncated>true</IsTruncated>"
                         "<Contents><Key>a</Key><Size>1</Size></Contents>"
                         "</ListBucketResult>")
          .has_value());
}

TEST(s3_xml_list, EmptyListing)
{
  auto r = ParseListObjectsV2(
      "<ListBucketResult><Name>b</Name><KeyCount>0</KeyCount>"
      "<IsTruncated>false</IsTruncated></ListBucketResult>");
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_TRUE(r->objects.empty());
  EXPECT_FALSE(r->is_truncated);
}

TEST(s3_xml_list, SelfClosingRootIsMissingIsTruncated)
{ EXPECT_FALSE(ParseListObjectsV2("<ListBucketResult/>").has_value()); }

TEST(s3_xml_list, MissingIsTruncatedIsAnError)
{
  EXPECT_FALSE(ParseListObjectsV2(
                   "<ListBucketResult><Contents><Key>a</Key><Size>1</Size>"
                   "</Contents></ListBucketResult>")
                   .has_value());
}

TEST(s3_xml_list, BadIsTruncatedValue)
{
  EXPECT_FALSE(
      ParseListObjectsV2("<ListBucketResult><IsTruncated>maybe</IsTruncated>"
                         "</ListBucketResult>")
          .has_value());
}

TEST(s3_xml_list, NamespacePrefixesAndAttributesAreIgnored)
{
  auto r = ParseListObjectsV2(
      "<s3:ListBucketResult xmlns:s3=\"http://s3.amazonaws.com/doc/\" "
      "a='x>y'><s3:IsTruncated>false</s3:IsTruncated>"
      "<s3:Contents id=\"1\"><s3:Key>k</s3:Key><s3:Size>7</s3:Size>"
      "</s3:Contents></s3:ListBucketResult>");
  ASSERT_TRUE(r.has_value()) << r.error();
  ASSERT_EQ(r->objects.size(), 1u);
  EXPECT_EQ(r->objects[0].key, "k");
  EXPECT_EQ(r->objects[0].size, 7u);
}

TEST(s3_xml_list, WhitespaceCommentsAndCdataAreTolerated)
{
  auto r = ParseListObjectsV2(
      "\xEF\xBB\xBF  <?xml version='1.0'?>\r\n<!-- listing -->\r\n"
      "<ListBucketResult>\r\n\t<IsTruncated>\r\n false \r\n</IsTruncated>\r\n"
      "<Contents><Key><![CDATA[a<b&c]]></Key><Size> 42 </Size></Contents>\r\n"
      "</ListBucketResult>\r\n<!-- end -->\r\n");
  ASSERT_TRUE(r.has_value()) << r.error();
  ASSERT_EQ(r->objects.size(), 1u);
  EXPECT_EQ(r->objects[0].key, "a<b&c");
  EXPECT_EQ(r->objects[0].size, 42u);
}

TEST(s3_xml_list, EntitiesInKeys)
{
  auto r = ParseListObjectsV2(
      "<ListBucketResult><IsTruncated>false</IsTruncated>"
      "<Contents><Key>a&amp;b&lt;c&gt;&quot;d&apos;&#65;&#x42;&#xe9;</Key>"
      "<Size>1</Size></Contents></ListBucketResult>");
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->objects[0].key, "a&b<c>\"d'AB\xC3\xA9");
}

TEST(s3_xml_list, KeysKeepSurroundingSpaces)
{
  auto r = ParseListObjectsV2(
      "<ListBucketResult><IsTruncated>false</IsTruncated>"
      "<Contents><Key> x </Key><Size>1</Size></Contents></ListBucketResult>");
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->objects[0].key, " x ");
}

TEST(s3_xml_list, UrlEncodedKeysAreDecoded)
{
  auto r = ParseListObjectsV2(
      "<ListBucketResult><EncodingType>url</EncodingType>"
      "<IsTruncated>false</IsTruncated>"
      "<Contents><Key>dir/a+b%2Bc%20d%C3%A9</Key><Size>1</Size></Contents>"
      "</ListBucketResult>");
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->objects[0].key, "dir/a b+c d\xC3\xA9");
}

TEST(s3_xml_list, UrlEncodingAfterContentsStillApplies)
{
  auto r = ParseListObjectsV2(
      "<ListBucketResult><IsTruncated>false</IsTruncated>"
      "<Contents><Key>a%20b</Key><Size>1</Size></Contents>"
      "<EncodingType>url</EncodingType></ListBucketResult>");
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->objects[0].key, "a b");
}

TEST(s3_xml_list, BadEscapeInUrlEncodedKey)
{
  EXPECT_FALSE(
      ParseListObjectsV2("<ListBucketResult><EncodingType>url</EncodingType>"
                         "<IsTruncated>false</IsTruncated>"
                         "<Contents><Key>a%zz</Key><Size>1</Size></Contents>"
                         "</ListBucketResult>")
          .has_value());
}

TEST(s3_xml_list, LargeSizeAndOverflow)
{
  auto r = ParseListObjectsV2(
      "<ListBucketResult><IsTruncated>false</IsTruncated>"
      "<Contents><Key>a</Key><Size>18446744073709551615</Size></Contents>"
      "</ListBucketResult>");
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->objects[0].size, 18446744073709551615ull);

  EXPECT_FALSE(ParseListObjectsV2(
                   "<ListBucketResult><IsTruncated>false</IsTruncated>"
                   "<Contents><Key>a</Key><Size>18446744073709551616</Size>"
                   "</Contents></ListBucketResult>")
                   .has_value());
}

TEST(s3_xml_list, BadContentsEntries)
{
  const char* const bad_sizes[] = {"", "-1", "12x", "1.5", "0x10"};
  for (const char* size : bad_sizes) {
    std::string xml = std::string(
                          "<ListBucketResult><IsTruncated>false"
                          "</IsTruncated><Contents><Key>a</Key><Size>")
                      + size + "</Size></Contents></ListBucketResult>";
    EXPECT_FALSE(ParseListObjectsV2(xml).has_value()) << size;
  }
  EXPECT_FALSE(ParseListObjectsV2(
                   "<ListBucketResult><IsTruncated>false</IsTruncated>"
                   "<Contents><Size>1</Size></Contents></ListBucketResult>")
                   .has_value());
  EXPECT_FALSE(
      ParseListObjectsV2("<ListBucketResult><IsTruncated>false</IsTruncated>"
                         "<Contents><Key>a</Key></Contents></ListBucketResult>")
          .has_value());
}

TEST(s3_xml_list, OwnerSizeIsNotTheObjectSize)
{
  auto r = ParseListObjectsV2(
      "<ListBucketResult><IsTruncated>false</IsTruncated>"
      "<Contents><Owner><Key>x</Key><Size>999</Size></Owner>"
      "<Key>real</Key><Size>5</Size></Contents></ListBucketResult>");
  ASSERT_TRUE(r.has_value()) << r.error();
  EXPECT_EQ(r->objects[0].key, "real");
  EXPECT_EQ(r->objects[0].size, 5u);
}

TEST(s3_xml_list, EmptyAndGarbageInput)
{
  EXPECT_FALSE(ParseListObjectsV2("").has_value());
  EXPECT_FALSE(ParseListObjectsV2("   \n").has_value());
  EXPECT_FALSE(ParseListObjectsV2("not xml at all").has_value());
  EXPECT_FALSE(ParseListObjectsV2(kXmlDecl).has_value());
  EXPECT_FALSE(ParseListObjectsV2("<html><body>502</body></html>").has_value());
  EXPECT_FALSE(ParseListObjectsV2("{\"json\": true}").has_value());
}

TEST(s3_xml_list, ErrorBodyIsNotAListing)
{
  auto r = ParseListObjectsV2(
      "<Error><Code>NoSuchBucket</Code><Message>x</Message></Error>");
  ASSERT_FALSE(r.has_value());
}

TEST(s3_xml_list, EveryPrefixOfAListingIsRejected)
{
  // A response cut off anywhere must never parse as a (shorter) listing.
  std::string full = kListing;
  std::string_view body = full;
  size_t end = body.rfind("</ListBucketResult>") + 19;
  for (size_t n = 0; n < end; ++n) {
    EXPECT_FALSE(ParseListObjectsV2(body.substr(0, n)).has_value())
        << "prefix of " << n << " bytes";
  }
  EXPECT_TRUE(ParseListObjectsV2(body.substr(0, end)).has_value());
}

TEST(s3_xml_list, MalformedStructure)
{
  const char* const bad[] = {
      "<ListBucketResult><IsTruncated>false</ListBucketResult>",
      "<ListBucketResult></IsTruncated></ListBucketResult>",
      "<ListBucketResult><IsTruncated>false</IsTruncated></ListBucketResult>"
      "<extra/>",
      "<ListBucketResult><IsTruncated>false</IsTruncated></ListBucketResult>"
      "trailing text",
      "<ListBucketResult><IsTruncated>false</IsTruncated></ListBucketResult>"
      "</ListBucketResult>",
      "<ListBucketResult><Contents><Key>a&bogus;</Key><Size>1</Size>"
      "</Contents><IsTruncated>false</IsTruncated></ListBucketResult>",
      "<ListBucketResult><Contents><Key>a&amp</Key><Size>1</Size>"
      "</Contents><IsTruncated>false</IsTruncated></ListBucketResult>",
      "<ListBucketResult><Contents><Key>&#0;</Key><Size>1</Size>"
      "</Contents><IsTruncated>false</IsTruncated></ListBucketResult>",
      "<ListBucketResult><Contents><Key>&#xD800;</Key><Size>1</Size>"
      "</Contents><IsTruncated>false</IsTruncated></ListBucketResult>",
      "<ListBucketResult><Contents><Key>&#x110000;</Key><Size>1</Size>"
      "</Contents><IsTruncated>false</IsTruncated></ListBucketResult>",
      "<ListBucketResult><Key><![CDATA[x</Key></ListBucketResult>",
      "<ListBucketResult><!-- never closed</ListBucketResult>",
      "<ListBucketResult attr=\"x></ListBucketResult>",
      "<!DOCTYPE x [<!ENTITY e \"v\">]><ListBucketResult>"
      "<IsTruncated>false</IsTruncated></ListBucketResult>",
      "<1bad><IsTruncated>false</IsTruncated></1bad>",
      "<>",
      "<",
  };
  for (const char* xml : bad) {
    EXPECT_FALSE(ParseListObjectsV2(xml).has_value()) << xml;
  }
}

TEST(s3_xml_list, RandomlyCorruptedInputNeverCrashes)
{
  // Fixed seed: flips, inserts and deletes bytes of a listing and an error
  // body; only the absence of crashes (run under a sanitizer) is checked.
  const std::string seeds[]
      = {kListing, "<Error><Code>A</Code><Message>m&amp;n</Message></Error>"};
  const char alphabet[] = "<>/&;#x\"'![]-?= \n\xEF\xBB\xBF";
  std::mt19937 rng(12345);
  for (const std::string& seed : seeds) {
    for (int round = 0; round < 3000; ++round) {
      std::string s = seed;
      int edits = 1 + static_cast<int>(rng() % 4);
      for (int e = 0; e < edits && !s.empty(); ++e) {
        size_t pos = rng() % s.size();
        switch (rng() % 3) {
          case 0:
            s[pos] = alphabet[rng() % (sizeof(alphabet) - 1)];
            break;
          case 1:
            s.insert(pos, 1, alphabet[rng() % (sizeof(alphabet) - 1)]);
            break;
          default:
            s.erase(pos, 1 + rng() % 8);
            break;
        }
      }
      (void)ParseListObjectsV2(s);
      (void)ParseS3Error(s);
    }
  }
}

TEST(s3_xml_list, DocumentNestedTooDeeplyIsRejected)
{
  std::string xml;
  for (int i = 0; i < 200; ++i) { xml += "<a>"; }
  for (int i = 0; i < 200; ++i) { xml += "</a>"; }
  EXPECT_FALSE(ParseListObjectsV2(xml).has_value());
}

TEST(s3_xml_list, ThousandKeys)
{
  std::string xml
      = "<ListBucketResult><IsTruncated>true</IsTruncated>"
        "<NextContinuationToken>tok</NextContinuationToken>";
  for (int i = 0; i < 1000; ++i) {
    xml += "<Contents><Key>Vol/" + std::to_string(i) + "</Key><Size>"
           + std::to_string(i * 3) + "</Size></Contents>";
  }
  xml += "</ListBucketResult>";
  auto r = ParseListObjectsV2(xml);
  ASSERT_TRUE(r.has_value()) << r.error();
  ASSERT_EQ(r->objects.size(), 1000u);
  EXPECT_EQ(r->objects[999].key, "Vol/999");
  EXPECT_EQ(r->objects[999].size, 2997u);
}

TEST(s3_xml_error, AccessDenied)
{
  auto e = ParseS3Error(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<Error><Code>AccessDenied</Code><Message>Access Denied</Message>"
      "<BucketName>bucket-a</BucketName>"
      "<RequestId>tx000001-0064a1b2c3-1a2b3c-vn-north-1a</RequestId>"
      "<HostId>1a2b3c-vn-north-1a-vn-north-1</HostId></Error>");
  ASSERT_TRUE(e.has_value()) << e.error();
  EXPECT_EQ(e->code, "AccessDenied");
  EXPECT_EQ(e->message, "Access Denied");
  EXPECT_EQ(e->request_id, "tx000001-0064a1b2c3-1a2b3c-vn-north-1a");
}

TEST(s3_xml_error, MessageAndRequestIdAreOptional)
{
  auto e = ParseS3Error("<Error><Code>SlowDown</Code></Error>");
  ASSERT_TRUE(e.has_value()) << e.error();
  EXPECT_EQ(e->code, "SlowDown");
  EXPECT_TRUE(e->message.empty());
  EXPECT_TRUE(e->request_id.empty());
}

TEST(s3_xml_error, EntitiesAndWhitespaceInMessage)
{
  auto e = ParseS3Error(
      "<Error>\n <Code> BadDigest </Code>\n"
      " <Message>The Content-MD5 you specified did not match what we "
      "received.&#10;&lt;retry&gt;</Message>\n</Error>\n");
  ASSERT_TRUE(e.has_value()) << e.error();
  EXPECT_EQ(e->code, "BadDigest");
  EXPECT_EQ(e->message,
            "The Content-MD5 you specified did not match what we "
            "received.\n<retry>");
}

TEST(s3_xml_error, MissingOrEmptyCodeIsAnError)
{
  EXPECT_FALSE(ParseS3Error("<Error><Message>m</Message></Error>").has_value());
  EXPECT_FALSE(ParseS3Error("<Error><Code></Code></Error>").has_value());
  EXPECT_FALSE(ParseS3Error("<Error><Code>  </Code></Error>").has_value());
  EXPECT_FALSE(ParseS3Error("<Error/>").has_value());
}

TEST(s3_xml_error, WrongRootAndGarbage)
{
  EXPECT_FALSE(ParseS3Error(kListing).has_value());
  EXPECT_FALSE(ParseS3Error("").has_value());
  EXPECT_FALSE(ParseS3Error("<html>Bad Gateway</html>").has_value());
  EXPECT_FALSE(ParseS3Error("<Error><Code>X</Message></Error>").has_value());
}

TEST(s3_xml_error, EveryPrefixOfAnErrorBodyIsRejected)
{
  std::string full
      = "<Error><Code>NoSuchKey</Code><Message>m</Message></Error>";
  for (size_t n = 0; n < full.size(); ++n) {
    EXPECT_FALSE(ParseS3Error(std::string_view(full).substr(0, n)).has_value())
        << "prefix of " << n << " bytes";
  }
  EXPECT_TRUE(ParseS3Error(full).has_value());
}
