/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Future_h
#define mozilla_Future_h

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include "mozilla/MoveOnlyFunction.h"
#include "mozilla/Mutex.h"
#include "mozilla/StaticString.h"
#include "nsIEventTarget.h"
#include "nsISupports.h"
#include "nsTArray.h"
#include "nsThreadUtils.h"

namespace mozilla {

template <class T>
class FutureResult;
template <class T>
class FutureResolver;
template <class T>
class Future;
template <class T>
class SharedFuture;

namespace future_detail {

template <class T>
class FutureKernel;

template <class T>
struct FutureTraits {
  static constexpr bool is_future = false;
  static constexpr bool is_future_result = false;
  using value_type = T;
};

template <class T>
struct FutureTraits<Future<T>> {
  static constexpr bool is_future = true;
  static constexpr bool is_future_result = false;
  using value_type = T;
};

template <class T>
struct FutureTraits<SharedFuture<T>> {
  static constexpr bool is_future = true;
  static constexpr bool is_future_result = false;
  using value_type = T;
};

template <class T>
struct FutureTraits<FutureResult<T>> {
  static constexpr bool is_future = false;
  static constexpr bool is_future_result = true;
  using value_type = std::remove_const_t<T>;
};

}  // namespace future_detail

template <class T>
concept IsFuture =
    future_detail::FutureTraits<std::remove_cvref_t<T>>::is_future;

template <class T>
concept IsFutureResult =
    future_detail::FutureTraits<std::remove_cvref_t<T>>::is_future_result;

template <class T>
using StripFuture =
    typename future_detail::FutureTraits<std::remove_cvref_t<T>>::value_type;

/**
 * Options which can be passed to [Shared]Future's `Then` methods in order to
 * customize the behaviour of the dispatched runnable.
 */
struct FutureDispatchOptions {
  uint32_t priority = nsIRunnablePriority::PRIORITY_NORMAL;
  bool mayBlock : 1 = false;
  bool disableDirectTaskDispatch : 1 = false;
};

/**
 * Container-like type for the resolved value from a Future.
 *
 * A FutureResult<T> keeps the Future's underlying storage alive, and provides
 * access if the result was successful. If the future fails for any reason, that
 * failure will be reflected on the FutureResult.
 *
 * No information about the cause of the Future failure is currently tracked,
 * but it would be available on this type if we decide to add it in the future.
 */
template <class T>
class FutureResult final {
 public:
  using value_type = T;

  FutureResult() = default;
  FutureResult(FutureResult&&) = default;
  FutureResult(const FutureResult&) = default;
  FutureResult& operator=(FutureResult&&) = default;
  FutureResult& operator=(const FutureResult&) = default;

  // Directly construct a FutureResult for an already-resolved value of the
  // given type.
  explicit FutureResult(std::in_place_t, auto&&... aArgs);

  bool isSuccess() const { return !!mKernel; }
  bool isFailure() const { return !isSuccess(); }

  explicit operator bool() const { return isSuccess(); }
  bool operator!() const { return isFailure(); }

  template <std::same_as<T> U = T>
    requires(!std::is_void_v<U>)
  U& ref() const {
    MOZ_RELEASE_ASSERT(isSuccess());
    return mKernel->ValueRef();
  }

  template <std::same_as<T> U = T>
    requires(!std::is_void_v<U>)
  U* operator->() const {
    return &ref();
  }

  template <std::same_as<T> U = T>
    requires(!std::is_void_v<U>)
  U& operator*() const {
    return ref();
  }

  // Allow implicitly adding a `const` qualifier.
  MOZ_IMPLICIT operator FutureResult<const T>() const
    requires(!std::is_const_v<T>)
  {
    return FutureResult<const T>{do_AddRef(mKernel)};
  }

  // INTERNAL - DO NOT CALL
  explicit FutureResult(
      already_AddRefed<future_detail::FutureKernel<std::remove_const_t<T>>>
          aKernel)
      : mKernel(std::move(aKernel)) {
    if (mKernel) {
      mKernel->AssertResolved();
    }
  }

