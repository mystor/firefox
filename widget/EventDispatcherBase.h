/* -*- Mode: c++; c-basic-offset: 2; tab-width: 20; indent-tabs-mode: nil; -*-
 * vim: set sw=2 ts=4 expandtab:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_EventDispatcherBase_h
#define mozilla_widget_EventDispatcherBase_h

#include "mozilla/EventTargetAndLockCapability.h"
#include "mozilla/Mutex.h"
#include "mozilla/widget/GeckoViewData.h"
#include "nsClassHashtable.h"
#include "nsIGeckoViewBridge.h"
#include "nsHashKeys.h"
#include "nsTObserverArray.h"

namespace mozilla::widget {

/**
 * EventDispatcherBase is the core Gecko implementation of the EventDispatcher
 * type in either Java or Swift. Together they make up a unified event bus.
 * Events dispatched from the embedder may notify listeners on the Gecko side
 * and vice versa.
 */
class EventDispatcherBase : public nsIGeckoViewEventDispatcher {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIGECKOVIEWEVENTDISPATCHER

  EventDispatcherBase() = default;

  // Base type to be used by embedder CallbackDelegate objects, used to wrap an
  // embedder-provided callback into a `nsIGeckoViewEventCallback` for use with
  // this type.
  class CallbackDelegateBase : public nsIGeckoViewEventCallback {
   public:
    NS_DECL_ISUPPORTS
    NS_DECL_NSIGECKOVIEWEVENTCALLBACK

    virtual void OnSuccess(const GeckoViewDataSource& aData,
                           ErrorResult& aRv) = 0;
    virtual void OnError(const GeckoViewDataSource& aData,
                         ErrorResult& aRv) = 0;

   protected:
    virtual ~CallbackDelegateBase() = default;
  };

  bool HasGeckoListener(const nsAString& aEvent) MOZ_EXCLUDES(mLock.Lock());
  nsresult DispatchToGecko(const nsAString& aEvent,
                           const GeckoViewDataSource& aData,
                           nsIGeckoViewEventCallback* aCallback)
      MOZ_REQUIRES(sMainThreadCapability);
  void Shutdown() MOZ_REQUIRES(sMainThreadCapability);

  virtual bool HasEmbedderListener(const nsAString& aEvent) = 0;
  virtual void DispatchToEmbedder(const nsAString& aEvent,
                                  const GeckoViewDataSource& aData,
                                  nsIGeckoViewEventCallback* aCallback,
                                  ErrorResult& aRv) = 0;

 protected:
  virtual ~EventDispatcherBase() = default;

 private:
  void Destroy();

  using ListenersList =
      nsAutoTObserverArray<nsCOMPtr<nsIGeckoViewEventListener>, 1>;

  // NOTE: This must be a nsClassHashtable to ensure that adding new keys to
  // mListenersMap does not cause the ListenersList instances within the array
  // to be relocated in memory.
  using ListenersMap = nsClassHashtable<nsStringHashKey, ListenersList>;

  MainThreadAndLockCapability<Mutex> mLock{"mozilla::EventDispatcherBase"};
  ListenersMap mListenersMap MOZ_GUARDED_BY(mLock);

  using IterateEventsCallback = nsresult (EventDispatcherBase::*)(
      const nsAString&, nsIGeckoViewEventListener*);

  nsresult IterateEvents(JSContext* aCx, JS::Handle<JS::Value> aEvents,
                         IterateEventsCallback aCallback,
                         nsIGeckoViewEventListener* aListener)
      MOZ_REQUIRES(sMainThreadCapability);
  nsresult RegisterEventLocked(const nsAString&, nsIGeckoViewEventListener*)
      MOZ_REQUIRES(mLock);
  nsresult UnregisterEventLocked(const nsAString&, nsIGeckoViewEventListener*)
      MOZ_REQUIRES(mLock);

  nsresult DispatchToGeckoInternal(ListenersList* list, const nsAString& aEvent,
                                   JS::Handle<JS::Value> aData,
                                   nsIGeckoViewEventCallback* aCallback)
      MOZ_REQUIRES(sMainThreadCapability);
};

}  // namespace mozilla::widget

#endif  // mozilla_widget_EventDispatcherBase_h
