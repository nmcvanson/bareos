#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2026-2026 Bareos GmbH & Co. KG
#
#   This program is Free Software; you can redistribute it and/or
#   modify it under the terms of version three of the GNU Affero General Public
#   License as published by the Free Software Foundation and included
#   in the file LICENSE.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#   Affero General Public License for more details.
#
#   You should have received a copy of the GNU Affero General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
#   02110-1301, USA.

# The native S3 transport of dplcompat against a test-only stub responder (a
# Python script that keeps the objects as files); it is a test double, never a
# storage backend.
get_filename_component(BASENAME ${CMAKE_CURRENT_BINARY_DIR} NAME)
find_program(DPLCOMPAT_NATIVE_PYTHON NAMES python3)
if(NOT DPLCOMPAT_NATIVE_BUILT)
  create_systemtest(
    ${SYSTEMTEST_PREFIX} ${BASENAME} DISABLED COMMENT
    "dplcompat is built without the native S3 transport"
  )
elseif(NOT DPLCOMPAT_NATIVE_PYTHON)
  create_systemtest(
    ${SYSTEMTEST_PREFIX} ${BASENAME} DISABLED COMMENT "no python3 for the stub"
  )
else()
  create_systemtest(${SYSTEMTEST_PREFIX} ${BASENAME})
endif()
