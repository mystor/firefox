/* -*- Mode: c++; c-basic-offset: 2; tab-width: 20; indent-tabs-mode: nil; -*-
 * vim: set sw=2 ts=4 expandtab:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "EventDispatcher.h"

#include "JavaBuiltins.h"
#include "nsAppShell.h"
#include "nsJSUtils.h"
#include "js/Array.h"  // JS::GetArrayLength, JS::IsArrayObject, JS::NewArrayObject
#include "js/PropertyAndElement.h"  // JS_Enumerate, JS_GetElement, JS_GetProperty, JS_GetPropertyById, JS_SetElement, JS_SetUCProperty
#include "js/String.h"              // JS::StringHasLatin1Chars
#include "js/Warnings.h"            // JS::WarnUTF8
#include "xpcpublic.h"

#include "mozilla/fallible.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/java/EventCallbackWrappers.h"
#include "mozilla/widget/GeckoViewDataJava.h"

namespace mozilla::widget {

namespace detail {

class JavaCallbackDelegate final
    : public EventDispatcherBase::CallbackDelegateBase {
 public:
  explicit JavaCallbackDelegate(java::EventCallback::Param aCallback)
      : mCallback(jni::GetGeckoThreadEnv(), aCallback) {}

  void OnSuccess(const GeckoViewDataSource& aData, ErrorResult& aRv) override {
    jni::Object::LocalRef data;
    GeckoViewDataJavaSink sink(data);
    aData.ToSink(sink, aRv);
    if (!aRv.Failed()) {
      mCallback->SendSuccess(data);
    }
  }

  void OnError(const GeckoViewDataSource& aData, ErrorResult& aRv) override {
    jni::Object::LocalRef data;
    GeckoViewDataJavaSink sink(data);
    aData.ToSink(sink, aRv);
    if (!aRv.Failed()) {
      mCallback->SendError(data);
    }
  }

 private:
  virtual ~JavaCallbackDelegate() = default;

  const java::EventCallback::GlobalRef mCallback;
};

class NativeCallbackDelegateSupport final
    : public java::EventDispatcher::NativeCallbackDelegate::Natives<
          NativeCallbackDelegateSupport> {
  using CallbackDelegate = java::EventDispatcher::NativeCallbackDelegate;
  using Base = CallbackDelegate::Natives<NativeCallbackDelegateSupport>;

 public:
  using Base::AttachNative;

  template <typename Functor>
  static void OnNativeCall(Functor&& aCall) {
    if (NS_IsMainThread()) {
      // Invoke callbacks synchronously if we're already on Gecko thread.
      return aCall();
    }
    NS_DispatchToMainThread(
        NS_NewRunnableFunction("OnNativeCall", std::forward<Functor>(aCall)));
  }

  static void Finalize(const CallbackDelegate::LocalRef& aInstance) {
    DisposeNative(aInstance);
  }

  explicit NativeCallbackDelegateSupport(nsIGeckoViewEventCallback* callback)
      : mCallback(callback) {}

  void SendSuccess(jni::Object::Param aData) {
    mCallback->DoOnSuccess(
        GeckoViewDataJavaSource(jni::GetGeckoThreadEnv(), aData));
  }

  void SendError(jni::Object::Param aData) {
    mCallback->DoOnError(
        GeckoViewDataJavaSource(jni::GetGeckoThreadEnv(), aData));
  }

 private:
  const nsCOMPtr<nsIGeckoViewEventCallback> mCallback;
};

}  // namespace detail

using namespace detail;

void EventDispatcher::DispatchToGecko(jni::String::Param aEvent,
                                      jni::Object::Param aData,
                                      jni::Object::Param aCallback) {
  AssertIsOnMainThread();

  nsCOMPtr<nsIGeckoViewEventCallback> callback;
  if (aCallback) {
    callback =
        new JavaCallbackDelegate(java::EventCallback::Ref::From(aCallback));
  }

  DispatchToGecko(aEvent->ToString(),
                  GeckoViewDataJavaSource(jni::GetGeckoThreadEnv(), aData),
                  callback);
}

bool EventDispatcher::HasEmbedderListener(const nsAString& aEvent) {
  java::EventDispatcher::LocalRef dispatcher(mDispatcher);
  if (!dispatcher) {
    return false;
  }

  return dispatcher->HasListener(aEvent);
}

void EventDispatcher::DispatchToEmbedder(const nsAString& aEvent,
                                         const GeckoViewDataSource& aData,
                                         nsIGeckoViewEventCallback* aCallback,
                                         ErrorResult& aRv) {
  JNIEnv* env = jni::GetGeckoThreadEnv();

  java::EventDispatcher::LocalRef dispatcher(env, mDispatcher);
  if (!dispatcher) {
    return;
  }

  jni::Object::LocalRef data(env);
  GeckoViewDataJavaSink sink(data);
  aData.ToSink(sink, aRv);
  if (aRv.Failed()) {
    return;
  }

  java::EventDispatcher::NativeCallbackDelegate::LocalRef callback(env);
  if (aCallback) {
    callback = java::EventDispatcher::NativeCallbackDelegate::New();
    NativeCallbackDelegateSupport::AttachNative(
        callback, MakeUnique<NativeCallbackDelegateSupport>(aCallback));
  }

  dom::AutoNoJSAPI nojsapi;
  dispatcher->DispatchToThreads(aEvent, data, callback);
}

void EventDispatcher::Attach(java::EventDispatcher::Param aDispatcher) {
  AssertIsOnMainThread();
  MOZ_ASSERT(aDispatcher);

  java::EventDispatcher::LocalRef dispatcher(mDispatcher);

  if (dispatcher) {
    if (dispatcher == aDispatcher) {
      return;
    }
    dispatcher->SetAttachedToGecko(java::EventDispatcher::REATTACHING);
  }

  dispatcher = java::EventDispatcher::LocalRef(aDispatcher);
  NativesBase::AttachNative(dispatcher, this);
  mDispatcher = dispatcher;

  dispatcher->SetAttachedToGecko(java::EventDispatcher::ATTACHED);
}

void EventDispatcher::Shutdown() {
  AssertIsOnMainThread();
  mDispatcher = nullptr;
  EventDispatcherBase::Shutdown();
}

void EventDispatcher::Detach() {
  AssertIsOnMainThread();
  MOZ_ASSERT(mDispatcher);

  java::EventDispatcher::GlobalRef dispatcher(mDispatcher);

  // SetAttachedToGecko will call disposeNative for us later on the Gecko
  // thread to make sure all pending dispatchToGecko calls have completed.
  if (dispatcher) {
    dispatcher->SetAttachedToGecko(java::EventDispatcher::DETACHED);
  }

  Shutdown();
}

}  // namespace mozilla::widget
