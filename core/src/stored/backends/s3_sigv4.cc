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

#include "stored/backends/s3_sigv4.h"

#include <algorithm>
#include <map>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace storagedaemon::s3 {

namespace {

constexpr std::string_view kAlgorithm = "AWS4-HMAC-SHA256";

bool IsUnreserved(unsigned char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
         || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
         || c == '~';
}

char ToLower(char c)
{ return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string Lower(std::string_view in)
{
  std::string out(in);
  for (char& c : out) { c = ToLower(c); }
  return out;
}

bool IsBlank(char c) { return c == ' ' || c == '\t'; }

// Trims blanks at both ends and collapses inner runs of blanks to one space.
std::string TrimAndCollapse(std::string_view in)
{
  std::string out;
  bool pending_space = false;
  for (char c : in) {
    if (IsBlank(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) { out.push_back(' '); }
    pending_space = false;
    out.push_back(c);
  }
  return out;
}

bool ValidAmzDate(std::string_view d)
{
  if (d.size() != 16 || d[8] != 'T' || d[15] != 'Z') { return false; }
  for (size_t i = 0; i < 15; ++i) {
    if (i == 8) { continue; }
    if (d[i] < '0' || d[i] > '9') { return false; }
  }
  return true;
}

int HexValue(char c)
{
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
  if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
  return -1;
}

void Cleanse(std::string& s)
{
  if (!s.empty()) { OPENSSL_cleanse(s.data(), s.size()); }
}

}  // namespace

std::string HexEncode(std::string_view data)
{
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (unsigned char c : data) {
    out.push_back(kDigits[c >> 4]);
    out.push_back(kDigits[c & 0xf]);
  }
  return out;
}

std::string Sha256Hex(std::string_view data)
{
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (EVP_Digest(data.data(), data.size(), md, &md_len, EVP_sha256(), nullptr)
      != 1) {
    return {};
  }
  return HexEncode(std::string_view(reinterpret_cast<const char*>(md), md_len));
}

std::string HmacSha256(std::string_view key, std::string_view data)
{
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char*>(data.data()), data.size(), md,
           &md_len)
      == nullptr) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(md), md_len);
}

std::string UriEncode(std::string_view in, bool encode_slash)
{
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    if (IsUnreserved(c) || (c == '/' && !encode_slash)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kDigits[c >> 4]);
      out.push_back(kDigits[c & 0xf]);
    }
  }
  return out;
}

tl::expected<std::string, std::string> UriDecode(std::string_view in)
{
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '%') {
      out.push_back(in[i]);
      continue;
    }
    if (i + 2 >= in.size()) {
      return tl::make_unexpected(std::string("truncated percent escape"));
    }
    int hi = HexValue(in[i + 1]);
    int lo = HexValue(in[i + 2]);
    if (hi < 0 || lo < 0) {
      return tl::make_unexpected(std::string("malformed percent escape"));
    }
    out.push_back(static_cast<char>(hi * 16 + lo));
    i += 2;
  }
  return out;
}

std::string CanonicalQueryString(const NameValueList& query)
{
  NameValueList encoded;
  encoded.reserve(query.size());
  for (const auto& [name, value] : query) {
    encoded.emplace_back(UriEncode(name, true), UriEncode(value, true));
  }
  std::sort(encoded.begin(), encoded.end());
  std::string out;
  for (const auto& [name, value] : encoded) {
    if (!out.empty()) { out.push_back('&'); }
    out += name;
    out.push_back('=');
    out += value;
  }
  return out;
}

