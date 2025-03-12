/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/widget/GeckoViewDataJava.h"

#include "mozilla/widget/GeckoViewDataJS.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/java/GeckoBundleWrappers.h"
#include "JavaBuiltins.h"
#include "mozilla/jni/Conversions.h"

namespace mozilla::widget {

void GeckoViewDataJSToJava(JSContext* aCx, JS::Handle<JS::Value> aValue,
                           jni::Object::LocalRef& aResult, ErrorResult& aRv) {
  aRv.MightThrowJSException();
  GeckoViewDataJavaSink sink(aResult);
  GeckoViewDataJSSource source(aCx, aValue);
  source.ToSink(sink, aRv);
}

void GeckoViewDataJavaToJS(JSContext* aCx, jni::Object::Param aValue,
                           JS::MutableHandle<JS::Value> aResult,
                           ErrorResult& aRv) {
  aRv.MightThrowJSException();
  GeckoViewDataJSSink sink(aCx, aResult);
  GeckoViewDataJavaSource source(jni::GetGeckoThreadEnv(), aValue);
  source.ToSink(sink, aRv);
}

//////////////////////////////
// Java Sink Implementation //
//////////////////////////////

void GeckoViewDataJavaSink::SetNull(ErrorResult&) { mValue = nullptr; }

void GeckoViewDataJavaSink::SetBool(bool aValue, ErrorResult&) {
  mValue = aValue ? java::sdk::Boolean::TRUE() : java::sdk::Boolean::FALSE();
}

void GeckoViewDataJavaSink::SetInt32(int32_t aValue, ErrorResult&) {
  mValue = java::sdk::Integer::ValueOf(aValue);
}

void GeckoViewDataJavaSink::SetNumber(double aValue, ErrorResult&) {
  mValue = java::sdk::Double::New(aValue);
}

void GeckoViewDataJavaSink::SetString(const nsAString& aValue,
                                      ErrorResult& aRv) {
  mValue = jni::StringParam(aValue, mValue.Env(), fallible);
  if (!mValue) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
  }
}

void GeckoViewDataJavaSink::SetBytes(Span<const uint8_t> aValue,
                                     ErrorResult& aRv) {
  mValue = jni::ByteArray::New(reinterpret_cast<const int8_t*>(aValue.data()),
                               aValue.Length());
  if (!mValue) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
  }
}

void GeckoViewDataJavaSink::HandleObject(size_t aSize,
                                         VisitPropertiesFunc aVisitProperties,
                                         ErrorResult& aRv) {
  auto keys = jni::ObjectArray::New<jni::String>(aSize);
  auto values = jni::ObjectArray::New<jni::Object>(aSize);

  size_t index = 0;
  auto addProperty = [&](const nsAString& aPropertyName) {
    jni::Object::LocalRef key(
        mValue.Env(), jni::StringParam(aPropertyName, mValue.Env(), fallible));
    if (!key) {
      aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
      return;
    }

    keys->SetElement(index, key);
    values->SetElement(index, mValue);
    index++;
  };
  aVisitProperties(addProperty);
  MOZ_ASSERT(index == aSize || aRv.Failed());

  mValue = java::GeckoBundle::New(keys, values);
  if (!mValue) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
  }
}

