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

#include "include/bareos.h"
#include "stored/backends/s3_native_store.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <fmt/format.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

#include "stored/backends/s3_xml.h"
#include "stored/backends/s3cfg_reader.h"
#include "stored/backends/util.h"

namespace utl = backends::util;
namespace s3 = storagedaemon::s3;

namespace {

constexpr int debug_info = 110;
constexpr int debug_trace = 130;

// Largest XML body read (listing page) and largest error body kept.
constexpr size_t kMaxTextBody = 16 * 1024 * 1024;
constexpr size_t kMaxErrorBody = 64 * 1024;
constexpr size_t kMaxIdleHandles = 64;
constexpr std::chrono::seconds kMaxRetryPause{60};
constexpr std::chrono::milliseconds kPauseSlice{100};
// A canceled job ends a request with no byte moving after this long (an
// upload that is fully sent gets m_abort_reply_grace instead).
constexpr std::chrono::seconds kAbortIdleGrace{1};
constexpr char kEmptySha256[]
    = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

tl::unexpected<StoreError> Fail(StoreErrc code, std::string message)
{ return tl::unexpected(StoreError{code, std::move(message)}); }

bool ParseInt(const std::string& text, int min, int max, int& out)
{
  int value = 0;
  auto [end, ec]
      = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() || value < min
      || value > max) {
    return false;
  }
  out = value;
  return true;
}

bool ParseYesNo(const std::string& text, bool& out)
{
  std::string v = text;
  for (char& c : v) {
    if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
  }
  if (v == "yes" || v == "true" || v == "on" || v == "1") {
    out = true;
    return true;
  }
  if (v == "no" || v == "false" || v == "off" || v == "0") {
    out = false;
    return true;
  }
  return false;
}

std::string Trimmed(const std::string& s, char c)
{
  size_t b = s.find_first_not_of(c);
  if (b == std::string::npos) { return {}; }
  return s.substr(b, s.find_last_not_of(c) - b + 1);
}

// Base64 of the MD5 of data, for the Content-MD5 header.
tl::expected<std::string, StoreError> Md5Base64(gsl::span<const char> data)
{
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (EVP_Digest(data.data(), data.size(), md, &md_len, EVP_md5(), nullptr)
      != 1) {
    return Fail(StoreErrc::kConfig,
                "MD5 is not available in this OpenSSL (FIPS mode?); set "
                "content_md5=no");
  }
  unsigned char out[EVP_MAX_MD_SIZE * 2];
  int n = EVP_EncodeBlock(out, md, static_cast<int>(md_len));
  return std::string(reinterpret_cast<char*>(out), static_cast<size_t>(n));
}

