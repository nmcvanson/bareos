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

#ifndef BAREOS_STORED_BACKENDS_OBJECT_STORE_H_
#define BAREOS_STORED_BACKENDS_OBJECT_STORE_H_

#include <cstddef>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <gsl/span>
#include "lib/bstringlist.h"
#include "tl/expected.hpp"

// What went wrong in a store operation, for the caller's retry decision.
enum class StoreErrc
{
  kNotFound,   // the object does not exist
  kTransient,  // may succeed when tried again
  kPermanent,  // will fail again, e.g. access denied
  kTimeout,    // the operation was stopped for taking too long
  kConfig,     // the transport is set up wrongly or lacks the feature
};

struct StoreError {
  StoreErrc code;
  std::string message;
};

inline std::ostream& operator<<(std::ostream& os, const StoreError& error)
{ return os << error.message; }

struct ObjectStat {
  size_t size{0};
};

// Bytes [offset, offset + length) of an object.
struct ByteRange {
  size_t offset{0};
  size_t length{0};
};

/* The operations dplcompat needs from an S3-like object store; an object is
 * <obj_name>/<obj_part>, e.g. volume and chunk. Implementations are called
 * from the job thread and from the io-threads at the same time. */
class ObjectStore {
 public:
  using ObjectList = std::map<std::string, ObjectStat>;

  virtual ~ObjectStore() = default;

  // Names of the transport options that set_option() accepts.
  virtual tl::expected<BStringList, StoreError> get_supported_options() = 0;
  virtual tl::expected<void, StoreError> set_option(const std::string& name,
                                                    const std::string& value)
      = 0;

  virtual tl::expected<void, StoreError> test_connection() = 0;
  virtual tl::expected<ObjectStat, StoreError> stat(std::string_view obj_name,
                                                    std::string_view obj_part)
      = 0;
  // Parts of obj_name by part name.
  virtual tl::expected<ObjectList, StoreError> list(std::string_view obj_name)
      = 0;
  virtual tl::expected<void, StoreError> upload(std::string_view obj_name,
                                                std::string_view obj_part,
                                                gsl::span<char> obj_data) = 0;
  /* Fills buffer with the whole object, or with the bytes of range when one
   * is given and supports_range_download() is true. */
  virtual tl::expected<gsl::span<char>, StoreError> download(
      std::string_view obj_name,
      std::string_view obj_part,
      gsl::span<char> buffer,
      std::optional<ByteRange> range) = 0;
  virtual tl::expected<void, StoreError> remove(std::string_view obj_name,
                                                std::string_view obj_part) = 0;

  virtual bool supports_range_download() const { return false; }
};

#endif  // BAREOS_STORED_BACKENDS_OBJECT_STORE_H_
