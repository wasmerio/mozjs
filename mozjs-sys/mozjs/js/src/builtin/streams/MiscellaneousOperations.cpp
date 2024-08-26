/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Miscellaneous operations. */

#include "builtin/streams/MiscellaneousOperations.h"

#include "mozilla/Assertions.h"     // MOZ_ASSERT

#include "js/CallAndConstruct.h"      // JS::IsCallable
#include "js/Conversions.h"           // JS::ToNumber
#include "js/ErrorReport.h"           // JS_ReportErrorNumberASCII
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/RootingAPI.h"            // JS::{,Mutable}Handle, JS::Rooted
#include "vm/Interpreter.h"           // js::{Call,GetAndClearException}
#include "vm/JSContext.h"             // JSContext
#include "vm/ObjectOperations.h"      // js::GetProperty
#include "vm/PromiseObject.h"         // js::PromiseObject
#include "vm/StringType.h"            // js::PropertyName

#include "vm/JSContext-inl.h"  // JSContext::check
#include "vm/JSObject-inl.h"   // js::IsCallable

using JS::Handle;
using JS::MutableHandle;
using JS::ToNumber;
using JS::Value;

// https://streams.spec.whatwg.org/#transfer-array-buffer
// As some parts of the specifcation want to use the abrupt completion value,
// this function may leave a pending exception if it returns nullptr.
[[nodiscard]] JSObject* js::TransferArrayBuffer(JSContext* cx,
                                                JS::Handle<JSObject*> buffer) {
  // Assert: ! IsDetachedBuffer(O) is false.
  MOZ_ASSERT(!JS::IsDetachedArrayBufferObject(buffer));

  // Let arrayBufferByteLength be O.[[ArrayBufferByteLength]].
  // (must get length before stealing data)
  size_t bufferLength = JS::GetArrayBufferByteLength(buffer);

  // Let arrayBufferData be O.[[ArrayBufferData]].
  UniquePtr<void, JS::FreePolicy> bufferData{
      JS::StealArrayBufferContents(cx, buffer)};

  // Perform ? DetachArrayBuffer(O).
  if (!JS::DetachArrayBuffer(cx, buffer)) {
    return nullptr;
  }

  // Return a new ArrayBuffer object, created in the current Realm, whose
  // [[ArrayBufferData]] internal slot value is arrayBufferData and whose
  // [[ArrayBufferByteLength]] internal slot value is arrayBufferByteLength.
  return JS::NewArrayBufferWithContents(cx, bufferLength, std::move(bufferData));
}

// https://streams.spec.whatwg.org/#can-transfer-array-buffer
[[nodiscard]] bool js::CanTransferArrayBuffer(JSContext* cx,
                                              JS::Handle<JSObject*> buffer) {
  // Step 1. Assert: Type(O) is Object. (Implicit in types)
  // Step 2. Assert: O has an [[ArrayBufferData]] internal slot.
  MOZ_ASSERT(JS::IsArrayBufferObject(buffer));

  // Step 3. If ! IsDetachedBuffer(O) is true, return false.
  if (JS::IsDetachedArrayBufferObject(buffer)) {
    return false;
  }

  // Step 4. If SameValue(O.[[ArrayBufferDetachKey]], undefined) is false,
  // return false.
  // Step 5. Return true.
  // Note: WASM memories are the only buffers that would qualify
  // as having an undefined [[ArrayBufferDetachKey]],
  bool hasDefinedArrayBufferDetachKey = false;
  if (!JS::HasDefinedArrayBufferDetachKey(cx, buffer,
                                          &hasDefinedArrayBufferDetachKey)) {
    return false;
  }
  return !hasDefinedArrayBufferDetachKey;
}

[[nodiscard]] js::PromiseObject* js::PromiseRejectedWithPendingError(
    JSContext* cx) {
  Rooted<Value> exn(cx);
  if (!cx->isExceptionPending() || !GetAndClearException(cx, &exn)) {
    // Uncatchable error. This happens when a slow script is killed or a
    // worker is terminated. Propagate the uncatchable error. This will
    // typically kill off the calling asynchronous process: the caller
    // can't hook its continuation to the new rejected promise.
    return nullptr;
  }
  return PromiseObject::unforgeableReject(cx, exn);
}

/*** 6.3. Miscellaneous operations ******************************************/

/**
 * Streams spec, 6.3.1.
 *      CreateAlgorithmFromUnderlyingMethod ( underlyingObject, methodName,
 *                                            algoArgCount, extraArgs )
 *
 * This function only partly implements the standard algorithm. We do not
 * actually create a new JSFunction completely encapsulating the new algorithm.
 * Instead, this just gets the specified method and checks for errors. It's the
 * caller's responsibility to make sure that later, when the algorithm is
 * "performed", the appropriate steps are carried out.
 */
