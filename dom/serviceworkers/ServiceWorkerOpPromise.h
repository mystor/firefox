/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_serviceworkeroppromise_h_
#define mozilla_dom_serviceworkeroppromise_h_

#include "mozilla/MozPromise.h"
#include "mozilla/MozPromiseFwd.h"
#include "mozilla/Result.h"
#include "mozilla/dom/SafeRefPtr.h"
#include "mozilla/dom/ServiceWorkerOpArgs.h"

namespace mozilla {

template <>
struct DefaultAbandonHandler<dom::CancelInterceptionArgs> {
  dom::CancelInterceptionArgs operator()() const {
    return dom::CancelInterceptionArgs(NS_ERROR_MOZPROMISE_ABANDONED,
                                       dom::FetchEventTimeStamps{});
  }
};

namespace dom {

class InternalResponse;

using SynthesizeResponseArgs = std::pair<SafeRefPtr<InternalResponse>,
                                         ChildToParentSynthesizeResponseArgs>;

using FetchEventRespondWithResult =
    Variant<SynthesizeResponseArgs, ResetInterceptionArgs,
            CancelInterceptionArgs>;

using FetchEventRespondWithPromise =
    MozPromise<FetchEventRespondWithResult, CancelInterceptionArgs, true>;

using FetchEventPreloadResponseAvailablePromise =
    MozPromise<SafeRefPtr<InternalResponse>, PromiseAbandoned, true>;

using FetchEventPreloadResponseTimingPromise =
    MozPromise<ResponseTiming, PromiseAbandoned, true>;

using FetchEventPreloadResponseEndPromise =
    MozPromise<ResponseEndArgs, PromiseAbandoned, true>;

using ServiceWorkerOpPromise =
    MozPromise<ServiceWorkerOpResult, nsresult, true>;

// Resolves once a push message's push event has settled, rejects with the
// reason the event never reached the worker
using PushHandledPromise = MozPromise<Ok, nsresult, /* IsExclusive = */ true>;

using ServiceWorkerFetchEventOpPromise =
    MozPromise<ServiceWorkerFetchEventOpResult, nsresult, true>;

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_serviceworkeroppromise_h_