 private:
  RefPtr<future_detail::FutureKernel<std::remove_const_t<T>>> mKernel;
};

/**
 * Resolver-side of a Future object.
 *
 * Can be used to provide a value on any thread which will be made available to
 * any [Shared]Future callbacks registered through those objects.
 *
 * See the header-level documentation for more details on how to use
 * FutureResolver.
 */
template <class T>
class FutureResolver final {
 public:
  FutureResolver() = default;
  FutureResolver(FutureResolver&&) = default;
  FutureResolver& operator=(FutureResolver&& aOther) = default;
  FutureResolver(const FutureResolver&) = default;
  FutureResolver& operator=(const FutureResolver&) = default;

  // Called to resolve the Future with a value of type T, constructing it
  // in-place in the Future's storage.
  bool Emplace(auto&&... aValue);

  // Resolve the Future with a value, another [Shared]Future, or a FutureResult.
  //
  // If [Shared]Future is passed, the future will remain unresolved until that
  // Future has settled.
  template <typename U>
  bool Resolve(U&& aValue)
    requires std::convertible_to<StripFuture<U>, T>;

  bool Resolve()
    requires std::is_void_v<T>;

  // INTERNAL - DO NOT CALL
  explicit FutureResolver(
      already_AddRefed<future_detail::FutureKernel<T>> aKernel)
      : mProxy(std::make_shared<Proxy>(std::move(aKernel))) {}

 private:
  // Resolvers hold their reference to the FutureKernel through a proxy, such
  // that when the last Resolver is destroyed, we can signal to the kernel to
  // mark the resolver as Failed.
  struct Proxy {
    // XXX(nika): We may want to consider making this reference weak, such that
    // the kernel's storage for T can go away if the only reference is from
    // resolvers.
    const RefPtr<future_detail::FutureKernel<T>> mKernel;
    ~Proxy() { mKernel->SetFailed(); }
  };

  std::shared_ptr<Proxy> mProxy;
};

/**
 * The `Future` type provides a mechanism to access the result of an
 * asynchronous operation.
 *
 * Note that `Future` is a unique reference to the result. To allow multiple
 * clients to listen to the future's result, call `Share()` to convert the
 * future into a `SharedFuture`.
 */
template <class T>
class Future final {
 public:
  using value_type = T;
  using result_type = FutureResult<T>;

  // Create a pair of an unresolved future, and the resolver which can be used
  // to resolve it. If FutureResolver is destroyed, the Future object will enter
  // an errored state and immediately settle.
  static std::pair<Future<T>, FutureResolver<T>> Create();

  // Create an invalid future which will never resolve with a value.
  Future() = default;
  Future(Future&&) = default;
  Future& operator=(Future&&) = default;
  Future(const Future&) = delete;
  Future& operator=(const Future&) = delete;

  // Construct an already-resolved Future in place. The given arguments will be
  // used to construct a value of type `T` which will be provided to callbacks.
  explicit Future(std::in_place_t, auto&&... aArgs);

  // Destroys this Future, moving read-side ownership over the shared kernel
  // into a new shared future, which can be `.Then`-ed multiple times.
  SharedFuture<T> Share();

  bool IsValid() const { return !!mKernel; }

  // Register `aCallback` to be invoked on `GetCurrentSerialEventTarget()` when
  // the future is settled.
  // If specified, `aOptions` can be used to customize the runnable dispatched
  // to run `aCallback`.
  nsresult Then(StaticString aName,
                std::invocable<FutureResult<T>> auto aCallback,
                FutureDispatchOptions aOptions = {}) {
    return ThenOn(aName, GetCurrentSerialEventTarget(), std::move(aCallback),
                  aOptions);
  }

  // Like `Then`, but allows specifying an arbitrary `nsIEventTarget`.
  nsresult ThenOn(StaticString aName, nsIEventTarget* aTarget,
                  std::invocable<FutureResult<T>> auto aCallback,
                  FutureDispatchOptions aOptions = {});