tl::expected<std::string, std::string> CanonicalRequest(
    const SigV4Request& request,
    std::string& signed_headers)
{
  if (request.method.empty()) {
    return tl::make_unexpected(std::string("empty request method"));
  }
  if (request.payload_hash.empty()) {
    return tl::make_unexpected(std::string("empty payload hash"));
  }
  if (!request.path.empty() && request.path[0] != '/') {
    return tl::make_unexpected(std::string("path must start with '/'"));
  }

  // Header names are lower-cased and sorted; the values of one name stay in
  // the order given and are joined by commas.
  std::map<std::string, std::string> headers;
  for (const auto& [name, value] : request.headers) {
    if (name.empty() || name.find_first_of(" \t\r\n:") != std::string::npos) {
      return tl::make_unexpected(std::string("invalid header name"));
    }
    if (value.find_first_of("\r\n") != std::string::npos) {
      return tl::make_unexpected(std::string("line break in a header value"));
    }
    auto [it, inserted] = headers.try_emplace(Lower(name));
    if (!inserted) { it->second.push_back(','); }
    it->second += TrimAndCollapse(value);
  }
  if (headers.find("host") == headers.end()) {
    return tl::make_unexpected(std::string("the host header is not signed"));
  }

  std::string canonical_headers;
  signed_headers.clear();
  for (const auto& [name, value] : headers) {
    canonical_headers += name;
    canonical_headers.push_back(':');
    canonical_headers += value;
    canonical_headers.push_back('\n');
    if (!signed_headers.empty()) { signed_headers.push_back(';'); }
    signed_headers += name;
  }

  std::string out = request.method;
  out.push_back('\n');
  out += UriEncode(request.path.empty() ? "/" : request.path, false);
  out.push_back('\n');
  out += CanonicalQueryString(request.query);
  out.push_back('\n');
  out += canonical_headers;
  out.push_back('\n');
  out += signed_headers;
  out.push_back('\n');
  out += request.payload_hash;
  return out;
}

std::string SigningKey(std::string_view secret_key,
                       std::string_view date,
                       std::string_view region,
                       std::string_view service)
{
  std::string seed = "AWS4";
  seed += secret_key;
  std::string k_date = HmacSha256(seed, date);
  Cleanse(seed);
  std::string k_region = HmacSha256(k_date, region);
  Cleanse(k_date);
  std::string k_service = HmacSha256(k_region, service);
  Cleanse(k_region);
  std::string k_signing = HmacSha256(k_service, "aws4_request");
  Cleanse(k_service);
  return k_signing;
}

tl::expected<SigV4Result, std::string> Sign(const SigV4Credentials& credentials,
                                            const SigV4Request& request,
                                            std::string_view amz_date)
{
  if (credentials.access_key.empty() || credentials.secret_key.empty()) {
    return tl::make_unexpected(std::string("missing access or secret key"));
  }
  if (credentials.region.empty() || credentials.service.empty()) {
    return tl::make_unexpected(std::string("missing region or service"));
  }
  if (!ValidAmzDate(amz_date)) {
    return tl::make_unexpected(
        std::string("invalid date, need YYYYMMDDTHHMMSSZ"));
  }

  SigV4Result result;
  auto canonical = CanonicalRequest(request, result.signed_headers);
  if (!canonical) { return tl::make_unexpected(canonical.error()); }
  result.canonical_request = std::move(*canonical);

  std::string_view date = amz_date.substr(0, 8);
  std::string scope(date);
  scope
      += '/' + credentials.region + '/' + credentials.service + "/aws4_request";

  result.string_to_sign = std::string(kAlgorithm) + '\n';
  result.string_to_sign += amz_date;
  result.string_to_sign
      += '\n' + scope + '\n' + Sha256Hex(result.canonical_request);

  std::string key = SigningKey(credentials.secret_key, date, credentials.region,
                               credentials.service);
  result.signature = HexEncode(HmacSha256(key, result.string_to_sign));
  Cleanse(key);

  result.authorization = std::string(kAlgorithm)
                         + " Credential=" + credentials.access_key + '/' + scope
                         + ", SignedHeaders=" + result.signed_headers
                         + ", Signature=" + result.signature;
  return result;
}

void AddS3SigningHeaders(SigV4Request& request, std::string_view amz_date)
{
  auto has = [&request](std::string_view name) {
    return std::any_of(request.headers.begin(), request.headers.end(),
                       [&](const auto& h) { return Lower(h.first) == name; });
  };
  if (!has("x-amz-content-sha256")) {
    request.headers.emplace_back("x-amz-content-sha256", request.payload_hash);
  }
  if (!has("x-amz-date")) {
    request.headers.emplace_back("x-amz-date", std::string(amz_date));
  }
}

std::string FormatAmzDate(std::time_t time)
{
  std::tm tm{};
  gmtime_r(&time, &tm);
  char buf[32];
  size_t n = std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
  return std::string(buf, n);
}

S3Target MakeS3Target(std::string_view endpoint,
                      std::string_view bucket,
                      std::string_view key,
                      bool path_style)
{
  S3Target target;
  if (path_style) {
    target.host = std::string(endpoint);
    target.path = "/" + std::string(bucket);
    if (!key.empty()) { target.path += "/" + std::string(key); }
  } else {
    target.host = std::string(bucket) + "." + std::string(endpoint);
    target.path = "/" + std::string(key);
  }
  return target;
}

}  // namespace storagedaemon::s3
