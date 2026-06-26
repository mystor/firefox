/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/MozPromise.h"

#include "nsIDirectTaskDispatcher.h"

namespace mozilla {

LazyLogModule gMozPromiseLog("MozPromise");

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

NS_IMPL_ISUPPORTS0(MozPromiseRequest)

namespace detail {

NS_IMPL_ISUPPORTS_INHERITED(MozPromiseThenValueBase, MozPromiseRequest,
                            nsIRunnable, nsIDiscardableRunnable,
                            nsICancelableRunnable, nsIRunnablePriority,
                            nsINamed, nsITargetShutdownTask)

MozPromiseThenValueBase::MozPromiseThenValueBase(
    nsISerialEventTarget* aResponseTarget, StaticString aCallSite,
    nsIEventTarget::DispatchFlags aDispatchFlags)
    : mResponseTarget(aResponseTarget),
      mCallSite(aCallSite),
      mDispatchFlags(aDispatchFlags) {
  MOZ_ASSERT(!(mDispatchFlags & NS_DISPATCH_AT_END),
             "invalid dispatch flag for MozPromise::Then");
  MOZ_ASSERT(aResponseTarget);
}

NS_IMETHODIMP
MozPromiseThenValueBase::GetPriority(uint32_t* aPriority) {
  MutexAutoLock lock(mMutex);
  *aPriority = mPriority;
  return NS_OK;
}

NS_IMETHODIMP
MozPromiseThenValueBase::GetName(nsACString& aName) {
  aName.Truncate();
  aName.AppendPrintf("MozPromiseRequest(%s)", mCallSite.get());
  return NS_OK;
}

bool MozPromiseThenValueBase::RegisterShutdownTask() {
  nsresult rv = NS_ERROR_NOT_IMPLEMENTED;
  if (mResponseTarget->GetFeatures() &
      nsIEventTarget::SUPPORTS_SHUTDOWN_TASKS) {
    // NOTE: Try to register a shutdown task, rather than checking
    // `nsIEventTarget::GetFeatures` first.
    rv = mResponseTarget->RegisterShutdownTask(this);
    if (NS_SUCCEEDED(rv)) {
      return true;
    }

    // mResponseTarget supports, but is no longer accepting, shutdown tasks.
    // It may still accept dispatches during shutdown, which we can use to
    // notify the request of target shutdown.
    rv = mResponseTarget->Dispatch(do_AddRef(this),
                                   mDispatchFlags | NS_DISPATCH_FALLIBLE);
    if (NS_SUCCEEDED(rv)) {
      return true;
    }

    PROMISE_WARNING("->Then from '%s' failed: target is already dead",
                    mCallSite.get());
  } else {
    // If you're seeing this warning for your MozPromise, it means that the
    // event target you're attaching your MozPromise to is a non-standard one
    // which does not support nsITargetShutdownTask.
    //
    // If this target shuts down before the promise is resolved, you may
    // experience crashes, as we will be unable to notify the promise.
    PROMISE_WARNING(
        "target for ->Then from '%s' doesn't support nsITargetShutdownTask",
        mCallSite.get());
  }

  MutexAutoLock lock(mMutex);
  mRegisterShutdownTaskRv = rv;
  return false;
}

void MozPromiseThenValueBase::Dispatch(MozPromiseBase* aPromise,
                                       bool aUseSynchronousTaskDispatch,
                                       bool aUseDirectTaskDispatch,
                                       uint32_t aPriority) {
  {
    MutexAutoLock lock(mMutex);
    MOZ_ASSERT(!mSettledPromise, "Dispatch must only be called once");
    if (!HasCallback()) {
      return;
    }
    mSettledPromise = aPromise;
    mPriority = aPriority;
  }

  PROMISE_LOG("Then() call made from %s, %s dispatch", mCallSite.get(),
              aUseSynchronousTaskDispatch ? "synchronous"
              : aUseDirectTaskDispatch    ? "directtask"
                                          : "normal");

  if (aUseSynchronousTaskDispatch && mResponseTarget->IsOnCurrentThread()) {
    PROMISE_LOG("ThenValue::Dispatch running task synchronously [this=%p]",
                this);
    Run();
    return;
  }

  if (aUseDirectTaskDispatch && mResponseTarget->IsOnCurrentThread()) {
    PROMISE_LOG(
        "ThenValue::Dispatch dispatch task via direct task queue [this=%p]",
        this);
    nsCOMPtr<nsIDirectTaskDispatcher> dispatcher =
        do_QueryInterface(mResponseTarget);
    if (dispatcher &&
        NS_SUCCEEDED(dispatcher->DispatchDirectTask(do_AddRef(this)))) {
      return;
    }

    PROMISE_WARNING("Direct Task dispatching not available for thread \"%s\"",
                    PR_GetThreadName(PR_GetCurrentThread()));
  }

  // NOTE: We always pass `NS_DISPATCH_FALLIBLE` here as it's always possible
  // someone raced with this dispatch and called `Disconnect()`, the shutdown
  // task started being notified, or any other number of reasons why this
  // dispatch failing shouldn't lead to a leak.
  nsresult rv = mResponseTarget->Dispatch(
      do_AddRef(this), mDispatchFlags | NS_DISPATCH_FALLIBLE);
  if (NS_FAILED(rv)) {
    // Tell our subclass that the dispatch failed, so it can record the error
    // code, and potentially abandon completion promises.
    DispatchFailed(rv);
  }
}

}  // namespace detail
}  // namespace mozilla

#undef PROMISE_LOG
#undef PROMISE_WARNING