  // Like `Then`, but the return value from the invoked callback is wrapped into
  // a new future, which is returned from this method, allowing for future
  // chaining.
  template <std::invocable<FutureResult<T>> F,
            class R = StripFuture<std::invoke_result_t<F, FutureResult<T>>>>
  auto ThenChain(StaticString aName, F aCallback,
                 FutureDispatchOptions aOptions = {}) -> Future<R> {
    return ThenChainOn(aName, GetCurrentSerialEventTarget(),
                       std::move(aCallback), aOptions);
  }

  // Like `ThenChain`, but allows specifying an arbitrary `nsIEventTarget`.
  template <std::invocable<FutureResult<T>> F,
            class R = StripFuture<std::invoke_result_t<F, FutureResult<T>>>>
  auto ThenChainOn(StaticString aName, nsIEventTarget* aTarget, F aCallback,
                   FutureDispatchOptions aOptions = {}) -> Future<R> {
    auto [fut, res] = Future<R>::Create();
    (void)ThenOn(
        aName, aTarget,
        [res = std::move(res),
         callback = std::move(aCallback)](FutureResult<T> aValue) mutable {
          if constexpr (std::is_void_v<
                            std::invoke_result_t<F, FutureResult<T>>>) {
            std::invoke(callback, aValue);
            res.Resolve();
          } else {
            res.Resolve(std::invoke(callback, aValue));
          }
        },
        aOptions);
    return std::move(fut);
  }

  // Like `ThenChain`, but the callback is not invoked if the future fails.
  // Instead, the returned future fails.
  template <std::same_as<T> U = T, std::invocable<U&&> F,
            class R = StripFuture<std::invoke_result_t<F, U&&>>>
    requires(!std::is_void_v<T>)
  auto ThenMap(StaticString aName, F aCallback,
               FutureDispatchOptions aOptions = {}) -> Future<R> {
    return ThenMapOn(aName, GetCurrentSerialEventTarget(), std::move(aCallback),
                     aOptions);
  }

  // Like `ThenMap`, but allows specifying an arbitrary `nsIEventTarget`.
  template <std::same_as<T> U = T, std::invocable<U&&> F,
            class R = StripFuture<std::invoke_result_t<F, U&&>>>
    requires(!std::is_void_v<T>)
  auto ThenMapOn(StaticString aName, nsIEventTarget* aTarget, F aCallback,
                 FutureDispatchOptions aOptions = {}) -> Future<R> {
    auto [fut, res] = Future<R>::Create();
    (void)ThenOn(
        aName, aTarget,
        [res = std::move(res),
         callback = std::move(aCallback)](FutureResult<T> aValue) mutable {
          if (aValue) {
            res.Resolve(std::invoke(callback, std::move(*aValue)));
          }
        },
        aOptions);
    return std::move(fut);
  }

  // Check if this Future has already been settled.
  // (i.e. calling `Then` will immediately dispatch the callback)
  bool IsSettled() const { return !mKernel || mKernel->IsSettled(); }

  // Immediately extract the result from this settled future.
  // NOTE: Can only be called if `IsSettled()`.
  FutureResult<T> ToResult();

  // Add a callback to be invoked synchronously when the future is settled.
  //
  // If the future is already settled, the callback will be immediately called.
  void UnsafeThenSync(StaticString aName,
                      std::invocable<FutureResult<T>> auto aCallback);

  // INTERNAL - DO NOT CALL
  explicit Future(already_AddRefed<future_detail::FutureKernel<T>> aKernel)
      : mKernel(std::move(aKernel)) {}

 private:
  // SAFETY NOTE: This is an exclusive future. So long as this future is held by
  // a Future<T>, it must be the only reference within a Future<T> or
  // SharedFuture<T>, and no caller may have registered a waker on it.
  RefPtr<future_detail::FutureKernel<T>> mKernel;
};

//
// MARK: SharedFuture
//

/**
 * The `SharedFuture` type provides a mechanism to access the result of an
 * asynchronous operation.
 *
 * Unlike `Future`, `SharedFuture` is not a unique reference, meaning multiple
 * clients can listen to the future's result, and the object can be cheaply
 * copied.
 */
template <class T>
class SharedFuture final {
 public:
  using value_type = const T;
  using result_type = FutureResult<const T>;

