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
    ${SYSTEMTEST_PREFIX} ${BASENAME} DISABLED COMMENT
    "no Python 3 File Daemon plugin"
  )
  return()
endif()

if(NOT DPLCOMPAT_NATIVE_BUILT)
  create_systemtest(
    ${SYSTEMTEST_PREFIX} ${BASENAME} DISABLED COMMENT
    "dplcompat is built without the native S3 transport"
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
    ${SYSTEMTEST_PREFIX} ${BASENAME} DISABLED COMMENT "Python < 3.9"
  )
  return()
endif()

create_systemtest(${SYSTEMTEST_PREFIX} ${BASENAME} NO_GRPC)
file(MAKE_DIRECTORY "${tests_dir}/${BASENAME}/python-modules")
file(
  CREATE_LINK
  "${CMAKE_SOURCE_DIR}/core/src/plugins/filed/python/openstack/bareos-fd-openstack.py"
  "${tests_dir}/${BASENAME}/python-modules/bareos-fd-openstack.py"
  SYMBOLIC
)
file(
  CREATE_LINK
  "${CMAKE_SOURCE_DIR}/core/src/plugins/filed/python/openstack/bareos_openstack"
  "${tests_dir}/${BASENAME}/python-modules/bareos_openstack" SYMBOLIC
)
file(CREATE_LINK
     "${CMAKE_SOURCE_DIR}/systemtests/tests/dplcompat-native/s3-stub.py"
     "${tests_dir}/${BASENAME}/s3-stub.py" SYMBOLIC
)
set(_agentless_tests
    "${SYSTEMTEST_PREFIX}${BASENAME}:01-full-group"
    "${SYSTEMTEST_PREFIX}${BASENAME}:02-incremental-member-change"
    "${SYSTEMTEST_PREFIX}${BASENAME}:03-restore-across-members"
    "${SYSTEMTEST_PREFIX}${BASENAME}:04-failover"
    "${SYSTEMTEST_PREFIX}${BASENAME}:05-cancel-during-upload"
    "${SYSTEMTEST_PREFIX}${BASENAME}:07-virtualfull"
    "${SYSTEMTEST_PREFIX}${BASENAME}:setup"
    "${SYSTEMTEST_PREFIX}${BASENAME}:cleanup"
)
set_tests_properties(${_agentless_tests} PROPERTIES TIMEOUT 900)
