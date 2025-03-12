/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_GeckoViewData_h
#define mozilla_widget_GeckoViewData_h

#include "mozilla/ErrorResult.h"
#include "mozilla/FunctionRef.h"
#include "mozilla/Span.h"
#include "nsString.h"
#include "jsapi.h"

namespace mozilla::widget {

// Methods on this object will be called by a GeckoViewDataSource to translate
// values between languages using a shared JSON-like data model.
//
// Methods on this object act on an implicit "active value", which is a
// translated value in the sink format. All methods either consume or update the
// "active value" to be a new value.
class GeckoViewDataSink {
 public:
  virtual ~GeckoViewDataSink() = default;

  // Set the "active value" to a primitive value
  virtual void SetNull(ErrorResult& aRv) = 0;
  virtual void SetBool(bool aValue, ErrorResult& aRv) = 0;
  virtual void SetInt32(int32_t aValue, ErrorResult& aRv) = 0;
  virtual void SetNumber(double aValue, ErrorResult& aRv) = 0;
  virtual void SetString(const nsAString& aValue, ErrorResult& aRv) = 0;
  virtual void SetBytes(Span<const uint8_t> aValue, ErrorResult& aRv) = 0;

  // Called to set the "active value" to an object.
  //
  // The relevant steps are as follows:
  //  1. The consumer sets up local state for building the object on the stack.
  //  2. The consumer calls `aVisitProperties`, passing down an
  //     `AddPropertyFunc` closure.
  //  3. For each property in the object, the provider first sets the "active
  //     value" to the value of the property, then calls the `AddPropertyFunc`,
  //     passing down the property name.
  //  4. Once all properties have been added, `aVisitProperties` will return.
  using AddPropertyFunc = FunctionRef<void(const nsAString&)>;
  using VisitPropertiesFunc = FunctionRef<void(AddPropertyFunc)>;
  virtual void HandleObject(size_t aSize, VisitPropertiesFunc aVisitProperties,
                            ErrorResult& aRv) = 0;

  // Called to set the "active value" to an array.
  //
  // Arrays are handled in a similar manner to objects (see `HandleObject`'s
  // documentation for details), but an index is passed down instead of the
  // property name.
  using AddElementFunc = FunctionRef<void(size_t)>;
  using VisitElementsFunc = FunctionRef<void(AddElementFunc)>;
  virtual void HandleArray(size_t aSize, VisitElementsFunc aVisitElements,
                           ErrorResult& aRv) = 0;
};

// Object acting as an abstracted source object for a GeckoViewData translation.
//
// See the GeckoViewDataSink type's documentation for details of the interface
// which needs to be implemented.
class GeckoViewDataSource {
 public:
  virtual ~GeckoViewDataSource() = default;

  // Drive a GeckoViewDataSink from this source, translating the underlying
  // object from the source language to the sink language.
  virtual void ToSink(GeckoViewDataSink& aSink, ErrorResult& aRv) const = 0;
};

// Basic GeckoViewDataSource for `null`.
class GeckoViewDataNullSource : public GeckoViewDataSource {
 public:
  void ToSink(GeckoViewDataSink& aSink, ErrorResult& aRv) const override {
    aSink.SetNull(aRv);
  }
};

}  // namespace mozilla::widget

#endif  // mozilla_widget_GeckoViewData_h
