/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ReadableStream controller classes and functions. */

#ifndef builtin_streams_ReadableStreamBYOBRequest_h
#define builtin_streams_ReadableStreamBYOBRequest_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Maybe.h"       // mozilla::Maybe, mozilla::Some

#include <stdint.h>  // uint32_t

#include "js/Class.h"       // JSClass, js::ClassSpec
#include "js/RootingAPI.h"  // JS::Handle
#include "js/Value.h"  // JS::Value, JS::{Number,Object,Private,Undefined}Value, JS::UndefinedHandleValue
#include "vm/List.h"   // js::ListObject
#include "vm/NativeObject.h"  // js::NativeObject

class JS_PUBLIC_API JSObject;

namespace js {

class PromiseObject;
class ReadableByteStreamController;

class ReadableStreamBYOBRequest : public NativeObject {
 public:
  enum Slots {
    /**
     * Optional pointer to make the stream participate in Gecko's cycle
     * collection. See also JSCLASS_SLOT0_IS_NSISUPPORTS.
     */
    Slot_ISupports,
    Slot_Controller,
    Slot_View,
    SlotCount
  };

  inline ReadableByteStreamController* controller() const;
  inline void setController(ReadableByteStreamController* controller);
  inline bool hasController();
  void clearController() {
    setFixedSlot(Slot_Controller, JS::UndefinedValue());
  }

  JS::Value view() const { return getFixedSlot(Slot_View); }
  void setView(const JS::Value& view) { setFixedSlot(Slot_View, view); }
  void clearView() { setFixedSlot(Slot_View, JS::NullValue()); }

  static bool constructor(JSContext* cx, unsigned argc, JS::Value* vp);
  static const ClassSpec classSpec_;
  static const JSClass class_;
  static const ClassSpec protoClassSpec_;
  static const JSClass protoClass_;
};

}  // namespace js

#endif  // builtin_streams_ReadableStreamsBYOBRequest_h
