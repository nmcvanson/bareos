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

/* S3NativeStore against a stub HTTP(S) server on 127.0.0.1 that gives fixed
 * replies and stores nothing (a test double, not a storage backend). */

#include "gtest/gtest.h"
#include "include/bareos.h"

#include "stored/backends/s3_native_store.h"
#include "stored/backends/s3_sigv4.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace s3 = storagedaemon::s3;
using namespace std::chrono_literals;

namespace {

constexpr char kAccessKey[] = "AKIAIOSFODNN7EXAMPLE";
constexpr char kSecretKey[] = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

struct RecordedRequest {
  std::string method;
  std::string target;
  std::string path;                            // decoded
  std::string query;                           // as sent
  std::map<std::string, std::string> headers;  // lower-case names
  std::string body;
  int connection{0};
};

struct StubReply {
  StubReply() = default;
  StubReply(int s) : status(s) {}
  int status{200};
  std::string body;
  std::map<std::string, std::string> headers;
  // Content-Length for a HEAD reply.
  long head_length{-1};
  int stall_before_ms{0};
  // Send only this much of the body, then stall_mid_ms, then the rest.
  size_t send_part{std::string::npos};
  int stall_mid_ms{0};
  // Send the body one byte at a time with this pause.
  int drip_ms{0};
};

const char* Reason(int status)
{
  switch (status) {
    case 200:
      return "OK";
    case 204:
      return "No Content";
    case 206:
      return "Partial Content";
    case 301:
      return "Moved Permanently";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 416:
      return "Range Not Satisfiable";
    case 503:
      return "Service Unavailable";
    default:
      return "Status";
  }
}

std::string ErrorXml(const std::string& code, const std::string& message)
{
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Error><Code>" + code
         + "</Code><Message>" + message + "</Message><RequestId>tx1</RequestId>"
         "</Error>";
}

class Channel {
 public:
  Channel(int fd, SSL_CTX* ctx) : fd_(fd)
  {
    if (ctx) {
      ssl_ = SSL_new(ctx);
      SSL_set_fd(ssl_, fd);
      if (SSL_accept(ssl_) != 1) { ok_ = false; }
    }
  }
  ~Channel()
  {
    if (ssl_) { SSL_free(ssl_); }
    close(fd_);
  }
  bool ok() const { return ok_; }
  ssize_t Read(char* buf, size_t n)
  {
    return ssl_ ? SSL_read(ssl_, buf, static_cast<int>(n)) : read(fd_, buf, n);
  }
  bool WriteAll(const char* p, size_t n)
  {
    while (n > 0) {
      ssize_t w = ssl_ ? SSL_write(ssl_, p, static_cast<int>(n))
                       : send(fd_, p, n, MSG_NOSIGNAL);
      if (w <= 0) { return false; }
      p += w;
      n -= static_cast<size_t>(w);
    }
    return true;
  }

 private:
  int fd_;
  SSL* ssl_{nullptr};
  bool ok_{true};
};

class StubServer {
 public:
  using Handler = std::function<StubReply(const RecordedRequest&)>;

  explicit StubServer(const std::string& cert = "", const std::string& key = "")
  {
    // The SD ignores SIGPIPE too; OpenSSL's writes to a closed peer raise it.
    signal(SIGPIPE, SIG_IGN);
    if (!cert.empty()) {
      ctx_ = SSL_CTX_new(TLS_server_method());
      EXPECT_EQ(
          SSL_CTX_use_certificate_file(ctx_, cert.c_str(), SSL_FILETYPE_PEM),
          1);
      EXPECT_EQ(
          SSL_CTX_use_PrivateKey_file(ctx_, key.c_str(), SSL_FILETYPE_PEM), 1);
    }
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(
        bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    EXPECT_EQ(listen(listen_fd_, 16), 0);
    socklen_t len = sizeof(addr);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    acceptor_ = std::thread([this] { AcceptLoop(); });
  }

  ~StubServer()
  {
    running_ = false;
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    acceptor_.join();
    {
      std::lock_guard lock(mutex_);
      for (int fd : open_fds_) { shutdown(fd, SHUT_RDWR); }
    }
    for (auto& t : workers_) { t.join(); }
    if (ctx_) { SSL_CTX_free(ctx_); }
  }

  int port() const { return port_; }
  void SetHandler(Handler handler)
  {
    std::lock_guard lock(mutex_);
    handler_ = std::move(handler);
  }
  // Pause after each read of a request body, to receive an upload slowly.
  void SetBodyReadPause(int ms) { body_read_pause_ms_ = ms; }
  std::vector<RecordedRequest> Requests() const
  {
    std::lock_guard lock(mutex_);
    return requests_;
  }
  int Connections() const { return connections_; }

 private:
  void AcceptLoop()
  {
    while (running_) {
      int fd = accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) { break; }
      int index = ++connections_;
      std::lock_guard lock(mutex_);
      open_fds_.push_back(fd);
      workers_.emplace_back([this, fd, index] { Serve(fd, index); });
    }
  }

  void Sleep(int ms)
  {
    for (int waited = 0; waited < ms && running_; waited += 20) {
      std::this_thread::sleep_for(20ms);
    }
  }

