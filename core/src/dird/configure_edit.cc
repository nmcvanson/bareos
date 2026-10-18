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

#include "dird/configure_edit.h"

#include <cctype>
#include <vector>

namespace directordaemon {

namespace {

struct Block {
  std::string keyword;
  size_t body_begin = 0;
  size_t body_end = 0;
};

struct Line {
  size_t begin = 0;
  size_t end = 0;
  int start_depth = 1;
  bool braces = false;
  bool quote_at_start = false;
  bool quote_at_end = false;
  std::string content;  // without comment
};

struct Match {
  size_t begin;
  size_t end;
  std::string value;
};

// Keyword as the lexer builds it: letters only, blanks dropped; any other
// character makes it no identifier (empty result).
std::string KeywordOf(const std::string& s)
{
  std::string k;
  for (unsigned char c : s) {
    if (std::isalpha(c)) {
      k += static_cast<char>(std::tolower(c));
    } else if (c != ' ' && c != '\t') {
      return std::string();
    }
  }
  return k;
}

std::string Trim(const std::string& s)
{
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) { return std::string(); }
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string Unquote(const std::string& s)
{
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

// Finds the top-level blocks; braces in quotes and comments are ignored.
bool ScanBlocks(const std::string& text,
                std::vector<Block>& blocks,
                std::string& err)
{
  int depth = 0;
  bool in_quote = false;
  bool in_comment = false;
  std::string header;
  Block cur;

  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];
    if (in_comment) {
      if (c == '\n') { in_comment = false; }
      continue;
    }
    if (in_quote) {
      if (c == '\\') {
        i++;
      } else if (c == '"') {
        in_quote = false;
      }
      continue;
    }
    if (c == '#') {
      in_comment = true;
    } else if (c == '"') {
      if (depth == 0) {
        err = "unexpected text outside a resource";
        return false;
      }
      in_quote = true;
    } else if (c == '{') {
      if (depth == 0) {
        cur.keyword = KeywordOf(header);
        cur.body_begin = i + 1;
        if (cur.keyword.empty()) {
          err = "resource without a type keyword";
          return false;
        }
        header.clear();
      }
      depth++;
    } else if (c == '}') {
      if (--depth < 0) {
        err = "unbalanced braces: extra closing brace";
        return false;
      }
      if (depth == 0) {
        cur.body_end = i;
        blocks.push_back(cur);
      }
    } else if (depth == 0 && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      header += c;
    }
  }
  if (in_quote || depth != 0) {
    err = "unbalanced braces or quotes";
    return false;
  }
  if (!header.empty()) {
    err = "unexpected text outside a resource";
    return false;
  }
  return true;
}

// Splits the body into lines with depth, quote and comment state; closer_begin
// is where the line holding the closing brace starts.
bool SplitBody(const std::string& text,
               const Block& b,
               std::vector<Line>& lines,
               size_t& closer_begin,
               std::string& err)
{
  int depth = 1;
  bool in_quote = false;
  bool in_comment = false;
  Line ln;
  ln.begin = b.body_begin;

  for (size_t i = b.body_begin; i < b.body_end; i++) {
    char c = text[i];
    if (c == '\n') {
      in_comment = false;
      ln.end = i + 1;
      ln.quote_at_end = in_quote;
      lines.push_back(ln);
      ln = Line();
      ln.begin = i + 1;
      ln.start_depth = depth;
      ln.quote_at_start = in_quote;
      if (in_quote) { ln.content += ' '; }
      continue;
    }
    if (in_comment) { continue; }
    ln.content += c;
    if (in_quote) {
      if (c == '\\' && i + 1 < b.body_end && text[i + 1] != '\n') {
        ln.content += text[++i];
      } else if (c == '"') {
        in_quote = false;
      }
    } else if (c == '#') {
      ln.content.pop_back();
      in_comment = true;
    } else if (c == '"') {
      in_quote = true;
    } else if (c == '{') {
      depth++;
      ln.braces = true;
    } else if (c == '}') {
      depth--;
      ln.braces = true;
    }
  }

  closer_begin = ln.begin;
  if (!Trim(ln.content).empty() || in_quote) {
    err = "closing brace shares its line with other text";
    return false;
  }
  if (lines.empty()) {
    err = "resource body is on a single line";
    return false;
  }
  if (!Trim(lines.front().content).empty()) {
    err = "text follows the opening brace on its line";
    return false;
  }
  return true;
}

// Quoted strings replaced by a placeholder, for checks on bare text.
std::string OutsideQuotes(const std::string& s)
{
  std::string r;
  bool q = false;
  for (char c : s) {
    if (c == '"') {
      if (!q) { r += 'Q'; }
      q = !q;
    } else if (!q) {
      r += c;
    }
  }
  return r;
}

// A bare word or a double-quoted string without escapes; with list, a comma
// list of those.
bool ValueFormOk(const std::string& value, bool list)
{
  std::string v = Trim(value);
  if (v.empty() || v.find_first_of("\\@\r\n") != std::string::npos
      || OutsideQuotes(v).find_first_of(";#{}=") != std::string::npos) {
    return false;
  }
  size_t pos = 0;
  bool more = true;
  while (more) {
    while (pos < v.size() && (v[pos] == ' ' || v[pos] == '\t')) { pos++; }
    size_t start = pos;
    if (pos < v.size() && v[pos] == '"') {
      size_t e = v.find('"', pos + 1);
      if (e == std::string::npos || e == pos + 1) { return false; }
      pos = e + 1;
    } else {
      while (pos < v.size() && v[pos] != ',' && v[pos] != ' '
             && v[pos] != '\t' && v[pos] != '"') {
        pos++;
      }
      if (pos == start) { return false; }
    }
    while (pos < v.size() && (v[pos] == ' ' || v[pos] == '\t')) { pos++; }
    if (pos == v.size()) {
      more = false;
    } else if (v[pos] == ',' && list) {
      pos++;
    } else {
      return false;
    }
  }
  return true;
}

// All depth-1 directives named item; a match covers its continuation lines.
bool FindDirectives(const std::vector<Line>& lines,
                    const std::string& item,
                    std::vector<Match>& found,
                    std::string& err)
{
  const std::string want = KeywordOf(item);
  for (size_t k = 0; k < lines.size(); k++) {
    const Line& l = lines[k];
    if (l.start_depth != 1 || l.braces || l.quote_at_start
        || Trim(l.content).empty()) {
      continue;
    }
    std::string bare = OutsideQuotes(l.content);
    if (bare.find(';') != std::string::npos) {
      err = "several directives on one line are not supported";
      return false;
    }
    if (bare.find('@') != std::string::npos) {
      err = "an include (@) directive is not supported";
      return false;
    }
    size_t eq = l.content.find('=');
    bool is_match = eq != std::string::npos
                    && KeywordOf(l.content.substr(0, eq)) == want;

    std::string joined
        = eq == std::string::npos ? std::string() : Trim(l.content.substr(eq + 1));
    size_t last = k;
    while (!Trim(lines[last].content).empty()
           && Trim(lines[last].content).back() == ',') {
      if (last + 1 >= lines.size() || lines[last + 1].braces
          || lines[last + 1].start_depth != 1) {
        err = "a continued value ends inside a nested block";
        return false;
      }
      last++;
      joined += " " + Trim(lines[last].content);
    }
    if (lines[last].quote_at_end) {
      err = "multi-line quoted values are not supported";
      return false;
    }
    if (is_match) {
      if (!found.empty()) {
        err = "the directive " + item + " appears more than once";
        return false;
      }
      if (!ValueFormOk(joined, want == "storage")) {
        err = "the value of " + item + " has a form that is not supported";
        return false;
      }
      found.push_back({l.begin, lines[last].end, Trim(joined)});
    }
    k = last;
  }
  return true;
}

bool ParseSingle(const std::string& text,
                 Block& block,
                 std::vector<Line>& lines,
                 size_t& closer_begin,
                 std::string& err)
{
  if (text.find('\r') != std::string::npos) {
    err = "CRLF line endings are not supported";
    return false;
  }
  std::vector<Block> blocks;
  if (!ScanBlocks(text, blocks, err)) { return false; }
  if (blocks.size() != 1) {
    err = blocks.empty() ? "no resource in the file"
                         : "the file holds more than one resource";
    return false;
  }
  block = blocks[0];
  return SplitBody(text, block, lines, closer_begin, err);
}

}  // namespace