[[nodiscard]] bool js::CreateAlgorithmFromUnderlyingMethod(
    JSContext* cx, Handle<Value> underlyingObject,
    const char* methodNameForErrorMessage, Handle<PropertyName*> methodName,
    MutableHandle<Value> method) {
  cx->check(underlyingObject);
  cx->check(methodName);
  cx->check(method);

  // Step 1: Assert: underlyingObject is not undefined.
  MOZ_ASSERT(!underlyingObject.isUndefined());

  // Step 2: Assert: ! IsPropertyKey(methodName) is true (implicit).
  // Step 3: Assert: algoArgCount is 0 or 1 (omitted).
  // Step 4: Assert: extraArgs is a List (omitted).

  // Step 5: Let method be ? GetV(underlyingObject, methodName).
  if (!GetProperty(cx, underlyingObject, methodName, method)) {
    return false;
  }

  // Step 6: If method is not undefined,
  if (!method.isUndefined()) {
    // Step a: If ! IsCallable(method) is false, throw a TypeError
    //         exception.
    if (!IsCallable(method)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_NOT_FUNCTION, methodNameForErrorMessage);
      return false;
    }

    // Step b: If algoArgCount is 0, return an algorithm that performs the
    //         following steps:
    //     Step i: Return ! PromiseCall(method, underlyingObject,
    //             extraArgs).
    // Step c: Otherwise, return an algorithm that performs the following
    //         steps, taking an arg argument:
    //     Step i: Let fullArgs be a List consisting of arg followed by the
    //             elements of extraArgs in order.
    //     Step ii: Return ! PromiseCall(method, underlyingObject,
    //                                   fullArgs).
    // (These steps are deferred to the code that performs the algorithm.
    // See Perform{Write,Close}Algorithm, ReadableStreamControllerCancelSteps,
    // and ReadableStreamControllerCallPullIfNeeded.)
    return true;
  }

  // Step 7: Return an algorithm which returns a promise resolved with
  //         undefined (implicit).
  return true;
}

/**
 * Streams spec, 6.3.2. InvokeOrNoop ( O, P, args )
 * As it happens, all callers pass exactly one argument.
 */
[[nodiscard]] bool js::InvokeOrNoop(JSContext* cx, Handle<Value> O,
                                    Handle<PropertyName*> P, Handle<Value> arg,
                                    MutableHandle<Value> rval) {
  cx->check(O, P, arg);

  // Step 1: Assert: O is not undefined.
  MOZ_ASSERT(!O.isUndefined());

  // Step 2: Assert: ! IsPropertyKey(P) is true (implicit).
  // Step 3: Assert: args is a List (implicit).
  // Step 4: Let method be ? GetV(O, P).
  Rooted<Value> method(cx);
  if (!GetProperty(cx, O, P, &method)) {
    return false;
  }

  // Step 5: If method is undefined, return.
  if (method.isUndefined()) {
    return true;
  }

  // Step 6: Return ? Call(method, O, args).
  return Call(cx, method, O, arg, rval);
}

/**
 * Streams spec, 6.3.7. ValidateAndNormalizeHighWaterMark ( highWaterMark,
 * defaultHighWaterMark )
 */
[[nodiscard]] bool js::ValidateAndNormalizeHighWaterMark(
    JSContext* cx, Handle<Value> highWaterMarkVal, double* highWaterMark,
    double defaultHighWaterMark) {
  if (highWaterMarkVal.isUndefined()) {
    *highWaterMark = defaultHighWaterMark;
    return true;
  }

  // Step 1: Set highWaterMark to ? ToNumber(highWaterMark).
  if (!ToNumber(cx, highWaterMarkVal, highWaterMark)) {
    return false;
  }

  // Step 2: If highWaterMark is NaN or highWaterMark < 0, throw a RangeError
  // exception.
  if (std::isnan(*highWaterMark) || *highWaterMark < 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_STREAM_INVALID_HIGHWATERMARK);
    return false;
  }

  // Step 3: Return highWaterMark.
  return true;
}

/**
 * Streams spec, 6.3.8. MakeSizeAlgorithmFromSizeFunction ( size )
 *
 * The standard makes a big deal of turning JavaScript functions (grubby,
 * touched by users, covered with germs) into algorithms (pristine,
 * respectable, purposeful). We don't bother. Here we only check for errors and
 * leave `size` unchanged. Then, in ReadableStreamDefaultControllerEnqueue and
 * WritableStreamDefaultControllerGetChunkSize where this value is used, we
 * check for undefined and behave as if we had "made" an "algorithm" for it.
 */
[[nodiscard]] bool js::MakeSizeAlgorithmFromSizeFunction(JSContext* cx,
                                                         Handle<Value> size) {
  cx->check(size);

  // Step 1: If size is undefined, return an algorithm that returns 1.
  if (size.isUndefined()) {
    // Deferred. Size algorithm users must check for undefined.
    return true;
  }

  // Step 2: If ! IsCallable(size) is false, throw a TypeError exception.
  if (!IsCallable(size)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_FUNCTION,
                              "ReadableStream argument options.size");
    return false;
  }

  // Step 3: Return an algorithm that performs the following steps, taking a
  //         chunk argument:
  //     a. Return ? Call(size, undefined, « chunk »).
  // Deferred. Size algorithm users must know how to call the size function.
  return true;
}
