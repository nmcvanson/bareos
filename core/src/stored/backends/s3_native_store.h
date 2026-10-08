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

#ifndef BAREOS_STORED_BACKENDS_S3_NATIVE_STORE_H_
#define BAREOS_STORED_BACKENDS_S3_NATIVE_STORE_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "stored/backends/object_store.h"
#include "stored/backends/s3_sigv4.h"

/* ObjectStore on S3 over libcurl with SigV4; <obj_name>/<obj_part> is the key
 * [prefix/]<obj_name>/<obj_part>. Credentials come only from the s3cfg file;
 * after set_option() all operations may run on several threads at once. */
class S3NativeStore final : public ObjectStore {
 public:
  S3NativeStore();
  ~S3NativeStore() override;
  S3NativeStore(const S3NativeStore&) = delete;
  S3NativeStore& operator=(const S3NativeStore&) = delete;

  tl::expected<BStringList, StoreError> get_supported_options() override;
  tl::expected<void, StoreError> set_option(const std::string& name,
                                            const std::string& value) override;

  tl::expected<void, StoreError> test_connection() override;
  tl::expected<ObjectStat, StoreError> stat(std::string_view obj_name,
                                            std::string_view obj_part) override;
  tl::expected<ObjectList, StoreError> list(std::string_view obj_name) override;
  tl::expected<void, StoreError> upload(std::string_view obj_name,
                                        std::string_view obj_part,
                                        gsl::span<char> obj_data) override;
  tl::expected<gsl::span<char>, StoreError> download(
      std::string_view obj_name,
      std::string_view obj_part,
      gsl::span<char> buffer,
      std::optional<ByteRange> range) override;
  tl::expected<void, StoreError> remove(std::string_view obj_name,
                                        std::string_view obj_part) override;
  bool supports_range_download() const override { return true; }
  // Ignored once the first operation has run, so readers need no lock.
  void set_abort_check(std::function<bool()> check) override
  {
    std::lock_guard lock(m_mutex);
    if (!m_options_frozen) { m_abort_check = std::move(check); }
  }

  // HTTP requests sent so far, retries included.
  uint64_t requests_sent() const { return m_requests_sent; }
  // Pause before the first retry, doubled for each further one (for tests).
  void set_retry_base(std::chrono::milliseconds base) { m_retry_base = base; }
  // How long a canceled job waits for the reply of a fully sent upload.
  void set_abort_reply_grace(std::chrono::milliseconds grace)
  { m_abort_reply_grace = grace; }

 private:
  // Longest wait of a canceled job for the reply of a fully sent upload.
  static constexpr std::chrono::milliseconds kAbortReplyGrace{10000};

  struct Options {
    std::string s3cfg;
    std::string bucket;
    std::string prefix;
    std::string storage_class;
    std::string endpoint;
    std::string region;
    std::string path_style{"auto"};
    std::string ca_file;
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds stall_timeout{120};
    int request_retries{5};
    bool content_md5{true};
  };
  struct Request;
  struct Reply;
  struct HandlePool;

  tl::expected<void, StoreError> EnsureReady();
  tl::expected<Reply, StoreError> Execute(const Request& request);
  tl::expected<Reply, StoreError> PerformOnce(const Request& request);
  std::string KeyOf(std::string_view obj_name, std::string_view obj_part) const;

  Options m_options;
  std::mutex m_mutex;
  bool m_ready{false};
  bool m_options_frozen{false};

  // Set by EnsureReady(), read-only afterwards.
  storagedaemon::s3::SigV4Credentials m_credentials;
  std::string m_host;
  bool m_https{true};
  bool m_path_style{true};
  bool m_verify_peer{true};
  std::string m_ca_file;
  std::string m_key_prefix;

  std::function<bool()> m_abort_check;
  std::chrono::milliseconds m_retry_base{1000};
  std::chrono::milliseconds m_abort_reply_grace{kAbortReplyGrace};
  std::atomic<uint64_t> m_requests_sent{0};
  std::unique_ptr<HandlePool> m_pool;
};

#endif  // BAREOS_STORED_BACKENDS_S3_NATIVE_STORE_H_
