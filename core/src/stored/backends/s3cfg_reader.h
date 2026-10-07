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

#ifndef BAREOS_STORED_BACKENDS_S3CFG_READER_H_
#define BAREOS_STORED_BACKENDS_S3CFG_READER_H_

#include <string>
#include <string_view>
#include "tl/expected.hpp"

// Reader for the [default] section of an s3cmd configuration file. Error
// messages name keys and line numbers, never values; the secret key is
// wiped from memory when the object goes away.
namespace storagedaemon::s3 {

class S3Config {
 public:
  S3Config() = default;
  S3Config(const S3Config&) = default;
  S3Config& operator=(const S3Config&) = default;
  ~S3Config();

  std::string access_key;
  std::string secret_key;
  std::string host_base{"s3.amazonaws.com"};
  std::string host_bucket{"%(bucket)s.s3.amazonaws.com"};
  bool use_https{true};
  std::string bucket_location{"US"};
  std::string ca_certs_file;
  bool check_ssl_certificate{true};
  // Set by ReadS3cfgFile: the file can be read by "other".
  bool world_readable{false};

  bool HasCredentials() const
  { return !access_key.empty() && !secret_key.empty(); }
  // Signing region: bucket_location, with s3cmd's "US" and "EU" mapped to
  // their AWS names.
  std::string Region() const;
  // True when host_bucket has no "%(bucket)s": the bucket goes in the path.
  bool PathStyle() const;
};

tl::expected<S3Config, std::string> ParseS3cfg(std::string_view text);
tl::expected<S3Config, std::string> ReadS3cfgFile(const std::string& path);

}  // namespace storagedaemon::s3

#endif  // BAREOS_STORED_BACKENDS_S3CFG_READER_H_
