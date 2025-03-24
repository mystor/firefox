/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/widget/GeckoViewDataCF.h"

#include "mozilla/widget/GeckoViewDataJS.h"
#include <CoreFoundation/CoreFoundation.h>

namespace mozilla::widget {

void GeckoViewDataJSToCF(JSContext* aCx, JS::Handle<JS::Value> aValue,
                         CFTypeRefPtr<CFTypeRef>& aResult, ErrorResult& aRv) {
  aRv.MightThrowJSException();
  GeckoViewDataCFSink sink(aResult);
  GeckoViewDataJSSource source(aCx, aValue);
  source.ToSink(sink, aRv);
}

void GeckoViewDataCFToJS(JSContext* aCx, CFTypeRef aValue,
                         JS::MutableHandle<JS::Value> aResult,
                         ErrorResult& aRv) {
  aRv.MightThrowJSException();
  GeckoViewDataJSSink sink(aCx, aResult);
  GeckoViewDataCFSource source(aValue);
  source.ToSink(sink, aRv);
}

////////////////////////////////////////
// CoreFoundation Sink Implementation //
////////////////////////////////////////

void GeckoViewDataCFSink::SetNull(ErrorResult&) { mValue = nil; }

void GeckoViewDataCFSink::SetBool(bool aValue, ErrorResult&) {
  mValue.AssignUnderGetRule(aValue ? kCFBooleanTrue : kCFBooleanFalse);
}

void GeckoViewDataCFSink::SetInt32(int32_t aValue, ErrorResult& aRv) {
  mValue.AssignUnderCreateRule(
      CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &aValue));
  if (!mValue) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
  }
}

void GeckoViewDataCFSink::SetNumber(double aValue, ErrorResult& aRv) {
  mValue.AssignUnderCreateRule(
      CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &aValue));
  if (!mValue) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
  }
}

void GeckoViewDataCFSink::SetString(const nsAString& aValue, ErrorResult& aRv) {
  static_assert(sizeof(UniChar) == sizeof(char16_t),
                "Expected UniChar to be UTF-16");
  mValue.AssignUnderCreateRule(CFStringCreateWithCharacters(
      kCFAllocatorDefault, (const UniChar*)aValue.BeginReading(),
      (CFIndex)aValue.Length()));
  if (!mValue) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
  }
}

void GeckoViewDataCFSink::SetBytes(Span<const uint8_t> aValue,
                                   ErrorResult& aRv) {
  mValue.AssignUnderCreateRule(
      CFDataCreate(kCFAllocatorDefault, aValue.data(), (CFIndex)aValue.size()));
  if (!mValue) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
  }
}

void GeckoViewDataCFSink::HandleObject(size_t aSize,
                                       VisitPropertiesFunc aVisitProperties,
                                       ErrorResult& aRv) {
  auto dict = CFTypeRefPtr<CFMutableDictionaryRef>::WrapUnderCreateRule(
      CFDictionaryCreateMutable(kCFAllocatorDefault, (CFIndex)aSize,
                                &kCFCopyStringDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks));
  if (!dict) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }

  auto addProperty = [&](const nsAString& aName) {
    auto key = CFTypeRefPtr<CFStringRef>::WrapUnderCreateRule(
        CFStringCreateWithCharacters(kCFAllocatorDefault,
                                     (const UniChar*)aName.BeginReading(),
                                     (CFIndex)aName.Length()));
    if (!key) {
      aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
      return;
    }
    CFDictionaryAddValue(dict.get(), key.get(),
                         mValue ? mValue.get() : kCFNull);
  };
  aVisitProperties(addProperty);

  // NOTE: A CFRetain() and CFRelease() could be avoided here if we could steal
  // the pointer from `dict`.
  mValue.AssignUnderGetRule(dict.get());
}

void GeckoViewDataCFSink::HandleArray(size_t aSize,
                                      VisitElementsFunc aVisitElements,
                                      ErrorResult& aRv) {
  auto array =
      CFTypeRefPtr<CFMutableArrayRef>::WrapUnderCreateRule(CFArrayCreateMutable(
          kCFAllocatorDefault, (CFIndex)aSize, &kCFTypeArrayCallBacks));
  if (!array) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }

  auto addElement = [&](size_t aIndex) {
    CFArrayAppendValue(array.get(), mValue ? mValue.get() : kCFNull);
  };
  aVisitElements(addElement);

  // NOTE: A CFRetain() and CFRelease() could be avoided here if we could steal
  // the pointer from `array`.
  mValue.AssignUnderGetRule(array.get());
}

