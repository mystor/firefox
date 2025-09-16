/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_FilterRange_h
#define mozilla_FilterRange_h

#include "mozilla/Attributes.h"

#include <iterator>
#include <type_traits>
#include <utility>

namespace mozilla {

/// Sentinel object used as the end sentinel for a FilterIterator iterator
/// range, allowing range-based for loops using FilterIterator to be implemented
/// efficiently.
class FilterSentinel {};

// Iterator type which filters entries based on a provided predicate from an
// underlying range.
//
// IMPLEMENTATION NOTE: Unlike c++20's std::ranges::filter_view, this type
// avoids holding any reference back to the original view other than an iterator
// and end sentinel. This makes it more useful for implementing iterators on
// other types which need to perform filtering.
template <typename Iterator, typename Predicate, typename Sentinel = Iterator>
class FilterIterator {
 public:
  // Limit the iterator category to either forward or input iteration based on
  // the base iterator.
  //
  // Notably, FilterIterator is not bidirectional, so reversing should happen
  // before wrapping the iterator in FilterIterator. This was not implemented,
  // as operator--() would be expensive (as it needs to perform filtering), and
  // is called by every std::reverse_iterator access.
  using iterator_category =
      std::conditional_t<std::is_base_of_v<std::forward_iterator_tag,
                                           typename std::iterator_traits<
                                               Iterator>::iterator_category>,
                         std::forward_iterator_tag, std::input_iterator_tag>;
  using value_type = typename std::iterator_traits<Iterator>::value_type;
  using difference_type =
      typename std::iterator_traits<Iterator>::difference_type;
  using pointer = typename std::iterator_traits<Iterator>::pointer;
  using reference = typename std::iterator_traits<Iterator>::reference;

  // Iterators are required to be default constructible in an invalid state.
  FilterIterator() = default;

  FilterIterator(const FilterIterator&) = default;
  FilterIterator(FilterIterator&&) = default;

  FilterIterator& operator=(const FilterIterator&) = default;
  FilterIterator& operator=(FilterIterator&&) = default;

  // Construct a FilterIterator given a range (as specified by an iterator and
  // end guard). The constructed iterator should be used as the "begin"
  // iterator, with a "FilterSentinel" sentinel used as an end iterator.
  //
  // NOTE: If a uniform iterator is required (i.e. one with a matching iterator
  // and end type), the "end" iterator may also be constructed using this type,
  // by passing the underlying "end" iterator as both aIter and aEnd.
  FilterIterator(Iterator aIter, Sentinel aEnd, Predicate aPredicate)
      : mIter(std::move(aIter)),
        mEnd(std::move(aEnd)),
        mPredicate(std::move(aPredicate)) {
    while (mIter != mEnd && !mPredicate(*mIter)) {
      ++mIter;
    }
  }

  const Iterator& base() const& { return mIter; }
  Iterator&& base() && { return std::move(mIter); }

  bool operator==(const FilterIterator& aRhs) const {
    return mIter == aRhs.mIter;
  }
  bool operator!=(const FilterIterator& aRhs) const {
    return mIter != aRhs.mIter;
  }

  bool operator==(const FilterSentinel&) const { return mIter == mEnd; }
  bool operator!=(const FilterSentinel&) const { return mIter != mEnd; }

  auto operator->() const { return mIter.operator->(); }
  reference operator*() const { return mIter.operator*(); }

  FilterIterator& operator++() {
    // Advance past the current entry, then continue advancing until we find an
    // unfiltered item.
    do {
      ++mIter;
    } while (mIter != mEnd && !mPredicate(*mIter));
    return *this;
  }
  FilterIterator& operator++(int) {
    FilterIterator tmp = *this;
    ++*this;
    return tmp;
  }

 private:
  Iterator mIter;
  MOZ_NO_UNIQUE_ADDRESS Sentinel mEnd;
  MOZ_NO_UNIQUE_ADDRESS Predicate mPredicate;
};

// Minimal range adapter which filters entries based on a provided predicate
// from an underlying range.
//
// NOTE: The end guards returned by FilterIterator use a sentinel, meaning that
// this type cannot be used with some STL algorithms. It is posisble to create
// uniform iterator types using `FilterIterator` directly, but that wasn't done
// here as this approach is slightly more efficient in the case of for loops.
template <typename Range, typename Predicate>
class FilterRange {
 private:
  using base_iterator = decltype(std::begin(std::declval<Range&>()));
  using base_sentinel = decltype(std::end(std::declval<Range&>()));
  using base_reverse_iterator = decltype(std::rbegin(std::declval<Range&>()));
  using base_reverse_sentinel = decltype(std::rend(std::declval<Range&>()));

 public:
  using iterator = FilterIterator<base_iterator, Predicate, base_sentinel>;
  using const_iterator = iterator;
  using reverse_iterator =
      FilterIterator<base_reverse_iterator, Predicate, base_reverse_sentinel>;
  using const_reverse_iterator = reverse_iterator;

  FilterRange(Range& aRange, Predicate aPredicate)
      : mRange(aRange), mPredicate(std::move(aPredicate)) {}

  // iterator getters
  iterator begin() const {
    return iterator{std::begin(mRange), std::end(mRange), mPredicate};
  }
  const_iterator cbegin() const { return begin(); }
  FilterSentinel end() const { return {}; }
  FilterSentinel cend() const { return end(); }
  reverse_iterator rbegin() const {
    return reverse_iterator{std::rbegin(mRange), std::rend(mRange), mPredicate};
  }
  const_reverse_iterator crbegin() const { return rbegin(); }
  FilterSentinel rend() const { return {}; }
  FilterSentinel crend() const { return rend(); }

 private:
  Range& mRange;
  MOZ_NO_UNIQUE_ADDRESS Predicate mPredicate;
};

}  // namespace mozilla

#endif  // mozilla_FilterRange_h