// A DNS label: what a bucket must be to appear in a host name.
bool ValidHostLabel(const std::string& s)
{
  if (s.empty() || s.size() > 63 || s.front() == '-' || s.back() == '-') {
    return false;
  }
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

struct CurlHeaders {
  curl_slist* list{nullptr};
  ~CurlHeaders() { curl_slist_free_all(list); }
  void Add(const std::string& line)
  {
    curl_slist* next = curl_slist_append(list, line.c_str());
    if (next) { list = next; }
  }
};

void GlobalInit()
{
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

}  // namespace

struct S3NativeStore::HandlePool {
  std::mutex mutex;
  std::vector<CURL*> idle;

  ~HandlePool()
  {
    for (CURL* h : idle) { curl_easy_cleanup(h); }
  }
  CURL* Acquire()
  {
    {
      std::lock_guard lock(mutex);
      if (!idle.empty()) {
        CURL* h = idle.back();
        idle.pop_back();
        return h;
      }
    }
    return curl_easy_init();
  }
  // A handle with a failed transfer is dropped, with its connection.
  void Release(CURL* h, bool reuse)
  {
    if (reuse) {
      curl_easy_reset(h);
      std::lock_guard lock(mutex);
      if (idle.size() < kMaxIdleHandles) {
        idle.push_back(h);
        return;
      }
    }
    curl_easy_cleanup(h);
  }
};

struct S3NativeStore::Request {
  const char* method{"GET"};
  // Key inside the bucket; empty addresses the bucket itself.
  std::string key;
  s3::NameValueList query;
  // Extra headers, signed with the request.
  s3::NameValueList headers;
  bool head{false};
  bool upload{false};
  const char* upload_data{nullptr};
  size_t upload_size{0};
  // SHA-256 of the upload, used when the connection is not TLS.
  std::string payload_hash;
  // Response bytes go to the buffer if to_buffer, else into Reply::body.
  bool to_buffer{false};
  char* out_buffer{nullptr};
  size_t out_capacity{0};
};

struct S3NativeStore::Reply {
  CURLcode curl_code{CURLE_OK};
  std::string curl_message;
  long status{0};
  std::string body;
  size_t received{0};
  curl_off_t content_length{-1};
  bool overflow{false};
  bool stalled{false};
  bool canceled{false};
};

namespace {

struct WriteState {
  CURL* handle;
  bool to_buffer;
  char* buffer;
  size_t capacity;
  std::string* body;
  size_t used{0};
  long status{0};
  bool overflow{false};
};

size_t WriteCallback(char* data, size_t size, size_t count, void* userdata)
{
  auto* ws = static_cast<WriteState*>(userdata);
  size_t length = size * count;
  if (ws->status == 0) {
    curl_easy_getinfo(ws->handle, CURLINFO_RESPONSE_CODE, &ws->status);
  }
  bool success = ws->status / 100 == 2;
  if (success && ws->to_buffer) {
    if (ws->used + length > ws->capacity) {
      ws->overflow = true;
      return 0;
    }
    std::memcpy(ws->buffer + ws->used, data, length);
    ws->used += length;
  } else if (success) {
    if (ws->body->size() + length > kMaxTextBody) {
      ws->overflow = true;
      return 0;
    }
    ws->body->append(data, length);
  } else if (ws->body->size() < kMaxErrorBody) {
    ws->body->append(data, std::min(length, kMaxErrorBody - ws->body->size()));
  }
  return length;
}

struct ReadState {
  const char* data;
  size_t size;
  size_t position{0};
};

size_t ReadCallback(char* out, size_t size, size_t count, void* userdata)
{
  auto* rs = static_cast<ReadState*>(userdata);
  size_t n = std::min(size * count, rs->size - rs->position);
  if (n > 0) { std::memcpy(out, rs->data + rs->position, n); }
  rs->position += n;
  return n;
}

// Stops a transfer in which no byte moved, in either direction, for limit.
struct ProgressState {
  std::chrono::steady_clock::time_point last_change{
      std::chrono::steady_clock::now()};
  curl_off_t last_bytes{-1};
  std::chrono::seconds limit;
  bool stalled{false};
  // Ends the transfer when it returns true (the job was canceled).
  const std::function<bool()>* abort_check{nullptr};
  curl_off_t upload_size{0};
  std::chrono::milliseconds reply_grace{0};
  bool canceled{false};
};

int ProgressCallback(void* userdata,
                     curl_off_t,
                     curl_off_t downloaded,
                     curl_off_t,
                     curl_off_t uploaded)
{
  auto* ps = static_cast<ProgressState*>(userdata);
  auto now = std::chrono::steady_clock::now();
  curl_off_t moved = downloaded + uploaded;
  const bool idle = moved == ps->last_bytes;
  if (!idle) {
    ps->last_bytes = moved;
    ps->last_change = now;
  }
  // A canceled job ends a request that has moved no byte for a moment; a
  // transfer that is still moving finishes. After the whole upload is sent
  // the drain of the socket and the server's reply get the reply grace.
  const bool body_sent = ps->upload_size > 0 && uploaded >= ps->upload_size;
  const std::chrono::milliseconds grace
      = body_sent ? ps->reply_grace
                  : std::chrono::duration_cast<std::chrono::milliseconds>(
                        kAbortIdleGrace);
  if (idle && now - ps->last_change >= grace && ps->abort_check
      && *ps->abort_check && (*ps->abort_check)()) {
    ps->canceled = true;
    return 1;
  }
  if (idle && now - ps->last_change >= ps->limit) {
    ps->stalled = true;
    return 1;
  }
  return 0;
}

enum class Outcome
{
  kOk,
  kRetry,
  kFail
};

bool IsCertificateProblem(CURLcode code)
{
  return code == CURLE_PEER_FAILED_VERIFICATION
         || code == CURLE_SSL_CACERT_BADFILE || code == CURLE_SSL_CERTPROBLEM
         || code == CURLE_SSL_ISSUER_ERROR || code == CURLE_SSL_CRL_BADFILE;
}

std::string Shorten(std::string text)
{
  if (text.size() > 300) { text.resize(300); }
  return text;
}

}  // namespace

S3NativeStore::S3NativeStore() : m_pool(std::make_unique<HandlePool>())
{ GlobalInit(); }

S3NativeStore::~S3NativeStore()
{
  if (!m_credentials.secret_key.empty()) {
    OPENSSL_cleanse(m_credentials.secret_key.data(),
                    m_credentials.secret_key.size());
  }
  if (!m_credentials.access_key.empty()) {
    OPENSSL_cleanse(m_credentials.access_key.data(),
                    m_credentials.access_key.size());
  }
}

tl::expected<BStringList, StoreError> S3NativeStore::get_supported_options()
{
  BStringList options;
  for (const char* name :
       {"s3cfg", "bucket", "prefix", "storage_class", "endpoint", "region",
        "path_style", "ca_file", "connect_timeout", "stall_timeout",
        "request_retries", "content_md5"}) {
    options << name;
  }
  return options;
}

tl::expected<void, StoreError> S3NativeStore::set_option(
    const std::string& name,
    const std::string& value)
{
  std::lock_guard lock(m_mutex);
  if (m_options_frozen) {
    return Fail(StoreErrc::kConfig,
                fmt::format("option {} set after the first use of the "
                            "transport",
                            name));
  }
  auto bad = [&name](const char* what) {
    return Fail(StoreErrc::kConfig, fmt::format("option {}: {}", name, what));
  };
  int number = 0;
  if (name == "s3cfg") {
    if (value.empty()) { return bad("empty path"); }
    m_options.s3cfg = value;
  } else if (name == "bucket") {
    if (value.empty() || value.find_first_of("/ \t\r\n") != std::string::npos) {
      return bad("not a bucket name");
    }
    m_options.bucket = value;
  } else if (name == "prefix") {
    m_options.prefix = Trimmed(value, '/');
  } else if (name == "storage_class") {
    if (value.find_first_of("\r\n") != std::string::npos) {
      return bad("not a storage class");
    }
    m_options.storage_class = value;
  } else if (name == "endpoint") {
    if (value.empty() || value.find_first_of("/ \t\r\n") != std::string::npos) {
      return bad("expected host or host:port");
    }
    m_options.endpoint = value;
  } else if (name == "region") {
    if (value.find_first_of("/ \t\r\n,") != std::string::npos) {
      return bad("not a region name");
    }
    m_options.region = value;
  } else if (name == "path_style") {
    bool unused;
    if (value != "auto" && !ParseYesNo(value, unused)) {
      return bad("expected yes, no or auto");
    }
    m_options.path_style = value;
  } else if (name == "ca_file") {
    m_options.ca_file = value;
  } else if (name == "connect_timeout") {
    if (!ParseInt(value, 1, 600, number)) { return bad("expected 1 to 600"); }
    m_options.connect_timeout = std::chrono::seconds(number);
  } else if (name == "stall_timeout") {
    if (!ParseInt(value, 1, 3600, number)) { return bad("expected 1 to 3600"); }
    m_options.stall_timeout = std::chrono::seconds(number);
  } else if (name == "request_retries") {
    if (!ParseInt(value, 0, 20, number)) { return bad("expected 0 to 20"); }
    m_options.request_retries = number;
  } else if (name == "content_md5") {
    if (!ParseYesNo(value, m_options.content_md5)) {
      return bad("expected yes or no");
    }
  } else {
    return Fail(StoreErrc::kConfig,
                fmt::format("unknown transport option {}", name));
  }
  return {};
}

tl::expected<void, StoreError> S3NativeStore::EnsureReady()
{
  std::lock_guard lock(m_mutex);
  m_options_frozen = true;
  if (m_ready) { return {}; }

  if (m_options.s3cfg.empty()) {
    return Fail(StoreErrc::kConfig, "option s3cfg is required");
  }
  if (m_options.bucket.empty()) {
    return Fail(StoreErrc::kConfig, "option bucket is required");
  }
  auto config = s3::ReadS3cfgFile(m_options.s3cfg);
  if (!config) { return Fail(StoreErrc::kConfig, config.error()); }
  if (!config->HasCredentials()) {
    return Fail(StoreErrc::kConfig, fmt::format("s3cfg file {} has no "
                                                "access_key or secret_key",
                                                m_options.s3cfg));
  }
  if (config->world_readable) {
    Emsg0(M_WARNING, 0,
          "dplcompat: s3cfg file %s can be read by other users, it holds the "
          "secret key\n",
          m_options.s3cfg.c_str());
  }

  bool path_style = config->PathStyle();
  if (m_options.path_style != "auto") {
    ParseYesNo(m_options.path_style, path_style);
  }
  if (!path_style && !ValidHostLabel(m_options.bucket)) {
    return Fail(StoreErrc::kConfig,
                "the bucket name cannot be used in a host name; use "
                "path_style=yes");
  }

  m_host = m_options.endpoint.empty() ? config->host_base : m_options.endpoint;
  m_https = config->use_https;
  m_verify_peer = config->check_ssl_certificate;
  m_ca_file
      = m_options.ca_file.empty() ? config->ca_certs_file : m_options.ca_file;
  m_path_style = path_style;
  m_key_prefix
      = m_options.prefix.empty() ? std::string{} : m_options.prefix + "/";
  m_credentials.access_key = std::move(config->access_key);
  m_credentials.secret_key = std::move(config->secret_key);
  m_credentials.region
      = m_options.region.empty() ? config->Region() : m_options.region;
  m_credentials.service = "s3";
  if (!m_verify_peer && m_https) {
    Emsg0(M_WARNING, 0,
          "dplcompat: certificate checks are off (check_ssl_certificate in "
          "%s)\n",
          m_options.s3cfg.c_str());
  }
  m_ready = true;
  return {};
}

std::string S3NativeStore::KeyOf(std::string_view obj_name,
                                 std::string_view obj_part) const
{
  std::string key = m_key_prefix;
  key += obj_name;
  key += '/';
  key += obj_part;
  return key;
}

tl::expected<S3NativeStore::Reply, StoreError> S3NativeStore::PerformOnce(
    const Request& request)
{
  s3::S3Target target
      = s3::MakeS3Target(m_host, m_options.bucket, request.key, m_path_style);

  s3::SigV4Request sr;
  sr.method = request.method;
  sr.path = target.path;
  sr.query = request.query;
  sr.headers.emplace_back("Host", target.host);
  for (const auto& header : request.headers) { sr.headers.push_back(header); }
  if (!request.upload) {
    sr.payload_hash = kEmptySha256;
  } else if (m_https) {
    sr.payload_hash = std::string(s3::kUnsignedPayload);
  } else {
    sr.payload_hash = request.payload_hash;
  }
  std::string date = s3::FormatAmzDate(std::time(nullptr));
  s3::AddS3SigningHeaders(sr, date);
  auto signature = s3::Sign(m_credentials, sr, date);
  if (!signature || signature->signature.size() != 64) {
    return Fail(StoreErrc::kPermanent,
                signature ? "signing failed (OpenSSL error)"
                          : "signing failed: " + signature.error());
  }

  std::string url = m_https ? "https://" : "http://";
  url += target.host;
  url += s3::UriEncode(target.path, false);
  if (!request.query.empty()) {
    url += '?';
    url += s3::CanonicalQueryString(request.query);
  }

  CurlHeaders headers;
  for (const auto& [name, value] : sr.headers) {
    headers.Add(name + ": " + value);
  }
  headers.Add("Authorization: " + signature->authorization);
  headers.Add("Expect:");
  headers.Add("Accept:");

  CURL* handle = m_pool->Acquire();
  if (!handle) {
    return Fail(StoreErrc::kTransient, "cannot create an HTTP handle");
  }

  Reply reply;
  char error_text[CURL_ERROR_SIZE] = {0};
  WriteState write_state{handle, request.to_buffer, request.out_buffer,
                         request.out_capacity, &reply.body};
  ReadState read_state{request.upload_data, request.upload_size};
  ProgressState progress_state;
  progress_state.limit = m_options.stall_timeout;
  progress_state.abort_check = &m_abort_check;
  progress_state.upload_size
      = request.upload ? static_cast<curl_off_t>(request.upload_size) : 0;
  progress_state.reply_grace = std::min<std::chrono::milliseconds>(
      m_abort_reply_grace, m_options.stall_timeout);

  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_text);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle, CURLOPT_HTTP_VERSION,
                   static_cast<long>(CURL_HTTP_VERSION_1_1));
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "bareos-sd-dplcompat");
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT,
                   static_cast<long>(m_options.connect_timeout.count()));
  curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
  curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &progress_state);
  curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.list);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &write_state);
  if (m_https) {
    if (!m_ca_file.empty()) {
      curl_easy_setopt(handle, CURLOPT_CAINFO, m_ca_file.c_str());
    }
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, m_verify_peer ? 1L : 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, m_verify_peer ? 2L : 0L);
  }
  if (request.upload) {
    curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(handle, CURLOPT_READFUNCTION, ReadCallback);
    curl_easy_setopt(handle, CURLOPT_READDATA, &read_state);
    curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE,
                     static_cast<curl_off_t>(request.upload_size));
  } else if (request.head) {
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
  } else if (std::strcmp(request.method, "GET") == 0) {
    curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
  } else {
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method);
  }

  ++m_requests_sent;
  reply.curl_code = curl_easy_perform(handle);
  if (reply.curl_code == CURLE_OK) {
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &reply.status);
    curl_easy_getinfo(handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                      &reply.content_length);
  } else {
    reply.curl_message
        = error_text[0] ? error_text : curl_easy_strerror(reply.curl_code);
  }
  reply.received = write_state.used;
  reply.overflow = write_state.overflow;
  reply.stalled = progress_state.stalled;
  reply.canceled = progress_state.canceled;
  m_pool->Release(handle, reply.curl_code == CURLE_OK);

  utl::Dfmt(debug_trace, FMT_STRING("{} {} -> status {} curl {}"),
            request.method, target.path, reply.status,
            static_cast<int>(reply.curl_code));
  return reply;
}

