/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef XPCOM_THREADS_MOZPROMISE_H_
#define XPCOM_THREADS_MOZPROMISE_H_

#include <concepts>
#include <type_traits>
#include <utility>

#include "mozilla/Attributes.h"
#include "mozilla/ErrorNames.h"
#include "mozilla/Logging.h"
#include "mozilla/Maybe.h"
#include "mozilla/Monitor.h"
#include "mozilla/MozPromiseFwd.h"
#include "mozilla/Mutex.h"
#include "mozilla/RefPtr.h"
#include "mozilla/StaticString.h"
#include "mozilla/Variant.h"
#include "nsICancelableRunnable.h"
#include "nsISerialEventTarget.h"
#include "nsITargetShutdownTask.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsThreadUtils.h"

#ifdef MOZ_WIDGET_ANDROID
#  include "mozilla/jni/GeckoResultUtils.h"
#endif

#if MOZ_DIAGNOSTIC_ASSERT_ENABLED
#  define PROMISE_DEBUG
#endif

#ifdef PROMISE_DEBUG
#  define PROMISE_ASSERT MOZ_RELEASE_ASSERT
#else
#  define PROMISE_ASSERT(...) \
    do {                      \
    } while (0)
#endif

#if DEBUG
#  include "nsPrintfCString.h"
#endif

