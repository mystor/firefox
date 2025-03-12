/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_GeckoViewDataJS_h
#define mozilla_widget_GeckoViewDataJS_h

#include "mozilla/widget/GeckoViewData.h"
#include "jsapi.h"

namespace mozilla::widget {

// Sink for translating from another GeckoViewData format into a JS::Value.
class MOZ_STACK_CLASS GeckoViewDataJSSink final : public GeckoViewDataSink {
 public:
  GeckoViewDataJSSink(JSContext* aCx, JS::MutableHandle<JS::Value> aValue)
      : mCx(aCx), mValue(aValue) {}

  void SetNull(ErrorResult& aRv) override;
  void SetBool(bool aValue, ErrorResult& aRv) override;
  void SetInt32(int32_t aValue, ErrorResult& aRv) override;
  void SetNumber(double aValue, ErrorResult& aRv) override;
  void SetString(const nsAString& aValue, ErrorResult& aRv) override;
  void SetBytes(Span<const uint8_t> aValue, ErrorResult& aRv) override;
  void HandleObject(size_t aSize, VisitPropertiesFunc aVisitProperties,
                    ErrorResult& aRv) override;
  void HandleArray(size_t aSize, VisitElementsFunc aVisitElements,
                   ErrorResult& aRv) override;

 private:
  JSContext* mCx;
  JS::MutableHandle<JS::Value> mValue;
};

// Source for translating from a JS::Value into another GeckoViewData format.
class MOZ_STACK_CLASS GeckoViewDataJSSource final : public GeckoViewDataSource {
 public:
  GeckoViewDataJSSource(JSContext* aCx, JS::Handle<JS::Value> aValue)
      : mCx(aCx), mValue(aValue) {}

  void ToSink(GeckoViewDataSink& aSink, ErrorResult& aRv) const override;

 private:
  JSContext* mCx;
  JS::Handle<JS::Value> mValue;
};

}  // namespace mozilla::widget

#endif  // mozilla_widget_GeckoViewData_h