namespace {

// Decides what a reply means; err is set unless the outcome is kOk.
template <typename ReplyType>
Outcome Classify(const ReplyType& reply, StoreError& err)
{
  if (reply.curl_code != CURLE_OK) {
    if (reply.canceled) {
      err = {StoreErrc::kCanceled, "canceled"};
      return Outcome::kFail;
    }
    if (reply.overflow) {
      err = {StoreErrc::kPermanent, "the response is larger than allowed"};
      return Outcome::kFail;
    }
    if (reply.stalled) {
      err = {StoreErrc::kTimeout, "no data moved for the stall timeout"};
      return Outcome::kRetry;
    }
    if (IsCertificateProblem(reply.curl_code)) {
      err = {StoreErrc::kConfig,
             "TLS certificate check failed: " + Shorten(reply.curl_message)};
      return Outcome::kFail;
    }
    if (reply.curl_code == CURLE_URL_MALFORMAT
        || reply.curl_code == CURLE_UNSUPPORTED_PROTOCOL) {
      err = {StoreErrc::kConfig,
             "bad endpoint: " + Shorten(reply.curl_message)};
      return Outcome::kFail;
    }
    err = {reply.curl_code == CURLE_OPERATION_TIMEDOUT ? StoreErrc::kTimeout
                                                       : StoreErrc::kTransient,
           Shorten(reply.curl_message)};
    return Outcome::kRetry;
  }

  long status = reply.status;
  if (status / 100 == 2) { return Outcome::kOk; }

  std::string text = fmt::format("HTTP {}", status);
  std::string code;
  if (auto parsed = s3::ParseS3Error(reply.body)) {
    code = parsed->code;
    text += " " + parsed->code;
    if (!parsed->message.empty()) { text += ": " + parsed->message; }
  }
  text = Shorten(text);

  if (status == 404) {
    err = {StoreErrc::kNotFound, text};
    return Outcome::kFail;
  }
  bool retry_code = code == "BadDigest" || code == "RequestTimeout"
                    || code == "IncompleteBody" || code == "SlowDown"
                    || code == "InternalError" || code == "ServiceUnavailable";
  if (status == 408 || status == 429 || status / 100 == 5
      || (status == 400 && retry_code)) {
    err = {StoreErrc::kTransient, text};
    return Outcome::kRetry;
  }
  if (status / 100 == 3) {
    text += " (redirect: check host_base, the region and the addressing "
            "style)";
  } else if (code == "RequestTimeTooSkewed") {
    text += " (the clock of this host differs from the server's)";
  }
  err = {StoreErrc::kPermanent, text};
  return Outcome::kFail;
}

}  // namespace

