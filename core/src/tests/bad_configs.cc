/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2022-2024 Bareos GmbH & Co. KG

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

#include "testing_dir_common.h"

#include "dird/ua.h"
#include "include/jcr.h"
#include "dird/ua_configure.cc"

TEST(BadConfig, changing_pw_type)
{
  InitDirGlobals();
  std::string path_to_config
      = std::string("configs/bad_configs/changing_pw_type.conf");

  auto* parser = directordaemon::InitDirConfig(path_to_config.c_str(), M_INFO);

  ASSERT_NE(parser, nullptr);
  directordaemon::my_config = parser; /* set the director global variable */

  EXPECT_FALSE(parser->ParseConfig());

  delete parser;
}

/* A.1.3: an unknown StorageGroupPolicy must be rejected, on the Pool and on
 * the Job. Both fixtures are otherwise valid, so the policy name is the only
 * possible reason to fail. */

TEST(BadConfig, storage_group_policy_pool)
{
  InitDirGlobals();
  std::string path_to_config
      = std::string("configs/bad_configs/storage_group_policy_pool.conf");

  auto* parser = directordaemon::InitDirConfig(path_to_config.c_str(), M_INFO);

  ASSERT_NE(parser, nullptr);
  directordaemon::my_config = parser;

  EXPECT_FALSE(parser->ParseConfig());

  delete parser;
}

TEST(BadConfig, storage_group_policy_job)
{
  InitDirGlobals();
  std::string path_to_config
      = std::string("configs/bad_configs/storage_group_policy_job.conf");

  auto* parser = directordaemon::InitDirConfig(path_to_config.c_str(), M_INFO);

  ASSERT_NE(parser, nullptr);
  directordaemon::my_config = parser;

  /* Job resources are deliberately not validated by SaveResource; they are
   * validated after JobDefs have been applied, in PopulateDefs, which the
   * daemon reaches through CheckResources. So parsing succeeds and the
   * rejection happens one phase later. Pools are validated during parsing,
   * which is why the Pool case above tests ParseConfig directly. */
  EXPECT_TRUE(parser->ParseConfig());
  EXPECT_FALSE(directordaemon::PopulateDefs());

  delete parser;
}

