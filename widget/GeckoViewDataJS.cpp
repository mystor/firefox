/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/TypedArray.h"
#include "mozilla/widget/GeckoViewDataJS.h"
#include "nsJSUtils.h"
#include "xpcpublic.h"

namespace mozilla::widget {

////////////////////////////
// JS Sink Implementation //
////////////////////////////

void GeckoViewDataJSSink::SetNull(ErrorResult&) { mValue.setNull(); }

void GeckoViewDataJSSink::SetBool(bool aValue, ErrorResult&) {
  mValue.setBoolean(aValue);
}

void GeckoViewDataJSSink::SetInt32(int32_t aValue, ErrorResult&) {
  mValue.setInt32(aValue);
}

void GeckoViewDataJSSink::SetNumber(double aValue, ErrorResult&) {
  mValue.setNumber(aValue);
}

void GeckoViewDataJSSink::SetString(const nsAString& aValue, ErrorResult& aRv) {
  if (!xpc::StringToJsval(mCx, aValue, mValue)) {
    aRv.NoteJSContextException(mCx);
  }
}

void GeckoViewDataJSSink::SetBytes(Span<const uint8_t> aValue,
                                   ErrorResult& aRv) {
  mValue.setObjectOrNull(dom::Uint8Array::Create(mCx, aValue, aRv));
}

void GeckoViewDataJSSink::HandleObject(size_t aSize,
                                       VisitPropertiesFunc aVisitProperties,
                                       ErrorResult& aRv) {
  JS::Rooted<JSObject*> obj(mCx, JS_NewPlainObject(mCx));
  if (!obj) {
    aRv.NoteJSContextException(mCx);
    return;
  }

  auto addProperty = [&](const nsAString& aPropertyName) {
    // Atomize the property name into a jsid
    JS::Rooted<JS::Value> idVal(mCx);
    if (!xpc::StringToJsval(mCx, aPropertyName, &idVal)) {
      aRv.NoteJSContextException(mCx);
      return;
    }
    JS::Rooted<jsid> id(mCx);
    if (!JS_ValueToId(mCx, idVal, &id)) {
      aRv.NoteJSContextException(mCx);
      return;
    }
    // Define the property on `obj`, using `mValue` as the value.
    if (!JS_DefinePropertyById(mCx, obj, id, mValue, JSPROP_ENUMERATE)) {
      aRv.NoteJSContextException(mCx);
      return;
    }
  };
  aVisitProperties(addProperty);

  mValue.setObject(*obj);
}

void GeckoViewDataJSSink::HandleArray(size_t aSize,
                                      VisitElementsFunc aVisitElements,
                                      ErrorResult& aRv) {
  JS::Rooted<JSObject*> arr(mCx, JS::NewArrayObject(mCx, aSize));
  if (!arr) {
    aRv.NoteJSContextException(mCx);
    return;
  }

  auto addElement = [&](size_t aIndex) {
    if (!JS_SetElement(mCx, arr, aIndex, mValue)) {
      aRv.NoteJSContextException(mCx);
    }
  };
  aVisitElements(addElement);

  mValue.setObject(*arr);
}

//////////////////////////////
// JS Source Implementation //
//////////////////////////////

static void JSValueSource(JSContext* aCx, JS::Handle<JS::Value> aValue,
                          GeckoViewDataSink& aSink, ErrorResult& aRv);

static void JSArraySource(JSContext* aCx, JS::Handle<JSObject*> aArray,
                          GeckoViewDataSink& aSink, ErrorResult& aRv) {
  uint32_t length = 0;
  if (!JS::GetArrayLength(aCx, aArray, &length)) {
    aRv.NoteJSContextException(aCx);
    return;
  }

  auto visitElements = [&](GeckoViewDataSink::AddElementFunc aAddElement) {
    JS::Rooted<JS::Value> elt(aCx);
    for (uint32_t i = 0; i < length; ++i) {
      if (!JS_GetElement(aCx, aArray, i, &elt)) {
        aRv.NoteJSContextException(aCx);
        return;
      }

      JSValueSource(aCx, elt, aSink, aRv);
      if (aRv.Failed()) {
        return;
      }

      aAddElement(i);
      if (aRv.Failed()) {
        return;
      }
    }
  };
  aSink.HandleArray(length, visitElements, aRv);
}

static void JSTypedArraySource(JSContext* aCx,
                               JS::Handle<JSObject*> aTypedArray,
                               GeckoViewDataSink& aSink, ErrorResult& aRv) {
  dom::RootedSpiderMonkeyInterface<dom::Uint8Array> typedArray(aCx);
  if (!typedArray.Init(aTypedArray)) {
    aRv.ThrowTypeError("Typed array object is not a Uint8Array");
    return;
  }
  if (JS::IsArrayBufferViewShared(typedArray.Obj())) {
    aRv.ThrowTypeError("Typed array object is shared");
    return;
  }
  if (JS::IsLargeArrayBufferView(typedArray.Obj())) {
    aRv.ThrowTypeError("Typed array object is too large");
    return;
  }
  if (JS::IsResizableArrayBufferView(typedArray.Obj())) {
    aRv.ThrowTypeError("Typed array object is resizable");
    return;
  }

  // NOTE: This could probably be a call to ProcessData for most consumers, but
  // the possibility to call a consumer like GeckoViewDataJSSink would flag
  // the hazard analysis. This is safer, and shouldn't be slower for the
  // expected use-case.
  typedArray.ProcessFixedData(
      [&](const Span<uint8_t>& aData) { aSink.SetBytes(aData, aRv); });
}

static void JSObjectSource(JSContext* aCx, JS::Handle<JSObject*> aObj,
                           GeckoViewDataSink& aSink, ErrorResult& aRv) {
  JS::Rooted<JS::IdVector> ids(aCx, JS::IdVector(aCx));
  if (!JS_Enumerate(aCx, aObj, &ids)) {
    aRv.NoteJSContextException(aCx);
    return;
  }

  size_t length = ids.length();
  auto visitProperties = [&](GeckoViewDataSink::AddPropertyFunc aAddProperty) {
    JS::Rooted<JS::Value> propVal(aCx);
    for (size_t i = 0; i < length; ++i) {
      // Set the "active value" in the consumer to the property value.
      if (!JS_GetPropertyById(aCx, aObj, ids[i], &propVal)) {
        aRv.NoteJSContextException(aCx);
        return;
      }
      JSValueSource(aCx, propVal, aSink, aRv);
      if (aRv.Failed()) {
        return;
      }

      // Convert the ID to a string key, and notify our consumer.
      nsAutoJSString key;
      if (!key.init(aCx, ids[i])) {
        aRv.NoteJSContextException(aCx);
        return;
      }
      aAddProperty(key);
      if (aRv.Failed()) {
        return;
      }
    }
  };
  aSink.HandleObject(length, visitProperties, aRv);
}

static void JSValueSource(JSContext* aCx, JS::Handle<JS::Value> aValue,
                          GeckoViewDataSink& aSink, ErrorResult& aRv) {
  if (aValue.isNullOrUndefined()) {
    aSink.SetNull(aRv);
  } else if (aValue.isBoolean()) {
    aSink.SetBool(aValue.toBoolean(), aRv);
  } else if (aValue.isInt32()) {
    aSink.SetInt32(aValue.toInt32(), aRv);
  } else if (aValue.isNumber()) {
    aSink.SetNumber(aValue.toNumber(), aRv);
  } else if (aValue.isString()) {
    nsAutoJSString str;
    if (!str.init(aCx, aValue)) {
      aRv.NoteJSContextException(aCx);
      return;
    }
    aSink.SetString(str, aRv);
  } else if (aValue.isObject()) {
    JS::Rooted<JSObject*> obj(aCx, &aValue.toObject());

    // Array objects
    bool isArray = false;
    if (!JS::IsArrayObject(aCx, obj, &isArray)) {
      aRv.NoteJSContextException(aCx);
      return;
    }
    if (isArray) {
      JSArraySource(aCx, obj, aSink, aRv);
      return;
    }

    // Typed array objects
    if (JS_IsTypedArrayObject(obj)) {
      JSTypedArraySource(aCx, obj, aSink, aRv);
      return;
    }

    // Plain JS objects
    JSObjectSource(aCx, obj, aSink, aRv);
  } else {
    aRv.ThrowTypeError("Type not supported by GeckoViewData");
  }
}

void GeckoViewDataJSSource::ToSink(GeckoViewDataSink& aSink,
                                   ErrorResult& aRv) const {
  JSValueSource(mCx, mValue, aSink, aRv);
}

}  // namespace mozilla::widget
