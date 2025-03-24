/* -*- Mode: c++; c-basic-offset: 2; tab-width: 20; indent-tabs-mode: nil; -*-
 * vim: set sw=2 ts=4 expandtab:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "EventDispatcher.h"

#include "mozilla/MacStringHelpers.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/widget/GeckoViewDataCF.h"
#include "mozilla/widget/GeckoViewSupport.h"

using namespace mozilla;
using namespace mozilla::widget;

namespace {
class SwiftCallbackDelegate final
    : public EventDispatcherBase::CallbackDelegateBase {
 public:
  explicit SwiftCallbackDelegate(id<EventCallback> aCallback)
      : mCallback(aCallback) {
    [aCallback retain];
  }

  void OnSuccess(const GeckoViewDataSource& aData, ErrorResult& aRv) override {
    CFTypeRefPtr<CFTypeRef> data;
    GeckoViewDataCFSink sink(data);
    aData.ToSink(sink, aRv);
    if (!aRv.Failed()) {
      [mCallback sendSuccess:(id)data.get()];
    }
  }

  void OnError(const GeckoViewDataSource& aData, ErrorResult& aRv) override {
    CFTypeRefPtr<CFTypeRef> data;
    GeckoViewDataCFSink sink(data);
    aData.ToSink(sink, aRv);
    if (!aRv.Failed()) {
      [mCallback sendError:(id)data.get()];
    }
  }

 private:
  virtual ~SwiftCallbackDelegate() { [mCallback release]; }

  id<EventCallback> mCallback;
};
}  // namespace

// Objective-C wrapper for a nsIGeckoViewEventCallback.
@interface NativeCallbackDelegateSupport : NSObject <EventCallback> {
  nsCOMPtr<nsIGeckoViewEventCallback> mCallback;
}

- (id)initWithCallback:(nsIGeckoViewEventCallback*)callback;
- (void)sendSuccess:(id)response;
- (void)sendError:(id)response;
@end

@implementation NativeCallbackDelegateSupport
- (id)initWithCallback:(nsIGeckoViewEventCallback*)callback {
  self = [super init];
  mCallback = callback;
  return self;
}
- (void)sendSuccess:(id)response {
  AssertIsOnMainThread();
  mCallback->DoOnSuccess(GeckoViewDataCFSource((CFTypeRef)response));
}
- (void)sendError:(id)response {
  AssertIsOnMainThread();
  mCallback->DoOnError(GeckoViewDataCFSource((CFTypeRef)response));
}
@end

// Objective-C wrapper for an EventDispatcher.
@interface EventDispatcherImpl : NSObject <GeckoEventDispatcher> {
  RefPtr<EventDispatcher> mDispatcher;
}

- (id)initWithDispatcher:(EventDispatcher*)dispatcher;

@end

@implementation EventDispatcherImpl

- (id)initWithDispatcher:(EventDispatcher*)dispatcher {
  self = [super init];
  self->mDispatcher = dispatcher;
  return self;
}

- (void)dispatchToGecko:(NSString*)type
                message:(id)message
               callback:(id<EventCallback>)callback {
  AssertIsOnMainThread();

  nsString event;
  CopyNSStringToXPCOMString(type, event);

  nsCOMPtr<nsIGeckoViewEventCallback> geckoCb;
  if (callback) {
    geckoCb = new SwiftCallbackDelegate(callback);
  }

  mDispatcher->DispatchToGecko(event, GeckoViewDataCFSource((CFTypeRef)message),
                               geckoCb);
}

- (BOOL)hasListener:(NSString*)type {
  nsString event;
  CopyNSStringToXPCOMString(type, event);

  return mDispatcher->HasGeckoListener(event);
}

@end

namespace mozilla::widget {

bool EventDispatcher::HasEmbedderListener(const nsAString& aEvent) {
  id<SwiftEventDispatcher> dispatcher = (id<SwiftEventDispatcher>)mDispatcher;
  return [dispatcher hasListener:XPCOMStringToNSString(aEvent)];
}

void EventDispatcher::DispatchToEmbedder(const nsAString& aEvent,
                                         const GeckoViewDataSource& aData,
                                         nsIGeckoViewEventCallback* aCallback,
                                         ErrorResult& aRv) {
  // Convert the data payload to CoreFoundation types
  CFTypeRefPtr<CFTypeRef> data;
  GeckoViewDataCFSink sink(data);
  aData.ToSink(sink, aRv);
  if (aRv.Failed()) {
    return;
  }

  // Wrap the callback if provided into a Swift callback.
  NativeCallbackDelegateSupport* callback = nil;
  if (aCallback) {
    callback = [[[NativeCallbackDelegateSupport alloc]
        initWithCallback:aCallback] autorelease];
  }

  // Call the swift dispatcher.
  dom::AutoNoJSAPI nojsapi;
  id<SwiftEventDispatcher> dispatcher = (id<SwiftEventDispatcher>)mDispatcher;
  [dispatcher dispatchToSwift:XPCOMStringToNSString(aEvent)
                      message:(id)data.get()
                     callback:callback];
}

void EventDispatcher::Attach(id aDispatcher) {
  AssertIsOnMainThread();
  MOZ_ASSERT(aDispatcher);

  id<SwiftEventDispatcher> prevDispatcher =
      (id<SwiftEventDispatcher>)mDispatcher;
  id<SwiftEventDispatcher> newDispatcher =
      (id<SwiftEventDispatcher>)aDispatcher;

  if (prevDispatcher && prevDispatcher == newDispatcher) {
    // Nothing to do if the new dispatcher is the same.
    return;
  }

  [prevDispatcher attach:nil];
  [prevDispatcher release];

  mDispatcher = [newDispatcher retain];

  EventDispatcherImpl* proxy =
      [[EventDispatcherImpl alloc] initWithDispatcher:this];
  [newDispatcher attach:[proxy autorelease]];
}

void EventDispatcher::Shutdown() {
  AssertIsOnMainThread();

  if (mDispatcher) {
    [mDispatcher release];
  }
  mDispatcher = nullptr;

  EventDispatcherBase::Shutdown();
}

void EventDispatcher::Detach() {
  AssertIsOnMainThread();
  MOZ_ASSERT(mDispatcher);

  // SetAttachedToGecko will call disposeNative for us later on the Gecko
  // thread to make sure all pending dispatchToGecko calls have completed.
  if (mDispatcher) {
    [(id<SwiftEventDispatcher>)mDispatcher attach:nil];
  }

  Shutdown();
}

EventDispatcher::~EventDispatcher() {
  if (mDispatcher) {
    [mDispatcher release];
  }
  mDispatcher = nullptr;
}

}  // namespace mozilla::widget
