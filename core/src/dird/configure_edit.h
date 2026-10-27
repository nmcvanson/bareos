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
/**
 * @file
 * Text edit of one directive in a resource file, for "configure set".
 */
#ifndef BAREOS_DIRD_CONFIGURE_EDIT_H_
#define BAREOS_DIRD_CONFIGURE_EDIT_H_

#include <string>

namespace directordaemon {

// First line of every file that "configure set" and "configure create" manage.
constexpr const char* kManagedMarker = "# managed-by: bareos-configure";

// True when text starts with the marker line and holds exactly one top-level
// resource of type res_type whose Name is name; otherwise false, reason in err.
bool CheckManagedResourceText(const std::string& text,
                              const char* res_type,
                              const char* name,
                              std::string& err);

// Sets or inserts the depth-1 directive item_name (value never empty). out
// gets the new text, old_value the previous value (empty if absent). Lexer
// forms the edit cannot keep byte-exact are refused; all other bytes stay.
bool SetDirective(const std::string& text,
                  const char* item_name,
                  const std::string& value,
                  std::string& out,
                  std::string& old_value,
                  std::string& err);

}  // namespace directordaemon

#endif  // BAREOS_DIRD_CONFIGURE_EDIT_H_