tl::expected<S3NativeStore::Reply, StoreError> S3NativeStore::Execute(
    const Request& request)
{
  StoreError last{StoreErrc::kTransient, "no attempt made"};
  const int attempts = m_options.request_retries + 1;
  auto canceled = [this] { return m_abort_check && m_abort_check(); };
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (attempt > 0) {
      // The pause is taken in short slices so that a cancel ends it early.
      auto pause = std::min<std::chrono::milliseconds>(
          m_retry_base * (1LL << std::min(attempt - 1, 20)), kMaxRetryPause);
      while (pause > std::chrono::milliseconds::zero() && !canceled()) {
        auto slice = std::min(pause, kPauseSlice);
        std::this_thread::sleep_for(slice);
        pause -= slice;
      }
    }
    if (canceled()) {
      return tl::unexpected(StoreError{StoreErrc::kCanceled, "canceled"});
    }
    auto reply = PerformOnce(request);
    if (!reply) { return tl::unexpected(reply.error()); }
    StoreError err;
    switch (Classify(*reply, err)) {
      case Outcome::kOk:
        return std::move(*reply);
      case Outcome::kFail:
        return tl::unexpected(std::move(err));
      case Outcome::kRetry:
        utl::Dfmt(debug_info, FMT_STRING("{} {} attempt {} failed: {}"),
                  request.method, request.key, attempt + 1, err.message);
        last = std::move(err);
        break;
    }
  }
  last.message += fmt::format(" (after {} attempts)", attempts);
  return tl::unexpected(std::move(last));
}