  void Serve(int fd, int index)
  {
    Channel channel(fd, ctx_);
    std::string buffer;
    while (channel.ok() && running_) {
      size_t head_end;
      while ((head_end = buffer.find("\r\n\r\n")) == std::string::npos) {
        char chunk[16384];
        ssize_t n = channel.Read(chunk, sizeof(chunk));
        if (n <= 0) { return; }
        buffer.append(chunk, static_cast<size_t>(n));
      }
      RecordedRequest req;
      req.connection = index;
      std::string head = buffer.substr(0, head_end);
      buffer.erase(0, head_end + 4);
      size_t line_end = head.find("\r\n");
      std::string request_line = head.substr(0, line_end);
      size_t sp1 = request_line.find(' ');
      size_t sp2 = request_line.find(' ', sp1 + 1);
      req.method = request_line.substr(0, sp1);
      req.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
      size_t q = req.target.find('?');
      std::string raw_path = req.target.substr(0, q);
      if (q != std::string::npos) { req.query = req.target.substr(q + 1); }
      auto decoded = s3::UriDecode(raw_path);
      req.path = decoded ? *decoded : raw_path;
      while (line_end != std::string::npos) {
        size_t start = line_end + 2;
        line_end = head.find("\r\n", start);
        std::string line = head.substr(start, line_end == std::string::npos
                                                  ? std::string::npos
                                                  : line_end - start);
        size_t colon = line.find(':');
        if (colon == std::string::npos) { continue; }
        std::string name = line.substr(0, colon);
        for (char& c : name) { c = static_cast<char>(tolower(c)); }
        size_t v = line.find_first_not_of(' ', colon + 1);
        req.headers[name] = v == std::string::npos ? "" : line.substr(v);
      }
      size_t content_length = 0;
      if (auto it = req.headers.find("content-length");
          it != req.headers.end()) {
        content_length = std::stoul(it->second);
      }
      while (buffer.size() < content_length) {
        char chunk[65536];
        ssize_t n = channel.Read(chunk, sizeof(chunk));
        if (n <= 0) { return; }
        buffer.append(chunk, static_cast<size_t>(n));
        Sleep(body_read_pause_ms_);
      }
      req.body = buffer.substr(0, content_length);
      buffer.erase(0, content_length);

      Handler handler;
      {
        std::lock_guard lock(mutex_);
        requests_.push_back(req);
        handler = handler_;
      }
      StubReply reply = handler ? handler(req) : StubReply{};
      Sleep(reply.stall_before_ms);
      bool head_only = req.method == "HEAD";
      size_t length = head_only && reply.head_length >= 0
                          ? static_cast<size_t>(reply.head_length)
                          : reply.body.size();
      std::string out = "HTTP/1.1 " + std::to_string(reply.status) + " "
                        + Reason(reply.status) + "\r\n";
      for (const auto& [k, v] : reply.headers) { out += k + ": " + v + "\r\n"; }
      out += "Content-Length: " + std::to_string(length) + "\r\n\r\n";
      if (!channel.WriteAll(out.data(), out.size())) { return; }
      if (head_only) { continue; }
      if (reply.drip_ms > 0) {
        for (char c : reply.body) {
          if (!channel.WriteAll(&c, 1)) { return; }
          Sleep(reply.drip_ms);
        }
        continue;
      }
      size_t first = std::min(reply.send_part, reply.body.size());
      if (!channel.WriteAll(reply.body.data(), first)) { return; }
      if (first < reply.body.size()) {
        Sleep(reply.stall_mid_ms);
        if (!channel.WriteAll(reply.body.data() + first,
                              reply.body.size() - first)) {
          return;
        }
      }
    }
  }

