/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_MozPromiseFwd_h
#define mozilla_MozPromiseFwd_h

#include "nsError.h"

namespace mozilla {

// Marker struct which can be used as a "reject" type when the only reason the
// promise can fail is due to the promise being abandoned.
struct PromiseAbandoned {};

template <typename RejectValueT>
struct DefaultAbandonHandler;

template <typename ResolveValueT, typename RejectValueT, bool IsExclusive,
          typename AbandonHandler = DefaultAbandonHandler<RejectValueT>>
class MozPromise;

// A generic promise type that does the trick for simple use cases.
//
// Vaguely deprecated: prefer explicitly naming the resolve- and reject-type.
// Additionally, prefer `mozilla::Ok` as the resolve-type if the boolean's value
// is irrelevant.
using GenericPromise = MozPromise<bool, nsresult, /* IsExclusive = */ true>;

// A generic, non-exclusive promise type that does the trick for simple use
// cases.
//
// Vaguely deprecated, as above.
using GenericNonExclusivePromise =
    MozPromise<bool, nsresult, /* IsExclusive = */ false>;

class MozPromiseRequest;

template <typename PromiseType>
class MozPromiseHolder;

template <typename PromiseType>
class MozMonitoredPromiseHolder;

template <typename PromiseType>
class MozPromiseRequestHolder;

}  // namespace mozilla

#endif  // mozilla_MozPromiseFwd_h