bool CheckManagedResourceText(const std::string& text,
                              const char* res_type,
                              const char* name,
                              std::string& err)
{
  const std::string marker = std::string(kManagedMarker) + "\n";
  if (text.compare(0, marker.size(), marker) != 0) {
    err = "not a managed file (marker line missing), edit by hand";
    return false;
  }
  Block block;
  std::vector<Line> lines;
  size_t closer;
  if (!ParseSingle(text, block, lines, closer, err)) { return false; }
  if (block.keyword != KeywordOf(res_type)) {
    err = "the resource in the file is not of type " + std::string(res_type);
    return false;
  }
  std::vector<Match> names;
  if (!FindDirectives(lines, "Name", names, err)) { return false; }
  if (names.size() != 1 || Unquote(names[0].value) != name) {
    err = "the resource in the file is not named " + std::string(name);
    return false;
  }
  return true;
}

bool SetDirective(const std::string& text,
                  const char* item_name,
                  const std::string& value,
                  std::string& out,
                  std::string& old_value,
                  std::string& err)
{
  Block block;
  std::vector<Line> lines;
  size_t closer;
  if (!ParseSingle(text, block, lines, closer, err)) { return false; }
  if (!ValueFormOk(value, KeywordOf(item_name) == "storage")) {
    err = "the new value is empty or has a form that is not supported";
    return false;
  }

  std::vector<Match> found;
  if (!FindDirectives(lines, item_name, found, err)) { return false; }

  old_value = found.empty() ? std::string() : found[0].value;
  const std::string line = "  " + std::string(item_name) + " = " + value + "\n";

  if (found.empty()) {
    out = text.substr(0, closer) + line + text.substr(closer);
  } else {
    out = text.substr(0, found[0].begin) + line + text.substr(found[0].end);
  }
  return true;
}

}  // namespace directordaemon
