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

#include "stored/backends/s3_xml.h"

#include <algorithm>
#include <limits>

#include "stored/backends/s3_sigv4.h"

namespace storagedaemon::s3 {

namespace {

using Error = tl::unexpected<std::string>;

constexpr size_t kMaxDepth = 64;

// Element tree of a document: local name, text and child elements.
struct Node {
  std::string name;
  std::string text;
  std::vector<Node> children;

  const Node* Child(std::string_view child_name) const
  {
    for (const Node& c : children) {
      if (c.name == child_name) { return &c; }
    }
    return nullptr;
  }
};

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view Trim(std::string_view s)
{
  while (!s.empty() && IsSpace(s.front())) { s.remove_prefix(1); }
  while (!s.empty() && IsSpace(s.back())) { s.remove_suffix(1); }
  return s;
}

bool IsNameStart(char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'
         || c == ':';
}

std::string_view LocalName(std::string_view qname)
{
  size_t colon = qname.rfind(':');
  return colon == std::string_view::npos ? qname : qname.substr(colon + 1);
}

bool AppendUtf8(std::string& out, uint32_t cp)
{
  if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return false;
  }
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return true;
}

// Appends text with its entity references resolved.
tl::expected<void, std::string> AppendDecoded(std::string& out,
                                              std::string_view text)
{
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out.push_back(text[i]);
      continue;
    }
    size_t semi = text.find(';', i);
    if (semi == std::string_view::npos || semi - i > 12) {
      return Error("unterminated entity reference");
    }
    std::string_view ref = text.substr(i + 1, semi - i - 1);
    i = semi;
    if (ref == "amp") {
      out.push_back('&');
    } else if (ref == "lt") {
      out.push_back('<');
    } else if (ref == "gt") {
      out.push_back('>');
    } else if (ref == "quot") {
      out.push_back('"');
    } else if (ref == "apos") {
      out.push_back('\'');
    } else if (ref.size() >= 2 && ref[0] == '#') {
      bool hex = ref[1] == 'x';
      std::string_view digits = ref.substr(hex ? 2 : 1);
      if (digits.empty()) { return Error("bad character reference"); }
      uint32_t cp = 0;
      for (char c : digits) {
        int v;
        if (c >= '0' && c <= '9') {
          v = c - '0';
        } else if (hex && c >= 'a' && c <= 'f') {
          v = c - 'a' + 10;
        } else if (hex && c >= 'A' && c <= 'F') {
          v = c - 'A' + 10;
        } else {
          return Error("bad character reference");
        }
        cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(v);
        if (cp > 0x10FFFF) { return Error("bad character reference"); }
      }
      if (!AppendUtf8(out, cp)) { return Error("bad character reference"); }
    } else {
      return Error("unknown entity reference");
    }
  }
  return {};
}

// Parses a whole document into its root element.
tl::expected<Node, std::string> ParseDocument(std::string_view xml)
{
  if (xml.substr(0, 3) == "\xEF\xBB\xBF") { xml.remove_prefix(3); }

  Node root;
  bool have_root = false;
  bool root_closed = false;
  std::vector<Node*> stack;
  size_t i = 0;

  while (i < xml.size()) {
    if (xml[i] != '<') {
      size_t end = xml.find('<', i);
      if (end == std::string_view::npos) { end = xml.size(); }
      std::string_view text = xml.substr(i, end - i);
      if (stack.empty()) {
        if (!Trim(text).empty()) {
          return Error("text outside the root element");
        }
      } else if (auto r = AppendDecoded(stack.back()->text, text); !r) {
        return Error(r.error());
      }
      i = end;
      continue;
    }

    std::string_view rest = xml.substr(i);
    if (rest.substr(0, 2) == "<?") {
      size_t end = rest.find("?>");
      if (end == std::string_view::npos) {
        return Error("unterminated declaration");
      }
      i += end + 2;
    } else if (rest.substr(0, 4) == "<!--") {
      size_t end = rest.find("-->", 4);
      if (end == std::string_view::npos) {
        return Error("unterminated comment");
      }
      i += end + 3;
    } else if (rest.substr(0, 9) == "<![CDATA[") {
      size_t end = rest.find("]]>", 9);
      if (end == std::string_view::npos) {
        return Error("unterminated CDATA section");
      }
      if (stack.empty()) { return Error("CDATA outside the root element"); }
      stack.back()->text.append(rest.substr(9, end - 9));
      i += end + 3;
    } else if (rest.substr(0, 2) == "<!") {
      // A DOCTYPE without an internal subset is skipped, entities are not
      // supported.
      size_t end = rest.find('>');
      if (end == std::string_view::npos
          || rest.substr(0, end).find('[') != std::string_view::npos) {
        return Error("unsupported markup declaration");
      }
      i += end + 1;
    } else if (rest.substr(0, 2) == "</") {
      size_t end = rest.find('>');
      if (end == std::string_view::npos) {
        return Error("unterminated end tag");
      }
      std::string_view name = Trim(rest.substr(2, end - 2));
      if (stack.empty() || LocalName(name) != stack.back()->name) {
        return Error("mismatched end tag");
      }
      stack.pop_back();
      if (stack.empty()) { root_closed = true; }
      i += end + 1;
    } else {
      if (root_closed) { return Error("content after the root element"); }
      if (rest.size() < 2 || !IsNameStart(rest[1])) {
        return Error("malformed start tag");
      }
      size_t pos = 1;
      while (pos < rest.size() && !IsSpace(rest[pos]) && rest[pos] != '/'
             && rest[pos] != '>') {
        ++pos;
      }
      std::string_view name = LocalName(rest.substr(1, pos - 1));
      if (name.empty()) { return Error("malformed start tag"); }

      // Skip the attributes up to the closing '>'; quoted values may hold '>'.
      char quote = 0;
      bool self_closing = false;
      bool closed = false;
      for (; pos < rest.size(); ++pos) {
        char c = rest[pos];
        if (quote) {
          if (c == quote) { quote = 0; }
        } else if (c == '"' || c == '\'') {
          quote = c;
        } else if (c == '>') {
          self_closing = rest[pos - 1] == '/';
          closed = true;
          break;
        }
      }
      if (!closed) { return Error("unterminated start tag"); }
      i += pos + 1;

      Node* node;
      if (stack.empty()) {
        root.name = std::string(name);
        have_root = true;
        node = &root;
      } else {
        if (stack.size() >= kMaxDepth) {
          return Error("document nested too deeply");
        }
        stack.back()->children.emplace_back();
        node = &stack.back()->children.back();
        node->name = std::string(name);
      }
      if (self_closing) {
        if (stack.empty()) { root_closed = true; }
      } else {
        stack.push_back(node);
      }
    }
  }

  if (!stack.empty() || !have_root) {
    return Error("unexpected end of document");
  }
  return root;
}

