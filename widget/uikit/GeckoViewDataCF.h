/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_GeckoViewDataCF_h
#define mozilla_widget_GeckoViewDataCF_h

#include <CoreFoundation/CFBase.h>
#include <objc/objc.h>

#include "jsapi.h"
#include "mozilla/widget/GeckoViewData.h"
#include "CFTypeRefPtr.h"

namespace mozilla::widget {

//
// Conversion Methods
//

// Convert some GeckoView data from JS to CoreFoundation and vice-versa.
void GeckoViewDataJSToCF(JSContext* aCx, JS::Handle<JS::Value> aValue,
                         CFTypeRefPtr<CFTypeRef>& aResult, ErrorResult& aRv);
void GeckoViewDataCFToJS(JSContext* aCx, CFTypeRef aValue,
                         JS::MutableHandle<JS::Value> aResult,
                         ErrorResult& aRv);

//
// Implementation
//

// Sink for translating from another GeckoViewData format into a CFTypeRef.
class MOZ_STACK_CLASS GeckoViewDataCFSink final : public GeckoViewDataSink {
 public:
  explicit GeckoViewDataCFSink(CFTypeRefPtr<CFTypeRef>& aValue)
      : mValue(aValue) {}

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
  CFTypeRefPtr<CFTypeRef>& mValue;
};

// Source for translating from a CFTypeRef into another GeckoViewData format.
class MOZ_STACK_CLASS GeckoViewDataCFSource final : public GeckoViewDataSource {
 public:
  explicit GeckoViewDataCFSource(CFTypeRef aValue) : mValue(aValue) {}

  void ToSink(GeckoViewDataSink& aSink, ErrorResult& aRv) const override;

 private:
  CFTypeRef mValue;
};

}  // namespace mozilla::widget

#endif  // mozilla_widget_GeckoViewDataCF_h