//////////////////////////////////////////
// CoreFoundation Source Implementation //
//////////////////////////////////////////

static void CFDataSource(CFTypeRef aValue, GeckoViewDataSink& aSink,
                         ErrorResult& aRv);

template <typename F>
static void CFStringSource(CFStringRef aValue, F aCallback) {
  CFIndex length = CFStringGetLength(aValue);

  // First attempt to use CFStringGetCharactersPtr to avoid copying, and fall
  // back to copying directly into a mutable buffer.
  nsAutoString string;
  if (const UniChar* chars = CFStringGetCharactersPtr(aValue)) {
    string.Rebind((const char16_t*)chars, length);
  } else {
    char16_t* data = nullptr;
    string.GetMutableData(&data, length);
    CFStringGetCharacters(aValue, CFRangeMake(0, length), (UniChar*)data);
  }

  aCallback(std::move(string));
}

static void CFDictionarySource(CFDictionaryRef aValue, GeckoViewDataSink& aSink,
                               ErrorResult& aRv) {
  size_t count = CFDictionaryGetCount(aValue);

  AutoTArray<CFTypeRef, 10> keys;
  AutoTArray<CFTypeRef, 10> values;
  keys.SetLength(count);
  values.SetLength(count);
  CFDictionaryGetKeysAndValues(aValue, keys.Elements(), values.Elements());

  auto visitProperties = [&](GeckoViewDataSink::AddPropertyFunc aAddProperty) {
    for (size_t i = 0; i < count; ++i) {
      CFDataSource(values[i], aSink, aRv);
      if (aRv.Failed()) {
        return;
      }

      if (CFGetTypeID(keys[i]) != CFStringGetTypeID()) {
        aRv.ThrowTypeError("Expected string dictionary key");
        return;
      }
      CFStringSource((CFStringRef)keys[i], aAddProperty);
      if (aRv.Failed()) {
        return;
      }
    }
  };
  aSink.HandleObject(count, visitProperties, aRv);
}

static void CFArraySource(CFArrayRef aValue, GeckoViewDataSink& aSink,
                          ErrorResult& aRv) {
  size_t count = CFArrayGetCount(aValue);

  auto visitElements = [&](GeckoViewDataSink::AddElementFunc aAddElement) {
    for (size_t i = 0; i < count; ++i) {
      CFDataSource(CFArrayGetValueAtIndex(aValue, (CFIndex)i), aSink, aRv);
      if (aRv.Failed()) {
        return;
      }

      aAddElement(i);
      if (aRv.Failed()) {
        return;
      }
    }
  };
  aSink.HandleArray(count, visitElements, aRv);
}

static void CFDataSource(CFTypeRef aValue, GeckoViewDataSink& aSink,
                         ErrorResult& aRv) {
  CFTypeID typeID = aValue ? CFGetTypeID(aValue) : CFNullGetTypeID();

  if (typeID == CFNullGetTypeID()) {
    aSink.SetNull(aRv);
  } else if (typeID == CFBooleanGetTypeID()) {
    aSink.SetBool(CFBooleanGetValue((CFBooleanRef)aValue), aRv);
  } else if (typeID == CFNumberGetTypeID()) {
    double numberValue = 0;
    CFNumberGetValue((CFNumberRef)aValue, kCFNumberDoubleType, &numberValue);
    aSink.SetNumber(numberValue, aRv);
  } else if (typeID == CFStringGetTypeID()) {
    CFStringSource((CFStringRef)aValue, [&](const nsAString& aString) {
      aSink.SetString(aString, aRv);
    });
  } else if (typeID == CFDataGetTypeID()) {
    size_t length = CFDataGetLength((CFDataRef)aValue);
    const uint8_t* data = CFDataGetBytePtr((CFDataRef)aValue);
    aSink.SetBytes(Span(data, length), aRv);
  } else if (typeID == CFDictionaryGetTypeID()) {
    CFDictionarySource((CFDictionaryRef)aValue, aSink, aRv);
  } else if (typeID == CFArrayGetTypeID()) {
    CFArraySource((CFArrayRef)aValue, aSink, aRv);
  } else {
    aRv.ThrowTypeError("Type not supported by GeckoViewData");
  }
}

void GeckoViewDataCFSource::ToSink(GeckoViewDataSink& aSink,
                                   ErrorResult& aRv) const {
  CFDataSource(mValue, aSink, aRv);
}

}  // namespace mozilla::widget