namespace mozilla {

namespace dom {
class Promise;
}

extern LazyLogModule gMozPromiseLog;

#define PROMISE_LOG(x, ...) \
  MOZ_LOG(gMozPromiseLog, mozilla::LogLevel::Debug, (x, ##__VA_ARGS__))

#ifdef DEBUG
#  define PROMISE_WARNING(x, ...)                         \
    do {                                                  \
      nsPrintfCString warning_message_(x, ##__VA_ARGS__); \
      NS_WARNING(warning_message_.get());                 \
      MOZ_LOG(gMozPromiseLog, mozilla::LogLevel::Warning, \
              ("%s", warning_message_.get()));            \
    } while (0)
#else
#  define PROMISE_WARNING(x, ...) \
    MOZ_LOG(gMozPromiseLog, mozilla::LogLevel::Warning, (x, ##__VA_ARGS__))
#endif

// Support default abandon handlers for any type which supports conversion from
// `nsresult`, which has a special error type for promise abandonment.
template <typename RejectValueT>
  requires std::constructible_from<RejectValueT, nsresult>
struct DefaultAbandonHandler<RejectValueT> {
  RejectValueT operator()() const {
    return RejectValueT{NS_ERROR_MOZPROMISE_ABANDONED};
  }
};

// Support default abandon handlers for any type which supports conversion from
// `PromiseAbandoned{}`, as a way for custom error types to quickly opt-in to
// abandon handling.
template <typename RejectValueT>
  requires std::constructible_from<RejectValueT, PromiseAbandoned>
struct DefaultAbandonHandler<RejectValueT> {
  RejectValueT operator()() const { return RejectValueT{PromiseAbandoned{}}; }
};

// Many users of MozPromise currently use generic string types as their error
// type, usually including some error message for why something went wrong.
template <>
struct DefaultAbandonHandler<nsCString> {
  nsCString operator()() const { return "promise abandoned"_ns; }
};

template <>
struct DefaultAbandonHandler<nsString> {
  nsString operator()() const { return u"promise abandoned"_ns; }
};

// FIXME: There are so many promises which use `bool` as their rejection type
// that to reduce churn, we basically have to support implicit conversions to
// it. We should remove this AbandonHandler in a follow-up and migrate these
// types to use something more descriptive.
template <>
struct DefaultAbandonHandler<bool> {
  bool operator()() const { return false; }
};

// Special abandon handler which can be used when abandoning should reject with
// a compile-time known value.
template <auto Value>
struct ValueAbandonHandler {
  auto operator()() const { return Value; }
};

namespace detail {

template <typename T>
constexpr static bool IsRefPtrMozPromise = false;

template <typename T, typename U, bool B, typename A>
constexpr static bool IsRefPtrMozPromise<RefPtr<MozPromise<T, U, B, A>>> = true;

// Historically the argument to most callbacks from MozPromise have been
// optional. Because of that, we need to use our own suite of invocable and
// invoke concepts to simulate this existing behaviour.

template <typename Fn, typename Arg>
concept OptArgInvocable = std::invocable<Fn, Arg> || std::invocable<Fn>;

template <typename Fn, typename Arg>
  requires OptArgInvocable<Fn, Arg>
auto OptArgInvoke(Fn&& aFunc, Arg&& aArg) {
  if constexpr (std::invocable<Fn, Arg>) {
    return std::invoke(std::forward<Fn>(aFunc), std::forward<Arg>(aArg));
  } else {
    return std::invoke(std::forward<Fn>(aFunc));
  }
}

template <typename Fn, typename Arg>
  requires OptArgInvocable<Fn, Arg>
using OptArgInvokeResult =
    decltype(OptArgInvoke(std::declval<Fn>(), std::declval<Arg>()));

template <typename T, typename RejectValueT>
concept AbandonHandlerFor = std::default_initializable<T> && requires(T t) {
  { t() } -> std::convertible_to<RejectValueT>;
};

}  // namespace detail

/*
 * A promise manages an asynchronous request that may or may not be able to be
 * fulfilled immediately. When an API returns a promise, the consumer may attach
 * callbacks to be invoked (asynchronously, on a specified thread) when the
 * request is either completed (resolved) or cannot be completed (rejected).
 * Whereas JS promise callbacks are dispatched from Microtask checkpoints,
 * MozPromises resolution/rejection make a normal round-trip through the event
 * loop, which simplifies their ordering semantics relative to other native
 * code.
 *
 * MozPromises attempt to mirror the spirit of JS Promises to the extent that
 * is possible (and desirable) in C++. While the intent is that MozPromises
 * feel familiar to programmers who are accustomed to their JS-implemented
 * cousin, we don't shy away from imposing restrictions and adding features that
 * make sense for the use cases we encounter.
 *
 * A MozPromise is ThreadSafe, and may be ->Then()ed on any thread. The Then()
 * call accepts resolve and reject callbacks, and returns a magic object which
 * will be implicitly converted to a MozPromise::Request or a MozPromise object
 * depending on how the return value is used. The magic object serves several
 * purposes for the consumer.
 *
 *   (1) When converting to a MozPromise::Request, it allows the caller to
 *       cancel the delivery of the resolve/reject value if it has not already
 *       occurred, via Disconnect() (this must be done on the target thread to
 *       avoid racing).
 *
 *   (2) When converting to a MozPromise (which is called a completion promise),
 *       it allows promise chaining so ->Then() can be called again to attach
 *       more resolve and reject callbacks. If the resolve/reject callback
 *       returns a new MozPromise, that promise is chained to the completion
 *       promise, such that its resolve/reject value will be forwarded along
 *       when it arrives. If the resolve/reject callback returns void, the
 *       completion promise is resolved/rejected with the same value that was
 *       passed to the callback.
 *
 * The MozPromise APIs skirt traditional XPCOM convention by returning nsRefPtrs
 * (rather than already_AddRefed) from various methods. This is done to allow
 * elegant chaining of calls without cluttering up the code with intermediate
 * variables, and without introducing separate API variants for callers that
 * want a return value (from, say, ->Then()) from those that don't.
 *
 * When IsExclusive is true, the MozPromise does a release-mode assertion that
 * there is at most one call to either Then(...) or ChainTo(...).
 *
 * Thread Safety & Dispatch Failures
 * =================================
 *
 * A MozPromise **MUST** be either resolved or rejected before it is destroyed,
 * and callbacks attached to a MozPromise with ->Then() are therefore required
 * to be executed and destroyed on the specified event target.
 *
 * This requirement can be relaxed in 2 ways:
 *
 *   (1) Individual calls to ->Then() can pass NS_DISPATCH_FALLIBLE. If this
 *       flag is passed, we will not assert if the callback is never called due
 *       to a Dispatch failure, and the callback may be destroyed on any thread.
 *
 *       Note that NS_DISPATCH_FALLIBLE is not transitive, and downstream
 *       callbacks due to ->Then() chaining may still assert if they are not
 *       executed before shutdown.
 *
 *   (2) Callers may ->Track() a returned MozPromise::Request, and later call
 *       Disconnect() on the target thread (e.g. before it goes away) to safely
 *       disarm the callback.
 *
 * Relaxing the requirements in these ways does not relax the requirement that
 * the root MozPromise must be resolved or rejected, however it does allow
 * avoiding issues around dispatch failure during shutdown.
 */

class MozPromiseBase {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MozPromiseBase)

 protected:
  virtual ~MozPromiseBase() = default;
};

class MozPromiseRequest : public nsISupports {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS

  virtual void Disconnect() = 0;

 protected:
  virtual ~MozPromiseRequest() = default;
};

namespace detail {

class MozPromiseThenValueBase : public MozPromiseRequest,
                                public nsIRunnable,
                                public nsIDiscardableRunnable,
                                public nsICancelableRunnable,
                                public nsIRunnablePriority,
                                public nsINamed,
                                public nsITargetShutdownTask {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIRUNNABLEPRIORITY
  NS_DECL_NSINAMED

  void OnDiscard() final { Run(); }
  nsresult Cancel() final { return Run(); }
  void TargetShutdown() final { Run(); }

  // Must be called before the Request is added to the MozPromise or dispatched
  // to try to register a shutdown task on aResponseTarget.
  bool RegisterShutdownTask();

  void Dispatch(MozPromiseBase* aPromise, bool mUseSynchronousTaskDispatch,
                bool mUseDirectTaskDispatch, uint32_t mPriority);

  nsISerialEventTarget* ResponseTarget() const { return mResponseTarget; }

 protected:
  MozPromiseThenValueBase(nsISerialEventTarget* aResponseTarget,
                          StaticString aCallSite,
                          nsIEventTarget::DispatchFlags aDispatchFlags);
  virtual ~MozPromiseThenValueBase() = default;

  // Check if the callback mOnSettled is still present (i.e. that the request
  // has not been notified or disconnected).
  virtual bool HasCallback() MOZ_REQUIRES(mMutex) = 0;

  // Called when Dispatch fails internally, passing in the dispatch result. This
  // is overridden by ThenValue to abandon chained completion promises and set
  // mDispatchRv.
  virtual void DispatchFailed(nsresult aDispatchRv) = 0;

  const nsCOMPtr<nsISerialEventTarget> mResponseTarget;
  const StaticString mCallSite;
  const nsIEventTarget::DispatchFlags mDispatchFlags;

  Mutex mMutex{"MozPromiseRequest::mMutex"};
  uint32_t mPriority MOZ_GUARDED_BY(mMutex) =
      nsIRunnablePriority::PRIORITY_NORMAL;
  nsresult mRegisterShutdownTaskRv MOZ_GUARDED_BY(mMutex) = NS_OK;
  nsresult mDispatchRv MOZ_GUARDED_BY(mMutex) = NS_OK;
  RefPtr<MozPromiseBase> mSettledPromise MOZ_GUARDED_BY(mMutex);
};

class MozPromiseAllBase {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MozPromiseAllBase)

 protected:
  virtual ~MozPromiseAllBase() = default;
};

}  // namespace detail

template <typename ResolveValueT, typename RejectValueT>
class MozPromiseValue {
 public:
  using ResolveValueType = ResolveValueT;
  using RejectValueType = RejectValueT;

  template <typename ResolveValueType_>
  void SetResolve(ResolveValueType_&& aResolveValue) {
    MOZ_ASSERT(IsNothing());
    mValue = Storage(VariantIndex<ResolveIndex>{},
                     std::forward<ResolveValueType_>(aResolveValue));
  }

  template <typename RejectValueType_>
  void SetReject(RejectValueType_&& aRejectValue) {
    MOZ_ASSERT(IsNothing());
    mValue = Storage(VariantIndex<RejectIndex>{},
                     std::forward<RejectValueType_>(aRejectValue));
  }

  template <typename ResolveValueType_>
  static MozPromiseValue MakeResolve(ResolveValueType_&& aResolveValue) {
    MozPromiseValue val;
    val.SetResolve(std::forward<ResolveValueType_>(aResolveValue));
    return val;
  }

  template <typename RejectValueType_>
  static MozPromiseValue MakeReject(RejectValueType_&& aRejectValue) {
    MozPromiseValue val;
    val.SetReject(std::forward<RejectValueType_>(aRejectValue));
    return val;
  }

  bool IsResolve() const { return mValue.template is<ResolveIndex>(); }
  bool IsReject() const { return mValue.template is<RejectIndex>(); }
  bool IsNothing() const { return mValue.template is<NothingIndex>(); }

  const ResolveValueType& ResolveValue() const {
    return mValue.template as<ResolveIndex>();
  }
  ResolveValueType& ResolveValue() {
    return mValue.template as<ResolveIndex>();
  }
  const RejectValueType& RejectValue() const {
    return mValue.template as<RejectIndex>();
  }
  RejectValueType& RejectValue() { return mValue.template as<RejectIndex>(); }

 private:
  enum { NothingIndex, ResolveIndex, RejectIndex };
  using Storage = Variant<Nothing, ResolveValueType, RejectValueType>;
  Storage mValue = Storage(VariantIndex<NothingIndex>{});
};

template <typename T>
class MozPromiseHolder;
template <typename PromiseType, typename ImplType>
class MozPromiseHolderBase;
template <typename T>
class MozPromiseRequestHolder;
template <typename ResolveValueT, typename RejectValueT, bool IsExclusive,
          typename AbandonHandler>
class MozPromise : public MozPromiseBase {
  static const uint32_t sMagic = 0xcecace11;

  template <typename T>
  using ParamType = std::conditional_t<IsExclusive, T&&, const T&>;

  // Return a |T&&| to enable move when IsExclusive is true or
  // a |const T&| to enforce copy otherwise.
  template <typename T>
  static ParamType<T> MaybeMove(T& aX) {
    return std::forward<ParamType<T>>(aX);
  }

 public:
  using ResolveValueType = ResolveValueT;
  using RejectValueType = RejectValueT;
  using ResolveOrRejectValue =
      MozPromiseValue<ResolveValueType, RejectValueType>;
  using AbandonHandlerType = AbandonHandler;

 protected:
  // MozPromise is the public type, and never constructed directly. Construct
  // a MozPromise::Private, defined below.
  MozPromise(StaticString aCreationSite, bool aIsCompletionPromise)
      : mCreationSite(aCreationSite)
#ifdef PROMISE_DEBUG
        ,
        mMagic4(&mMutex)
#endif
  {
    static_assert(
        detail::AbandonHandlerFor<AbandonHandlerType, RejectValueType>,
        "Must have a valid abandon handler");
    PROMISE_LOG("%s creating MozPromise (%p)", mCreationSite.get(), this);
  }

 public:
  // MozPromise::Private allows us to separate the public interface (upon which
  // consumers of the promise may invoke methods like Then()) from the private
  // interface (upon which the creator of the promise may invoke Resolve() or
  // Reject()). APIs should create and store a MozPromise::Private (usually
  // via a MozPromiseHolder), and return a MozPromise to consumers.
  //
  // NB: We can include the definition of this class inline once B2G ICS is
  // gone.
  class Private;

  template <typename ResolveValueType_>
  [[nodiscard]] static RefPtr<MozPromise> CreateAndResolve(
      ResolveValueType_&& aResolveValue, StaticString aResolveSite) {
    static_assert(std::is_convertible_v<ResolveValueType_, ResolveValueT>,
                  "Resolve() argument must be implicitly convertible to "
                  "MozPromise's ResolveValueT");
    RefPtr<typename MozPromise::Private> p =
        new MozPromise::Private(aResolveSite);
    p->Resolve(std::forward<ResolveValueType_>(aResolveValue), aResolveSite);
    return p;
  }

  template <typename RejectValueType_>
  [[nodiscard]] static RefPtr<MozPromise> CreateAndReject(
      RejectValueType_&& aRejectValue, StaticString aRejectSite) {
    static_assert(std::is_convertible_v<RejectValueType_, RejectValueT>,
                  "Reject() argument must be implicitly convertible to "
                  "MozPromise's RejectValueT");
    RefPtr<typename MozPromise::Private> p =
        new MozPromise::Private(aRejectSite);
    p->Reject(std::forward<RejectValueType_>(aRejectValue), aRejectSite);
    return p;
  }

  template <typename ResolveOrRejectValueType_>
  [[nodiscard]] static RefPtr<MozPromise> CreateAndResolveOrReject(
      ResolveOrRejectValueType_&& aValue, StaticString aSite) {
    RefPtr<typename MozPromise::Private> p = new MozPromise::Private(aSite);
    p->ResolveOrReject(std::forward<ResolveOrRejectValueType_>(aValue), aSite);
    return p;
  }

  using AllPromiseType =
      MozPromise<CopyableTArray<ResolveValueType>, RejectValueType, IsExclusive,
                 AbandonHandlerType>;
  using AllSettledPromiseType = MozPromise<CopyableTArray<ResolveOrRejectValue>,
                                           PromiseAbandoned, IsExclusive>;

 private:
  class AllPromiseHolder : public detail::MozPromiseAllBase {
   public:
    explicit AllPromiseHolder(size_t aDependentPromises)
        : mPromise(new typename AllPromiseType::Private(
              __func__, true /* aIsCompletionPromise */)),
          mOutstandingPromises(aDependentPromises) {
      MOZ_ASSERT(aDependentPromises > 0);
      mResolveValues.SetLength(aDependentPromises);
    }

    template <typename ResolveValueType_>
    void Resolve(size_t aIndex, ResolveValueType_&& aResolveValue) {
      if (!mPromise) {
        // Already rejected.
        return;
      }

      mResolveValues[aIndex].emplace(
          std::forward<ResolveValueType_>(aResolveValue));
      if (--mOutstandingPromises == 0) {
        nsTArray<ResolveValueType> resolveValues;
        resolveValues.SetCapacity(mResolveValues.Length());
        for (auto&& resolveValue : mResolveValues) {
          resolveValues.AppendElement(std::move(resolveValue.ref()));
        }

        mPromise->Resolve(std::move(resolveValues), __func__);
        mPromise = nullptr;
        mResolveValues.Clear();
      }
    }

    template <typename RejectValueType_>
    void Reject(RejectValueType_&& aRejectValue) {
      if (!mPromise) {
        // Already rejected.
        return;
      }

      mPromise->Reject(std::forward<RejectValueType_>(aRejectValue), __func__);
      mPromise = nullptr;
      mResolveValues.Clear();
    }

    AllPromiseType* Promise() { return mPromise; }

   private:
    nsTArray<Maybe<ResolveValueType>> mResolveValues;
    RefPtr<typename AllPromiseType::Private> mPromise;
    size_t mOutstandingPromises;
  };

  // Trying to pass ResolveOrRejectValue by value fails static analysis checks,
  // so we need to use either a const& or an rvalue reference, depending on
  // whether IsExclusive is true or not.
  using ResolveOrRejectValueParam = ParamType<ResolveOrRejectValue>;

  using ResolveValueTypeParam = ParamType<ResolveValueType>;

  using RejectValueTypeParam = ParamType<RejectValueType>;

  class AllSettledPromiseHolder : public detail::MozPromiseAllBase {
   public:
    explicit AllSettledPromiseHolder(size_t aDependentPromises)
        : mPromise(new typename AllSettledPromiseType::Private(
              __func__, true /* aIsCompletionPromise */)),
          mOutstandingPromises(aDependentPromises) {
      MOZ_ASSERT(aDependentPromises > 0);
      mValues.SetLength(aDependentPromises);
    }

    void Settle(size_t aIndex, ResolveOrRejectValueParam aValue) {
      if (!mPromise) {
        // Already rejected.
        return;
      }

      mValues[aIndex].emplace(MaybeMove(aValue));
      if (--mOutstandingPromises == 0) {
        nsTArray<ResolveOrRejectValue> values;
        values.SetCapacity(mValues.Length());
        for (auto&& value : mValues) {
          values.AppendElement(std::move(value.ref()));
        }

        mPromise->Resolve(std::move(values), __func__);
        mPromise = nullptr;
        mValues.Clear();
      }
    }

    AllSettledPromiseType* Promise() { return mPromise; }

   private:
    nsTArray<Maybe<ResolveOrRejectValue>> mValues;
    RefPtr<typename AllSettledPromiseType::Private> mPromise;
    size_t mOutstandingPromises;
  };

 public:
  [[nodiscard]] static RefPtr<AllPromiseType> All(
      nsISerialEventTarget* aProcessingTarget,
      nsTArray<RefPtr<MozPromise>>& aPromises,
      nsIEventTarget::DispatchFlags aDispatchFlags = NS_DISPATCH_NORMAL) {
    if (aPromises.Length() == 0) {
      return AllPromiseType::CreateAndResolve(
          CopyableTArray<ResolveValueType>(), __func__);
    }

    RefPtr holder = MakeRefPtr<AllPromiseHolder>(aPromises.Length());
    RefPtr<AllPromiseType> promise = holder->Promise();
    for (size_t i = 0; i < aPromises.Length(); ++i) {
      aPromises[i]->Then(
          aProcessingTarget, __func__,
          [holder, i](ResolveValueTypeParam aResolveValue) -> void {
            holder->Resolve(i, MaybeMove(aResolveValue));
          },
          [holder](RejectValueTypeParam aRejectValue) -> void {
            holder->Reject(MaybeMove(aRejectValue));
          },
          aDispatchFlags);
    }
    return promise;
  }

  [[nodiscard]] static RefPtr<AllSettledPromiseType> AllSettled(
      nsISerialEventTarget* aProcessingTarget,
      nsTArray<RefPtr<MozPromise>>& aPromises,
      nsIEventTarget::DispatchFlags aDispatchFlags = NS_DISPATCH_NORMAL) {
    if (aPromises.Length() == 0) {
      return AllSettledPromiseType::CreateAndResolve(
          CopyableTArray<ResolveOrRejectValue>(), __func__);
    }

    RefPtr holder = MakeRefPtr<AllSettledPromiseHolder>(aPromises.Length());
    RefPtr<AllSettledPromiseType> promise = holder->Promise();
    for (size_t i = 0; i < aPromises.Length(); ++i) {
      aPromises[i]->Then(
          aProcessingTarget, __func__,
          [holder, i](ResolveOrRejectValueParam aValue) -> void {
            holder->Settle(i, MaybeMove(aValue));
          },
          aDispatchFlags);
    }
    return promise;
  }

 protected:
  // Helper type for working with completion promises if they are supported
  // generically.
  template <typename InvokeResult>
  struct CompletionPromiseHolder {
    constexpr static bool SupportChaining = false;

    bool Exists() const { return false; }

    void Abandon(StaticString) {}

    template <typename OnSettled, typename... Args>
    void Invoke(Maybe<OnSettled>& aOnSettled, Args&&... aArgs) {
      detail::OptArgInvoke(aOnSettled.extract(), std::forward<Args>(aArgs)...);
    }
  };

  template <typename InvokeResult>
    requires detail::IsRefPtrMozPromise<InvokeResult>
  struct CompletionPromiseHolder<InvokeResult> {
    constexpr static bool SupportChaining = true;

    using PromiseType = RemoveSmartPointer<InvokeResult>;
    using PromisePrivateType = typename PromiseType::Private;

    RefPtr<PromiseType> Create() {
      MOZ_ASSERT(!mPromise,
                 "Can only call CompletionPromiseHolder::Create once");
      RefPtr promise = MakeRefPtr<PromisePrivateType>(
          "<completion promise>", true /* aIsCompletionPromise */);
      mPromise = promise;
      return promise.forget();
    }

    bool Exists() const { return !!mPromise; }

    void Abandon(StaticString aCallSite) {
      if (RefPtr promise = mPromise.forget()) {
        promise->Abandon(aCallSite);
      }
    }

    template <typename OnSettled, typename... Args>
    void Invoke(Maybe<OnSettled>& aOnSettled, Args&&... aArgs) {
      InvokeResult result = detail::OptArgInvoke(aOnSettled.extract(),
                                                 std::forward<Args>(aArgs)...);
      if (mPromise) {
        MOZ_DIAGNOSTIC_ASSERT(
            result,
            "Can't do promise chaining for a non-promise-returning method.");
        result->ChainTo(mPromise.forget(), "<chained completion promise>");
      }
    }

    RefPtr<PromisePrivateType> mPromise;
  };

  /*
   * A ThenValue tracks a single consumer waiting on the promise. When a
   * consumer invokes promise->Then(...), a ThenValue is created. Once the
   * Promise is resolved or rejected, a {Resolve,Reject}Runnable is dispatched,
   * which invokes the resolve/reject method and then deletes the ThenValue.
   */
  template <typename OnSettled>
  class ThenValue final : public detail::MozPromiseThenValueBase {
   public:
    using CompletionHolder = CompletionPromiseHolder<
        detail::OptArgInvokeResult<OnSettled, ResolveOrRejectValueParam>>;
    constexpr static bool SupportChaining = CompletionHolder::SupportChaining;

    ThenValue(nsISerialEventTarget* aResponseTarget, OnSettled&& aOnSettled,
              StaticString aCallSite,
              nsIEventTarget::DispatchFlags aDispatchFlags)
        : detail::MozPromiseThenValueBase(aResponseTarget, aCallSite,
                                          aDispatchFlags),
          mOnSettled(std::in_place, std::forward<OnSettled>(aOnSettled)) {}

    auto EnsureCompletionPromise()
      requires(SupportChaining)
    {
      // NOTE: Technically we could probably skip this mutex, as this shouldn't
      // be shared yet, but an uncontested lock is cheap.
      MutexAutoLock lock(mMutex);
      return mCompletionPromise.Create();
    }

    bool HasCallback() final MOZ_REQUIRES(mMutex) {
      return mOnSettled.isSome();
    }

    void DispatchFailed(nsresult aDispatchRv) final {
      CompletionHolder completionPromise;
      {
        MutexAutoLock lock(mMutex);
        if (!mOnSettled) {
          // We were disconnected or notified while dispatching.
          return;
        }

        mDispatchRv = aDispatchRv;

        // If we didn't successfully register a shutdown task, which will call
        // us back before the target goes away, emit a warning and abandon our
        // completion promise. We know it will never be invoked.
        if (NS_FAILED(mRegisterShutdownTaskRv)) {
          PROMISE_WARNING("Request[%p] dispatch failed (rv=%s, shutdown rv=%s)",
                          this, GetStaticErrorName(mDispatchRv),
                          GetStaticErrorName(mRegisterShutdownTaskRv));

          completionPromise = std::exchange(mCompletionPromise, {});
        }
      }

      completionPromise.Abandon("<dispatch failure>");
    }

    void Disconnect() override {
      // Ensure that any shutdown task, if previously registered, has been
      // unregistered, as we don't need to be notified if the target goes away
      // anymore.
      (void)mResponseTarget->UnregisterShutdownTask(this);

      // Clear out the internal state for this MozPromise, destroying it on the
      // current thread.
      // We will not notify this Request even if a runnable was already
      // dispatched.
      Maybe<std::decay_t<OnSettled>> onSettled;
      RefPtr<MozPromiseBase> settledPromise;
      CompletionHolder completionPromise;
      {
        MutexAutoLock lock(mMutex);
        if (!mOnSettled) {
          MOZ_ASSERT(!mSettledPromise);
          MOZ_ASSERT(!mCompletionPromise.Exists());
          return;
        }
        onSettled.emplace(mOnSettled.extract());
        settledPromise = mSettledPromise.forget();
        completionPromise = std::exchange(mCompletionPromise, {});
      }

      // If we have a completion promise, abandon it.
      //
      // NOTE: This is currently dead, as it's currently not possible to
      // explicitly Disconnect a promise in the middle of a completion chain,
      // but we can support it in case we add new ways to disconnect promises.
      completionPromise.Abandon("<request disconnected>");
    }

    NS_IMETHOD Run() final {
      // It is normal for this shutdown task to already be unregistered, or for
      // us to have failed to register a shutdown task. Either way, make sure
      // we're not registered as a shutdown task anymore as soon as we are run.
      (void)mResponseTarget->UnregisterShutdownTask(this);

      Maybe<std::decay_t<OnSettled>> onSettled;
      RefPtr<MozPromise> settledPromise;
      CompletionHolder completionPromise;
      {
        MutexAutoLock lock(mMutex);
        if (!mOnSettled) {
          MOZ_ASSERT(!mSettledPromise);
          MOZ_ASSERT(!mCompletionPromise.Exists());
          return NS_OK;
        }
        onSettled.emplace(mOnSettled.extract());
        settledPromise =
            mSettledPromise.forget().template downcast<MozPromise>();
        completionPromise = std::exchange(mCompletionPromise, {});
      }

      // Determine which value we're going to invoke with. If we have a
      // value promise, we'll use the value from that type, otherwise we need to
      // manufacture an abandoned reject reason and notify using that.
      ResolveOrRejectValue failedValue;
      ResolveOrRejectValue* value;
      if (settledPromise) {
        MOZ_ASSERT(!settledPromise->IsPending());
        value = &settledPromise->mValue;
      } else {
        failedValue.SetReject(AbandonHandler{}());
        value = &failedValue;
      }

      // Invoke onSettled, and chain the result to our completion promise if
      // configured.
      completionPromise.Invoke(onSettled, MaybeMove(*value));
      return NS_OK;
    }

   private:
    ~ThenValue() {
      if (MOZ_LIKELY(!mOnSettled)) {
        return;
      }

      // If the promise was marked as NS_DISPATCH_FALLIBLE, recover by
      // abandoning the completion promise if we have one.
      // The caller has OK-ed destroying mOnSettled on any thread.
      if (mDispatchFlags & NS_DISPATCH_FALLIBLE) {
        PROMISE_WARNING(
            "NS_DISPATCH_FALLIBLE request '%s' destroyed without being "
            "notified or disconnected, (dispatchRv=%s,shutdownRv=%s)",
            mCallSite.get(), GetStaticErrorName(mDispatchRv),
            GetStaticErrorName(mRegisterShutdownTaskRv));
        mCompletionPromise.Abandon("<fallible request destroyed>");
        return;
      }

      // This is unfortunately an unrecoverable situation, as we cannot allow
      // mOnSettled to be destroyed on the wrong event target. Instead we'll
      // emit a useful crash message.
      MOZ_CRASH_UNSAFE_PRINTF(
          "Infallible request '%s' destroyed without being notified or "
          "disconnected, (dispatchRv=%s,shutdownRv=%s)",
          mCallSite.get(), GetStaticErrorName(mDispatchRv),
          GetStaticErrorName(mRegisterShutdownTaskRv));
    }

    // The callback wrapped by this ThenValue. If this is `Nothing()`, the
    // ThenValue has already been notified, disconnected, or cancelled and all
    // other members must also be `Nothing()`.
    Maybe<std::decay_t<OnSettled>> mOnSettled MOZ_GUARDED_BY(mMutex);

    // Helper type holding the completion promise. This is an empty type if this
    // callback doesn't return a promise.
    MOZ_NO_UNIQUE_ADDRESS
    CompletionHolder mCompletionPromise MOZ_GUARDED_BY(mMutex);
  };

  void ThenInternal(
      already_AddRefed<detail::MozPromiseThenValueBase> aThenValue,
      StaticString aCallSite) {
    PROMISE_ASSERT(mMagic1 == sMagic && mMagic2 == sMagic &&
                   mMagic3 == sMagic && mMagic4 == &mMutex);
    RefPtr<detail::MozPromiseThenValueBase> thenValue = aThenValue;
    thenValue->RegisterShutdownTask();

    MutexAutoLock lock(mMutex);
    MOZ_ASSERT(
        !mRequireTailDispatch || (thenValue->ResponseTarget()->GetFeatures() &
                                  nsIEventTarget::SUPPORTS_TAIL_DISPATCH),
        "This promise requires that Then() event targets support tail "
        "dispatch");
    MOZ_DIAGNOSTIC_ASSERT(
        !IsExclusive || !mHaveRequest,
        "Using an exclusive promise in a non-exclusive fashion");
    mHaveRequest = true;
    PROMISE_LOG("%s invoking Then() [this=%p, aThenValue=%p, isPending=%d]",
                aCallSite.get(), this, thenValue.get(), (int)IsPending());
    if (!IsPending()) {
      thenValue->Dispatch(this, mUseSynchronousTaskDispatch,
                          mUseDirectTaskDispatch, mPriority);
    } else {
      mThenValues.AppendElement(thenValue.forget());
    }
  }

  /*
   * A command object to store all information needed to make a request to the
   * promise. This allows us to delay the request until further use is known
   * (whether it is ->Then() again for more promise chaining or ->Track() to
   * terminate chaining and issue the request).
   *
   * This allows a unified syntax for promise chaining and disconnection, and
   * feels more like its JS counterpart.
   *
   * Note that a ThenCommand is always exclusive, even if its source or result
   * promises are not. To attach multiple continuations, explicitly convert it
   * to a promise first.
   */
  template <typename ThenValueType>
  class MOZ_TEMPORARY_CLASS ThenCommand {
    // Allow Promise1::ThenCommand to access the private constructor
    // Promise2::ThenCommand(ThenCommand&&).
    template <typename, typename, bool, typename>
    friend class MozPromise;

    ThenCommand(StaticString aCallSite,
                already_AddRefed<ThenValueType> aThenValue,
                MozPromise* aReceiver)
        : mCallSite(aCallSite), mThenValue(aThenValue), mReceiver(aReceiver) {}

    ThenCommand(ThenCommand&& aOther) noexcept = default;

   public:
    ~ThenCommand() {
      // Issue the request now if the return value of Then() is not used.
      if (mThenValue) {
        mReceiver->ThenInternal(mThenValue.forget(), mCallSite);
      }
    }

    auto ToPromise()
      requires(ThenValueType::SupportChaining)
    {
      RefPtr promise = mThenValue->EnsureCompletionPromise();
      mReceiver->ThenInternal(mThenValue.forget(), mCallSite);
      return promise;
    }

    // Allow RefPtr<MozPromise> p = somePromise->Then();
    //       p->Then(thread1, ...);
    //       p->Then(thread2, ...);
    template <std::same_as<ThenValueType> ThenValueType_ = ThenValueType>
      requires(ThenValueType_::SupportChaining)
    operator RefPtr<typename ThenValueType_::CompletionHolder::PromiseType>() {
      return ToPromise();
    }

    auto Then(auto&&... aArgs) {
      return ToPromise()->Then(std::forward<decltype(aArgs)>(aArgs)...);
    }

    auto Map(auto&&... aArgs) {
      return ToPromise()->Map(std::forward<decltype(aArgs)>(aArgs)...);
    }

    auto MapErr(auto&&... aArgs) {
      return ToPromise()->MapErr(std::forward<decltype(aArgs)>(aArgs)...);
    }

    void Track(MozPromiseRequestHolder<MozPromise>& aRequestHolder) {
      aRequestHolder.Track(do_AddRef(mThenValue));
      mReceiver->ThenInternal(mThenValue.forget(), mCallSite);
    }

    // Allow calling ->Then() again for more promise chaining or ->Track() to
    // end chaining and track the request for future disconnection.
    ThenCommand* operator->() { return this; }

   private:
    StaticString mCallSite;
    RefPtr<ThenValueType> mThenValue;
    RefPtr<MozPromise> mReceiver;
  };

 public:
  template <detail::OptArgInvocable<ResolveOrRejectValueParam> OnSettled>
  auto Then(nsISerialEventTarget* aResponseTarget, StaticString aCallSite,
            OnSettled&& aOnSettled,
            nsIEventTarget::DispatchFlags aFlags = NS_DISPATCH_NORMAL) {
    RefPtr thenValue = MakeRefPtr<ThenValue<OnSettled>>(
        aResponseTarget, std::forward<OnSettled>(aOnSettled), aCallSite,
        aFlags);
    return ThenCommand<ThenValue<OnSettled>>(aCallSite, thenValue.forget(),
                                             this);
  }

  template <typename ThisType, typename OnSettled>
  auto Then(nsISerialEventTarget* aResponseTarget, StaticString aCallSite,
            ThisType* aThisVal, OnSettled&& aOnSettled,
            nsIEventTarget::DispatchFlags aFlags = NS_DISPATCH_NORMAL) {
    return Then(
        aResponseTarget, aCallSite,
        std::bind_front(std::forward<OnSettled>(aOnSettled), RefPtr{aThisVal}),
        aFlags);
  }

  template <detail::OptArgInvocable<ResolveValueTypeParam> OnResolve,
            detail::OptArgInvocable<RejectValueTypeParam> OnReject>
    requires std::common_with<
        detail::OptArgInvokeResult<OnResolve, ResolveValueTypeParam>,
        detail::OptArgInvokeResult<OnReject, RejectValueTypeParam>>
  auto Then(nsISerialEventTarget* aResponseTarget, StaticString aCallSite,
            OnResolve&& aOnResolve, OnReject&& aOnReject,
            nsIEventTarget::DispatchFlags aFlags = NS_DISPATCH_NORMAL) {
    using CommonType = std::common_type_t<
        detail::OptArgInvokeResult<OnResolve, ResolveValueTypeParam>,
        detail::OptArgInvokeResult<OnReject, RejectValueTypeParam>>;
    return Then(
        aResponseTarget, aCallSite,
        [onResolve = std::forward<OnResolve>(aOnResolve),
         onReject = std::forward<OnReject>(aOnReject)](
            ResolveOrRejectValueParam aValue) mutable -> CommonType {
          if (aValue.IsResolve()) {
            return detail::OptArgInvoke(std::move(onResolve),
                                        MaybeMove(aValue.ResolveValue()));
          }
          return detail::OptArgInvoke(std::move(onReject),
                                      MaybeMove(aValue.RejectValue()));
        },
        aFlags);
  }

  template <typename ThisType, typename OnResolve, typename OnReject>
  auto Then(nsISerialEventTarget* aResponseTarget, StaticString aCallSite,
            ThisType* aThisVal, OnResolve&& aOnResolve, OnReject&& aOnReject,
            nsIEventTarget::DispatchFlags aFlags = NS_DISPATCH_NORMAL) {
    return Then(
        aResponseTarget, aCallSite,
        std::bind_front(std::forward<OnResolve>(aOnResolve), RefPtr{aThisVal}),
        std::bind_front(std::forward<OnReject>(aOnReject), RefPtr{aThisVal}),
        aFlags);
  }

  // Shorthand for a `Then` which simply forwards the reject-value, but performs
  // some additional work with the resolve-value.
  template <std::invocable<ResolveValueTypeParam> MapResolve>
  auto Map(nsISerialEventTarget* aResponseTarget, StaticString aCallSite,
           MapResolve&& aMapResolve,
           nsIEventTarget::DispatchFlags aFlags = NS_DISPATCH_NORMAL) {
    using PromiseType = MozPromise<
        std::decay_t<std::invoke_result_t<MapResolve, ResolveValueTypeParam>>,
        RejectValueType, IsExclusive, AbandonHandlerType>;
    return Then(
        aResponseTarget, aCallSite,
        [aCallSite, mapResolve = std::forward<MapResolve>(aMapResolve)](
            ResolveOrRejectValueParam aValue) {
          if (aValue.IsResolve()) {
            return PromiseType::CreateAndResolve(
                std::invoke(mapResolve, MaybeMove(aValue.ResolveValue())),
                aCallSite);
          }
          return PromiseType::CreateAndReject(MaybeMove(aValue.RejectValue()),
                                              aCallSite);
        },
        aFlags);
  }

  // Shorthand for a `Then` which simply forwards the resolve-value, but
  // performs some additional work with the reject-value.
  template <std::invocable<RejectValueTypeParam> MapReject>
  auto MapErr(nsISerialEventTarget* aResponseTarget, StaticString aCallSite,
              MapReject&& aMapReject,
              nsIEventTarget::DispatchFlags aFlags = NS_DISPATCH_NORMAL) {
    using MappedReject = std::invoke_result_t<MapReject, RejectValueTypeParam>;
    // In the future we may want to allow a non-default abandon handler here.
    static_assert(detail::AbandonHandlerFor<DefaultAbandonHandler<MappedReject>,
                                            MappedReject>,
                  "MapErr result must have a default abandon handler");
    using PromiseType = MozPromise<ResolveValueType, MappedReject, IsExclusive>;
    return Then(
        aResponseTarget, aCallSite,
        [aCallSite, mapReject = std::forward<MapReject>(aMapReject)](
            ResolveOrRejectValueParam aValue) {
          if (aValue.IsReject()) {
            return PromiseType::CreateAndReject(
                std::invoke(mapReject, MaybeMove(aValue.RejectValue())),
                aCallSite);
          }
          return PromiseType::CreateAndResolve(MaybeMove(aValue.ResolveValue()),
                                               aCallSite);
        },
        aFlags);
  }

  void ChainTo(already_AddRefed<Private> aChainedPromise,
               StaticString aCallSite) {
    MutexAutoLock lock(mMutex);
    MOZ_DIAGNOSTIC_ASSERT(
        !IsExclusive || !mHaveRequest,
        "Using an exclusive promise in a non-exclusive fashion");
    mHaveRequest = true;
    RefPtr<Private> chainedPromise = aChainedPromise;
    PROMISE_LOG(
        "%s invoking Chain() [this=%p, chainedPromise=%p, isPending=%d]",
        aCallSite.get(), this, chainedPromise.get(), (int)IsPending());

    // We want to use the same type of dispatching method with the chained
    // promises.

    // We need to ensure that the UseSynchronousTaskDispatch branch isn't taken
    // at compilation time to ensure we're not triggering the static_assert in
    // UseSynchronousTaskDispatch method. if constexpr (IsExclusive) ensures
    // that.
    if (mUseDirectTaskDispatch) {
      chainedPromise->UseDirectTaskDispatch(aCallSite);
    } else if constexpr (IsExclusive) {
      if (mUseSynchronousTaskDispatch) {
        chainedPromise->UseSynchronousTaskDispatch(aCallSite);
      }
    } else {
      chainedPromise->SetTaskPriority(mPriority, aCallSite);
    }

    if (!IsPending()) {
      ForwardTo(chainedPromise);
    } else {
      mChainedPromises.AppendElement(chainedPromise);
    }
  }

#ifdef MOZ_WIDGET_ANDROID
  // Creates a C++ MozPromise from its Java counterpart, GeckoResult.
  [[nodiscard]] static RefPtr<MozPromise> FromGeckoResult(
      java::GeckoResult::Param aGeckoResult) {
    using jni::GeckoResultCallback;
    RefPtr<Private> p = new Private("GeckoResult Glue", false);
    auto resolve = GeckoResultCallback::CreateAndAttach<ResolveValueType>(
        [p](ResolveValueType&& aArg) {
          p->Resolve(MaybeMove(aArg), __func__);
        });
    auto reject = GeckoResultCallback::CreateAndAttach<RejectValueType>(
        [p](RejectValueType&& aArg) { p->Reject(MaybeMove(aArg), __func__); });
    aGeckoResult->NativeThen(resolve, reject);
    return p;
  }
#endif

 protected:
  // NOTE: Methods like `IsPending()` and `Value()` are intentionally marked as
  // `protected`, as they are not safe to call from outside of the MozPromise
  // code.  The only way to inspect the state of a MozPromise is intentionally
  // to call `->Then` on the promise, and receive an async callback.
  //
  // MozPromise has somewhat complex locking and thread safety properties which
  // can't be extended outside of the code in this file.
  bool IsPending() const { return mValue.IsNothing(); }

  ResolveOrRejectValue& Value() {
    // This method should only be called once the value has stabilized. As
    // such, we don't need to acquire the lock here.
    MOZ_DIAGNOSTIC_ASSERT(!IsPending());
    return mValue;
  }

  void DispatchAll() {
    mMutex.AssertCurrentThreadOwns();
    for (auto&& thenValue : mThenValues) {
      thenValue->Dispatch(this, mUseSynchronousTaskDispatch,
                          mUseDirectTaskDispatch, mPriority);
    }
    mThenValues.Clear();

    for (auto&& chainedPromise : mChainedPromises) {
      ForwardTo(chainedPromise);
    }
    mChainedPromises.Clear();
  }

  void ForwardTo(Private* aOther) {
    MOZ_ASSERT(!IsPending());
    if (mValue.IsResolve()) {
      aOther->Resolve(MaybeMove(mValue.ResolveValue()), "<chained promise>");
    } else {
      aOther->Reject(MaybeMove(mValue.RejectValue()), "<chained promise>");
    }
  }

  virtual ~MozPromise() {
    PROMISE_LOG("MozPromise::~MozPromise [this=%p]", this);
    PROMISE_ASSERT(mMagic1 == sMagic && mMagic2 == sMagic &&
                   mMagic3 == sMagic && mMagic4 == &mMutex);
    // If we still have any pending then values or chained promises when the
    // last reference is dropped, we know we will never be resolved.
    // Create a new rejected promise, and notify all chained promises and
    // requests with it.
    if (!mThenValues.IsEmpty() || !mChainedPromises.IsEmpty()) {
      PROMISE_LOG("Notifying abandoned callbacks and chained promises");
      MOZ_ASSERT(IsPending());
      RefPtr<Private> p = MakeRefPtr<Private>("~MozPromise");
      // Copy over dispatch configuration flags
      p->mUseSynchronousTaskDispatch = mUseSynchronousTaskDispatch;
      p->mUseDirectTaskDispatch = mUseDirectTaskDispatch;
#ifdef DEBUG
      p->mRequireTailDispatch = mRequireTailDispatch;
#endif
      p->mPriority = mPriority;
      // Move over the values which need to be notified
      p->mThenValues = std::move(mThenValues);
      p->mChainedPromises = std::move(mChainedPromises);
      p->mHaveRequest = mHaveRequest;
      // Abandon the promise, notifying outstanding requests & chained promises
      p->Abandon("<promise destructor>");
    }
#ifdef PROMISE_DEBUG
    mMagic1 = 0;
    mMagic2 = 0;
    mMagic3 = 0;
    mMagic4 = nullptr;
#endif
  };

  const StaticString mCreationSite;  // For logging and profiling
  Mutex mMutex{"MozPromise Mutex"};

  // NOTE: mValue is intentionally not MOZ_GUARDED_BY(mMutex)
  // Once the promise has settled, it will be accessed without the mutex held.
  // Accessing mValue before settling _DOES_ require holding the mutex.
  ResolveOrRejectValue mValue;
  bool mUseSynchronousTaskDispatch MOZ_GUARDED_BY(mMutex) = false;
  bool mUseDirectTaskDispatch MOZ_GUARDED_BY(mMutex) = false;
#ifdef DEBUG
  bool mRequireTailDispatch MOZ_GUARDED_BY(mMutex) = false;
#endif
  uint32_t mPriority MOZ_GUARDED_BY(mMutex) =
      nsIRunnablePriority::PRIORITY_NORMAL;
#ifdef PROMISE_DEBUG
  uint32_t mMagic1 = sMagic;
#endif
  // Try shows we never have more than 3 elements when IsExclusive is false.
  // So '3' is a good value to avoid heap allocation in most cases.
  AutoTArray<RefPtr<detail::MozPromiseThenValueBase>, IsExclusive ? 1 : 3>
      mThenValues MOZ_GUARDED_BY(mMutex);
#ifdef PROMISE_DEBUG
  uint32_t mMagic2 = sMagic;
#endif
  nsTArray<RefPtr<Private>> mChainedPromises MOZ_GUARDED_BY(mMutex);
#ifdef PROMISE_DEBUG
  uint32_t mMagic3 = sMagic;
#endif
  bool mHaveRequest MOZ_GUARDED_BY(mMutex) = false;
#ifdef PROMISE_DEBUG
  void* mMagic4;
#endif
};

template <typename ResolveValueT, typename RejectValueT, bool IsExclusive,
          typename AbandonHandler>
class MozPromise<ResolveValueT, RejectValueT, IsExclusive,
                 AbandonHandler>::Private
    : public MozPromise<ResolveValueT, RejectValueT, IsExclusive,
                        AbandonHandler> {
 public:
  explicit Private(StaticString aCreationSite,
                   bool aIsCompletionPromise = false)
      : MozPromise(aCreationSite, aIsCompletionPromise) {}

  template <typename ResolveValueT_>
  void Resolve(ResolveValueT_&& aResolveValue, StaticString aResolveSite) {
    PROMISE_ASSERT(mMagic1 == sMagic && mMagic2 == sMagic &&
                   mMagic3 == sMagic && mMagic4 == &mMutex);
    MutexAutoLock lock(mMutex);
    PROMISE_LOG("%s resolving MozPromise (%p created at %s)",
                aResolveSite.get(), this, mCreationSite.get());
    if (!IsPending()) {
      PROMISE_LOG(
          "%s ignored already resolved or rejected MozPromise (%p created at "
          "%s)",
          aResolveSite.get(), this, mCreationSite.get());
      return;
    }
    mValue.SetResolve(std::forward<ResolveValueT_>(aResolveValue));
    DispatchAll();
  }

  template <typename RejectValueT_>
  void Reject(RejectValueT_&& aRejectValue, StaticString aRejectSite) {
    PROMISE_ASSERT(mMagic1 == sMagic && mMagic2 == sMagic &&
                   mMagic3 == sMagic && mMagic4 == &mMutex);
    MutexAutoLock lock(mMutex);
    PROMISE_LOG("%s rejecting MozPromise (%p created at %s)", aRejectSite.get(),
                this, mCreationSite.get());
    if (!IsPending()) {
      PROMISE_LOG(
          "%s ignored already resolved or rejected MozPromise (%p created at "
          "%s)",
          aRejectSite.get(), this, mCreationSite.get());
      return;
    }
    mValue.SetReject(std::forward<RejectValueT_>(aRejectValue));
    DispatchAll();
  }

  void Abandon(StaticString aAbandonSite) {
    PROMISE_ASSERT(mMagic1 == sMagic && mMagic2 == sMagic &&
                   mMagic3 == sMagic && mMagic4 == &mMutex);
    MutexAutoLock lock(mMutex);
    PROMISE_LOG("%s abandoning MozPromise (%p created at %s)",
                aAbandonSite.get(), this, mCreationSite.get());
    if (!IsPending()) {
      PROMISE_LOG(
          "%s ignored already resolved or rejected MozPromise (%p created at "
          "%s)",
          aAbandonSite.get(), this, mCreationSite.get());
      return;
    }
    mValue.SetReject(AbandonHandlerType{}());
    DispatchAll();
  }

  template <typename ResolveOrRejectValue_>
  void ResolveOrReject(ResolveOrRejectValue_&& aValue, StaticString aSite) {
    PROMISE_ASSERT(mMagic1 == sMagic && mMagic2 == sMagic &&
                   mMagic3 == sMagic && mMagic4 == &mMutex);
    MutexAutoLock lock(mMutex);
    PROMISE_LOG("%s resolveOrRejecting MozPromise (%p created at %s)",
                aSite.get(), this, mCreationSite.get());
    if (!IsPending()) {
      PROMISE_LOG(
          "%s ignored already resolved or rejected MozPromise (%p created at "
          "%s)",
          aSite.get(), this, mCreationSite.get());
      return;
    }
    mValue = std::forward<ResolveOrRejectValue_>(aValue);
    DispatchAll();
  }

  // If the caller and target are both on the same thread, run the the resolve
  // or reject callback synchronously. Otherwise, the task will be dispatched
  // via the target Dispatch method.
  void UseSynchronousTaskDispatch(const char* aSite) {
    static_assert(
        IsExclusive,
        "Synchronous dispatch can only be used with exclusive promises");
    PROMISE_ASSERT(mMagic1 == sMagic && mMagic2 == sMagic &&
                   mMagic3 == sMagic && mMagic4 == &mMutex);
    MutexAutoLock lock(mMutex);
    PROMISE_LOG("%s UseSynchronousTaskDispatch MozPromise (%p created at %s)",
                aSite, this, mCreationSite.get());
    MOZ_ASSERT(IsPending(),
               "A Promise must not have been already resolved or rejected to "
               "set dispatch state");
    mUseSynchronousTaskDispatch = true;
  }

  // If the caller and target are both on the same thread, run the
  // resolve/reject callback off the direct task queue instead. This avoids a
  // full trip to the back of the event queue for each additional asynchronous
  // step when using MozPromise, and is similar (but not identical to) the
  // microtask semantics of JS promises.
  void UseDirectTaskDispatch(const char* aSite) {
    PROMISE_ASSERT(mMagic1 == sMagic && mMagic2 == sMagic &&
                   mMagic3 == sMagic && mMagic4 == &mMutex);
    MutexAutoLock lock(mMutex);
    PROMISE_LOG("%s UseDirectTaskDispatch MozPromise (%p created at %s)", aSite,
                this, mCreationSite.get());
    MOZ_ASSERT(IsPending(),
               "A Promise must not have been already resolved or rejected to "
               "set dispatch state");
    MOZ_ASSERT(!mUseSynchronousTaskDispatch,
               "Promise already set for synchronous dispatch");
    mUseDirectTaskDispatch = true;
  }

  // If the resolve/reject will be handled on a thread supporting priorities,
  // one may want to tweak the priority of the task by passing a
  // nsIRunnablePriority::PRIORITY_* to SetTaskPriority.
  void SetTaskPriority(uint32_t aPriority, const char* aSite) {
    PROMISE_ASSERT(mMagic1 == sMagic && mMagic2 == sMagic &&
                   mMagic3 == sMagic && mMagic4 == &mMutex);
    MutexAutoLock lock(mMutex);
    PROMISE_LOG("%s TaskPriority MozPromise (%p created at %s)", aSite, this,
                mCreationSite.get());
    MOZ_ASSERT(IsPending(),
               "A Promise must not have been already resolved or rejected to "
               "set dispatch state");
    MOZ_ASSERT(!mUseSynchronousTaskDispatch,
               "Promise already set for synchronous dispatch");
    MOZ_ASSERT(!mUseDirectTaskDispatch,
               "Promise already set for direct dispatch");
    mPriority = aPriority;
  }

 private:
  template <typename, typename>
  friend class MozPromiseHolderBase;

  // See MozPromiseHolderBase::RequireTailDispatch().
  void RequireTailDispatch(const char* aSite) {
#ifdef DEBUG
    PROMISE_ASSERT(mMagic1 == sMagic && mMagic2 == sMagic &&
                   mMagic3 == sMagic && mMagic4 == &mMutex);
    MutexAutoLock lock(mMutex);
    PROMISE_LOG("%s RequireTailDispatch MozPromise (%p created at %s)", aSite,
                this, mCreationSite.get());
    MOZ_ASSERT(IsPending(),
               "A Promise must not have been already resolved or rejected to "
               "set dispatch state");
    mRequireTailDispatch = true;
#endif
  }
};

/*
 * Class to encapsulate a promise for a particular role. Use this as the member
 * variable for a class whose method returns a promise.
 */
template <typename PromiseType, typename ImplType>
class MozPromiseHolderBase {
 public:
  MozPromiseHolderBase() = default;

  MozPromiseHolderBase(MozPromiseHolderBase&& aOther) noexcept = default;
  MozPromiseHolderBase& operator=(MozPromiseHolderBase&& aOther) noexcept =
      default;

  ~MozPromiseHolderBase() { MOZ_ASSERT(!mPromise); }

  already_AddRefed<PromiseType> Ensure(StaticString aMethodName) {
    static_cast<ImplType*>(this)->Check();
    if (!mPromise) {
      mPromise = new (typename PromiseType::Private)(aMethodName);
    }
    RefPtr<PromiseType> p = mPromise.get();
    return p.forget();
  }

  bool IsEmpty() const {
    static_cast<const ImplType*>(this)->Check();
    return !mPromise;
  }

  already_AddRefed<typename PromiseType::Private> Steal() {
    static_cast<ImplType*>(this)->Check();
    return mPromise.forget();
  }

  template <typename ResolveValueType_>
  void Resolve(ResolveValueType_&& aResolveValue, StaticString aMethodName) {
    static_assert(std::is_convertible_v<ResolveValueType_,
                                        typename PromiseType::ResolveValueType>,
                  "Resolve() argument must be implicitly convertible to "
                  "MozPromise's ResolveValueT");

    static_cast<ImplType*>(this)->Check();
    MOZ_ASSERT(mPromise);
    mPromise->Resolve(std::forward<ResolveValueType_>(aResolveValue),
                      aMethodName);
    mPromise = nullptr;
  }

  template <typename ResolveValueType_>
  void ResolveIfExists(ResolveValueType_&& aResolveValue,
                       StaticString aMethodName) {
    if (!IsEmpty()) {
      Resolve(std::forward<ResolveValueType_>(aResolveValue), aMethodName);
    }
  }

  template <typename RejectValueType_>
  void Reject(RejectValueType_&& aRejectValue, StaticString aMethodName) {
    static_assert(std::is_convertible_v<RejectValueType_,
                                        typename PromiseType::RejectValueType>,
                  "Reject() argument must be implicitly convertible to "
                  "MozPromise's RejectValueT");

    static_cast<ImplType*>(this)->Check();
    MOZ_ASSERT(mPromise);
    mPromise->Reject(std::forward<RejectValueType_>(aRejectValue), aMethodName);
    mPromise = nullptr;
  }

  template <typename RejectValueType_>
  void RejectIfExists(RejectValueType_&& aRejectValue,
                      StaticString aMethodName) {
    if (!IsEmpty()) {
      Reject(std::forward<RejectValueType_>(aRejectValue), aMethodName);
    }
  }

  template <typename ResolveOrRejectValueType_>
  void ResolveOrReject(ResolveOrRejectValueType_&& aValue,
                       StaticString aMethodName) {
    static_cast<ImplType*>(this)->Check();
    MOZ_ASSERT(mPromise);
    mPromise->ResolveOrReject(std::forward<ResolveOrRejectValueType_>(aValue),
                              aMethodName);
    mPromise = nullptr;
  }

  template <typename ResolveOrRejectValueType_>
  void ResolveOrRejectIfExists(ResolveOrRejectValueType_&& aValue,
                               StaticString aMethodName) {
    if (!IsEmpty()) {
      ResolveOrReject(std::forward<ResolveOrRejectValueType_>(aValue),
                      aMethodName);
    }
  }

  void UseSynchronousTaskDispatch(const char* aSite) {
    MOZ_ASSERT(mPromise);
    mPromise->UseSynchronousTaskDispatch(aSite);
  }

  void UseDirectTaskDispatch(const char* aSite) {
    MOZ_ASSERT(mPromise);
    mPromise->UseDirectTaskDispatch(aSite);
  }

  void SetTaskPriority(uint32_t aPriority, const char* aSite) {
    MOZ_ASSERT(mPromise);
    mPromise->SetTaskPriority(aPriority, aSite);
  }

  // Require the event target of each Then() on the promise to support tail
  // dispatch, see nsIEventTarget::SUPPORTS_TAIL_DISPATCH, so that a producer
  // settling the promise from a tail dispatching AbstractThread only queues the
  // resolve/reject callbacks on its tail dispatcher. Asserted in debug builds
  // when the callbacks are dispatched. UseSynchronousTaskDispatch() and
  // UseDirectTaskDispatch() take precedence for same-thread targets.
  void RequireTailDispatch(const char* aSite) {
    MOZ_ASSERT(mPromise);
    mPromise->RequireTailDispatch(aSite);
  }

 private:
  RefPtr<typename PromiseType::Private> mPromise;
};

template <typename PromiseType>
class MozPromiseHolder
    : public MozPromiseHolderBase<PromiseType, MozPromiseHolder<PromiseType>> {
 public:
  using MozPromiseHolderBase<
      PromiseType, MozPromiseHolder<PromiseType>>::MozPromiseHolderBase;
  static constexpr void Check() {};
};

template <typename PromiseType>
class MozMonitoredPromiseHolder
    : public MozPromiseHolderBase<PromiseType,
                                  MozMonitoredPromiseHolder<PromiseType>> {
 public:
  // Provide a Monitor that should always be held when accessing this instance.
  explicit MozMonitoredPromiseHolder(Monitor* const aMonitor)
      : mMonitor(aMonitor) {
    MOZ_ASSERT(aMonitor);
  }

  MozMonitoredPromiseHolder(MozMonitoredPromiseHolder&& aOther) = delete;
  MozMonitoredPromiseHolder& operator=(MozMonitoredPromiseHolder&& aOther) =
      delete;

  void Check() const { mMonitor->AssertCurrentThreadOwns(); }

 private:
  Monitor* const mMonitor;
};

/*
 * Class to encapsulate a MozPromise::Request reference. Use this as the member
 * variable for a class waiting on a MozPromise.
 */
template <typename PromiseType>
class MozPromiseRequestHolder {
 public:
  MozPromiseRequestHolder() = default;
  ~MozPromiseRequestHolder() { MOZ_ASSERT(!mRequest); }

  void Track(already_AddRefed<MozPromiseRequest> aRequest) {
    MOZ_DIAGNOSTIC_ASSERT(!Exists());
    mRequest = aRequest;
  }

  void Complete() {
    MOZ_DIAGNOSTIC_ASSERT(Exists());
    mRequest = nullptr;
  }

  // Disconnects and forgets an outstanding promise. The resolve/reject methods
  // will never be called.
  //
  // The callback object will be destroyed on the thread calling Disconnect.
  // This usually means you want to call it only on the request's target thread,
  // although it can technically be cancelled on any thread.
  void Disconnect() {
    MOZ_ASSERT(Exists());
    RefPtr request = std::move(mRequest);
    request->Disconnect();
  }

  void DisconnectIfExists() {
    if (Exists()) {
      Disconnect();
    }
  }

  bool Exists() const { return !!mRequest; }

 private:
  RefPtr<MozPromiseRequest> mRequest;
};

// Asynchronous Potentially-Cross-Thread Method Calls.
//
// This machinery allows callers to schedule a promise-returning function
// (a method and object, or a function object like a lambda) to be invoked
// asynchronously on a given thread, while at the same time receiving a
// promise upon which to invoke Then() immediately. InvokeAsync dispatches a
// task to invoke the function on the proper thread and also chain the
// resulting promise to the one that the caller received, so that resolve/
// reject values are forwarded through.

// Invoke a function object (e.g., lambda) asynchronously.
// Return a promise that the function should eventually resolve or reject.
template <std::invocable Function>
static auto InvokeAsync(
    nsISerialEventTarget* aTarget, StaticString aCallerName,
    Function&& aFunction,
    nsIEventTarget::DispatchFlags aDispatchFlags = NS_DISPATCH_NORMAL)
    -> std::invoke_result_t<Function> {
  static_assert(!std::is_lvalue_reference_v<Function>,
                "Function object must not be passed by lvalue-ref (to avoid "
                "unplanned copies); Consider move()ing the object.");

  static_assert(detail::IsRefPtrMozPromise<std::invoke_result_t<Function>>,
                "Function object must return RefPtr<MozPromise>");
  MOZ_ASSERT(aTarget);

  auto promise = MakeRefPtr<
      typename RemoveSmartPointer<std::invoke_result_t<Function>>::Private>(
      aCallerName, true /* aIsCompletionPromise */);
  nsresult rv = aTarget->Dispatch(
      NS_NewCancelableRunnableFunction(
          "InvokeAsync",
          [promise, function = std::forward<Function>(aFunction)]() mutable {
            auto result = std::invoke(std::move(function));
            result->ChainTo(promise.forget(), "<Proxy Promise>");
          }),
      aDispatchFlags);
  if (NS_FAILED(rv)) {
    // The task will never run, so the caller-visible promise would otherwise
    // never settle. Abandon it so downstream consumers are notified.
    promise->Abandon(aCallerName);
  }
  return promise;
}

// InvokeAsync on a method with optional explicitly-specified storages.
// See WrapAsFunctor in nsThreadUtils.h for help.
template <typename... Storages, typename PromiseType, typename ThisType,
          typename... ArgTypes, typename... ActualArgTypes>
static RefPtr<PromiseType> InvokeAsync(
    nsISerialEventTarget* aTarget, ThisType* aThisVal, StaticString aCallerName,
    RefPtr<PromiseType> (ThisType::*aMethod)(ArgTypes...),
    ActualArgTypes&&... aArgs) {
  static_assert(sizeof...(ArgTypes) == sizeof...(ActualArgTypes),
                "Method's ArgTypes and ActualArgTypes should have equal sizes");
  if constexpr (sizeof...(Storages) == 0) {
    static_assert(
        (!std::is_pointer_v<std::remove_reference_t<ActualArgTypes>> && ...),
        "Cannot pass pointer types through InvokeAsync, Storages must be "
        "provided");
    return InvokeAsync(
        aTarget, aCallerName,
        detail::WrapAsFunctor<std::decay_t<ActualArgTypes>...>(
            aThisVal, aMethod, std::forward<ActualArgTypes>(aArgs)...));
  } else {
    static_assert(
        sizeof...(Storages) == sizeof...(ArgTypes),
        "Provided Storages and method's ArgTypes should have equal sizes");
    return InvokeAsync(
        aTarget, aCallerName,
        detail::WrapAsFunctor<Storages...>(
            aThisVal, aMethod, std::forward<ActualArgTypes>(aArgs)...));
  }
}

#undef PROMISE_LOG
#undef PROMISE_WARNING
#undef PROMISE_ASSERT
#undef PROMISE_DEBUG

}  // namespace mozilla

#endif