  static std::pair<SharedFuture<T>, FutureResolver<T>> Create();

  SharedFuture() = default;
  SharedFuture(SharedFuture&&) = default;
  SharedFuture& operator=(SharedFuture&&) = default;
  SharedFuture(const SharedFuture&) = default;
  SharedFuture& operator=(const SharedFuture&) = default;

  // Construct an already-resolved SharedFuture in place. The given arguments
  // will be used to construct a value of type `T` which will be provided to
  // callbacks.
  explicit SharedFuture(std::in_place_t, auto&&... aArgs);

  bool IsValid() const { return !!mKernel; }

  // Register `aCallback` to be invoked on `GetCurrentSerialEventTarget()` when
  // the future is settled.
  // If specified, `aOptions` can be used to customize the runnable dispatched
  // to run `aCallback`.
  nsresult Then(StaticString aName,
                std::invocable<FutureResult<const T>> auto aCallback,
                FutureDispatchOptions aOptions = {}) const {
    return ThenOn(aName, GetCurrentSerialEventTarget(), std::move(aCallback),
                  aOptions);
  }

  // Like `Then`, but allows specifying an arbitrary `nsIEventTarget`.
  nsresult ThenOn(StaticString aName, nsIEventTarget* aTarget,
                  std::invocable<FutureResult<const T>> auto aCallback,
                  FutureDispatchOptions aOptions = {}) const;

  // Like `Then`, but the return value from the invoked callback is wrapped into
  // a new future, which is returned from this method, allowing for future
  // chaining.
  template <
      std::invocable<FutureResult<const T>> F,
      class R = StripFuture<std::invoke_result_t<F, FutureResult<const T>>>>
  auto ThenChain(StaticString aName, F aCallback,
                 FutureDispatchOptions aOptions = {}) const -> Future<R> {
    return ThenChainOn(aName, GetCurrentSerialEventTarget(),
                       std::move(aCallback), aOptions);
  }

  // Like `ThenChain`, but allows specifying an arbitrary `nsIEventTarget`.
  template <
      std::invocable<FutureResult<const T>> F,
      class R = StripFuture<std::invoke_result_t<F, FutureResult<const T>>>>
  auto ThenChainOn(StaticString aName, nsIEventTarget* aTarget, F aCallback,
                   FutureDispatchOptions aOptions = {}) const -> Future<R> {
    auto [fut, res] = Future<R>::Create();
    (void)ThenOn(
        aName, aTarget,
        [res = std::move(res), callback = std::move(aCallback)](
            FutureResult<const T> aValue) mutable {
          if constexpr (std::is_void_v<
                            std::invoke_result_t<F, FutureResult<const T>>>) {
            std::invoke(callback, aValue);
            res.Resolve();
          } else {
            res.Resolve(std::invoke(callback, aValue));
          }
        },
        aOptions);
    return std::move(fut);
  }

  // Like `ThenChain`, but the callback is not invoked if the future fails.
  // Instead, the returned future fails.
  template <std::invocable<const T&> F,
            class R = StripFuture<std::invoke_result_t<F, const T&>>>
    requires(!std::is_void_v<T>)
  auto ThenMap(StaticString aName, F aCallback,
               FutureDispatchOptions aOptions = {}) const -> Future<R> {
    return ThenMapOn(aName, GetCurrentSerialEventTarget(), std::move(aCallback),
                     aOptions);
  }

