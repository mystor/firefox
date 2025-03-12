/* -*- Mode: c++; c-basic-offset: 2; tab-width: 20; indent-tabs-mode: nil; -*-
 * vim: set sw=2 ts=4 expandtab:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "EventDispatcherBase.h"

#include "js/Array.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/widget/GeckoViewDataJS.h"
#include "nsJSUtils.h"

namespace mozilla::widget {

namespace {
// Helper type used internally to transform a pair of
// `nsIGeckoViewEventCallback` and `nsIGeckoViewEventFinalizer` into a single
// `nsIGeckoViewEventCallback` which invokes the finalizer in its destructor.
class FinalizingCallbackDelegate final : public nsIGeckoViewEventCallback {
 public:
  NS_DECL_ISUPPORTS
  NS_FORWARD_NSIGECKOVIEWEVENTCALLBACK(mCallback->);

  FinalizingCallbackDelegate(nsIGeckoViewEventCallback* aCallback,
                             nsIGeckoViewEventFinalizer* aFinalizer)
      : mCallback(aCallback), mFinalizer(aFinalizer) {}

  nsIGeckoViewEventCallback* WrappedCallback() { return mCallback; }

 private:
  virtual ~FinalizingCallbackDelegate() {
    if (mFinalizer) {
      mFinalizer->OnFinalize();
    }
  }

  const nsCOMPtr<nsIGeckoViewEventCallback> mCallback;
  const nsCOMPtr<nsIGeckoViewEventFinalizer> mFinalizer;
};

NS_IMPL_ISUPPORTS(FinalizingCallbackDelegate, nsIGeckoViewEventCallback)
}  // namespace

NS_IMETHODIMP EventDispatcherBase::CallbackDelegateBase::OnSuccess(
    JS::Handle<JS::Value> aValue, JSContext* aCx) {
  ErrorResult error;
  OnSuccess(GeckoViewDataJSSource(aCx, aValue), error);
  if (error.MaybeSetPendingException(aCx,
                                     "Error dispatching GeckoView callback")) {
    JS::WarnUTF8(aCx, "Error dispatching GeckoView callback");
    return NS_ERROR_INVALID_ARG;
  }
  return NS_OK;
}
NS_IMETHODIMP EventDispatcherBase::CallbackDelegateBase::OnError(
    JS::Handle<JS::Value> aValue, JSContext* aCx) {
  ErrorResult error;
  OnSuccess(GeckoViewDataJSSource(aCx, aValue), error);
  if (error.MaybeSetPendingException(aCx,
                                     "Error dispatching GeckoView callback")) {
    JS::WarnUTF8(aCx, "Error dispatching GeckoView callback");
    return NS_ERROR_INVALID_ARG;
  }
  return NS_OK;
}

NS_IMPL_ISUPPORTS(EventDispatcherBase::CallbackDelegateBase,
                  nsIGeckoViewEventCallback)

// This type is threadsafe refcounted, as it could theoretically be acessed from
// off-main-thread, but must only be destroyed on the main thread (due to
// holding main-thread only references to JS objects).
NS_IMPL_ADDREF(EventDispatcherBase)
NS_IMPL_RELEASE_WITH_DESTROY(EventDispatcherBase, Destroy())
NS_IMPL_QUERY_INTERFACE(EventDispatcherBase, nsIGeckoViewEventDispatcher)

void EventDispatcherBase::Destroy() {
  NS_PROXY_DELETE_TO_EVENT_TARGET(EventDispatcherBase,
                                  GetMainThreadSerialEventTarget());
}

nsresult EventDispatcherBase::DispatchToGeckoInternal(
    ListenersList* list, const nsAString& aEvent, JS::Handle<JS::Value> aData,
    nsIGeckoViewEventCallback* aCallback) {
  mLock.NoteOnMainThread();

  dom::AutoNoJSAPI nojsapi;

  for (const auto& ent : list->ForwardRange()) {
    // NOTE: Hold a strong reference to the listener, as the observer array can
    // be mutated during this call.
    nsCOMPtr<nsIGeckoViewEventListener> listener = ent;
    nsresult rv = listener->OnEvent(aEvent, aData, aCallback);

    // Discard any errors encountered while dispatching so we don't miss
    // listeners.
    Unused << NS_WARN_IF(NS_FAILED(rv));
  }

  return NS_OK;
}

NS_IMETHODIMP
EventDispatcherBase::Dispatch(JS::Handle<JS::Value> aEvent,
                              JS::Handle<JS::Value> aData,
                              nsIGeckoViewEventCallback* aCallback,
                              nsIGeckoViewEventFinalizer* aFinalizer,
                              JSContext* aCx) {
  AssertIsOnMainThread();
  mLock.NoteOnMainThread();

  // Manually convert the event string from JS.
  // See bug 1334728 for why AString is not used here.
  if (!aEvent.isString()) {
    NS_WARNING("Invalid event name");
    return NS_ERROR_INVALID_ARG;
  }
  nsAutoJSString event;
  if (!event.init(aCx, aEvent.toString())) {
    JS_ClearPendingException(aCx);
    return NS_ERROR_OUT_OF_MEMORY;
  }

  // If a finalizer is provided, use FinalizingCallbackDelegate to wrap the
  // type.
  nsCOMPtr<nsIGeckoViewEventCallback> callback =
      (aCallback && aFinalizer)
          ? new FinalizingCallbackDelegate(aCallback, aFinalizer)
          : aCallback;

  // Don't need to lock here because we're on the main thread, and we can't
  // race against Register/UnregisterListener.

  if (ListenersList* list = mListenersMap.Get(event)) {
    return DispatchToGeckoInternal(list, event, aData, callback);
  }

  ErrorResult error;
  DispatchToEmbedder(event, GeckoViewDataJSSource(aCx, aData), callback, error);
  if (error.MaybeSetPendingException(aCx,
                                     "Error dispatching GeckoView event")) {
    return NS_ERROR_INVALID_ARG;
  }
  return NS_OK;
}

// Given a JS value which is either a string or an array of strings, call the
// given `aCallback` method for each string with the mutex held.
nsresult EventDispatcherBase::IterateEvents(
    JSContext* aCx, JS::Handle<JS::Value> aEvents,
    IterateEventsCallback aCallback, nsIGeckoViewEventListener* aListener) {
  MutexAutoLock lock(mLock.Lock());
  mLock.NoteExclusiveAccess();

  auto processEvent = [&](JS::Handle<JS::Value> event) -> nsresult {
    nsAutoJSString str;
    if (!str.init(aCx, event.toString())) {
      JS_ClearPendingException(aCx);
      return NS_ERROR_OUT_OF_MEMORY;
    }
    return (this->*aCallback)(str, aListener);
  };

  // NOTE: This does manual jsapi processing, rather than using something like
  // WebIDL for simplicity for historical reasons.
  // It may be related to wanting to avoid invalid values being passed in and
  // coerced to strings.
  if (aEvents.isString()) {
    return processEvent(aEvents);
  }

  bool isArray = false;
  NS_ENSURE_TRUE(aEvents.isObject(), NS_ERROR_INVALID_ARG);
  if (!JS::IsArrayObject(aCx, aEvents, &isArray)) {
    JS_ClearPendingException(aCx);
    return NS_ERROR_INVALID_ARG;
  }
  NS_ENSURE_TRUE(isArray, NS_ERROR_INVALID_ARG);

  JS::Rooted<JSObject*> events(aCx, &aEvents.toObject());
  uint32_t length = 0;
  if (!JS::GetArrayLength(aCx, events, &length)) {
    JS_ClearPendingException(aCx);
    return NS_ERROR_INVALID_ARG;
  }
  NS_ENSURE_TRUE(length, NS_ERROR_INVALID_ARG);

  for (size_t i = 0; i < length; i++) {
    JS::Rooted<JS::Value> event(aCx);
    if (!JS_GetElement(aCx, events, i, &event)) {
      JS_ClearPendingException(aCx);
      return NS_ERROR_INVALID_ARG;
    }
    NS_ENSURE_TRUE(event.isString(), NS_ERROR_INVALID_ARG);

    nsresult rv = processEvent(event);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  return NS_OK;
}

nsresult EventDispatcherBase::RegisterEventLocked(
    const nsAString& aEvent, nsIGeckoViewEventListener* aListener) {
  ListenersList* list = mListenersMap.GetOrInsertNew(aEvent);

  // NOTE: Previously this code would return an error if the entry already
  // existed, but only in debug builds. This has been upgraded to a debug
  // assert, making the code always return success for more consistency between
  // debug & release builds in terms of runtime behaviour.
  if (NS_WARN_IF(list->Contains(aListener))) {
    MOZ_ASSERT_UNREACHABLE("Attempt to register the same listener twice");
    return NS_OK;
  }

  list->AppendElement(aListener);
  return NS_OK;
}

NS_IMETHODIMP
EventDispatcherBase::RegisterListener(nsIGeckoViewEventListener* aListener,
                                      JS::Handle<JS::Value> aEvents,
                                      JSContext* aCx) {
  AssertIsOnMainThread();
  return IterateEvents(aCx, aEvents, &EventDispatcherBase::RegisterEventLocked,
                       aListener);
}

nsresult EventDispatcherBase::UnregisterEventLocked(
    const nsAString& aEvent, nsIGeckoViewEventListener* aListener) {
  // NOTE: Previously this code would return an error if the entry didn't exist
  // but only in debug builds. This has been upgraded to a debug assert, making
  // the code always return success for more consistency between debug & release
  // builds in terms of runtime behaviour.
  ListenersList* list = mListenersMap.Get(aEvent);
  MOZ_ASSERT(list);
  NS_ENSURE_TRUE(list, NS_OK);

  DebugOnly<bool> found = list->RemoveElement(aListener);
  MOZ_ASSERT(found);

  // NOTE: We intentionally do not remove the entry from `mListenersMap` here,
  // as other code higher up the stack could be holding a reference to this
  // nsTObserverArray through an iterator.
  return NS_OK;
}

NS_IMETHODIMP
EventDispatcherBase::UnregisterListener(nsIGeckoViewEventListener* aListener,
                                        JS::Handle<JS::Value> aEvents,
                                        JSContext* aCx) {
  AssertIsOnMainThread();
  return IterateEvents(aCx, aEvents,
                       &EventDispatcherBase::UnregisterEventLocked, aListener);
}

bool EventDispatcherBase::HasGeckoListener(const nsAString& aEvent) {
  // NOTE: This can be called on any thread, so must hold the mutex.
  MutexAutoLock lock(mLock.Lock());
  mLock.NoteLockHeld();

  ListenersList* list = mListenersMap.Get(aEvent);
  return list && !list->IsEmpty();
}

nsresult EventDispatcherBase::DispatchToGecko(
    const nsAString& aEvent, const GeckoViewDataSource& aData,
    nsIGeckoViewEventCallback* aCallback) {
  mLock.NoteOnMainThread();

  // If there are no Gecko listeners for this event, abort early.
  ListenersList* list = mListenersMap.Get(aEvent);
  if (!list || list->IsEmpty()) {
    return NS_OK;
  }

  // Convert data into a JS object in the shared JS Module scope.
  // We always use this scope, as it is long lived, and the callback will always
  // be in a system scope of some kind.
  dom::AutoJSAPI jsapi;
  if (NS_WARN_IF(!jsapi.Init(xpc::PrivilegedJunkScope()))) {
    return NS_ERROR_FAILURE;
  }

  ErrorResult error;
  JS::Rooted<JS::Value> data(jsapi.cx());
  GeckoViewDataJSSink sink(jsapi.cx(), &data);
  aData.ToSink(sink, error);

  // Report errors by throwing them into the context (they'll be reported in
  // the AutoJSAPI destructor), and a
  if (error.MaybeSetPendingException(jsapi.cx(),
                                     "Error dispatching GeckoView event")) {
    JS::WarnUTF8(jsapi.cx(), "Error dispatching %s",
                 NS_ConvertUTF16toUTF8(aEvent).get());
    return NS_ERROR_INVALID_ARG;
  }

  // Actually call the Gecko listeners.
  return DispatchToGeckoInternal(list, aEvent, data, aCallback);
}

void EventDispatcherBase::Shutdown() {
  ListenersMap listeners;
  {
    MutexAutoLock lock(mLock.Lock());
    mLock.NoteExclusiveAccess();

    // Ensure listeners are dropped while the lock isn't held.
    listeners = std::move(mListenersMap);
    mListenersMap.Clear();
  }
}

}  // namespace mozilla::widget

static void DoCallImpl(nsIGeckoViewEventCallback* aCallback,
                       const mozilla::widget::GeckoViewDataSource& aData,
                       nsresult (nsIGeckoViewEventCallback::*aCall)(
                           JS::Handle<JS::Value>, JSContext*)) {
  MOZ_ASSERT(NS_IsMainThread());

  mozilla::dom::AutoJSAPI jsapi;
  NS_ENSURE_TRUE_VOID(jsapi.Init(xpc::PrivilegedJunkScope()));

  // Translate aData to JS with GeckoViewData. If an error is encountered, set
  // it on the JSContext. It will be reported by the AutoJSAPI destructor.
  JS::Rooted<JS::Value> data(jsapi.cx());
  {
    mozilla::ErrorResult error;
    mozilla::widget::GeckoViewDataJSSink sink(jsapi.cx(), &data);
    aData.ToSink(sink, error);
    if (error.MaybeSetPendingException(jsapi.cx(),
                                       "Error dispatching callback")) {
      JS::WarnUTF8(jsapi.cx(), "Error dispatching GeckoView callback");
      return;
    }
  }

  NS_ENSURE_SUCCESS_VOID((aCallback->*aCall)(data, jsapi.cx()));
}

void nsIGeckoViewEventCallback::DoOnSuccess(
    const mozilla::widget::GeckoViewDataSource& aData) {
  DoCallImpl(this, aData, &nsIGeckoViewEventCallback::OnSuccess);
}

void nsIGeckoViewEventCallback::DoOnError(
    const mozilla::widget::GeckoViewDataSource& aData) {
  DoCallImpl(this, aData, &nsIGeckoViewEventCallback::OnError);
}