  SSL_CTX* ctx_{nullptr};
  int listen_fd_{-1};
  int port_{0};
  std::atomic<bool> running_{true};
  std::atomic<int> connections_{0};
  std::atomic<int> body_read_pause_ms_{0};
  mutable std::mutex mutex_;
  Handler handler_;
  std::vector<RecordedRequest> requests_;
  std::vector<int> open_fds_;
  std::vector<std::thread> workers_;
  std::thread acceptor_;
};

// Writes cert.pem and key.pem of a self-signed certificate for 127.0.0.1.
void MakeCertificate(const fs::path& dir)
{
  EVP_PKEY* key = EVP_EC_gen("P-256");
  ASSERT_NE(key, nullptr);
  X509* x = X509_new();
  X509_set_version(x, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
  X509_gmtime_adj(X509_getm_notBefore(x), -3600);
  X509_gmtime_adj(X509_getm_notAfter(x), 24 * 3600);
  X509_set_pubkey(x, key);
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
  X509_set_issuer_name(x, name);
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
  X509_EXTENSION* san = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name,
                                            "IP:127.0.0.1");
  X509_add_ext(x, san, -1);
  X509_EXTENSION_free(san);
  X509_EXTENSION* ca
      = X509V3_EXT_conf_nid(nullptr, &ctx, NID_basic_constraints, "CA:TRUE");
  X509_add_ext(x, ca, -1);
  X509_EXTENSION_free(ca);
  ASSERT_GT(X509_sign(x, key, EVP_sha256()), 0);
  FILE* f = fopen((dir / "cert.pem").c_str(), "w");
  PEM_write_X509(f, x);
  fclose(f);
  f = fopen((dir / "key.pem").c_str(), "w");
  PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
  fclose(f);
  X509_free(x);
  EVP_PKEY_free(key);
}

std::string Md5Base64Reference(const std::string& data)
{
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_Digest(data.data(), data.size(), md, &len, EVP_md5(), nullptr);
  unsigned char out[64];
  int n = EVP_EncodeBlock(out, md, static_cast<int>(len));
  return std::string(reinterpret_cast<char*>(out), static_cast<size_t>(n));
}

// Rebuilds the signature from what the stub received and compares it with the
// one in the Authorization header: the signed request is the sent request.
::testing::AssertionResult SignatureMatches(const RecordedRequest& req,
                                            const std::string& region)
{
  auto it = req.headers.find("authorization");
  if (it == req.headers.end()) {
    return ::testing::AssertionFailure() << "no Authorization";
  }
  const std::string& auth = it->second;
  auto field = [&auth](const std::string& tag, const std::string& end) {
    size_t b = auth.find(tag);
    if (b == std::string::npos) { return std::string(); }
    b += tag.size();
    size_t e = end.empty() ? std::string::npos : auth.find(end, b);
    return auth.substr(b, e == std::string::npos ? e : e - b);
  };
  std::string signed_headers = field("SignedHeaders=", ",");
  std::string signature = field("Signature=", "");
  s3::SigV4Request sr;
  sr.method = req.method;
  sr.path = req.path;
  size_t pos = 0;
  while (pos < req.query.size()) {
    size_t amp = req.query.find('&', pos);
    std::string pair = req.query.substr(
        pos, amp == std::string::npos ? std::string::npos : amp - pos);
    size_t eq = pair.find('=');
    auto n = s3::UriDecode(pair.substr(0, eq));
    auto v = s3::UriDecode(eq == std::string::npos ? "" : pair.substr(eq + 1));
    if (!n || !v) { return ::testing::AssertionFailure() << "bad query"; }
    sr.query.emplace_back(*n, *v);
    pos = amp == std::string::npos ? req.query.size() : amp + 1;
  }
  size_t start = 0;
  while (start <= signed_headers.size()) {
    size_t semi = signed_headers.find(';', start);
    std::string name = signed_headers.substr(
        start, semi == std::string::npos ? std::string::npos : semi - start);
    auto h = req.headers.find(name);
    if (h == req.headers.end()) {
      return ::testing::AssertionFailure() << "signed header missing: " << name;
    }
    sr.headers.emplace_back(name, h->second);
    if (semi == std::string::npos) { break; }
    start = semi + 1;
  }
  sr.payload_hash = req.headers.count("x-amz-content-sha256")
                        ? req.headers.at("x-amz-content-sha256")
                        : "";
  s3::SigV4Credentials creds{kAccessKey, kSecretKey, region, "s3"};
  auto result = s3::Sign(creds, sr, req.headers.at("x-amz-date"));
  if (!result) { return ::testing::AssertionFailure() << result.error(); }
  if (result->signature != signature) {
    return ::testing::AssertionFailure() << "signature differs";
  }
  return ::testing::AssertionSuccess();
}

class s3_native : public ::testing::Test {
 protected:
  void SetUp() override
  {
    dir_
        = fs::temp_directory_path()
          / ("s3_native_store_test_" + std::to_string(getpid()) + "_"
             + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::create_directories(dir_);
  }
  void TearDown() override
  {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  std::string WriteFile(const std::string& name,
                        const std::string& content,
                        mode_t mode = 0600)
  {
    fs::path p = dir_ / name;
    {
      std::ofstream out(p, std::ios::binary);
      out << content;
    }
    chmod(p.c_str(), mode);
    return p.string();
  }

  std::string S3cfg(const StubServer& server,
                    bool https,
                    const std::string& extra = "")
  {
    std::string host = "127.0.0.1:" + std::to_string(server.port());
    return WriteFile(
        "s3cfg", std::string("[default]\naccess_key = ") + kAccessKey
                     + "\nsecret_key = " + kSecretKey + "\nhost_base = " + host
                     + "\nhost_bucket = " + host
                     + "\nuse_https = " + (https ? "True" : "False")
                     + "\nbucket_location = us-east-1\n" + extra);
  }

  std::unique_ptr<S3NativeStore> Store(
      const StubServer& server,
      bool https = false,
      const std::vector<std::pair<std::string, std::string>>& options = {},
      const std::string& cfg_extra = "")
  {
    auto store = std::make_unique<S3NativeStore>();
    store->set_retry_base(1ms);
    EXPECT_TRUE(store->set_option("s3cfg", S3cfg(server, https, cfg_extra)));
    EXPECT_TRUE(store->set_option("bucket", "bkt"));
    EXPECT_TRUE(store->set_option("stall_timeout", "1"));
    for (const auto& [name, value] : options) {
      EXPECT_TRUE(store->set_option(name, value)) << name;
    }
    return store;
  }

  fs::path dir_;
};

std::vector<char> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace

TEST_F(s3_native, SupportedOptions)
{
  S3NativeStore store;
  auto options = store.get_supported_options();
  ASSERT_TRUE(options.has_value());
  EXPECT_EQ(options->size(), 12u);
  for (const char* name :
       {"s3cfg", "bucket", "prefix", "storage_class", "endpoint", "region",
        "path_style", "ca_file", "connect_timeout", "stall_timeout",
        "request_retries", "content_md5"}) {
    EXPECT_NE(std::find(options->begin(), options->end(), name), options->end())
        << name;
  }
  EXPECT_TRUE(store.supports_range_download());
}

TEST_F(s3_native, OptionValuesAreChecked)
{
  S3NativeStore store;
  auto code = [&store](const std::string& n, const std::string& v) {
    auto r = store.set_option(n, v);
    return r ? StoreErrc::kTransient : r.error().code;
  };
  EXPECT_EQ(code("nonsense", "1"), StoreErrc::kConfig);
  EXPECT_EQ(code("connect_timeout", "0"), StoreErrc::kConfig);
  EXPECT_EQ(code("connect_timeout", "abc"), StoreErrc::kConfig);
  EXPECT_EQ(code("connect_timeout", "601"), StoreErrc::kConfig);
  EXPECT_EQ(code("stall_timeout", "-1"), StoreErrc::kConfig);
  EXPECT_EQ(code("stall_timeout", "10s"), StoreErrc::kConfig);
  EXPECT_EQ(code("request_retries", "21"), StoreErrc::kConfig);
  EXPECT_EQ(code("content_md5", "maybe"), StoreErrc::kConfig);
  EXPECT_EQ(code("path_style", "sometimes"), StoreErrc::kConfig);
  EXPECT_EQ(code("bucket", "a/b"), StoreErrc::kConfig);
  EXPECT_EQ(code("bucket", ""), StoreErrc::kConfig);
  EXPECT_EQ(code("endpoint", "https://host"), StoreErrc::kConfig);
  EXPECT_EQ(code("s3cfg", ""), StoreErrc::kConfig);
  EXPECT_EQ(code("region", "us east"), StoreErrc::kConfig);
  EXPECT_NE(code("connect_timeout", "10"), StoreErrc::kConfig);
  EXPECT_NE(code("stall_timeout", "120"), StoreErrc::kConfig);
  EXPECT_NE(code("request_retries", "0"), StoreErrc::kConfig);
  EXPECT_NE(code("content_md5", "No"), StoreErrc::kConfig);
  EXPECT_NE(code("path_style", "auto"), StoreErrc::kConfig);
}

TEST_F(s3_native, OptionsAreFixedAfterFirstUse)
{
  StubServer server;
  auto store = Store(server);
  ASSERT_TRUE(store->test_connection());
  auto r = store->set_option("prefix", "late");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, StoreErrc::kConfig);
}

TEST_F(s3_native, MissingOrBadConfigurationIsAConfigError)
{
  {
    S3NativeStore store;
    auto r = store.test_connection();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StoreErrc::kConfig);
  }
  {
    S3NativeStore store;
    store.set_option("s3cfg", (dir_ / "absent").string());
    store.set_option("bucket", "bkt");
    auto r = store.test_connection();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StoreErrc::kConfig);
  }
  {
    S3NativeStore store;
    store.set_option("s3cfg",
                     WriteFile("nokeys", "[default]\nhost_base = h\n"));
    store.set_option("bucket", "bkt");
    auto r = store.test_connection();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StoreErrc::kConfig);
  }
  {
    S3NativeStore store;
    store.set_option("s3cfg", WriteFile("c",
                                        "[default]\naccess_key = A\n"
                                        "secret_key = TOPSECRETKEY\n"
                                        "use_https = maybe\n"));
    store.set_option("bucket", "bkt");
    auto r = store.test_connection();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StoreErrc::kConfig);
    EXPECT_EQ(r.error().message.find("TOPSECRET"), std::string::npos);
  }
  {
    S3NativeStore store;
    store.set_option("s3cfg", WriteFile("d",
                                        "[default]\naccess_key = A\n"
                                        "secret_key = B\n"));
    auto r = store.test_connection();  // no bucket option
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StoreErrc::kConfig);
  }
}

