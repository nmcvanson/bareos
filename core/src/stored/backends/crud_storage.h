/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2026 Bareos GmbH & Co. KG

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

#ifndef BAREOS_STORED_BACKENDS_CRUD_STORAGE_H_
#define BAREOS_STORED_BACKENDS_CRUD_STORAGE_H_

#include <map>
#include <string_view>
#include <unordered_map>
#include <gsl/span>
#include <chrono>
#include "lib/bstringlist.h"
#include "object_store.h"
#include "tl/expected.hpp"

// The program transport: every operation runs an external program (s3cmd).
class CrudStorage : public ObjectStore {
  std::string m_program{"/bin/false"};
  std::chrono::seconds m_program_timeout{30};
  std::unordered_map<std::string, std::string> m_env_vars{};

 public:
  tl::expected<void, std::string> set_program(const std::string& program);
  void set_program_timeout(std::chrono::seconds timeout);
  tl::expected<BStringList, StoreError> get_supported_options() override;
  tl::expected<void, StoreError> set_option(const std::string& name,
                                            const std::string& value) override;
  tl::expected<void, StoreError> test_connection() override;
  tl::expected<ObjectStat, StoreError> stat(std::string_view obj_name,
                                            std::string_view obj_part) override;
  tl::expected<ObjectList, StoreError> list(std::string_view obj_name) override;
  // Parses "<name> <size>" lines; an empty output is an empty listing.
  static tl::expected<ObjectList, std::string> parse_list_output(
      std::string_view output);
  tl::expected<void, StoreError> upload(std::string_view obj_name,
                                        std::string_view obj_part,
                                        gsl::span<char> obj_data) override;
  // The program cannot read a range: a range is refused with kConfig.
  tl::expected<gsl::span<char>, StoreError> download(
      std::string_view obj_name,
      std::string_view obj_part,
      gsl::span<char> buffer,
      std::optional<ByteRange> range) override;
  tl::expected<void, StoreError> remove(std::string_view obj_name,
                                        std::string_view obj_part) override;
};
#endif  // BAREOS_STORED_BACKENDS_CRUD_STORAGE_H_
