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

#include "gtest/gtest.h"

#include "dird/configure_edit.h"

using directordaemon::CheckManagedResourceText;
using directordaemon::kManagedMarker;
using directordaemon::SetDirective;

namespace {

const std::string kJob = std::string(kManagedMarker) + "\n" +
    "# demo job\n"
    "Job {\n"
    "  Name = \"web-1\"   # the name\n"
    "\n"
    "  Type = Backup\n"
    "  Storage = File2,\n"
    "            File1a\n"
    "  # keep this comment\n"
    "  RunScript {\n"
    "    Command = \"echo\"\n"
    "    Storage = Nested\n"
    "  }\n"
    "  Messages = Standard\n"
    "}\n";

struct Result {
  bool ok;
  std::string out, old, err;
};

Result Set(const std::string& text, const char* item, const std::string& value)
{
  Result r;
  r.ok = SetDirective(text, item, value, r.out, r.old, r.err);
  return r;
}

std::string Body(const std::string& body)
{
  return std::string(kManagedMarker) + "\nJob {\n  Name = j\n" + body + "}\n";
}

}  // namespace

TEST(ConfigureEdit, CheckAccepts)
{
  std::string err;
  EXPECT_TRUE(CheckManagedResourceText(kJob, "Job", "web-1", err)) << err;
  EXPECT_TRUE(CheckManagedResourceText(kJob, "job", "web-1", err)) << err;
}

TEST(ConfigureEdit, CheckRefuses)
{
  std::string err;
  EXPECT_FALSE(CheckManagedResourceText(kJob, "Job", "other", err));
  EXPECT_FALSE(CheckManagedResourceText(kJob, "Pool", "web-1", err));
  EXPECT_FALSE(CheckManagedResourceText(kJob + kJob.substr(kJob.find('\n') + 1),
                                        "Job", "web-1", err));
  EXPECT_FALSE(CheckManagedResourceText("", "Job", "web-1", err));
  // no marker
  EXPECT_FALSE(CheckManagedResourceText(
      kJob.substr(kJob.find('\n') + 1), "Job", "web-1", err));
  EXPECT_NE(err.find("managed"), std::string::npos);
  // marker not on the first line
  EXPECT_FALSE(
      CheckManagedResourceText("\n" + kJob, "Job", "web-1", err));
}

TEST(ConfigureEdit, ReplaceKeepsEverythingElse)
{
  Result r = Set(kJob, "Type", "Restore");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.old, "Backup");
  std::string expect = kJob;
  expect.replace(expect.find("  Type = Backup\n"), 16, "  Type = Restore\n");
  EXPECT_EQ(r.out, expect);
}

TEST(ConfigureEdit, InsertBeforeClosingBrace)
{
  Result r = Set(kJob, "StorageGroupPolicy", "LeastUsed");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.old, "");
  std::string expect = kJob;
  expect.insert(expect.size() - 2, "  StorageGroupPolicy = LeastUsed\n");
  EXPECT_EQ(r.out, expect);
}

TEST(ConfigureEdit, KeywordVariants)
{
  for (const char* kw : {"Storage Group Policy", "StorageGroupPolicy",
                         "storagegrouppolicy", "STORAGE GROUP POLICY"}) {
    Result r = Set(Body(std::string("  ") + kw + " = ListedOrder\n"),
                   "StorageGroupPolicy", "LeastUsed");
    ASSERT_TRUE(r.ok) << kw << ": " << r.err;
    EXPECT_EQ(r.old, "ListedOrder") << kw;
    EXPECT_EQ(r.out, Body("  StorageGroupPolicy = LeastUsed\n")) << kw;
  }
  // an underscore does not match, as in the parser: the directive is inserted
  Result r = Set(Body("  Storage_Group_Policy = x\n"), "StorageGroupPolicy",
                 "LeastUsed");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.old, "");
}

