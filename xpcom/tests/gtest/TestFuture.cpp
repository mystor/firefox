/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "mozilla/Future.h"
#include "mozilla/FutureUtils.h"
#include "mozilla/gtest/MozAssertions.h"

using namespace mozilla;

TEST(Future, BasicResolve)
{
  bool ran = false;
  nsresult rv = FutureResolve(42).Then("Future::BasicResolve",
                                       [&ran](FutureResult<int> aValue) {
                                         EXPECT_FALSE(ran);
                                         EXPECT_TRUE(aValue);
                                         EXPECT_EQ(*aValue, 42);
                                         ran = true;
                                       });
  ASSERT_NS_SUCCEEDED(rv);
  ASSERT_FALSE(ran);
  NS_ProcessPendingEvents(nullptr);
  ASSERT_TRUE(ran);
}

TEST(Future, BasicResolveVoid)
{
  bool ran = false;
  nsresult rv = FutureResolve().Then("Future::BasicResolve",
                                     [&ran](FutureResult<void> aValue) {
                                       EXPECT_FALSE(ran);
                                       EXPECT_TRUE(aValue);
                                       ran = true;
                                     });
  ASSERT_NS_SUCCEEDED(rv);
  ASSERT_FALSE(ran);
  NS_ProcessPendingEvents(nullptr);
  ASSERT_TRUE(ran);
}

TEST(Future, BasicResolveShared)
{
  bool ran1 = false;
  bool ran2 = false;
  auto future = FutureResolve(42).Share();
  nsresult rv = future.Then("Future::BasicResolve",
                            [&ran1](FutureResult<const int> aValue) {
                              EXPECT_FALSE(ran1);
                              EXPECT_TRUE(aValue);
                              EXPECT_EQ(*aValue, 42);
                              ran1 = true;
                            });
  ASSERT_NS_SUCCEEDED(rv);
  rv = future.Then("Future::BasicResolve",
                   [&ran2](FutureResult<const int> aValue) {
                     EXPECT_FALSE(ran2);
                     EXPECT_TRUE(aValue);
                     EXPECT_EQ(*aValue, 42);
                     ran2 = true;
                   });
  ASSERT_NS_SUCCEEDED(rv);

  ASSERT_FALSE(ran1);
  ASSERT_FALSE(ran2);
  NS_ProcessPendingEvents(nullptr);
  ASSERT_TRUE(ran1);
  ASSERT_TRUE(ran2);
}

TEST(Future, AsyncResolve)
{
  bool ran = false;
  auto [future, resolver] = Future<int>::Create();
  future.Then("Future::AsyncResolve", [&ran](FutureResult<int> aValue) {
    EXPECT_FALSE(ran);
    EXPECT_TRUE(aValue);
    EXPECT_EQ(*aValue, 42);
    ran = true;
  });
  ASSERT_FALSE(ran);
  NS_ProcessPendingEvents(nullptr);
  ASSERT_FALSE(ran);
  resolver.Resolve(42);
  ASSERT_FALSE(ran);
  NS_ProcessPendingEvents(nullptr);
  ASSERT_TRUE(ran);
}

TEST(Future, ResolverDestroyedFails)
{
  bool ran = false;
  auto [future, resolver] = Future<int>::Create();
  future.Then("Future::ResolverDestroyedFails",
              [&ran](FutureResult<int> aValue) {
                EXPECT_FALSE(ran);
                EXPECT_FALSE(aValue);
                ran = true;
              });
  ASSERT_FALSE(ran);
  NS_ProcessPendingEvents(nullptr);
  ASSERT_FALSE(ran);
  resolver = {};  // destroy `resolver`
  ASSERT_FALSE(ran);
  NS_ProcessPendingEvents(nullptr);
  ASSERT_TRUE(ran);
}

TEST(Future, ThenChain)
{
  bool ran1 = false;
  bool ran2 = false;
  FutureResolve(42)
      .ThenChain("Future::ThenChain",
                 [&](FutureResult<int> aValue) {
                   EXPECT_FALSE(ran1);
                   EXPECT_FALSE(ran2);
                   EXPECT_TRUE(aValue);
                   EXPECT_EQ(*aValue, 42);
                   ran1 = true;
                   return 3.14f;
                 })
      .Then("Future::ThenChain", [&](FutureResult<float> aValue) {
        EXPECT_TRUE(ran1);
        EXPECT_FALSE(ran2);
        EXPECT_TRUE(aValue);
        EXPECT_EQ(*aValue, 3.14f);
        ran2 = true;
      });
  ASSERT_FALSE(ran1);
  ASSERT_FALSE(ran2);
  NS_ProcessPendingEvents(nullptr);
  ASSERT_TRUE(ran1);
  ASSERT_TRUE(ran2);
}
