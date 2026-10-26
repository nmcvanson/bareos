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

/* SigV4 helpers against AWS's published examples: [S3] S3 API reference, [IAM]
 * General Reference, [SUITE] aws-sig-v4-test-suite, [RFC] RFC 4231, [OWN] fixed
 * requests of this project. AWS example keys only. */

#include "gtest/gtest.h"

#include "stored/backends/s3_sigv4.h"

#include <string>

using namespace storagedaemon::s3;

namespace {

constexpr char kS3AccessKey[] = "AKIAIOSFODNN7EXAMPLE";
constexpr char kS3SecretKey[] = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
constexpr char kS3Date[] = "20130524T000000Z";
constexpr char kEmptyHash[]
    = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

constexpr char kSuiteAccessKey[] = "AKIDEXAMPLE";
constexpr char kSuiteSecretKey[] = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
constexpr char kSuiteDate[] = "20150830T123600Z";

SigV4Credentials S3Credentials(std::string region = "us-east-1")
{ return {kS3AccessKey, kS3SecretKey, std::move(region), "s3"}; }

SigV4Credentials SuiteCredentials()
{ return {kSuiteAccessKey, kSuiteSecretKey, "us-east-1", "service"}; }

// The [S3] examples all sign host, x-amz-content-sha256 and x-amz-date.
SigV4Request S3Example(std::string method,
                       std::string path,
                       NameValueList query,
                       NameValueList extra_headers,
                       const std::string& payload_hash)
{
  SigV4Request r;
  r.method = std::move(method);
  r.path = std::move(path);
  r.query = std::move(query);
  r.headers = {{"Host", "examplebucket.s3.amazonaws.com"}};
  for (auto& h : extra_headers) { r.headers.push_back(std::move(h)); }
  r.headers.emplace_back("x-amz-content-sha256", payload_hash);
  r.headers.emplace_back("x-amz-date", kS3Date);
  r.payload_hash = payload_hash;
  return r;
}

SigV4Request SuiteRequest(NameValueList headers,
                          std::string method = "GET",
                          std::string path = "/",
                          NameValueList query = {},
                          std::string payload_hash = kEmptyHash)
{
  SigV4Request r;
  r.method = std::move(method);
  r.path = std::move(path);
  r.query = std::move(query);
  r.headers = std::move(headers);
  r.payload_hash = std::move(payload_hash);
  return r;
}

SigV4Result MustSign(const SigV4Credentials& c,
                     const SigV4Request& r,
                     std::string_view date)
{
  auto result = Sign(c, r, date);
  EXPECT_TRUE(result.has_value()) << (result ? "" : result.error());
  return result ? *result : SigV4Result{};
}

}  // namespace

