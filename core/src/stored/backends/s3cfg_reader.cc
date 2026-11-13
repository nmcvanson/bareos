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

#include "stored/backends/s3cfg_reader.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <openssl/crypto.h>
#include <sys/stat.h>
#include <unistd.h>

namespace storagedaemon::s3 {

namespace {

using Error = tl::unexpected<std::string>;

constexpr size_t kMaxFileSize = 1024 * 1024;

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view Trim(std::string_view s)
{
  while (!s.empty() && IsSpace(s.front())) { s.remove_prefix(1); }
  while (!s.empty() && IsSpace(s.back())) { s.remove_suffix(1); }
  return s;
}

std::string Lower(std::string_view in)
{
  std::string out(in);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
  }
  return out;
}

// s3cmd's spelling of booleans, case-insensitive.
bool ParseBool(std::string_view value, bool& out)
{
  std::string v = Lower(value);
  if (v == "true" || v == "yes" || v == "on" || v == "1") {
    out = true;
    return true;
  }
  if (v == "false" || v == "no" || v == "off" || v == "0") {
    out = false;
    return true;
  }
  return false;
}

}  // namespace

S3Config::~S3Config()
{
  if (!secret_key.empty()) {
    OPENSSL_cleanse(secret_key.data(), secret_key.size());
  }
  if (!access_key.empty()) {
    OPENSSL_cleanse(access_key.data(), access_key.size());
  }
}

std::string S3Config::Region() const
{
  if (bucket_location.empty() || bucket_location == "US") {
    return "us-east-1";
  }
  if (bucket_location == "EU") { return "eu-west-1"; }
  return bucket_location;
}

bool S3Config::PathStyle() const
{ return host_bucket.find("%(bucket)s") == std::string::npos; }

tl::expected<S3Config, std::string> ParseS3cfg(std::string_view text)
{
  if (text.substr(0, 3) == "\xEF\xBB\xBF") { text.remove_prefix(3); }

  S3Config config;
  // Keys before any section header count as [default].
  bool in_default = true;
  size_t line_number = 0;

  while (!text.empty()) {
    ++line_number;
    size_t eol = text.find('\n');
    std::string_view line = Trim(text.substr(0, eol));
    text = eol == std::string_view::npos ? std::string_view{}
                                         : text.substr(eol + 1);

    if (line.empty() || line[0] == '#' || line[0] == ';') { continue; }

    if (line[0] == '[') {
      size_t close = line.find(']');
      if (close == std::string_view::npos) {
        return Error("line " + std::to_string(line_number)
                     + ": unterminated section header");
      }
      in_default = Lower(Trim(line.substr(1, close - 1))) == "default";
      continue;
    }
    if (!in_default) { continue; }

    size_t delim = line.find_first_of("=:");
    if (delim == std::string_view::npos) {
      return Error("line " + std::to_string(line_number)
                   + ": expected 'key = value'");
    }
    std::string key = Lower(Trim(line.substr(0, delim)));
    std::string_view value = Trim(line.substr(delim + 1));
    std::string where = "line " + std::to_string(line_number) + ": ";

    if (key == "access_key") {
      config.access_key = std::string(value);
    } else if (key == "secret_key") {
      config.secret_key = std::string(value);
    } else if (key == "host_base" || key == "host_bucket") {
      if (value.empty()) { return Error(where + "empty " + key); }
      (key == "host_base" ? config.host_base : config.host_bucket)
          = std::string(value);
    } else if (key == "bucket_location") {
      config.bucket_location = std::string(value);
    } else if (key == "ca_certs_file") {
      config.ca_certs_file = std::string(value);
    } else if (key == "use_https" || key == "check_ssl_certificate") {
      bool& target = key == "use_https" ? config.use_https
                                        : config.check_ssl_certificate;
      if (!ParseBool(value, target)) {
        return Error(where + key + " is not a boolean");
      }
    }
  }
  return config;
}

tl::expected<S3Config, std::string> ReadS3cfgFile(const std::string& path)
{
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return Error("cannot open s3cfg file " + path + ": " + strerror(errno));
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return Error("s3cfg file " + path + " is not a regular file");
  }
  if (static_cast<size_t>(st.st_size) > kMaxFileSize) {
    close(fd);
    return Error("s3cfg file " + path + " is too large");
  }

  std::string content;
  content.reserve(static_cast<size_t>(st.st_size));
  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    content.append(buf, static_cast<size_t>(n));
    if (content.size() > kMaxFileSize) { break; }
  }
  bool read_failed = n < 0;
  close(fd);
  OPENSSL_cleanse(buf, sizeof(buf));
  if (read_failed || content.size() > kMaxFileSize) {
    if (!content.empty()) { OPENSSL_cleanse(content.data(), content.size()); }
    return Error("cannot read s3cfg file " + path);
  }

  auto config = ParseS3cfg(content);
  if (!content.empty()) { OPENSSL_cleanse(content.data(), content.size()); }
  if (!config) { return Error("s3cfg file " + path + ": " + config.error()); }
  config->world_readable = (st.st_mode & S_IROTH) != 0;
  return config;
}

}  // namespace storagedaemon::s3
