/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_LibdispatchTarget_h
#define mozilla_LibdispatchTarget_h

#include <dispatch/dispatch.h>

#include "mozilla/DarwinObjectPtr.h"
#include "nsISerialEventTarget.h"
#include "nsThreadUtils.h"

namespace mozilla {

class LibdispatchTarget : public nsISerialEventTarget {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIEVENTTARGET_FULL

  // Create a serial dispatch_queue_t with the given label, and wrap it as an
  // XPCOM event target.
  LibdispatchTarget(const char* aLabel);

  // The raw serial dispatch_queue_t which is used to implement this target.
  //
  // NOTE: GCD blocks dispatched to this queue will not pass
  // `IsOnCurrentThread()` unless there is an active `AutoOnQueue` on the stack.
  dispatch_queue_t Queue() { return mQueue.get(); }

  // Helper RAII type to indicate that a block is expected to run on this
  // target. Will assert that we're on the queue, and the current thread on the
  // event target.
  class MOZ_RAII AutoOnQueue {
   public:
    AutoOnQueue(LibdispatchTarget* aTarget);
    ~AutoOnQueue();

    AutoOnQueue(const AutoOnQueue&) = delete;
    AutoOnQueue& operator=(const AutoOnQueue&) = delete;

   private:
    LibdispatchTarget* mTarget;
    SerialEventTargetGuard mInnerGuard;
  };

 private:
  virtual ~LibdispatchTarget() = default;

  const DarwinObjectPtr<dispatch_queue_t> mQueue;
};

}  // namespace mozilla

#endif  // mozilla_LibdispatchTarget_h