TEST_F(s3_native, VirtualHostNeedsAHostLabelBucket)
{
  StubServer server;
  S3NativeStore s;
  s.set_option("s3cfg", S3cfg(server, false));
  s.set_option("bucket", "Bad_Bucket.name");
  s.set_option("path_style", "no");
  auto r = s.test_connection();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, StoreErrc::kConfig);
  EXPECT_TRUE(server.Requests().empty());
}

TEST_F(s3_native, TestConnectionIsOneHeadOfTheBucket)
{
  StubServer server;
  auto store = Store(server);
  ASSERT_TRUE(store->test_connection());
  auto reqs = server.Requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0].method, "HEAD");
  EXPECT_EQ(reqs[0].target, "/bkt");
  EXPECT_TRUE(SignatureMatches(reqs[0], "us-east-1"));
  EXPECT_EQ(store->requests_sent(), 1u);
}

TEST_F(s3_native, TestConnectionErrors)
{
  StubServer server;
  auto store = Store(server);
  server.SetHandler([](const RecordedRequest&) { return StubReply{404}; });
  auto missing = store->test_connection();
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, StoreErrc::kConfig);
  EXPECT_NE(missing.error().message.find("bkt"), std::string::npos);

  server.SetHandler([](const RecordedRequest&) {
    StubReply r{403};
    r.body = ErrorXml("AccessDenied", "Access Denied");
    return r;
  });
  auto denied = store->test_connection();
  ASSERT_FALSE(denied.has_value());
  EXPECT_EQ(denied.error().code, StoreErrc::kPermanent);
  EXPECT_NE(denied.error().message.find("403"),
            std::string::npos);  // HEAD has no body
}

TEST_F(s3_native, UploadRequestShapeAndSignatureOverHttp)
{
  StubServer server;
  auto store = Store(server, false,
                     {{"prefix", "/base/"}, {"storage_class", "STANDARD_IA"}});
  std::string body = "hello";
  auto data = Bytes(body);
  ASSERT_TRUE(store->upload("Vol-0001", "0003", data));
  auto reqs = server.Requests();
  ASSERT_EQ(reqs.size(), 1u);
  const RecordedRequest& r = reqs[0];
  EXPECT_EQ(r.method, "PUT");
  EXPECT_EQ(r.target, "/bkt/base/Vol-0001/0003");
  EXPECT_EQ(r.body, body);
  EXPECT_EQ(r.headers.at("content-length"), "5");
  EXPECT_EQ(r.headers.at("content-md5"), "XUFAKrxLKna5cZ2REBfFkg==");
  EXPECT_EQ(r.headers.at("x-amz-content-sha256"), s3::Sha256Hex(body));
  EXPECT_EQ(r.headers.at("x-amz-storage-class"), "STANDARD_IA");
  EXPECT_EQ(r.headers.at("host"), "127.0.0.1:" + std::to_string(server.port()));
  EXPECT_EQ(r.headers.count("expect"), 0u);
  EXPECT_NE(r.headers.at("authorization")
                .find("Credential=" + std::string(kAccessKey) + "/"),
            std::string::npos);
  EXPECT_NE(
      r.headers.at("authorization").find("SignedHeaders=content-md5;host;"),
      std::string::npos);
  EXPECT_TRUE(SignatureMatches(r, "us-east-1"));
}

TEST_F(s3_native, KeysAreEncodedOnceAsSigned)
{
  StubServer server;
  auto store = Store(server);
  auto data = Bytes("x");
  ASSERT_TRUE(store->upload("Vol 1+a", "00 00", data));
  auto reqs = server.Requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0].target, "/bkt/Vol%201%2Ba/00%2000");
  EXPECT_EQ(reqs[0].path, "/bkt/Vol 1+a/00 00");
  EXPECT_TRUE(SignatureMatches(reqs[0], "us-east-1"));
}

TEST_F(s3_native, RegionOptionOverridesTheS3cfg)
{
  StubServer server;
  auto store = Store(server, false, {{"region", "vn-north-1"}});
  ASSERT_TRUE(store->test_connection());
  auto reqs = server.Requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_NE(
      reqs[0].headers.at("authorization").find("/vn-north-1/s3/aws4_request"),
      std::string::npos);
  EXPECT_TRUE(SignatureMatches(reqs[0], "vn-north-1"));
}

TEST_F(s3_native, ContentMd5CanBeSwitchedOff)
{
  StubServer server;
  auto store = Store(server, false, {{"content_md5", "no"}});
  auto data = Bytes("hello");
  ASSERT_TRUE(store->upload("V", "0", data));
  EXPECT_EQ(server.Requests().at(0).headers.count("content-md5"), 0u);
}

TEST_F(s3_native, LargeUploadArrivesIntact)
{
  StubServer server;
  auto store = Store(server);
  std::string body(8 * 1024 * 1024, '\0');
  for (size_t i = 0; i < body.size(); ++i) {
    body[i] = static_cast<char>(i * 7 + (i >> 9));
  }
  auto data = Bytes(body);
  ASSERT_TRUE(store->upload("V", "0000", data));
  auto reqs = server.Requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(s3::Sha256Hex(reqs[0].body), s3::Sha256Hex(body));
  EXPECT_EQ(reqs[0].headers.at("content-md5"), Md5Base64Reference(body));
}