TEST(ConfigureEdit, ContinuedValueIsOneDirective)
{
  Result r = Set(kJob, "Storage", "File3");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.old, "File2, File1a");
  std::string expect = kJob;
  size_t b = expect.find("  Storage = File2,");
  size_t e = expect.find("  # keep this");
  expect.replace(b, e - b, "  Storage = File3\n");
  EXPECT_EQ(r.out, expect);
  EXPECT_NE(r.out.find("    Storage = Nested\n"), std::string::npos);
}

TEST(ConfigureEdit, ListValueAndQuotedValue)
{
  Result r = Set(kJob, "Storage", "File1a,\"File 2\"");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_NE(r.out.find("  Storage = File1a,\"File 2\"\n"), std::string::npos);
  r = Set(Body("  Description = \"a b\"\n"), "Description", "\"c d\"");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.old, "\"a b\"");
}

TEST(ConfigureEdit, CommentsAndBlankLinesStayIdentical)
{
  Result r = Set(kJob, "Type", "Restore");
  ASSERT_TRUE(r.ok);
  EXPECT_NE(r.out.find("# demo job\n"), std::string::npos);
  EXPECT_NE(r.out.find("  Name = \"web-1\"   # the name\n\n"),
            std::string::npos);
  EXPECT_NE(r.out.find("  # keep this comment\n"), std::string::npos);
}

TEST(ConfigureEdit, NestedBlockUntouched)
{
  Result r = Set(kJob, "Command", "x");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.old, "");
  EXPECT_NE(r.out.find("    Command = \"echo\"\n"), std::string::npos);
  EXPECT_NE(r.out.find("  Command = x\n}\n"), std::string::npos);
}

TEST(ConfigureEdit, BracesInQuotesAndCommentsIgnored)
{
  std::string text = Body("  Description = \"{ not a block\"  # a } here\n");
  Result r = Set(text, "Type", "Backup");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.out, text.substr(0, text.size() - 2) + "  Type = Backup\n}\n");
}

TEST(ConfigureEdit, Refusals)
{
  const std::string m = std::string(kManagedMarker) + "\n";
  // two resources
  EXPECT_FALSE(Set(kJob + kJob.substr(m.size()), "Type", "x").ok);
  // unbalanced brace / quote
  EXPECT_FALSE(Set(m + "Job {\n  Name = j\n", "Type", "x").ok);
  EXPECT_FALSE(Set(m + "Job {\n  Name = j\n}\n}\n", "Type", "x").ok);
  EXPECT_FALSE(Set(Body("  Description = \"abc\n"), "Type", "x").ok);
  // two directives on a line, include, backslash in the old value
  EXPECT_FALSE(Set(Body("  Type = a; Level = b\n"), "Level", "x").ok);
  EXPECT_FALSE(Set(Body("  @other.conf\n"), "Type", "x").ok);
  EXPECT_FALSE(Set(Body("  Description = \"a\\\"b\"\n"), "Description",
                   "\"x\"").ok);
  // duplicate directive
  EXPECT_FALSE(Set(Body("  Type = a\n  Type = b\n"), "Type", "x").ok);
  // block on one line
  EXPECT_FALSE(Set(m + "Job { Name = j }\n", "Type", "x").ok);
  // new value forms
  EXPECT_FALSE(Set(kJob, "Type", "").ok);
  EXPECT_FALSE(Set(kJob, "Type", "a;b").ok);
  EXPECT_FALSE(Set(kJob, "Type", "@file").ok);
  EXPECT_FALSE(Set(kJob, "Type", "a\x5c" "b").ok);
  EXPECT_FALSE(Set(kJob, "Type", "\"a").ok);
  EXPECT_FALSE(Set(kJob, "Type", "a\nb").ok);
  EXPECT_FALSE(Set(kJob, "Type", "A,B").ok);  // list only for Storage
}

TEST(ConfigureEdit, CrlfRefused)
{
  Result r = Set(std::string(kManagedMarker) + "\r\nJob {\r\n  Name = j\r\n}\r\n",
                 "Type", "Backup");
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.err.find("CRLF"), std::string::npos);
}