void GeckoViewDataJavaSink::HandleArray(size_t aSize,
                                        VisitElementsFunc aVisitElements,
                                        ErrorResult& aRv) {
  // Unlike other targets for the GeckoViewData consumer, Java code requires
  // that every element in the array be the same type, and needs to know the
  // type of that data up-front, as the array types are not stored as an
  // `Object[]`.
  //
  // Because of this, the specific type to allocate will be determined lazily
  // when known. `selectedType` tracks the current known type information about
  // the array.
  enum class Ty {
    None,
    // Primitive Types
    Boolean,
    Integer,
    Double,
    // Object types
    Null,
    String,
    GeckoBundle,
  };
  Ty selectedType = Ty::None;

  // Local variables to hold each type of array. At most one of these local
  // variables will be in use at a time.
  nsTArray<bool> boolArray;
  nsTArray<int32_t> intArray;
  nsTArray<double> doubleArray;
  jni::ObjectArray::LocalRef objectArray(mValue.Env());

  auto addElement = [&](size_t aIndex) {
    MOZ_ASSERT(selectedType != Ty::None || aIndex == 0,
               "First argument must narrow type");
    if (!mValue) {
      // Store nulls in either a GeckoBundle or String object array.
      // If neither type has been seen yet, just track that we've seen `Null`.
      if (selectedType == Ty::None) {
        selectedType = Ty::Null;
      }
      if (selectedType != Ty::Null && selectedType != Ty::String &&
          selectedType != Ty::GeckoBundle) {
        aRv.ThrowTypeError("Unexpected null in primitive array");
      }
      // Object array elements are `null` by default, skip SetElement.
    } else if (mValue.IsInstanceOf<jni::Boolean>()) {
      // Store booleans in boolArray
      if (selectedType == Ty::None) {
        selectedType = Ty::Boolean;
        boolArray.SetCapacity(aSize);
      }
      if (selectedType != Ty::Boolean) {
        aRv.ThrowTypeError("Unexpected boolean in non-boolean array");
        return;
      }
      MOZ_ASSERT(boolArray.Length() == aIndex);
      boolArray.AppendElement(jni::Java2Native<bool>(mValue, mValue.Env()));
    } else if (mValue.IsInstanceOf<jni::Integer>()) {
      // Store integers in intArray or doubleArray
      if (selectedType == Ty::None) {
        selectedType = Ty::Integer;
        intArray.SetCapacity(aSize);
      } else if (selectedType == Ty::Double) {
        // Implicitly promote an `int32_t` to double.
        MOZ_ASSERT(doubleArray.Length() == aIndex);
        doubleArray.AppendElement(
            static_cast<double>(jni::Java2Native<int>(mValue, mValue.Env())));
        return;
      }
      if (selectedType != Ty::Integer) {
        aRv.ThrowTypeError("Unexpected number in non-numeric array");
        return;
      }
      MOZ_ASSERT(intArray.Length() == aIndex);
      intArray.AppendElement(jni::Java2Native<int>(mValue, mValue.Env()));
    } else if (mValue.IsInstanceOf<jni::Double>()) {
      // Store doubles in doubleArray, promoting integers if this is the first
      // double.
      if (selectedType == Ty::None) {
        selectedType = Ty::Double;
        doubleArray.SetCapacity(aSize);
      } else if (selectedType == Ty::Integer) {
        // Promote the int32_t array to a double array.
        selectedType = Ty::Double;
        doubleArray.SetCapacity(aSize);
        doubleArray.AppendElements(intArray);
        intArray.Clear();
      }
      if (selectedType != Ty::Double) {
        aRv.ThrowTypeError("Unexpected number in non-numeric array");
        return;
      }
      MOZ_ASSERT(doubleArray.Length() == aIndex);
      doubleArray.AppendElement(jni::Java2Native<double>(mValue, mValue.Env()));
    } else if (mValue.IsInstanceOf<jni::String>()) {
      // Store strings in an String object array.
      if (selectedType == Ty::None || selectedType == Ty::Null) {
        selectedType = Ty::String;
        objectArray = jni::ObjectArray::New<jni::String>(aSize);
      }
      if (selectedType != Ty::String) {
        aRv.ThrowTypeError("Unexpected string in non-string array");
        return;
      }
      objectArray->SetElement(aIndex, mValue);
    } else if (mValue.IsInstanceOf<java::GeckoBundle>()) {
      // Store GeckoBundles in a GeckoBundle object array.
      if (selectedType == Ty::None || selectedType == Ty::Null) {
        selectedType = Ty::GeckoBundle;
        objectArray = jni::ObjectArray::New<java::GeckoBundle>(aSize);
      }
      if (selectedType != Ty::GeckoBundle) {
        aRv.ThrowTypeError("Unexpected GeckoBundle in non-GeckoBundle array");
        return;
      }
      objectArray->SetElement(aIndex, mValue);
    } else {
      aRv.ThrowTypeError("Unexpected value in array");
    }
  };
  aVisitElements(addElement);
  if (aRv.Failed()) {
    return;
  }

  // Wrap the selected array into a Java object now that it has been populated.
  switch (selectedType) {
    case Ty::None:
      // Always represent empty arrays as an empty boolean array.
      MOZ_ASSERT(aSize == 0);
      mValue = java::GeckoBundle::EMPTY_BOOLEAN_ARRAY();
      break;
    case Ty::Boolean:
      mValue = jni::BooleanArray::New(boolArray.Elements(), boolArray.Length());
      break;
    case Ty::Integer:
      mValue = jni::IntArray::New(intArray.Elements(), intArray.Length());
      break;
    case Ty::Double:
      mValue =
          jni::DoubleArray::New(doubleArray.Elements(), doubleArray.Length());
      break;
    case Ty::Null:
      // If the array only contained `null`, fall back to a string array.
      mValue = jni::ObjectArray::New<jni::String>(aSize);
      break;
    case Ty::String:
    case Ty::GeckoBundle:
      mValue = objectArray;
      break;
  }
  if (!mValue) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
  }
}

