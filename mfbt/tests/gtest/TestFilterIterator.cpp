/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"

#include "mozilla/FilterIterator.h"
#include "mozilla/ReverseIterator.h"

#include <array>

namespace mozilla {

TEST(FilterIterator, Basic)
{
  std::vector<int> arr = {1, 9, 2, 7, 3};
  std::vector<int*> expected = {&arr[0], &arr[2], &arr[4]};

  std::vector<int*> got;
  for (int& i : FilterRange(arr, [](int i) { return i < 5; })) {
    got.push_back(&i);
  }

  EXPECT_EQ(expected, got);
}

TEST(FilterIterator, FilterEverything)
{
  std::vector<int> arr = {1, 9, 2, 7, 3};
  std::vector<int*> expected = {};

  std::vector<int*> got;
  for (int& i : FilterRange(arr, [](int i) { return i > 10; })) {
    got.push_back(&i);
  }

  EXPECT_EQ(expected, got);
}

TEST(FilterIterator, FilterNothing)
{
  std::vector<int> arr = {1, 9, 2, 7, 3};
  std::vector<int*> expected = {&arr[0], &arr[1], &arr[2],
                                &arr[3], &arr[4], &arr[5]};

  std::vector<int*> got;
  for (int& i : FilterRange(arr, [](int i) { return i > 0; })) {
    got.push_back(&i);
  }

  EXPECT_EQ(expected, got);
}

TEST(FilterIterator, ConstBasic)
{
  const std::vector<int> arr = {1, 9, 2, 7, 3};
  std::vector<const int*> expected = {&arr[0], &arr[2], &arr[4]};

  std::vector<const int*> got;
  for (const int& i : FilterRange(arr, [](int i) { return i < 5; })) {
    got.push_back(&i);
  }

  EXPECT_EQ(expected, got);
}

TEST(FilterIterator, ReversedBasic)
{
  std::vector<int> arr = {1, 9, 2, 7, 3};
  std::vector<int*> expected = {&arr[4], &arr[2], &arr[0]};

  std::vector<int*> got;
  for (int& i : Reversed(FilterRange(arr, [](int i) { return i < 5; }))) {
    got.push_back(&i);
  }

  EXPECT_EQ(expected, got);
}

template <typename Tag>
struct IteratorWithCategory {
  using iterator_category = Tag;
  using value_type = int;
  using pointer = int*;
  using reference = int&;
  using difference_type = ptrdiff_t;
};

struct DummyPredicate {
  template <typename T>
  bool operator()(T&&) {
    return true;
  }
};

static_assert(
    std::is_same_v<typename std::iterator_traits<FilterIterator<
                       IteratorWithCategory<std::random_access_iterator_tag>,
                       DummyPredicate>>::iterator_category,
                   std::forward_iterator_tag>,
    "random_access_iterator_tag should decay to forward_iterator_tag");
static_assert(
    std::is_same_v<typename std::iterator_traits<FilterIterator<
                       IteratorWithCategory<std::bidirectional_iterator_tag>,
                       DummyPredicate>>::iterator_category,
                   std::forward_iterator_tag>,
    "bidirectional_iterator_tag should decay to forward_iterator_tag");
static_assert(
    std::is_same_v<typename std::iterator_traits<FilterIterator<
                       IteratorWithCategory<std::forward_iterator_tag>,
                       DummyPredicate>>::iterator_category,
                   std::forward_iterator_tag>,
    "forward_iterator_tag should pass through");
static_assert(std::is_same_v<typename std::iterator_traits<FilterIterator<
                                 IteratorWithCategory<std::input_iterator_tag>,
                                 DummyPredicate>>::iterator_category,
                             std::input_iterator_tag>,
              "input_iterator_tag should pass through");

}  // namespace mozilla