TEST_F(s3_native, EmptyUpload)
{
  StubServer server;
  auto store = Store(server);
  std::vector<char> data;
  ASSERT_TRUE(store->upload("V", "0000", data));
  EXPECT_EQ(server.Requests().at(0).headers.at("content-length"), "0");
}

TEST_F(s3_native, StubRejectingAWrongDigestAcceptsOurUpload)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest& r) {
    if (r.method == "PUT"
        && r.headers.at("content-md5") != Md5Base64Reference(r.body)) {
      StubReply bad{400};
      bad.body = ErrorXml("BadDigest",
                          "The Content-MD5 you specified did not match");
      return bad;
    }
    return StubReply{};
  });
  auto store = Store(server);
  auto data = Bytes("payload that must hash right");
  ASSERT_TRUE(store->upload("V", "0001", data));
  EXPECT_EQ(server.Requests().size(), 1u);
}

TEST_F(s3_native, BadDigestIsRetriedWithTheSameRequest)
{
  StubServer server;
  std::atomic<int> puts{0};
  server.SetHandler([&puts](const RecordedRequest& r) {
    if (r.method == "PUT" && puts++ == 0) {
      StubReply bad{400};
      bad.body = ErrorXml("BadDigest", "digest mismatch");
      return bad;
    }
    return StubReply{};
  });
  auto store = Store(server);
  auto data = Bytes("abcdef");
  ASSERT_TRUE(store->upload("V", "0001", data));
  auto reqs = server.Requests();
  ASSERT_EQ(reqs.size(), 2u);
  EXPECT_EQ(reqs[0].headers.at("content-md5"),
            reqs[1].headers.at("content-md5"));
  EXPECT_EQ(reqs[0].body, reqs[1].body);
  EXPECT_NE(reqs[0].headers.at("authorization"), "");
  EXPECT_TRUE(SignatureMatches(reqs[1], "us-east-1"));
  EXPECT_EQ(store->requests_sent(), 2u);
}

TEST_F(s3_native, ServerErrorsAreRetriedThenSucceed)
{
  StubServer server;
  std::atomic<int> n{0};
  server.SetHandler([&n](const RecordedRequest&) {
    int i = n++;
    if (i == 0) { return StubReply{503}; }
    if (i == 1) {
      StubReply r{429};
      r.body = ErrorXml("SlowDown", "slow");
      return r;
    }
    return StubReply{};
  });
  auto store = Store(server);
  auto data = Bytes("x");
  ASSERT_TRUE(store->upload("V", "0", data));
  EXPECT_EQ(server.Requests().size(), 3u);
}

TEST_F(s3_native, RetriesAreBoundedAndEndTransient)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) { return StubReply{503}; });
  auto store = Store(server, false, {{"request_retries", "2"}});
  auto data = Bytes("x");
  auto r = store->upload("V", "0", data);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, StoreErrc::kTransient);
  EXPECT_NE(r.error().message.find("after 3 attempts"), std::string::npos);
  EXPECT_EQ(server.Requests().size(), 3u);
  EXPECT_EQ(store->requests_sent(), 3u);
}

TEST_F(s3_native, NoRetriesWhenSetToZero)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) { return StubReply{503}; });
  auto store = Store(server, false, {{"request_retries", "0"}});
  auto r = store->stat("V", "0");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(server.Requests().size(), 1u);
}

TEST_F(s3_native, PermanentErrorsAreNotRetried)
{
  StubServer server;
  for (int status : {400, 403, 301, 416}) {
    server.SetHandler([status](const RecordedRequest&) {
      StubReply r{status};
      r.body = ErrorXml("SomethingElse", "no");
      return r;
    });
    size_t before = server.Requests().size();
    auto store = Store(server);
    auto data = Bytes("x");
    auto r = store->upload("V", "0", data);
    ASSERT_FALSE(r.has_value()) << status;
    EXPECT_EQ(r.error().code, StoreErrc::kPermanent) << status;
    EXPECT_EQ(server.Requests().size() - before, 1u) << status;
  }
}

TEST_F(s3_native, ClockSkewIsNamedInTheError)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r{403};
    r.body = ErrorXml("RequestTimeTooSkewed", "skewed");
    return r;
  });
  auto store = Store(server);
  auto r = store->list("V");
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("RequestTimeTooSkewed"), std::string::npos);
  EXPECT_NE(r.error().message.find("clock"), std::string::npos);
}

TEST_F(s3_native, StatAndRemove)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest& r) {
    StubReply reply;
    if (r.method == "HEAD" && r.path == "/bkt/V/0001") {
      reply.head_length = 123456;
    } else if (r.method == "HEAD") {
      reply.status = 404;
    } else if (r.method == "DELETE" && r.path == "/bkt/V/0001") {
      reply.status = 204;
    } else {
      reply.status = 404;
    }
    return reply;
  });
  auto store = Store(server);
  auto st = store->stat("V", "0001");
  ASSERT_TRUE(st.has_value());
  EXPECT_EQ(st->size, 123456u);
  auto missing = store->stat("V", "0002");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, StoreErrc::kNotFound);
  EXPECT_TRUE(store->remove("V", "0001"));
  EXPECT_TRUE(store->remove("V", "0009"));  // missing object: success
  auto reqs = server.Requests();
  ASSERT_EQ(reqs.size(), 4u);
  EXPECT_EQ(reqs[2].method, "DELETE");
  EXPECT_TRUE(SignatureMatches(reqs[0], "us-east-1"));
  EXPECT_TRUE(SignatureMatches(reqs[2], "us-east-1"));
}

TEST_F(s3_native, RemoveFailureIsReported)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) { return StubReply{403}; });
  auto store = Store(server);
  auto r = store->remove("V", "0");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, StoreErrc::kPermanent);
}

