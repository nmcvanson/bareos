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

// Byte bound of the messages a backup job holds in the storage daemon.

#include "gtest/gtest.h"
#include "stored/append_queue_bytes.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace storagedaemon;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST(append_queue_bytes, NoBoundNeverWaits)
{
  AppendQueueBytes bytes{0};
  bytes.Add(1'000'000'000);
  const auto start = Clock::now();
  EXPECT_TRUE(bytes.WaitBelowBound(1s));
  EXPECT_LT(Clock::now() - start, 100ms);
}

TEST(append_queue_bytes, BelowTheBoundGoesOnAtOnce)
{
  AppendQueueBytes bytes{1000};
  bytes.Add(999);
  const auto start = Clock::now();
  EXPECT_TRUE(bytes.WaitBelowBound(1s));
  EXPECT_LT(Clock::now() - start, 100ms);
}

TEST(append_queue_bytes, AtOrAboveTheBoundWaitsForTheTimeout)
{
  AppendQueueBytes bytes{1000};
  bytes.Add(1000);
  auto start = Clock::now();
  EXPECT_FALSE(bytes.WaitBelowBound(200ms));
  EXPECT_GE(Clock::now() - start, 200ms);

  bytes.Add(500);
  start = Clock::now();
  EXPECT_FALSE(bytes.WaitBelowBound(200ms));
  EXPECT_GE(Clock::now() - start, 200ms);
  EXPECT_EQ(bytes.Held(), 1500u);
}

TEST(append_queue_bytes, TakingAMessageWakesTheReader)
{
  AppendQueueBytes bytes{1000};
  bytes.Add(600);
  bytes.Add(600);
  std::thread consumer([&bytes] {
    std::this_thread::sleep_for(300ms);
    bytes.Remove(600);
  });
  const auto start = Clock::now();
  EXPECT_TRUE(bytes.WaitBelowBound(10s));
  const auto waited = Clock::now() - start;
  EXPECT_GE(waited, 250ms);
  EXPECT_LT(waited, 2s);
  consumer.join();
  EXPECT_EQ(bytes.Held(), 600u);
}

TEST(append_queue_bytes, ReaderStopsWaitingWhenTheJobStops)
{
  // The reader polls in slices and checks whether the job still reads.
  AppendQueueBytes bytes{1000};
  bytes.Add(2000);
  std::atomic<bool> job_stopped{false};
  std::thread stop([&job_stopped] {
    std::this_thread::sleep_for(300ms);
    job_stopped = true;
  });
  const auto start = Clock::now();
  bool below = false;
  while (!(below = bytes.WaitBelowBound(100ms)) && !job_stopped) {}
  EXPECT_FALSE(below);
  EXPECT_LT(Clock::now() - start, 1s);
  stop.join();
}

TEST(append_queue_bytes, RemovingMoreThanHeldEndsAtZero)
{
  AppendQueueBytes bytes{1000};
  bytes.Add(100);
  bytes.Remove(500);
  EXPECT_EQ(bytes.Held(), 0u);
  EXPECT_TRUE(bytes.WaitBelowBound(0ms));
}
