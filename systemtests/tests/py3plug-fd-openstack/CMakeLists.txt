# BAREOS - Backup Archiving REcovery Open Sourced
#
# Copyright (C) 2026-2026 Bareos GmbH & Co. KG
#
# This program is Free Software; you can redistribute it and/or
# modify it under the terms of version three of the GNU Affero General Public
# License as published by the Free Software Foundation, which is
# listed in the file LICENSE.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public
# License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

get_filename_component(BASENAME ${CMAKE_CURRENT_BINARY_DIR} NAME)

if(NOT TARGET python3-fd)
  create_systemtest(
    ${SYSTEMTEST_PREFIX} ${BASENAME} DISABLED COMMENT "no python plugin"
  )
  return()
endif()

if(RUN_SYSTEMTESTS_ON_INSTALLED_FILES)
  create_systemtest(
    ${SYSTEMTEST_PREFIX} ${BASENAME} DISABLED COMMENT
    "requires source-tree Python modules"
  )
  return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
  create_systemtest(
    ${SYSTEMTEST_PREFIX} ${BASENAME} DISABLED COMMENT "python < 3.9"
  )
  return()
endif()

create_systemtest(${SYSTEMTEST_PREFIX} ${BASENAME})
set(_openstack_test_dirs "${BASENAME}")
if(ENABLE_GRPC)
  string(REPLACE "py3plug" "py3grpc" _grpc_dir "${BASENAME}")
  string(REPLACE "py3plug" "py3agrpc" _agrpc_dir "${BASENAME}")
  list(APPEND _openstack_test_dirs "${_grpc_dir}" "${_agrpc_dir}")
endif()
foreach(_test_dir IN LISTS _openstack_test_dirs)
  file(MAKE_DIRECTORY "${tests_dir}/${_test_dir}/python-modules")
  file(
    CREATE_LINK
    "${CMAKE_SOURCE_DIR}/core/src/plugins/filed/python/openstack/bareos-fd-openstack.py"
    "${tests_dir}/${_test_dir}/python-modules/bareos-fd-openstack.py"
    SYMBOLIC
  )
  file(
    CREATE_LINK
    "${CMAKE_SOURCE_DIR}/core/src/plugins/filed/python/openstack/bareos_openstack"
    "${tests_dir}/${_test_dir}/python-modules/bareos_openstack"
    SYMBOLIC
  )
endforeach()