TEST_F(s3_native, DownloadWholeObject)
{
  StubServer server;
  std::string object = "0123456789abcdef";
  server.SetHandler([&object](const RecordedRequest&) {
    StubReply r;
    r.body = object;
    return r;
  });
  auto store = Store(server);
  std::vector<char> buffer(64);
  auto got = store->download("V", "0", buffer, std::nullopt);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(std::string(got->data(), got->size()), object);
  auto reqs = server.Requests();
  EXPECT_EQ(reqs.at(0).method, "GET");
  EXPECT_EQ(reqs.at(0).headers.count("range"), 0u);
  EXPECT_TRUE(SignatureMatches(reqs[0], "us-east-1"));
}

TEST_F(s3_native, DownloadLargerThanTheBufferIsAnError)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r;
    r.body = std::string(100, 'x');
    return r;
  });
  auto store = Store(server);
  std::vector<char> buffer(10);
  auto got = store->download("V", "0", buffer, std::nullopt);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code, StoreErrc::kPermanent);
  EXPECT_EQ(server.Requests().size(), 1u);
}

TEST_F(s3_native, DownloadMissingObject)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r{404};
    r.body = ErrorXml("NoSuchKey", "nope");
    return r;
  });
  auto store = Store(server);
  std::vector<char> buffer(10);
  auto got = store->download("V", "0", buffer, std::nullopt);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code, StoreErrc::kNotFound);
}

TEST_F(s3_native, RangedDownload)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest& r) {
    StubReply reply;
    reply.status = 206;
    reply.body = "56789";
    reply.headers["Content-Range"] = "bytes 5-9/16";
    EXPECT_EQ(r.headers.at("range"), "bytes=5-9");
    return reply;
  });
  auto store = Store(server);
  std::vector<char> buffer(64);
  auto got = store->download("V", "0", buffer, ByteRange{5, 5});
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(std::string(got->data(), got->size()), "56789");
  EXPECT_TRUE(SignatureMatches(server.Requests().at(0), "us-east-1"));
}

TEST_F(s3_native, RangedDownloadNeedsPartialContent)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r;  // 200: the server ignored the Range header
    r.body = "56789";
    return r;
  });
  auto store = Store(server);
  std::vector<char> buffer(64);
  auto got = store->download("V", "0", buffer, ByteRange{5, 5});
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code, StoreErrc::kPermanent);
}

TEST_F(s3_native, RangeLargerThanTheBufferOrEmptyIsRefused)
{
  StubServer server;
  auto store = Store(server);
  std::vector<char> buffer(8);
  auto big = store->download("V", "0", buffer, ByteRange{0, 9});
  ASSERT_FALSE(big.has_value());
  EXPECT_EQ(big.error().code, StoreErrc::kConfig);
  auto empty = store->download("V", "0", buffer, ByteRange{0, 0});
  ASSERT_FALSE(empty.has_value());
  EXPECT_TRUE(server.Requests().empty());
}

TEST_F(s3_native, ShortRangeAtTheEndOfAnObjectReturnsTheBytesThatExist)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r{206};
    r.body = "xyz";
    return r;
  });
  auto store = Store(server);
  std::vector<char> buffer(64);
  auto got = store->download("V", "0", buffer, ByteRange{100, 10});
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->size(), 3u);
}

namespace {

std::string ListingXml(const std::vector<std::pair<std::string, size_t>>& objs,
                       const std::string& next_token)
{
  std::string xml
      = "<?xml version=\"1.0\"?><ListBucketResult "
        "xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
        "<Name>bkt</Name><EncodingType>url</EncodingType>";
  xml += std::string("<IsTruncated>") + (next_token.empty() ? "false" : "true")
         + "</IsTruncated>";
  if (!next_token.empty()) {
    xml += "<NextContinuationToken>" + next_token + "</NextContinuationToken>";
  }
  for (const auto& [key, size] : objs) {
    xml += "<Contents><Key>" + key + "</Key><Size>" + std::to_string(size)
           + "</Size></Contents>";
  }
  return xml + "</ListBucketResult>";
}

std::string QueryValue(const std::string& query, const std::string& name)
{
  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    std::string pair
        = query.substr(pos, amp == std::string::npos ? amp : amp - pos);
    size_t eq = pair.find('=');
    if (pair.substr(0, eq) == name) {
      auto v = s3::UriDecode(pair.substr(eq + 1));
      return v ? *v : "";
    }
    pos = amp == std::string::npos ? query.size() : amp + 1;
  }
  return "<absent>";
}

}  // namespace

TEST_F(s3_native, ListOnePage)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r;
    r.body = ListingXml({{"pre/V/0000", 10485760}, {"pre/V/0001", 77}}, "");
    return r;
  });
  auto store = Store(server, false, {{"prefix", "pre"}});
  auto list = store->list("V");
  ASSERT_TRUE(list.has_value());
  ASSERT_EQ(list->size(), 2u);
  EXPECT_EQ(list->at("0000").size, 10485760u);
  EXPECT_EQ(list->at("0001").size, 77u);
  auto reqs = server.Requests();
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0].method, "GET");
  EXPECT_EQ(reqs[0].path, "/bkt");
  EXPECT_EQ(QueryValue(reqs[0].query, "list-type"), "2");
  EXPECT_EQ(QueryValue(reqs[0].query, "prefix"), "pre/V/");
  EXPECT_EQ(QueryValue(reqs[0].query, "encoding-type"), "url");
  EXPECT_EQ(QueryValue(reqs[0].query, "continuation-token"), "<absent>");
  EXPECT_TRUE(SignatureMatches(reqs[0], "us-east-1"));
}

TEST_F(s3_native, ListFollowsContinuationTokens)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest& r) {
    StubReply reply;
    std::string token = QueryValue(r.query, "continuation-token");
    if (token == "<absent>") {
      reply.body = ListingXml({{"V/0000", 1}, {"V/0001", 2}}, "tok+1/=");
    } else if (token == "tok+1/=") {
      reply.body = ListingXml({{"V/0002", 3}}, "tok2");
    } else {
      reply.body = ListingXml({{"V/0003", 4}}, "");
    }
    return reply;
  });
  auto store = Store(server);
  auto list = store->list("V");
  ASSERT_TRUE(list.has_value());
  EXPECT_EQ(list->size(), 4u);
  EXPECT_EQ(list->at("0003").size, 4u);
  auto reqs = server.Requests();
  ASSERT_EQ(reqs.size(), 3u);
  EXPECT_EQ(QueryValue(reqs[1].query, "continuation-token"), "tok+1/=");
  for (const auto& r : reqs) { EXPECT_TRUE(SignatureMatches(r, "us-east-1")); }
}

