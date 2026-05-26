/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_FutureUtils_h
#define mozilla_FutureUtils_h

#include <tuple>
#include <utility>
#include <variant>

#include "mozilla/Future.h"
#include "mozilla/Span.h"
#include "nsTArray.h"

namespace mozilla {

/**
 * Immediately construct an already-resolved future from a value, a
 * [Shared]Future, or a FutureResult.
 *
 * See also the `std::in_place` constructors on Future<T>.
 */
template <class T>
auto FutureResolve(T&& aValue) -> Future<StripFuture<T>> {
  if constexpr (IsFuture<T> || IsFutureResult<T>) {
    auto [future, resolver] = Future<StripFuture<T>>::Create();
    resolver.Resolve(std::forward<T>(aValue));
    return std::move(future);
  } else {
    return Future<StripFuture<T>>{std::in_place, std::forward<T>(aValue)};
  }
}

inline Future<void> FutureResolve() { return Future<void>(std::in_place); }

namespace future_detail {

// Helper for handling `FutureAll` and its overloads.
// This helper struct will resolve `mResolver` when the destructor is fired.
template <class T>
struct FutureAllState final {
  explicit FutureAllState(FutureResolver<T> aResolver)
      : mResolver(std::move(aResolver)) {}

  // Each outstanding future should write the read-in value to their exclusive
  // element in mValue before dropping their reference to the state. Once the
  // state is dropped, the future will be resolved.
  ~FutureAllState() { mResolver.Resolve(std::move(mValue)); }

  FutureResolver<T> mResolver;
  T mValue{};
};

}  // namespace future_detail

/**
 * Wait for all of argument futures to settle, and return a single Future with a
 * tuple of the results from all futures.
 */
template <IsFuture... Ts, class R = std::tuple<typename Ts::result_type...>>
auto FutureAll(Ts&&... aFutures) -> Future<R> {
  auto [future, resolver] = Future<R>::Create();
  auto state =
      std::make_shared<future_detail::FutureAllState<R>>(std::move(resolver));
  [&]<size_t... Is>(std::index_sequence<Is...>) {
    // SAFETY: The 'ThenSync' callback is only ever called once, so this is the
    // only reader/writer for each FutureResult until 'state' is destroyed.
    (aFutures.UnsafeThenSync("FutureAll",
                             [state](auto aValue) {
                               std::get<Is>(state->mValue) = std::move(aValue);
                             }),
     ...);
  }(std::index_sequence_for<Ts...>{});
  return std::move(future);
}

/**
 * Wait for all of the argument futures to settle, and return a single Future
 * with an array of the results from all futures.
 */
template <IsFuture T, class R = nsTArray<typename T::result_type>>
auto FutureAll(const Span<T>& aFutures) -> Future<R> {
  auto [future, resolver] = Future<R>::Create();
  auto state =
      std::make_shared<future_detail::FutureAllState<R>>(std::move(resolver));
  state->mValue.AppendElements(aFutures.Length());
  for (size_t i = 0; i < aFutures.Length(); ++i) {
    // SAFETY: The 'ThenSync' callback is only ever called once, so this is the
    // only reader/writer for each FutureResult until 'state' is destroyed.
    aFutures[i].UnsafeThenSync("FutureAll", [i, state](auto aValue) {
      state->mValue[i] = std::move(aValue);
    });
  }
  return std::move(future);
}

/**
 * Wait for one of the argument futures to settle, and return the result from
 * that future.
 */
template <IsFuture... Ts, class R = std::variant<typename Ts::result_type...>>
auto FutureRace(Ts&&... aFutures) -> Future<R> {
  static_assert(sizeof...(Ts) > 0, "FutureRace requires at least one future");

  auto [future, resolver] = Future<R>::Create();
  [&]<size_t... Is>(std::index_sequence<Is...>) {
    (aFutures.UnsafeThenSync("FutureRace",
                             [resolver](auto aValue) mutable {
                               resolver.Emplace(std::in_place_index<Is>,
                                                std::move(aValue));
                             }),
     ...);
  }(std::index_sequence_for<Ts...>{});
  return std::move(future);
}

/**
 * Wait for one of the argument futures to settle, and return the result from
 * that future, alongside the index of the settled future in aFutures.
 */
template <IsFuture T, class R = std::pair<size_t, typename T::result_type>>
auto FutureRace(Span<T>& aFutures) -> Future<R> {
  if (aFutures.IsEmpty()) {
    return {};  // Return an invalid future if we have no entries.
  }

  auto [future, resolver] = Future<R>::Create();
  for (size_t i = 0; i < aFutures.Length(); ++i) {
    aFutures[i].UnsafeThenSync(
        "FutureRace", [i, resolver](auto aValue) mutable {
          resolver.Resolve(std::pair{i, std::move(aValue)});
        });
  }
  return std::move(future);
}

/**
 * Run a task on the given event target, converting the return value into a
 * Future which is returned.
 */
template <std::invocable F, class R = StripFuture<std::invoke_result_t<F>>>
auto FutureSpawnOn(StaticString aName, nsIEventTarget* aTarget, F aCallback,
                   nsIEventTarget::DispatchFlags aFlags = NS_DISPATCH_NORMAL)
    -> Future<R> {
  auto [future, resolver] = Future<R>::Create();
  nsresult rv = aTarget ? aTarget->Dispatch(
                              NS_NewRunnableFunction(
                                  aName,
                                  [resolver = std::move(resolver),
                                   callback = std::move(aCallback)]() mutable {
                                    resolver.Resolve(std::invoke(callback));
                                  }),
                              aFlags)
                        : NS_ERROR_INVALID_ARG;
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return Future<R>{};
  }
  return std::move(future);
}

}  // namespace mozilla

#endif  // mozilla_FutureUtils_h