TEST(s3_sigv4_primitives, Sha256HexKnownValues)
{
  EXPECT_EQ(Sha256Hex(""), kEmptyHash);
  EXPECT_EQ(Sha256Hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // [S3] payload of the PUT Object example.
  EXPECT_EQ(Sha256Hex("Welcome to Amazon S3."),
            "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072");
}

TEST(s3_sigv4_primitives, HmacSha256Rfc4231Case2)
{
  EXPECT_EQ(HexEncode(HmacSha256("Jefe", "what do ya want for nothing?")),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(s3_sigv4_primitives, HexEncodeIncludingZeroBytes)
{
  EXPECT_EQ(HexEncode(""), "");
  EXPECT_EQ(HexEncode(std::string_view("\x00\x0f\xa0\xff", 4)), "000fa0ff");
}

TEST(s3_sigv4_encoding, UriEncodeKeepsOnlyUnreservedCharacters)
{
  EXPECT_EQ(UriEncode("AZaz09-_.~", true), "AZaz09-_.~");
  EXPECT_EQ(UriEncode("a b+c=d&e", true), "a%20b%2Bc%3Dd%26e");
  EXPECT_EQ(UriEncode("$!*'()", true), "%24%21%2A%27%28%29");
  EXPECT_EQ(UriEncode("", true), "");
}

TEST(s3_sigv4_encoding, UriEncodeSlashOnRequest)
{
  EXPECT_EQ(UriEncode("a/b c", true), "a%2Fb%20c");
  EXPECT_EQ(UriEncode("a/b c", false), "a/b%20c");
}

TEST(s3_sigv4_encoding, UriEncodeUtf8BytesWithUpperCaseHex)
{
  // U+00E9 is C3 A9, U+20AC is E2 82 AC.
  EXPECT_EQ(UriEncode("\xC3\xA9\xE2\x82\xAC", true), "%C3%A9%E2%82%AC");
}

TEST(s3_sigv4_encoding, UriEncodeEncodesNulAndControlBytes)
{ EXPECT_EQ(UriEncode(std::string_view("a\0b\n", 4), true), "a%00b%0A"); }

TEST(s3_sigv4_encoding, UriDecodeRoundTrip)
{
  std::string raw = "dir/a b+c~\xC3\xA9&=.txt";
  auto decoded = UriDecode(UriEncode(raw, true));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, raw);
}

TEST(s3_sigv4_encoding, UriDecodeKeepsPlusAndAcceptsLowerCaseHex)
{
  auto decoded = UriDecode("a+b%2f%2F");
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, "a+b//");
}

TEST(s3_sigv4_encoding, UriDecodeRejectsMalformedEscapes)
{
  EXPECT_FALSE(UriDecode("%").has_value());
  EXPECT_FALSE(UriDecode("abc%4").has_value());
  EXPECT_FALSE(UriDecode("%zz").has_value());
  EXPECT_FALSE(UriDecode("%4g").has_value());
}

TEST(s3_sigv4_canonical, QueryStringSortedByNameThenValue)
{
  EXPECT_EQ(CanonicalQueryString({}), "");
  EXPECT_EQ(CanonicalQueryString({{"prefix", "J"}, {"max-keys", "2"}}),
            "max-keys=2&prefix=J");
  EXPECT_EQ(CanonicalQueryString({{"a", "2"}, {"a", "1"}, {"B", "x"}}),
            "B=x&a=1&a=2");
}

TEST(s3_sigv4_canonical, QueryStringKeepsEmptyValueAndEncodes)
{
  EXPECT_EQ(CanonicalQueryString({{"lifecycle", ""}}), "lifecycle=");
  EXPECT_EQ(CanonicalQueryString(
                {{"continuation-token", "a+b/c="}, {"prefix", "x y/"}}),
            "continuation-token=a%2Bb%2Fc%3D&prefix=x%20y%2F");
}

TEST(s3_sigv4_canonical, QueryStringSortsOnTheEncodedForm)
{
  // "%20" (space) sorts before "a" and after nothing: encoded bytes decide.
  EXPECT_EQ(CanonicalQueryString({{"a", "1"}, {" ", "2"}}), "%20=2&a=1");
}

TEST(s3_sigv4_canonical, EmptyPathMeansRoot)
{
  SigV4Request r = SuiteRequest(
      {{"host", "example.amazonaws.com"}, {"x-amz-date", kSuiteDate}}, "GET",
      "");
  std::string signed_headers;
  auto cr = CanonicalRequest(r, signed_headers);
  ASSERT_TRUE(cr.has_value());
  EXPECT_EQ(cr->substr(0, 6), "GET\n/\n");
}

TEST(s3_sigv4_canonical, HeadersLowerCasedSortedTrimmedAndJoined)
{
  SigV4Request r = SuiteRequest({{"X-Amz-Date", kSuiteDate},
                                 {"HOST", "example.amazonaws.com"},
                                 {"My-Header", "  a \t  b   c  "},
                                 {"my-header", "z"}});
  std::string signed_headers;
  auto cr = CanonicalRequest(r, signed_headers);
  ASSERT_TRUE(cr.has_value());
  EXPECT_EQ(signed_headers, "host;my-header;x-amz-date");
  EXPECT_EQ(*cr, std::string("GET\n/\n\n"
                             "host:example.amazonaws.com\n"
                             "my-header:a b c,z\n"
                             "x-amz-date:")
                     + kSuiteDate + "\n\nhost;my-header;x-amz-date\n"
                     + kEmptyHash);
}

TEST(s3_sigv4_canonical, RejectsBadRequests)
{
  std::string signed_headers;
  SigV4Request good = SuiteRequest({{"host", "h"}});
  EXPECT_TRUE(CanonicalRequest(good, signed_headers).has_value());

  SigV4Request r = good;
  r.method.clear();
  EXPECT_FALSE(CanonicalRequest(r, signed_headers).has_value());

  r = good;
  r.payload_hash.clear();
  EXPECT_FALSE(CanonicalRequest(r, signed_headers).has_value());

  r = good;
  r.path = "relative";
  EXPECT_FALSE(CanonicalRequest(r, signed_headers).has_value());

  r = good;
  r.headers = {{"x-amz-date", kSuiteDate}};
  EXPECT_FALSE(CanonicalRequest(r, signed_headers).has_value());

  r = good;
  r.headers.emplace_back("bad name", "v");
  EXPECT_FALSE(CanonicalRequest(r, signed_headers).has_value());

  r = good;
  r.headers.emplace_back("", "v");
  EXPECT_FALSE(CanonicalRequest(r, signed_headers).has_value());

  r = good;
  r.headers.emplace_back("x-test", "a\r\nInjected: 1");
  EXPECT_FALSE(CanonicalRequest(r, signed_headers).has_value());
}

// [IAM] "Derive a signing key", secret ...EXAMPLEKEY, 20150830, us-east-1, iam.
TEST(s3_sigv4_sign, SigningKeyPublishedVector)
{
  EXPECT_EQ(
      HexEncode(SigningKey(kSuiteSecretKey, "20150830", "us-east-1", "iam")),
      "c4afb1cc5771d871763a393e44b703571b55cc28424d1a5e86da6ed3c154a4b9");
}

// [IAM] ListUsers example: canonical request hash and signature.
TEST(s3_sigv4_sign, IamListUsersPublishedExample)
{
  SigV4Credentials c{kSuiteAccessKey, kSuiteSecretKey, "us-east-1", "iam"};
  SigV4Request r = SuiteRequest(
      {{"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"},
       {"Host", "iam.amazonaws.com"},
       {"X-Amz-Date", kSuiteDate}},
      "GET", "/", {{"Action", "ListUsers"}, {"Version", "2010-05-08"}});
  SigV4Result res = MustSign(c, r, kSuiteDate);
  EXPECT_EQ(res.signed_headers, "content-type;host;x-amz-date");
  EXPECT_EQ(Sha256Hex(res.canonical_request),
            "f536975d06c0309214f805bb90ccff089219ecd68b2577efef23edd43b7e1a59");
  EXPECT_EQ(res.signature,
            "5d672d79c15b13162d9279b0855cfba6789a8edb4c82c400e06b5924a6f2b5d7");
}

// [S3] GET Object with a Range header.
TEST(s3_sigv4_sign, S3GetObjectWithRangePublishedExample)
{
  SigV4Request r
      = S3Example("GET", "/test.txt", {}, {{"Range", "bytes=0-9"}}, kEmptyHash);
  SigV4Result res = MustSign(S3Credentials(), r, kS3Date);
  EXPECT_EQ(res.canonical_request,
            std::string("GET\n/test.txt\n\n"
                        "host:examplebucket.s3.amazonaws.com\n"
                        "range:bytes=0-9\n"
                        "x-amz-content-sha256:")
                + kEmptyHash
                + "\nx-amz-date:20130524T000000Z\n\n"
                  "host;range;x-amz-content-sha256;x-amz-date\n"
                + kEmptyHash);
  EXPECT_EQ(res.string_to_sign,
            "AWS4-HMAC-SHA256\n20130524T000000Z\n"
            "20130524/us-east-1/s3/aws4_request\n"
            "7344ae5b7ee6c3e7e6b0fe0640412a37625d1fbfff95c48bbb2dc43964946972");
  EXPECT_EQ(res.signature,
            "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
  EXPECT_EQ(res.authorization,
            "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/"
            "us-east-1/s3/aws4_request, "
            "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
            "Signature="
            "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
}

// [S3] PUT Object: '$' in the key and a signed payload.
TEST(s3_sigv4_sign, S3PutObjectPublishedExample)
{
  std::string payload_hash = Sha256Hex("Welcome to Amazon S3.");
  SigV4Request r = S3Example("PUT", "/test$file.text", {},
                             {{"Date", "Fri, 24 May 2013 00:00:00 GMT"},
                              {"x-amz-storage-class", "REDUCED_REDUNDANCY"}},
                             payload_hash);
  SigV4Result res = MustSign(S3Credentials(), r, kS3Date);
  EXPECT_NE(res.canonical_request.find("PUT\n/test%24file.text\n"),
            std::string::npos);
  EXPECT_EQ(res.signed_headers,
            "date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class");
  EXPECT_EQ(res.signature,
            "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");
}

// [S3] GET Bucket lifecycle: a sub-resource without a value.
TEST(s3_sigv4_sign, S3GetBucketLifecyclePublishedExample)
{
  SigV4Request r = S3Example("GET", "/", {{"lifecycle", ""}}, {}, kEmptyHash);
  SigV4Result res = MustSign(S3Credentials(), r, kS3Date);
  EXPECT_NE(res.canonical_request.find("GET\n/\nlifecycle=\n"),
            std::string::npos);
  EXPECT_EQ(res.signature,
            "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543");
}

// [S3] GET Bucket (List Objects) with max-keys and prefix.
TEST(s3_sigv4_sign, S3ListObjectsPublishedExample)
{
  // The query is given in the opposite order to the canonical one.
  SigV4Request r = S3Example("GET", "/", {{"prefix", "J"}, {"max-keys", "2"}},
                             {}, kEmptyHash);
  SigV4Result res = MustSign(S3Credentials(), r, kS3Date);
  EXPECT_NE(res.canonical_request.find("GET\n/\nmax-keys=2&prefix=J\n"),
            std::string::npos);
  EXPECT_EQ(res.signature,
            "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7");
}

// [SUITE] get-vanilla.
TEST(s3_sigv4_sign, SuiteGetVanilla)
{
  SigV4Request r = SuiteRequest(
      {{"Host", "example.amazonaws.com"}, {"X-Amz-Date", kSuiteDate}});
  SigV4Result res = MustSign(SuiteCredentials(), r, kSuiteDate);
  EXPECT_EQ(res.signature,
            "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");
  EXPECT_EQ(res.authorization,
            "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/"
            "service/aws4_request, SignedHeaders=host;x-amz-date, Signature="
            "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");
}

// [SUITE] get-vanilla-query-order-key-case.
TEST(s3_sigv4_sign, SuiteQueryOrder)
{
  SigV4Request r = SuiteRequest(
      {{"Host", "example.amazonaws.com"}, {"X-Amz-Date", kSuiteDate}}, "GET",
      "/", {{"Param2", "value2"}, {"Param1", "value1"}});
  EXPECT_EQ(MustSign(SuiteCredentials(), r, kSuiteDate).signature,
            "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500");
}

// [SUITE] get-vanilla-empty-query-key.
TEST(s3_sigv4_sign, SuiteSingleQueryParameter)
{
  SigV4Request r = SuiteRequest(
      {{"Host", "example.amazonaws.com"}, {"X-Amz-Date", kSuiteDate}}, "GET",
      "/", {{"Param1", "value1"}});
  EXPECT_EQ(MustSign(SuiteCredentials(), r, kSuiteDate).signature,
            "a67d582fa61cc504c4bae71f336f98b97f1ea3c7a6bfe1b6e45aec72011b9aeb");
}

// [SUITE] get-header-key-duplicate: values keep their order, joined by ','.
TEST(s3_sigv4_sign, SuiteDuplicateHeaderKeys)
{
  SigV4Request r = SuiteRequest({{"Host", "example.amazonaws.com"},
                                 {"My-Header1", "value2"},
                                 {"My-Header1", "value2"},
                                 {"My-Header1", "value1"},
                                 {"X-Amz-Date", kSuiteDate}});
  SigV4Result res = MustSign(SuiteCredentials(), r, kSuiteDate);
  EXPECT_NE(res.canonical_request.find("my-header1:value2,value2,value1\n"),
            std::string::npos);
  EXPECT_EQ(res.signature,
            "c9d5ea9f3f72853aea855b47ea873832890dbdd183b4468f858259531a5138ea");
}

// [SUITE] get-header-value-trim: leading blanks and runs of blanks.
TEST(s3_sigv4_sign, SuiteHeaderValueTrim)
{
  SigV4Request r = SuiteRequest({{"Host", "example.amazonaws.com"},
                                 {"My-Header1", " value1"},
                                 {"My-Header2", " \"a   b   c\""},
                                 {"X-Amz-Date", kSuiteDate}});
  SigV4Result res = MustSign(SuiteCredentials(), r, kSuiteDate);
  EXPECT_NE(res.canonical_request.find("my-header2:\"a b c\"\n"),
            std::string::npos);
  EXPECT_EQ(res.signature,
            "acc3ed3afb60bb290fc8d2dd0098b9911fcaa05412b367055dee359757a9c736");
}

// [SUITE] get-space-normalized (aws-signing-test-suite): space encoded once.
TEST(s3_sigv4_sign, SuiteSpaceInPath)
{
  SigV4Request r = SuiteRequest(
      {{"Host", "example.amazonaws.com"}, {"X-Amz-Date", kSuiteDate}}, "GET",
      "/example space/");
  SigV4Result res = MustSign(SuiteCredentials(), r, kSuiteDate);
  EXPECT_NE(res.canonical_request.find("GET\n/example%20space/\n"),
            std::string::npos);
  EXPECT_EQ(res.signature,
            "652487583200325589f1fba4c7e578f72c47cb61beeca81406b39ddec1366741");
}

// [SUITE] post-x-www-form-urlencoded: a signed payload hash.
TEST(s3_sigv4_sign, SuitePostWithPayload)
{
  SigV4Request r
      = SuiteRequest({{"Content-Type", "application/x-www-form-urlencoded"},
                      {"Host", "example.amazonaws.com"},
                      {"X-Amz-Date", kSuiteDate}},
                     "POST", "/", {}, Sha256Hex("Param1=value1"));
  EXPECT_EQ(MustSign(SuiteCredentials(), r, kSuiteDate).signature,
            "ff11897932ad3f4e8b18135d722051e5ac45fc38421b1da7b9d196a0fe09473a");
}

// [OWN] virtual-host addressing, UNSIGNED-PAYLOAD, region vn-north-1.
TEST(s3_sigv4_sign, OwnVirtualHostPutUnsignedPayload)
{
  SigV4Request r;
  r.method = "PUT";
  S3Target t = MakeS3Target("s3.example.com", "mybucket",
                            "bdrtest-Vol-0001/0003", false);
  r.path = t.path;
  r.headers = {{"Host", t.host}};
  r.payload_hash = std::string(kUnsignedPayload);
  AddS3SigningHeaders(r, "20260106T081530Z");
  SigV4Result res
      = MustSign(S3Credentials("vn-north-1"), r, "20260106T081530Z");
  EXPECT_NE(res.canonical_request.find(
                "PUT\n/bdrtest-Vol-0001/0003\n\nhost:mybucket.s3.example.com\n"
                "x-amz-content-sha256:UNSIGNED-PAYLOAD\n"),
            std::string::npos);
  EXPECT_EQ(res.signature,
            "66aac4766147da3b4bbaff926eaa352ba69a3b392f7de977af45242cd7bfc96f");
}

// [OWN] path-style listing with a port in the host and encoded query values.
TEST(s3_sigv4_sign, OwnPathStyleListWithPortAndEncodedQuery)
{
  SigV4Request r;
  r.method = "GET";
  S3Target t = MakeS3Target("s3.example.com:9000", "mybucket", "", true);
  r.path = t.path;
  r.query = {{"list-type", "2"},
             {"prefix", "bdrtest-Vol 0001/"},
             {"continuation-token", "a+b/c="}};
  r.headers = {{"Host", t.host}};
  r.payload_hash = kEmptyHash;
  AddS3SigningHeaders(r, "20260106T081530Z");
  SigV4Result res = MustSign(S3Credentials(), r, "20260106T081530Z");
  EXPECT_EQ(res.canonical_request,
            std::string("GET\n/mybucket\n"
                        "continuation-token=a%2Bb%2Fc%3D&list-type=2&"
                        "prefix=bdrtest-Vol%200001%2F\n"
                        "host:s3.example.com:9000\n"
                        "x-amz-content-sha256:")
                + kEmptyHash
                + "\nx-amz-date:20260106T081530Z\n\n"
                  "host;x-amz-content-sha256;x-amz-date\n"
                + kEmptyHash);
  EXPECT_EQ(res.signature,
            "7900c7296931226291c22f6478089f0ea4fffb1d36c4ccb7e0e07ea9623d0dee");
}

// [OWN] key with space, plus, tilde and a non-ASCII letter.
TEST(s3_sigv4_sign, OwnPathEncoding)
{
  SigV4Request r;
  r.method = "GET";
  r.path = "/mybucket/dir/a b+c~\xC3\xA9.txt";
  r.headers = {{"Host", "s3.example.com"}};
  r.payload_hash = kEmptyHash;
  AddS3SigningHeaders(r, "20260106T081530Z");
  SigV4Result res = MustSign(S3Credentials(), r, "20260106T081530Z");
  EXPECT_NE(
      res.canonical_request.find("GET\n/mybucket/dir/a%20b%2Bc~%C3%A9.txt\n\n"),
      std::string::npos);
  EXPECT_EQ(res.signature,
            "9baae7e78166876fd14afe84144433555d66dad76324310471661b344c0f58dc");
}

TEST(s3_sigv4_sign, PayloadHashChangesTheSignature)
{
  SigV4Request unsigned_body
      = S3Example("PUT", "/k", {}, {}, std::string(kUnsignedPayload));
  SigV4Request signed_body = S3Example("PUT", "/k", {}, {}, Sha256Hex("data"));
  EXPECT_NE(MustSign(S3Credentials(), unsigned_body, kS3Date).signature,
            MustSign(S3Credentials(), signed_body, kS3Date).signature);
}

TEST(s3_sigv4_sign, SecretKeyNeverAppearsInTheResult)
{
  SigV4Request r = S3Example("GET", "/test.txt", {}, {}, kEmptyHash);
  SigV4Result res = MustSign(S3Credentials(), r, kS3Date);
  for (const std::string* s :
       {&res.canonical_request, &res.string_to_sign, &res.signed_headers,
        &res.signature, &res.authorization}) {
    EXPECT_EQ(s->find(kS3SecretKey), std::string::npos);
  }
}

TEST(s3_sigv4_sign, RejectsMissingCredentialsAndBadDates)
{
  SigV4Request r = S3Example("GET", "/", {}, {}, kEmptyHash);

  SigV4Credentials c = S3Credentials();
  c.access_key.clear();
  EXPECT_FALSE(Sign(c, r, kS3Date).has_value());
  c = S3Credentials();
  c.secret_key.clear();
  EXPECT_FALSE(Sign(c, r, kS3Date).has_value());
  c = S3Credentials("");
  EXPECT_FALSE(Sign(c, r, kS3Date).has_value());
  c = S3Credentials();
  c.service.clear();
  EXPECT_FALSE(Sign(c, r, kS3Date).has_value());

  EXPECT_FALSE(Sign(S3Credentials(), r, "").has_value());
  EXPECT_FALSE(Sign(S3Credentials(), r, "20130524").has_value());
  EXPECT_FALSE(Sign(S3Credentials(), r, "20130524T000000").has_value());
  EXPECT_FALSE(Sign(S3Credentials(), r, "2013-05-24T00:00:00Z").has_value());
  EXPECT_FALSE(Sign(S3Credentials(), r, "20130524x000000Z").has_value());
  EXPECT_FALSE(Sign(S3Credentials(), r, "2013052aT000000Z").has_value());
  EXPECT_TRUE(Sign(S3Credentials(), r, kS3Date).has_value());
}

TEST(s3_sigv4_sign, SignPropagatesRequestErrors)
{
  SigV4Request r = S3Example("GET", "/", {}, {}, kEmptyHash);
  r.headers = {{"x-amz-date", kS3Date}};
  EXPECT_FALSE(Sign(S3Credentials(), r, kS3Date).has_value());
}

TEST(s3_sigv4_helpers, AddS3SigningHeadersAddsTheTwoHeaders)
{
  SigV4Request r;
  r.headers = {{"Host", "h"}};
  r.payload_hash = std::string(kUnsignedPayload);
  AddS3SigningHeaders(r, kS3Date);
  ASSERT_EQ(r.headers.size(), 3u);
  EXPECT_EQ(r.headers[1], (std::pair<std::string, std::string>{
                              "x-amz-content-sha256", "UNSIGNED-PAYLOAD"}));
  EXPECT_EQ(r.headers[2],
            (std::pair<std::string, std::string>{"x-amz-date", kS3Date}));
}

TEST(s3_sigv4_helpers, AddS3SigningHeadersKeepsExistingOnesAnyCase)
{
  SigV4Request r;
  r.headers = {{"Host", "h"},
               {"X-Amz-Date", "20000101T000000Z"},
               {"X-AMZ-Content-SHA256", "abc"}};
  r.payload_hash = "def";
  AddS3SigningHeaders(r, kS3Date);
  EXPECT_EQ(r.headers.size(), 3u);
  EXPECT_EQ(r.headers[1].second, "20000101T000000Z");
  EXPECT_EQ(r.headers[2].second, "abc");
}

TEST(s3_sigv4_helpers, FormatAmzDate)
{
  EXPECT_EQ(FormatAmzDate(0), "19700101T000000Z");
  EXPECT_EQ(FormatAmzDate(1369353600), "20130524T000000Z");
  EXPECT_EQ(FormatAmzDate(1767687330), "20260106T081530Z");
  EXPECT_EQ(FormatAmzDate(4102444799), "20991231T235959Z");
}

TEST(s3_sigv4_helpers, MakeS3TargetPathStyle)
{
  S3Target t = MakeS3Target("os.example.vn", "bucket-a", "Vol-1/0000", true);
  EXPECT_EQ(t.host, "os.example.vn");
  EXPECT_EQ(t.path, "/bucket-a/Vol-1/0000");
  t = MakeS3Target("os.example.vn:8443", "bucket-a", "", true);
  EXPECT_EQ(t.host, "os.example.vn:8443");
  EXPECT_EQ(t.path, "/bucket-a");
}

TEST(s3_sigv4_helpers, MakeS3TargetVirtualHost)
{
  S3Target t = MakeS3Target("os.example.vn", "bucket-a", "Vol-1/0000", false);
  EXPECT_EQ(t.host, "bucket-a.os.example.vn");
  EXPECT_EQ(t.path, "/Vol-1/0000");
  t = MakeS3Target("os.example.vn", "bucket-a", "", false);
  EXPECT_EQ(t.host, "bucket-a.os.example.vn");
  EXPECT_EQ(t.path, "/");
}