TEST_F(s3_native, ListEmptyVolumeAndForeignKeys)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r;
    r.body = ListingXml({{"V/", 0}, {"V/0001", 5}, {"Other/0002", 6}}, "");
    return r;
  });
  auto store = Store(server);
  auto list = store->list("V");
  ASSERT_TRUE(list.has_value());
  ASSERT_EQ(list->size(), 1u);
  EXPECT_EQ(list->count("0001"), 1u);

  server.SetHandler([](const RecordedRequest&) {
    StubReply r;
    r.body = ListingXml({}, "");
    return r;
  });
  auto empty = store->list("V");
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
}

TEST_F(s3_native, ListDecodesUrlEncodedKeys)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r;
    r.body = ListingXml({{"Vol+1/part+a%2Bb", 9}}, "");
    return r;
  });
  auto store = Store(server);
  auto list = store->list("Vol 1");
  ASSERT_TRUE(list.has_value());
  EXPECT_EQ(list->count("part a+b"), 1u);
}

TEST_F(s3_native, ListErrors)
{
  StubServer server;
  auto store = Store(server);

  server.SetHandler([](const RecordedRequest&) {
    StubReply r;
    r.body = "<html>not a listing</html>";
    return r;
  });
  auto garbage = store->list("V");
  ASSERT_FALSE(garbage.has_value());
  EXPECT_EQ(garbage.error().code, StoreErrc::kTransient);

  server.SetHandler([](const RecordedRequest&) {
    StubReply r;
    r.body = ListingXml({{"V/0000", 1}}, "").substr(0, 150);  // cut off
    return r;
  });
  EXPECT_FALSE(store->list("V").has_value());

  server.SetHandler([](const RecordedRequest&) {
    StubReply r;
    r.body = ListingXml({{"V/0000", 1}}, "same");
    return r;
  });
  auto loop = store->list("V");
  ASSERT_FALSE(loop.has_value());
  EXPECT_EQ(loop.error().code, StoreErrc::kPermanent);

  server.SetHandler([](const RecordedRequest&) {
    StubReply r{403};
    r.body = ErrorXml("AccessDenied", "Access Denied");
    return r;
  });
  auto denied = store->list("V");
  ASSERT_FALSE(denied.has_value());
  EXPECT_EQ(denied.error().code, StoreErrc::kPermanent);
}

TEST_F(s3_native, OversizedListingBodyIsRefused)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r;
    r.body = std::string(17 * 1024 * 1024, ' ');
    return r;
  });
  auto store = Store(server, false, {{"request_retries", "0"}});
  auto list = store->list("V");
  ASSERT_FALSE(list.has_value());
  EXPECT_EQ(list.error().code, StoreErrc::kPermanent);
}

TEST_F(s3_native, ErrorBodyBeyondTheCapIsNotRead)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r{403};
    // 60000 bytes arrive first, so the next chunk straddles the 64 KiB cap
    // and the Code at the end (about 70000) is cut off by a capped body.
    const std::string head = "<Error><Message>";
    const std::string tail = "</Message><Code>QuotaExceeded</Code></Error>";
    r.body = head + std::string(70000 - head.size() - tail.size(), 'a') + tail;
    r.send_part = 60000;
    r.stall_mid_ms = 300;
    return r;
  });
  auto store = Store(server, false, {{"request_retries", "0"}});
  auto list = store->list("V");
  ASSERT_FALSE(list.has_value());
  EXPECT_EQ(list.error().code, StoreErrc::kPermanent);
  EXPECT_NE(list.error().message.find("403"), std::string::npos);
  EXPECT_EQ(list.error().message.find("QuotaExceeded"), std::string::npos);
}

TEST_F(s3_native, SlowButMovingUploadIsNotAStall)
{
  StubServer server;
  server.SetBodyReadPause(8);  // about 8 s for the 24 MiB, stall timeout 3 s
  auto store = Store(server, false,
                     {{"request_retries", "0"}, {"stall_timeout", "3"}});
  std::vector<char> data(24 * 1024 * 1024, 'u');
  auto up = store->upload("V", "0", data);
  ASSERT_TRUE(up.has_value()) << up.error().message;
  ASSERT_EQ(server.Requests().size(), 1u);
  EXPECT_EQ(server.Requests()[0].body.size(), data.size());
}

TEST_F(s3_native, ConnectionsAreKeptAliveBetweenRequests)
{
  StubServer server;
  auto store = Store(server);
  auto data = Bytes("x");
  for (int i = 0; i < 6; ++i) {
    ASSERT_TRUE(store->upload("V", std::to_string(i), data));
    ASSERT_TRUE(store->stat("V", std::to_string(i)).has_value());
  }
  EXPECT_EQ(server.Connections(), 1);
  EXPECT_EQ(server.Requests().size(), 12u);
}

TEST_F(s3_native, ThreadsShareTheStoreAndEachKeepsItsConnection)
{
  StubServer server;
  auto store = Store(server);
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      auto data = Bytes("payload" + std::to_string(t));
      for (int i = 0; i < 10; ++i) {
        if (!store->upload("V" + std::to_string(t), std::to_string(i), data)) {
          ++failures;
        }
      }
    });
  }
  for (auto& th : threads) { th.join(); }
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(server.Requests().size(), 40u);
  EXPECT_LE(server.Connections(), 4);
  EXPECT_EQ(store->requests_sent(), 40u);
  for (const auto& r : server.Requests()) {
    EXPECT_TRUE(SignatureMatches(r, "us-east-1"));
  }
}