////////////////////////////////
// Java Source Implementation //
////////////////////////////////

static void ObjectSource(JNIEnv* aEnv, jni::Object::Param aValue,
                         GeckoViewDataSink& aSink, ErrorResult& aRv);

template <class Callback>
static void StringSource(JNIEnv* aEnv, jni::String::Param aParam,
                         Callback&& aCallback, ErrorResult& aRv) {
  // Helper function which borrows the string into a nsDependentSubstring,
  // instead of copying it into a nsString.
  size_t len = aEnv->GetStringLength(aParam.Get());
  const jchar* jchars = aEnv->GetStringChars(aParam.Get(), nullptr);
  if (NS_WARN_IF(!jchars)) {
    aEnv->ExceptionClear();
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }
  auto releaseStr = MakeScopeExit([&] {
    aEnv->ReleaseStringChars(aParam.Get(), jchars);
    aEnv->ExceptionClear();
  });

  aCallback(
      nsDependentSubstring(reinterpret_cast<const char16_t*>(jchars), len));
}

static void GeckoBundleSource(JNIEnv* aEnv, java::GeckoBundle::Param aValue,
                              GeckoViewDataSink& aSink, ErrorResult& aRv) {
  jni::ObjectArray::LocalRef keys = aValue->Keys();
  jni::ObjectArray::LocalRef values = aValue->Values();
  size_t length = keys->Length();
  if (length != values->Length()) {
    aRv.ThrowUnknownError("GeckoBundle has mismatched keys and values");
    return;
  }

  auto visitProperties = [&](GeckoViewDataSink::AddPropertyFunc aAddProperty) {
    for (size_t i = 0; i < length; ++i) {
      // Set the "active value" to the value for the key.
      ObjectSource(aEnv, values->GetElement(i), aSink, aRv);
      if (aRv.Failed()) {
        return;
      }

      // Provide the key to the consumer.
      jni::Object::LocalRef key = keys->GetElement(i);
      MOZ_ASSERT(key.IsInstanceOf<jni::String>());
      StringSource(aEnv, jni::String::Ref::From(key), aAddProperty, aRv);
      if (aRv.Failed()) {
        return;
      }
    }
  };
  aSink.HandleObject(length, visitProperties, aRv);
}

static void BytesSource(JNIEnv* aEnv, jni::ByteArray::Param aValue,
                        GeckoViewDataSink& aSink, ErrorResult& aRv) {
  jbyte* data = aEnv->GetByteArrayElements(aValue.Get(), nullptr);
  if (NS_WARN_IF(!data)) {
    aEnv->ExceptionClear();
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }
  auto releaseArray = MakeScopeExit([&] {
    aEnv->ReleaseByteArrayElements(aValue.Get(), data, JNI_ABORT);
    aEnv->ExceptionClear();
  });

  size_t length = aEnv->GetArrayLength(aValue.Get());
  aSink.SetBytes(AsBytes(Span{data, length}), aRv);
}

template <typename Type, typename JNIType, typename ArrayType,
          JNIType* (JNIEnv::*GetElements)(ArrayType, jboolean*),
          void (JNIEnv::*ReleaseElements)(ArrayType, JNIType*, jint),
          void (GeckoViewDataSink::*SetValue)(Type, ErrorResult&)>
