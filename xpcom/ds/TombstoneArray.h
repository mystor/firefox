/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_TombstoneArray_h
#define mozilla_TombstoneArray_h

#include "mozilla/FilterIterator.h"
#include "nsTArray.h"

namespace mozilla {

template <typename T>
class TombstoneArray {
 private:
  using base_iterator = typename nsTArray<T>::const_iterator;
  using base_reverse_iterator = typename nsTArray<T>::const_reverse_iterator;

  struct IsTombstonePredicate {
    bool operator()(const T& aValue) const { return IsTombstone(aValue); }
  };

 public:
  using iterator = FilterIterator<base_iterator, IsTombstonePredicate>;
  using const_iterator = iterator;
  using reverse_iterator =
      FilterIterator<base_reverse_iterator, IsTombstonePredicate>;
  using const_reverse_iterator = reverse_iterator;

  // Support move construction and assignment.
  TombstoneArray() = default;
  TombstoneArray(TombstoneArray&&) = default;
  TombstoneArray& operator=(TombstoneArray&&) = default;

  // Support for iterating over the TombstoneArray in both directions.
  iterator begin() const {
    return iterator{mEntries.begin(), mEntries.end(), IsTombstonePredicate{}};
  }
  const_iterator cbegin() const { return begin(); }
  iterator end() const {
    return iterator{mEntries.end(), mEntries.end(), IsTombstonePredicate{}};
  }
  const_iterator cend() const { return end(); }

  // NOTE: reverse_iterator is done by doing the filtering after reversing, as
  // reversing after filtering would be much less efficient (due to each access
  // performing a operator--()).
  reverse_iterator rbegin() const {
    return reverse_iterator{mEntries.rbegin(), mEntries.rend(),
                            IsTombstonePredicate{}};
  }
  const_reverse_iterator crbegin() const { return rbegin(); }
  reverse_iterator rend() const {
    return reverse_iterator{mEntries.rend(), mEntries.rend(),
                            IsTombstonePredicate{}};
  }
  const_reverse_iterator crend() const { return rend(); }

  // Construct a new element in the TombstoneArray.
  template <typename... Args>
  NotNull<T*> Emplace(Args&&... aArgs) {
    // If EmplaceBack is going to grow mEntries, first try to compact by
    // removing tombstones, in case that allows us to avoid a realloc.
    if (mEntries.Length() >= mEntries.Capacity()) {
      RemoveTombstones();
    }
    return mEntries.EmplaceBack(std::forward<Args>(aArgs)...);
  }

  // Remove the entry referenced by aIter.
  void Remove(iterator aIter) { RemoveInternal(aIter.base()); }
  void Remove(reverse_iterator aIter) {
    RemoveInternal(std::prev(aIter.base().base()));
  }

  // Remove the first entry which matches aPredicate from the TombstoneArray,
  // replacing it with a Tombstone.
  // Returns `true` if the entry was found and removed, and `false` otherwise.
  template <typename Pred>
  bool RemoveBy(Pred&& aPredicate) {
    for (reverse_iterator iter = rbegin(), end = rend(); iter != end; ++iter) {
      if (aPredicate(*iter)) {
        Remove(iter);
        return true;
      }
    }
    return false;
  }

  template <typename U>
  bool Remove(U&& aValue) {
    return RemoveBy([&](const T& entry) { return entry == aValue; });
  }

  // Update the array, removing any tombstone entries from it.
  void RemoveTombstones() { mEntries.RemoveElementsBy(IsTombstone); }

 private:
  // All current users of TombstoneArray support using `nullptr` as the
  // tombstone value, such that `IsTombstone` and `MakeTombstone` can be defined
  // in terms of assignment and/or comparison to `nullptr`.
  static bool IsTombstone(const T& aValue) { return aValue == nullptr; }

  void RemoveInternal(typename nsTArray<T>::const_iterator aIter) {
    MOZ_ASSERT(aIter.GetArray() == &mEntries,
               "Iterator isn't for this TombstoneArray?");

    if (std::next(aIter) == mEntries.end()) {
      // FIXME: This could probably be more efficient by doing a search followed
      // by a single truncate.
      mEntries.RemoveLastElement();
      while (!mEntries.IsEmpty() && IsTombstone(mEntries.LastElement())) {
        mEntries.RemoveLastElement();
      }
      return;
    }

    const_cast<T&>(*aIter) = nullptr;
    MOZ_ASSERT(IsTombstone(*aIter));
  }

  nsTArray<T> mEntries;
};

}  // namespace mozilla

#endif  // mozilla_TombstoneArray_h