TEST_F(s3_native, StalledDownloadIsAbortedAndRetried)
{
  StubServer server;
  std::atomic<int> gets{0};
  server.SetHandler([&gets](const RecordedRequest&) {
    StubReply r;
    r.body = "0123456789";
    if (gets++ == 0) {
      r.send_part = 4;
      r.stall_mid_ms = 6000;
    }
    return r;
  });
  auto store = Store(server);
  std::vector<char> buffer(64);
  auto start = std::chrono::steady_clock::now();
  auto got = store->download("V", "0", buffer, std::nullopt);
  auto took = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(std::string(got->data(), got->size()), "0123456789");
  EXPECT_EQ(server.Requests().size(), 2u);
  EXPECT_LT(took, 5s);
  EXPECT_GE(took, 1s);
}

TEST_F(s3_native, SlowButMovingTransferIsNotAStall)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r;
    r.body = "abcdef";
    r.drip_ms = 400;  // 2.4 s in total, more than the 1 s stall timeout
    return r;
  });
  auto store = Store(server);
  std::vector<char> buffer(64);
  auto got = store->download("V", "0", buffer, std::nullopt);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(std::string(got->data(), got->size()), "abcdef");
  EXPECT_EQ(server.Requests().size(), 1u);
}

TEST_F(s3_native, SilentServerEndsAsTimeoutAfterTheRetries)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest&) {
    StubReply r;
    r.stall_before_ms = 6000;
    return r;
  });
  auto store = Store(server, false, {{"request_retries", "1"}});
  auto data = Bytes("x");
  auto r = store->upload("V", "0", data);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, StoreErrc::kTimeout);
  EXPECT_NE(r.error().message.find("after 2 attempts"), std::string::npos);
  EXPECT_EQ(store->requests_sent(), 2u);
}

TEST_F(s3_native, UnreachableServerIsTransientAndBounded)
{
  int port;
  {
    StubServer server;
    port = server.port();
  }
  S3NativeStore store;
  store.set_retry_base(1ms);
  std::string host = "127.0.0.1:" + std::to_string(port);
  store.set_option(
      "s3cfg",
      WriteFile(
          "s",
          std::string("[default]\naccess_key = A\nsecret_key = B\nhost_base = ")
              + host + "\nhost_bucket = " + host + "\nuse_https = False\n"));
  store.set_option("bucket", "bkt");
  store.set_option("request_retries", "2");
  auto r = store.stat("V", "0");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, StoreErrc::kTransient);
  EXPECT_EQ(store.requests_sent(), 3u);
}

TEST_F(s3_native, HttpsUsesUnsignedPayloadAndTheCaFile)
{
  MakeCertificate(dir_);
  StubServer server((dir_ / "cert.pem").string(), (dir_ / "key.pem").string());
  auto store = Store(server, true, {{"ca_file", (dir_ / "cert.pem").string()}});
  ASSERT_TRUE(store->test_connection());
  auto data = Bytes("secure body");
  ASSERT_TRUE(store->upload("V", "0001", data));
  auto got = server.Requests();
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[1].method, "PUT");
  EXPECT_EQ(got[1].body, "secure body");
  EXPECT_EQ(got[1].headers.at("x-amz-content-sha256"), "UNSIGNED-PAYLOAD");
  EXPECT_EQ(got[1].headers.at("content-md5"),
            Md5Base64Reference("secure body"));
  EXPECT_TRUE(SignatureMatches(got[1], "us-east-1"));
  EXPECT_EQ(server.Connections(), 1);
}

TEST_F(s3_native, CaFileFromTheS3cfgIsUsed)
{
  MakeCertificate(dir_);
  StubServer server((dir_ / "cert.pem").string(), (dir_ / "key.pem").string());
  auto store = Store(server, true, {},
                     "ca_certs_file = " + (dir_ / "cert.pem").string() + "\n");
  EXPECT_TRUE(store->test_connection());
}

TEST_F(s3_native, UnknownCertificateIsAConfigErrorWithoutRetries)
{
  MakeCertificate(dir_);
  StubServer server((dir_ / "cert.pem").string(), (dir_ / "key.pem").string());
  auto store = Store(server, true);
  auto r = store->test_connection();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, StoreErrc::kConfig);
  EXPECT_EQ(store->requests_sent(), 1u);
  EXPECT_TRUE(server.Requests().empty());
}

TEST_F(s3_native, CertificateChecksCanBeSwitchedOffInTheS3cfg)
{
  MakeCertificate(dir_);
  StubServer server((dir_ / "cert.pem").string(), (dir_ / "key.pem").string());
  auto store = Store(server, true, {}, "check_ssl_certificate = False\n");
  EXPECT_TRUE(store->test_connection());
}

TEST_F(s3_native, WrongCaFileIsAConfigError)
{
  MakeCertificate(dir_);
  StubServer server((dir_ / "cert.pem").string(), (dir_ / "key.pem").string());
  auto store
      = Store(server, true, {{"ca_file", (dir_ / "missing.pem").string()}});
  auto r = store->test_connection();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, StoreErrc::kConfig);
}

TEST_F(s3_native, TheSecretKeyNeverLeavesThroughRequestsOrErrors)
{
  StubServer server;
  server.SetHandler([](const RecordedRequest& r) {
    return r.method == "GET" ? StubReply{403} : StubReply{};
  });
  auto store = Store(server, false, {{"request_retries", "0"}});
  auto data = Bytes("x");
  (void)store->upload("V", "0", data);
  (void)store->stat("V", "0");
  auto err = store->list("V");
  ASSERT_FALSE(err.has_value());
  EXPECT_EQ(err.error().message.find(kSecretKey), std::string::npos);
  for (const auto& r : server.Requests()) {
    EXPECT_EQ(r.target.find(kSecretKey), std::string::npos);
    for (const auto& [name, value] : r.headers) {
      EXPECT_EQ(value.find(kSecretKey), std::string::npos) << name;
    }
    EXPECT_EQ(r.body.find(kSecretKey), std::string::npos);
  }
}

TEST_F(s3_native, WorldReadableS3cfgStillWorks)
{
  StubServer server;
  auto store = std::make_unique<S3NativeStore>();
  std::string cfg = S3cfg(server, false);
  chmod(cfg.c_str(), 0644);
  store->set_option("s3cfg", cfg);
  store->set_option("bucket", "bkt");
  EXPECT_TRUE(store->test_connection());
}