  // Like `ThenMap`, but allows specifying an arbitrary `nsIEventTarget`.
  template <std::invocable<const T&> F,
            class R = StripFuture<std::invoke_result_t<F, const T&>>>
    requires(!std::is_void_v<T>)
  auto ThenMapOn(StaticString aName, nsIEventTarget* aTarget, F aCallback,
                 FutureDispatchOptions aOptions = {}) const -> Future<R> {
    auto [fut, res] = Future<R>::Create();
    (void)ThenOn(
        aName, aTarget,
        [res = std::move(res), callback = std::move(aCallback)](
            FutureResult<const T> aValue) mutable {
          if (aValue) {
            res.Resolve(std::invoke(callback, *aValue));
          }
        },
        aOptions);
    return std::move(fut);
  }

  // Check if this Future has already been settled.
  // (i.e. calling `Then` will immediately dispatch the callback)
  bool IsSettled() const { return !mKernel || mKernel->IsSettled(); }

  // Immediately extract a result from this settled future.
  // NOTE: Can only be called if `IsSettled()`.
  FutureResult<const T> ToResult() const;

  // Add a callback to be invoked synchronously when the future is settled.
  //
  // If the future is already settled, the callback will be immediately called.
  void UnsafeThenSync(
      StaticString aName,
      std::invocable<FutureResult<const T>> auto aCallback) const;

  // INTERNAL - DO NOT CALL
  explicit SharedFuture(
      already_AddRefed<future_detail::FutureKernel<T>> aKernel)
      : mKernel(std::move(aKernel)) {}

 private:
  RefPtr<future_detail::FutureKernel<T>> mKernel;
};

////////////////////
// Inline Methods //
////////////////////

namespace future_detail {

class FutureKernelBase;

// Interface used by FutureWaker implementations.
// NOTE: This is an internal interface.
class IFutureWaker : public nsISupports {
 public:
  virtual void Wake(FutureKernelBase* aKernel) = 0;
};

class FutureKernelBase {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(FutureKernelBase);

  void SetFailed();

  bool IsSettled();

  bool IsResolved();

 protected:
  enum class State : uint8_t {
    Unresolved,
    Resolving,
    Resolved,
    Failed,
  };

  explicit FutureKernelBase(State aState = State::Unresolved)
      : mState(aState) {}

  [[nodiscard]] bool BeginResolving();
  void FinishResolving();

  // Register a waker to be invoked when the FutureKernel is resolved.
  void AddWaker(IFutureWaker* aWaker);

  nsresult AddDispatchWaker(
      StaticString aName, nsIEventTarget* aTarget,
      FutureDispatchOptions aOptions,
      MoveOnlyFunction<void(FutureKernelBase*)> aCallback);

  void AddSyncWaker(StaticString aName,
                    MoveOnlyFunction<void(FutureKernelBase*)> aCallback);

  virtual ~FutureKernelBase() = default;

  Mutex mMutex{"FutureKernel"};
  State mState MOZ_GUARDED_BY(mMutex);
  // NOTE: This can probably be an invasive singly linked list, as we don't
  // really remove elements from the list, and we can run them in any order.
  nsTArray<RefPtr<IFutureWaker>> mWakers MOZ_GUARDED_BY(mMutex);
};

template <class T>
struct FutureKernelStorage {
  FutureKernelStorage() {}
  ~FutureKernelStorage() {}

  template <class... Us>
  void Construct(Us&&... aArgs) {
    new (&mValue) T(std::forward<Us>(aArgs)...);
  }
  void Destroy() { mValue.~T(); }

  // The resolved value within this FutureKernel.
  // It is only safe to initialize this while State::Resolving (as set by the
  // current thread), and only safe to access otherwise if State::Resolved, and
  // in a callback.
  union {
    T mValue;
  };
};

template <>
struct FutureKernelStorage<void> {
  FutureKernelStorage() = default;

