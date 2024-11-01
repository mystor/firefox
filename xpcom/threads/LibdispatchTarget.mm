/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "LibdispatchTarget.h"

namespace mozilla {

NS_IMPL_ISUPPORTS(LibdispatchTarget, nsISerialEventTarget, nsIEventTarget)

LibdispatchTarget::LibdispatchTarget(const char* aLabel)
    : mQueue(AdoptDarwinObject(
          dispatch_queue_create(aLabel, DISPATCH_QUEUE_SERIAL))) {}

// NOTE: The SerialEventTargetGuard will assert IsOnCurrentThread() in its
// constructor, so we must ensure the relevant state is initialized before that
// subobject constructor is invoked. To do this, the state is set up in an
// initializer.
LibdispatchTarget::AutoOnQueue::AutoOnQueue(LibdispatchTarget* aTarget)
    : mTarget([&aTarget] {
        dispatch_assert_queue(aTarget->mQueue.get());

        // Update mThread to reflect this current thread during the execution of
        // this block so that `IsOnCurrentThread()` reflects that this thread is
        // executing the queue.
        PRThread* prev = aTarget->mThread.exchange(PR_GetCurrentThread());
        MOZ_RELEASE_ASSERT(!prev);
        return aTarget;
      }()),
      // Now that mTarget->mThread has been updated, we can safely initialize
      // `mInnerGuard`.
      mInnerGuard(mTarget) {}

LibdispatchTarget::AutoOnQueue::~AutoOnQueue() {
  PRThread* prev = mTarget->mThread.exchange(nullptr);
  MOZ_RELEASE_ASSERT(prev == PR_GetCurrentThread());
  // NOTE: `mInnerGuard` will be cleaned up after `IsOnCurrentThread` is no
  // longer valid, but that's OK here.
}

bool LibdispatchTarget::IsOnCurrentThreadInfallible() {
  if (PRThread* thread = mThread) {
    return thread == PR_GetCurrentThread();
  }

  // If `mThread` is not set, assert that we're not on the dispatch queue. If
  // this assertion fires, it means that `IsOnCurrentThread` was called on the
  // queue when `AutoOnQueue` was not on the stack.
  dispatch_assert_queue_not(mQueue.get());
  return false;
}

nsresult LibdispatchTarget::IsOnCurrentThread(bool* _retval) {
  *_retval = IsOnCurrentThread();
  return NS_OK;
}

nsresult LibdispatchTarget::Dispatch(already_AddRefed<nsIRunnable> event,
                                     uint32_t flags) {
  dispatch_async(
      mQueue.get(), [self = RefPtr{this}, event = RefPtr{event}]() mutable {
        AutoOnQueue guard(self);
        event->Run();
        event = nullptr;  // Ensure event is released while on the queue.
      });
  return NS_OK;
}

nsresult LibdispatchTarget::DispatchFromScript(nsIRunnable* event,
                                               uint32_t flags) {
  return Dispatch(do_AddRef(event), flags);
}

nsresult LibdispatchTarget::DelayedDispatch(already_AddRefed<nsIRunnable> event,
                                            uint32_t delay) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

nsresult LibdispatchTarget::RegisterShutdownTask(nsITargetShutdownTask* task) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

nsresult LibdispatchTarget::UnregisterShutdownTask(
    nsITargetShutdownTask* task) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

}  // namespace mozilla