tl::expected<void, StoreError> S3NativeStore::test_connection()
{
  if (auto ready = EnsureReady(); !ready) {
    return tl::unexpected(ready.error());
  }
  Request request;
  request.method = "HEAD";
  request.head = true;
  auto reply = Execute(request);
  if (!reply) {
    if (reply.error().code == StoreErrc::kNotFound) {
      return Fail(StoreErrc::kConfig,
                  fmt::format("bucket {} not found", m_options.bucket));
    }
    return tl::unexpected(reply.error());
  }
  return {};
}

tl::expected<ObjectStat, StoreError> S3NativeStore::stat(
    std::string_view obj_name,
    std::string_view obj_part)
{
  if (auto ready = EnsureReady(); !ready) {
    return tl::unexpected(ready.error());
  }
  Request request;
  request.method = "HEAD";
  request.head = true;
  request.key = KeyOf(obj_name, obj_part);
  auto reply = Execute(request);
  if (!reply) { return tl::unexpected(reply.error()); }
  if (reply->content_length < 0) {
    return Fail(StoreErrc::kTransient, "the server sent no Content-Length");
  }
  return ObjectStat{static_cast<size_t>(reply->content_length)};
}

tl::expected<ObjectStore::ObjectList, StoreError> S3NativeStore::list(
    std::string_view obj_name)
{
  if (auto ready = EnsureReady(); !ready) {
    return tl::unexpected(ready.error());
  }
  std::string prefix = m_key_prefix;
  prefix += obj_name;
  prefix += '/';

  ObjectList result;
  std::string token;
  for (int page = 0; page < 1000000; ++page) {
    Request request;
    request.query
        = {{"list-type", "2"}, {"encoding-type", "url"}, {"prefix", prefix}};
    if (!token.empty()) {
      request.query.emplace_back("continuation-token", token);
    }
    auto reply = Execute(request);
    if (!reply) { return tl::unexpected(reply.error()); }
    auto parsed = s3::ParseListObjectsV2(reply->body);
    if (!parsed) {
      return Fail(StoreErrc::kTransient,
                  "unreadable listing: " + Shorten(parsed.error()));
    }
    for (const auto& object : parsed->objects) {
      if (object.key.compare(0, prefix.size(), prefix) != 0
          || object.key.size() == prefix.size()) {
        continue;
      }
      result[object.key.substr(prefix.size())] = ObjectStat{object.size};
    }
    if (!parsed->is_truncated) { return result; }
    if (parsed->next_continuation_token == token) {
      return Fail(StoreErrc::kPermanent,
                  "the server repeats the continuation token of a listing");
    }
    token = parsed->next_continuation_token;
  }
  return Fail(StoreErrc::kPermanent, "listing has too many pages");
}