static void ArrayPrimitiveSource(JNIEnv* aEnv, jni::Object::Param aValue,
                                 GeckoViewDataSink& aSink, ErrorResult& aRv) {
  ArrayType jarray = ArrayType(aValue.Get());
  JNIType* array = (aEnv->*GetElements)(jarray, nullptr);
  if (NS_WARN_IF(!array)) {
    aEnv->ExceptionClear();
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }
  auto releaseArray = MakeScopeExit([&] {
    (aEnv->*ReleaseElements)(jarray, array, JNI_ABORT);
    aEnv->ExceptionClear();
  });

  size_t length = aEnv->GetArrayLength(jarray);
  auto visitElements = [&](GeckoViewDataSink::AddElementFunc aAddElement) {
    for (size_t i = 0; i < length; ++i) {
      (aSink.*SetValue)(array[i], aRv);
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

static void ArrayObjectSource(JNIEnv* aEnv, jni::ObjectArray::Param aValue,
                              GeckoViewDataSink& aSink, ErrorResult& aRv) {
  size_t length = aValue->Length();
  auto visitElements = [&](GeckoViewDataSink::AddElementFunc aAddElement) {
    for (size_t i = 0; i < length; ++i) {
      ObjectSource(aEnv, aValue->GetElement(i), aSink, aRv);
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

static void ObjectSource(JNIEnv* aEnv, jni::Object::Param aValue,
                         GeckoViewDataSink& aSink, ErrorResult& aRv) {
  if (!aValue) {
    aSink.SetNull(aRv);
  } else if (aValue.IsInstanceOf<jni::Boolean>()) {
    aSink.SetBool(jni::Java2Native<bool>(aValue, aEnv), aRv);
  } else if (aValue.IsInstanceOf<jni::Integer>()) {
    aSink.SetInt32(jni::Java2Native<int>(aValue, aEnv), aRv);
  } else if (aValue.IsInstanceOf<jni::Byte>() ||
             aValue.IsInstanceOf<jni::Short>()) {
    aSink.SetInt32(java::sdk::Number::Ref::From(aValue)->IntValue(), aRv);
  } else if (aValue.IsInstanceOf<jni::Double>()) {
    aSink.SetNumber(jni::Java2Native<double>(aValue, aEnv), aRv);
  } else if (aValue.IsInstanceOf<jni::Float>() ||
             aValue.IsInstanceOf<jni::Long>()) {
    aSink.SetNumber(java::sdk::Number::Ref::From(aValue)->DoubleValue(), aRv);
  } else if (aValue.IsInstanceOf<jni::String>()) {
    StringSource(
        aEnv, jni::String::Ref::From(aValue),
        [&](const nsAString& s) { return aSink.SetString(s, aRv); }, aRv);
  } else if (aValue.IsInstanceOf<jni::Character>()) {
    StringSource(
        aEnv, jni::String::Ref::From(java::sdk::String::ValueOf(aValue)),
        [&](const nsAString& s) { return aSink.SetString(s, aRv); }, aRv);
  } else if (aValue.IsInstanceOf<java::GeckoBundle>()) {
    GeckoBundleSource(aEnv, java::GeckoBundle::Ref::From(aValue), aSink, aRv);
  } else if (aValue.IsInstanceOf<jni::ByteArray>()) {
    BytesSource(aEnv, jni::ByteArray::Ref::From(aValue), aSink, aRv);
  } else if (aValue.IsInstanceOf<jni::BooleanArray>()) {
    ArrayPrimitiveSource<
        bool, jboolean, jbooleanArray, &JNIEnv::GetBooleanArrayElements,
        &JNIEnv::ReleaseBooleanArrayElements, &GeckoViewDataSink::SetBool>(
        aEnv, aValue, aSink, aRv);
  } else if (aValue.IsInstanceOf<jni::IntArray>()) {
    ArrayPrimitiveSource<int32_t, jint, jintArray, &JNIEnv::GetIntArrayElements,
                         &JNIEnv::ReleaseIntArrayElements,
                         &GeckoViewDataSink::SetInt32>(aEnv, aValue, aSink,
                                                       aRv);
  } else if (aValue.IsInstanceOf<jni::DoubleArray>()) {
    ArrayPrimitiveSource<
        double, jdouble, jdoubleArray, &JNIEnv::GetDoubleArrayElements,
        &JNIEnv::ReleaseDoubleArrayElements, &GeckoViewDataSink::SetNumber>(
        aEnv, aValue, aSink, aRv);
  } else if (aValue.IsInstanceOf<jni::ObjectArray>()) {
    ArrayObjectSource(aEnv, jni::ObjectArray::Ref::From(aValue), aSink, aRv);
  } else {
    aRv.ThrowTypeError("Type not supported by GeckoViewData");
  }
}

void GeckoViewDataJavaSource::ToSink(GeckoViewDataSink& aSink,
                                     ErrorResult& aRv) const {
  ObjectSource(mEnv, mValue, aSink, aRv);
}

}  // namespace mozilla::widget
