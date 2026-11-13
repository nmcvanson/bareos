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

#ifndef BAREOS_STORED_BACKENDS_S3_SIGV4_H_
#define BAREOS_STORED_BACKENDS_S3_SIGV4_H_

#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "tl/expected.hpp"

// AWS Signature Version 4 for S3: pure functions, no network, no logging.
namespace storagedaemon::s3 {

using NameValueList = std::vector<std::pair<std::string, std::string>>;

// Payload hash value for requests whose body is not hashed (HTTPS only).
inline constexpr std::string_view kUnsignedPayload = "UNSIGNED-PAYLOAD";

struct SigV4Credentials {
  std::string access_key;
  std::string secret_key;
  std::string region;
  std::string service{"s3"};
};

struct SigV4Request {
  std::string method;
  // Absolute path, not URI-encoded; empty means "/".
  std::string path;
  // Query parameters, names and values not URI-encoded.
  NameValueList query;
  // Headers to sign, values as sent; "host" must be among them.
  NameValueList headers;
  // Lower-case hex SHA-256 of the body, or kUnsignedPayload.
  std::string payload_hash;
};

struct SigV4Result {
  std::string canonical_request;
  std::string string_to_sign;
  std::string signed_headers;
  std::string signature;
  // Value of the Authorization header.
  std::string authorization;
};

// Where a request for an object goes: Host header value and URL path.
struct S3Target {
  std::string host;
  std::string path;
};

std::string HexEncode(std::string_view data);
std::string Sha256Hex(std::string_view data);
// Raw 32-byte HMAC-SHA256.
std::string HmacSha256(std::string_view key, std::string_view data);

// Percent-encodes everything but A-Z a-z 0-9 - _ . ~ (and '/' if requested).
std::string UriEncode(std::string_view in, bool encode_slash);
// Percent-decodes; fails on a malformed escape.
tl::expected<std::string, std::string> UriDecode(std::string_view in);

// "name=value" pairs, encoded, sorted by name then value, joined by '&'.
std::string CanonicalQueryString(const NameValueList& query);
// Canonical request of the request; signed_headers receives the header list.
tl::expected<std::string, std::string> CanonicalRequest(
    const SigV4Request& request,
    std::string& signed_headers);
// Raw signing key for the date (YYYYMMDD), region and service.
std::string SigningKey(std::string_view secret_key,
                       std::string_view date,
                       std::string_view region,
                       std::string_view service);

// amz_date is "YYYYMMDDTHHMMSSZ". Signs exactly the headers in the request.
tl::expected<SigV4Result, std::string> Sign(const SigV4Credentials& credentials,
                                            const SigV4Request& request,
                                            std::string_view amz_date);

// Adds x-amz-date and x-amz-content-sha256 unless already present.
void AddS3SigningHeaders(SigV4Request& request, std::string_view amz_date);

// "YYYYMMDDTHHMMSSZ" of a UTC time.
std::string FormatAmzDate(std::time_t time);

// Host and path of an object; endpoint may carry a port. With path_style the
// bucket is the first path segment, otherwise a prefix of the host name.
S3Target MakeS3Target(std::string_view endpoint,
                      std::string_view bucket,
                      std::string_view key,
                      bool path_style);

}  // namespace storagedaemon::s3

#endif  // BAREOS_STORED_BACKENDS_S3_SIGV4_H_
