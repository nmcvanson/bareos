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

#ifndef BAREOS_STORED_BACKENDS_S3_XML_H_
#define BAREOS_STORED_BACKENDS_S3_XML_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "tl/expected.hpp"

// Readers for the two XML bodies the S3 transport needs. They ignore
// namespaces and attributes and are strict about structure: a body that is
// cut off or not well-formed is an error, never a partial result.
namespace storagedaemon::s3 {

struct ListedObject {
  std::string key;
  uint64_t size{0};
};

struct ListObjectsResult {
  std::vector<ListedObject> objects;
  bool is_truncated{false};
  // Set when is_truncated is true.
  std::string next_continuation_token;
};

struct S3Error {
  std::string code;
  std::string message;
  std::string request_id;
};

// Parses a ListObjectsV2 response (ListBucketResult). Keys are url-decoded
// when the response says EncodingType url. A truncated listing without a
// continuation token is an error.
tl::expected<ListObjectsResult, std::string> ParseListObjectsV2(
    std::string_view xml);

// Parses an S3 error response (Error). Code is required.
tl::expected<S3Error, std::string> ParseS3Error(std::string_view xml);

}  // namespace storagedaemon::s3

#endif  // BAREOS_STORED_BACKENDS_S3_XML_H_