tl::expected<uint64_t, std::string> ParseSize(std::string_view text)
{
  text = Trim(text);
  if (text.empty()) { return Error("empty Size"); }
  uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') { return Error("Size is not a number"); }
    uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
      return Error("Size out of range");
    }
    value = value * 10 + digit;
  }
  return value;
}

// "+" is a space in url-encoded keys; a literal plus comes as %2B.
tl::expected<std::string, std::string> DecodeKey(std::string_view key)
{
  std::string plus_decoded(key);
  std::replace(plus_decoded.begin(), plus_decoded.end(), '+', ' ');
  return UriDecode(plus_decoded);
}

}  // namespace

tl::expected<ListObjectsResult, std::string> ParseListObjectsV2(
    std::string_view xml)
{
  auto doc = ParseDocument(xml);
  if (!doc) { return Error(doc.error()); }
  if (doc->name != "ListBucketResult") {
    return Error("not a ListBucketResult document");
  }

  ListObjectsResult result;
  bool have_truncated = false;
  bool url_encoded = false;
  for (const Node& child : doc->children) {
    if (child.name == "IsTruncated") {
      std::string_view value = Trim(child.text);
      if (value == "true") {
        result.is_truncated = true;
      } else if (value == "false") {
        result.is_truncated = false;
      } else {
        return Error("IsTruncated is neither true nor false");
      }
      have_truncated = true;
    } else if (child.name == "NextContinuationToken") {
      result.next_continuation_token = std::string(Trim(child.text));
    } else if (child.name == "EncodingType") {
      url_encoded = Trim(child.text) == "url";
    } else if (child.name == "Contents") {
      const Node* key = child.Child("Key");
      const Node* size = child.Child("Size");
      if (!key) { return Error("Contents without Key"); }
      if (!size) { return Error("Contents without Size"); }
      auto parsed = ParseSize(size->text);
      if (!parsed) { return Error(parsed.error()); }
      result.objects.push_back({key->text, *parsed});
    }
  }
  if (!have_truncated) { return Error("missing IsTruncated"); }
  if (result.is_truncated && result.next_continuation_token.empty()) {
    return Error("truncated listing without NextContinuationToken");
  }
  if (url_encoded) {
    for (ListedObject& object : result.objects) {
      auto key = DecodeKey(object.key);
      if (!key) { return Error("Key: " + key.error()); }
      object.key = std::move(*key);
    }
  }
  return result;
}

tl::expected<S3Error, std::string> ParseS3Error(std::string_view xml)
{
  auto doc = ParseDocument(xml);
  if (!doc) { return Error(doc.error()); }
  if (doc->name != "Error") { return Error("not an Error document"); }

  S3Error error;
  const Node* code = doc->Child("Code");
  if (!code || Trim(code->text).empty()) { return Error("Error without Code"); }
  error.code = std::string(Trim(code->text));
  if (const Node* message = doc->Child("Message")) {
    error.message = std::string(Trim(message->text));
  }
  if (const Node* request_id = doc->Child("RequestId")) {
    error.request_id = std::string(Trim(request_id->text));
  }
  return error;
}

}  // namespace storagedaemon::s3
