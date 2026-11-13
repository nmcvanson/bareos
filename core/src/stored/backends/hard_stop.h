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

#ifndef BAREOS_STORED_BACKENDS_HARD_STOP_H_
#define BAREOS_STORED_BACKENDS_HARD_STOP_H_

#include <atomic>

namespace storagedaemon {

/* Set by a thread that runs a speculative read: a transport request of that
 * thread ends as canceled as soon as the flag is true, even while bytes move.
 * A thread that does not set it keeps the abort check of its job. */
inline thread_local const std::atomic<bool>* tls_hard_stop = nullptr;

}  // namespace storagedaemon

#endif  // BAREOS_STORED_BACKENDS_HARD_STOP_H_