  void Construct() {}
  void Destroy() {}
};

// Core shared object representing the state of a Future.
//
// !!! **NEVER** use a FutureKernel directly !!!
//
// This is an internal implementation detail with public guts, and is trivial to
// cause UAFs/races with if used directly.
//
// A reference to a FutureKernel is the core of all Future types, as the
// FutureKernel holds all state relevant to that future.
template <class T>
class FutureKernel final : public FutureKernelBase,
                           private FutureKernelStorage<T> {
 public:
  FutureKernel() : FutureKernelBase(State::Unresolved) {}

  explicit FutureKernel(std::in_place_t, auto&&... aArgs)
      : FutureKernelBase(State::Resolved) {
    this->Construct(std::forward<decltype(aArgs)>(aArgs)...);
  }

  bool Resolve(auto&&... aArgs);

  template <class A, std::invocable<FutureResult<A>> F>
  nsresult Await(StaticString aName, nsIEventTarget* aTarget,
                 FutureDispatchOptions aOptions, F aCallback);

  template <class A, std::invocable<FutureResult<A>> F>
  void UnsafeAwaitSync(StaticString aName, F aCallback);

 private:
  friend class FutureResult<T>;
  friend class FutureResult<const T>;

  void AssertResolved() MOZ_NO_THREAD_SAFETY_ANALYSIS {
    // SAFETY: This assertion INTENTIONALLY does not lock mMutex!
    // Whichever thread constructs FutureResult this MUST know mState ==
    // State::Resolved without any other synchronization.
    // TSAN warnings on this read are probably real.
    MOZ_ASSERT(mState == State::Resolved,
               "Kernel must be in a stable resolved state");
  }

  template <class A, std::invocable<FutureResult<A>> F>
  auto WrapCallback(F aCallback) {
    return [cb = std::move(aCallback)](FutureKernelBase* aKernel) mutable {
      FutureKernel* kernel = static_cast<FutureKernel*>(aKernel);
      std::invoke(cb, FutureResult<A>{do_AddRef(kernel)});
    };
  }

  ~FutureKernel() {
    // NOTE: dtor has exclusive access.
    if (mState == State::Resolved) {
      this->Destroy();
    }
  }

  template <class U = T>
    requires(!std::is_void_v<U>)
  U& ValueRef() {
    return this->mValue;
  }
};

template <class T>
bool FutureKernel<T>::Resolve(auto&&... aArgs) {
  if (!BeginResolving()) {
    return false;
  }
  this->Construct(std::forward<decltype(aArgs)>(aArgs)...);
  FinishResolving();
  return true;
}

template <class T>
template <class A, std::invocable<FutureResult<A>> F>
nsresult FutureKernel<T>::Await(StaticString aName, nsIEventTarget* aTarget,
                                FutureDispatchOptions aOptions, F aCallback) {
  return AddDispatchWaker(aName, aTarget, aOptions,
                          WrapCallback<A>(std::move(aCallback)));
}

template <class T>
template <class A, std::invocable<FutureResult<A>> F>
void FutureKernel<T>::UnsafeAwaitSync(StaticString aName, F aCallback) {
  AddSyncWaker(aName, WrapCallback<A>(std::move(aCallback)));
}

}  // namespace future_detail

template <class T>
bool FutureResolver<T>::Emplace(auto&&... aValue) {
  return mProxy &&
         mProxy->mKernel->Resolve(std::forward<decltype(aValue)>(aValue)...);
}

template <class T>
template <class U>
bool FutureResolver<T>::Resolve(U&& aValue)
  requires std::convertible_to<StripFuture<U>, T>
{
  if (!mProxy) {
    return false;
  }

  if constexpr (IsFuture<U>) {
    aValue.UnsafeThenSync("FutureResolver::Resolve",
                          [self = *this](auto aValue) mutable {
                            self.Resolve(std::move(aValue));
                          });
  } else if constexpr (IsFutureResult<U>) {
    if (aValue.isSuccess()) {
      if constexpr (std::is_void_v<T>) {
        Emplace();
      } else {
        Emplace(std::move(*aValue));
      }
    } else {
      mProxy->mKernel->SetFailed();
    }
  } else {
    Emplace(std::forward<U>(aValue));
  }
  return true;
}

template <class T>
bool FutureResolver<T>::Resolve()
  requires std::is_void_v<T>
{
  return Emplace();
}

template <class T>
FutureResult<T>::FutureResult(std::in_place_t, auto&&... aArgs)
    : FutureResult(
          MakeAndAddRef<future_detail::FutureKernel<std::remove_const_t<T>>>(
              std::in_place, std::forward<decltype(aArgs)>(aArgs)...)) {}

template <class T>
Future<T>::Future(std::in_place_t, auto&&... aArgs)
    : Future(MakeAndAddRef<future_detail::FutureKernel<T>>(
          std::in_place, std::forward<decltype(aArgs)>(aArgs)...)) {}

template <class T>
SharedFuture<T>::SharedFuture(std::in_place_t, auto&&... aArgs)
    : SharedFuture(MakeAndAddRef<future_detail::FutureKernel<T>>(
          std::in_place, std::forward<decltype(aArgs)>(aArgs)...)) {}

template <class T>
std::pair<Future<T>, FutureResolver<T>> Future<T>::Create() {
  Future<T> future(MakeAndAddRef<future_detail::FutureKernel<T>>());
  FutureResolver<T> resolver(do_AddRef(future.mKernel));
  return std::pair{std::move(future), std::move(resolver)};
}

template <class T>
std::pair<SharedFuture<T>, FutureResolver<T>> SharedFuture<T>::Create() {
  auto [future, resolver] = Future<T>::Create();
  return std::pair{future.Share(), std::move(resolver)};
}

template <class T>
void Future<T>::UnsafeThenSync(StaticString aName,
                               std::invocable<FutureResult<T>> auto aCallback) {
  if (RefPtr kernel = mKernel.forget()) {
    kernel->template UnsafeAwaitSync<T>(aName, std::move(aCallback));
  } else {
    std::invoke(aCallback, FutureResult<T>{});
  }
}

template <class T>
void SharedFuture<T>::UnsafeThenSync(
    StaticString aName,
    std::invocable<FutureResult<const T>> auto aCallback) const {
  if (mKernel) {
    mKernel->template UnsafeAwaitSync<const T>(aName, std::move(aCallback));
  } else {
    std::invoke(aCallback, FutureResult<const T>{});
  }
}

template <class T>
nsresult Future<T>::ThenOn(StaticString aName, nsIEventTarget* aTarget,
                           std::invocable<FutureResult<T>> auto aCallback,
                           FutureDispatchOptions aOptions) {
  nsresult rv = mKernel ? mKernel->template Await<T>(aName, aTarget, aOptions,
                                                     std::move(aCallback))
                        : NS_ERROR_INVALID_ARG;
  if (NS_SUCCEEDED(rv)) {
    // On success, clear out mKernel, ensuring we only register a single waker.
    mKernel = nullptr;
  }
  return rv;
}

template <class T>
nsresult SharedFuture<T>::ThenOn(
    StaticString aName, nsIEventTarget* aTarget,
    std::invocable<FutureResult<const T>> auto aCallback,
    FutureDispatchOptions aOptions) const {
  return mKernel ? mKernel->template Await<const T>(aName, aTarget, aOptions,
                                                    std::move(aCallback))
                 : NS_ERROR_INVALID_ARG;
}

template <class T>
FutureResult<T> Future<T>::ToResult() {
  MOZ_ASSERT(IsSettled());
  RefPtr kernel = mKernel.forget();
  if (kernel && kernel->IsResolved()) {
    return FutureResult<T>{kernel.forget()};
  }
  return FutureResult<T>{};
}

template <class T>
FutureResult<const T> SharedFuture<T>::ToResult() const {
  MOZ_ASSERT(IsSettled());
  if (mKernel && mKernel->IsResolved()) {
    return FutureResult<const T>{do_AddRef(mKernel)};
  }
  return FutureResult<const T>{};
}

template <class T>
SharedFuture<T> Future<T>::Share() {
  return SharedFuture(mKernel.forget());
}

}  // namespace mozilla

#endif  // mozilla_Future_h
