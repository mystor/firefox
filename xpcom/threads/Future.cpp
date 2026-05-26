/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Future.h"

#include "nsIDirectTaskDispatcher.h"
#include "nsIEventTarget.h"
#include "nsISupports.h"
#include "nsITargetShutdownTask.h"

namespace mozilla {

namespace future_detail {

// A "Waker" object, representing a call to "Await" on a future's kernel. This
// object represents the request for a task to be run on a given event target
// when the future is fulfilled.
//
// The FutureWaker also handles various failure states, such as the target dying
// before the future can be resolved. There is a reference cycle between mTarget
// and the waker. This cycle is broken when the task is run, or the target is
// shutdown.
//
// If the callback is called with `nullptr` as the kernel argument, that
// indicates a failure to resolve.
class DispatchWaker final : public Runnable,
                            public IFutureWaker,
                            public nsITargetShutdownTask,
                            public nsIRunnablePriority {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIRUNNABLE
  NS_DECL_NSIRUNNABLEPRIORITY

  DispatchWaker(StaticString aName, nsIEventTarget* aTarget,
                FutureDispatchOptions aOptions,
                MoveOnlyFunction<void(FutureKernelBase*)> aCallback)
      : Runnable(aName),
        mTarget(aTarget),
        mOptions(aOptions),
        mMutex{"FutureWaker"},
        mValue(nullptr),
        mCallback(std::move(aCallback)) {}

  nsresult Init();

  void Wake(FutureKernelBase* aKernel) override;

  void TargetShutdown() override { Run(); }

 private:
  ~DispatchWaker() = default;

  const nsCOMPtr<nsIEventTarget> mTarget;
  const FutureDispatchOptions mOptions;

  Mutex mMutex;
  RefPtr<FutureKernelBase> mValue MOZ_GUARDED_BY(mMutex);
  MoveOnlyFunction<void(FutureKernelBase*)> mCallback MOZ_GUARDED_BY(mMutex);
};

NS_IMPL_ISUPPORTS_INHERITED(DispatchWaker, Runnable, nsITargetShutdownTask,
                            nsIRunnablePriority)

nsresult DispatchWaker::Init() {
  nsresult rv = mTarget->RegisterShutdownTask(this);
  if (NS_FAILED(rv)) {
    MOZ_DIAGNOSTIC_ASSERT(rv == NS_ERROR_UNEXPECTED,
                          "Future target must support RegisterShutdownTask");
    NS_WARNING("Future init after target is shutdown");
    return rv;
  }
  return NS_OK;
}

void DispatchWaker::Wake(FutureKernelBase* aKernel) {
  {
    MutexAutoLock lock(mMutex);
    MOZ_ASSERT(!mValue, "Calling Dispatch on the same FutureWaker twice?");
    if (!mCallback) {
      // We've already been woken (e.g. by TargetShutdown).
      return;
    }
    mValue = aKernel;
  }

  // Attempt DTD if we've been configured to. Fall back to dispatch on failure.
  if (!mOptions.disableDirectTaskDispatch && mTarget->IsOnCurrentThread()) {
    nsCOMPtr<nsIDirectTaskDispatcher> dtd = do_QueryInterface(mTarget);
    if (dtd && NS_SUCCEEDED(dtd->DispatchDirectTask(do_AddRef(this)))) {
      return;
    }
  }

  // NOTE: Always set NS_DISPATCH_FALLIBLE, and ignore the return status.
  // If this dispatch fails, we'll still be run due to nsITargetShutdownTask.
  nsIEventTarget::DispatchFlags flags = NS_DISPATCH_FALLIBLE;
  if (mOptions.mayBlock) {
    flags |= NS_DISPATCH_EVENT_MAY_BLOCK;
  }
  (void)mTarget->Dispatch(do_AddRef(this), flags);
}

NS_IMETHODIMP DispatchWaker::Run() {
  // This can fail e.g. if we're already being run due to a shutdown task, but
  // it doesn't hurt to ensure that we're definitely no longer a shutdown task
  // on the target.
  (void)mTarget->UnregisterShutdownTask(this);

  RefPtr<FutureKernelBase> value;
  MoveOnlyFunction<void(FutureKernelBase*)> callback;
  {
    MutexAutoLock lock(mMutex);
    value = mValue.forget();
    callback = std::exchange(mCallback, nullptr);
  }
  if (callback) {
    callback(value);
  }
  return NS_OK;
}

NS_IMETHODIMP DispatchWaker::GetPriority(uint32_t* aPriority) {
  *aPriority = mOptions.priority;
  return NS_OK;
}

// A dummy synchronous waiter object.
// SyncWaker directly invokes the callback on the waking thread.
class SyncWaker final : public IFutureWaker {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS

  SyncWaker(StaticString aName,
            MoveOnlyFunction<void(FutureKernelBase*)> aCallback)
      : mCallback(std::move(aCallback)) {}

  void Wake(FutureKernelBase* aKernel) override {
    mCallback(aKernel);
    mCallback = nullptr;
  }

 private:
  ~SyncWaker() = default;

  MoveOnlyFunction<void(FutureKernelBase*)> mCallback;
};

NS_IMPL_ISUPPORTS0(SyncWaker)

bool FutureKernelBase::IsSettled() {
  MutexAutoLock lock(mMutex);
  return mState == State::Resolved || mState == State::Failed;
}

bool FutureKernelBase::IsResolved() {
  MutexAutoLock lock(mMutex);
  MOZ_ASSERT(mState == State::Resolved || mState == State::Failed,
             "Kernel must be settled before calling IsResolved");
  return mState == State::Resolved;
}

void FutureKernelBase::SetFailed() {
  nsTArray<RefPtr<IFutureWaker>> wakers;
  {
    MutexAutoLock lock(mMutex);
    if (mState != State::Unresolved) {
      return;
    }
    mState = State::Failed;
    wakers.SwapElements(mWakers);
  }

  for (auto& waker : wakers) {
    waker->Wake(nullptr);
  }
}

bool FutureKernelBase::BeginResolving() {
  MutexAutoLock lock(mMutex);
  if (mState != State::Unresolved) {
    return false;
  }
  mState = State::Resolving;
  return true;
}

void FutureKernelBase::FinishResolving() {
  nsTArray<RefPtr<IFutureWaker>> wakers;
  {
    MutexAutoLock lock(mMutex);
    MOZ_ASSERT(mState == State::Resolving,
               "Must have previously called BeginResolving");
    mState = State::Resolved;
    wakers.SwapElements(mWakers);
  }

  for (auto& waker : wakers) {
    waker->Wake(this);
  }
}

void FutureKernelBase::AddWaker(IFutureWaker* aWaker) {
  FutureKernelBase* result = nullptr;
  {
    MutexAutoLock lock(mMutex);
    // If we're not resolved or failed yet, add to the mWakers queue.
    if (mState == State::Unresolved || mState == State::Resolving) {
      mWakers.AppendElement(aWaker);
      return;
    }

    // Otherwise, we've already been resolved or rejected,
    // Immediately wake `aWaker`.
    MOZ_ASSERT(mState == State::Resolved || mState == State::Failed);
    result = mState == State::Resolved ? this : nullptr;
  }
  aWaker->Wake(result);
}

nsresult FutureKernelBase::AddDispatchWaker(
    StaticString aName, nsIEventTarget* aTarget, FutureDispatchOptions aOptions,
    MoveOnlyFunction<void(FutureKernelBase*)> aCallback) {
  RefPtr<DispatchWaker> waker =
      MakeRefPtr<DispatchWaker>(aName, aTarget, aOptions, std::move(aCallback));
  nsresult rv = waker->Init();
  NS_ENSURE_SUCCESS(rv, rv);
  AddWaker(waker);
  return NS_OK;
}

void FutureKernelBase::AddSyncWaker(
    StaticString aName, MoveOnlyFunction<void(FutureKernelBase*)> aCallback) {
  RefPtr<SyncWaker> waker = MakeRefPtr<SyncWaker>(aName, std::move(aCallback));
  AddWaker(waker);
}

}  // namespace future_detail

}  // namespace mozilla