tl::expected<void, StoreError> S3NativeStore::upload(std::string_view obj_name,
                                                     std::string_view obj_part,
                                                     gsl::span<char> obj_data)
{
  if (auto ready = EnsureReady(); !ready) {
    return tl::unexpected(ready.error());
  }
  Request request;
  request.method = "PUT";
  request.upload = true;
  request.key = KeyOf(obj_name, obj_part);
  request.upload_data = obj_data.data();
  request.upload_size = obj_data.size();
  if (m_options.content_md5) {
    auto md5 = Md5Base64(obj_data);
    if (!md5) { return tl::unexpected(md5.error()); }
    request.headers.emplace_back("Content-MD5", *md5);
  }
  if (!m_options.storage_class.empty()) {
    request.headers.emplace_back("x-amz-storage-class",
                                 m_options.storage_class);
  }
  if (!m_https) {
    request.payload_hash
        = s3::Sha256Hex(std::string_view(obj_data.data(), obj_data.size()));
    if (request.payload_hash.empty()) {
      return Fail(StoreErrc::kPermanent, "SHA-256 failed (OpenSSL error)");
    }
  }
  auto reply = Execute(request);
  if (!reply) { return tl::unexpected(reply.error()); }
  return {};
}

tl::expected<gsl::span<char>, StoreError> S3NativeStore::download(
    std::string_view obj_name,
    std::string_view obj_part,
    gsl::span<char> buffer,
    std::optional<ByteRange> range)
{
  if (auto ready = EnsureReady(); !ready) {
    return tl::unexpected(ready.error());
  }
  Request request;
  request.key = KeyOf(obj_name, obj_part);
  request.to_buffer = true;
  request.out_buffer = buffer.data();
  request.out_capacity = buffer.size();
  if (range) {
    if (range->length == 0 || range->length > buffer.size()) {
      return Fail(StoreErrc::kConfig,
                  "byte range is empty or larger than the buffer");
    }
    request.out_capacity = range->length;
    request.headers.emplace_back(
        "Range", fmt::format("bytes={}-{}", range->offset,
                             range->offset + range->length - 1));
  }
  auto reply = Execute(request);
  if (!reply) {
    if (reply.error().code == StoreErrc::kPermanent
        && reply.error().message.find("larger than allowed")
               != std::string::npos) {
      return Fail(StoreErrc::kPermanent,
                  "the object is larger than the download buffer");
    }
    return tl::unexpected(reply.error());
  }
  if (range && reply->status != 206) {
    return Fail(StoreErrc::kPermanent,
                fmt::format("the server answered a ranged request with HTTP "
                            "{}",
                            reply->status));
  }
  return gsl::span<char>(buffer.data(), reply->received);
}

tl::expected<void, StoreError> S3NativeStore::remove(std::string_view obj_name,
                                                     std::string_view obj_part)
{
  if (auto ready = EnsureReady(); !ready) {
    return tl::unexpected(ready.error());
  }
  Request request;
  request.method = "DELETE";
  request.key = KeyOf(obj_name, obj_part);
  auto reply = Execute(request);
  if (!reply && reply.error().code != StoreErrc::kNotFound) {
    return tl::unexpected(reply.error());
  }
  return {};
}
