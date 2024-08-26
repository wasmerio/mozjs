/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Class ReadableStreamBYOBReader.
 */

#include "builtin/streams/ClassSpecMacro.h"  // JS_STREAMS_CLASS_SPEC
#include "builtin/streams/MiscellaneousOperations.h"  // js::ReturnPromiseRejectedWithPendingError
#include "builtin/streams/PullIntoDescriptor.h"  // js::PullIntoDescriptor
#include "builtin/streams/ReadableByteStreamControllerOperations.h"  // js::ReadableByteStreamControllerFillPullIntoDescriptorFromQueue
#include "builtin/streams/ReadableStream.h"            // js::ReadableStream
#include "builtin/streams/ReadableStreamController.h"  // js::ReadableStream{,Default}Controller, js::ReadableStreamDefaultControllerPullSteps, js::ReadableStreamControllerStart{,Failed}Handler
#include "builtin/streams/ReadableStreamInternals.h"  // js::ReadableStream{Cancel,CreateReadResult}
#include "builtin/streams/ReadableStreamReader.h"  // js::ForAuthorCodeBool, js::ReadableStream{,Default}Reader
#include "js/ErrorReport.h"           // JS_ReportErrorNumberASCII
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*

#include "vm/ArrayBufferViewObject.h"  // js::UnwrapArrayBufferView
#include "vm/Interpreter.h"
#include "vm/PromiseObject.h"  // js::UnwrapAndTypeCheckThis
#include "vm/SavedFrame.h"     // js::SavedFrame
#include "builtin/streams/ReadableStreamReader-inl.h"
#include "vm/Compartment-inl.h"   // js::UnwrapAndTypeCheckThis
#include "vm/JSObject-inl.h"      // js::NewObjectWithClassProto
#include "vm/NativeObject-inl.h"  // js::ThrowIfNotConstructing

using JS::CallArgs;
using JS::Handle;
using JS::ObjectValue;
using JS::Rooted;
using JS::RootedObject;
using JS::RootedValueArray;
using JS::UndefinedHandleValue;
using JS::Value;

using js::ForAuthorCodeBool;
using js::GetErrorMessage;
using js::ListObject;
using js::NewObjectWithClassProto;
using js::PromiseObject;
using js::PullIntoDescriptor;
using js::ReadableByteStreamController;
using js::ReadableStream;
using js::ReadableStreamBYOBReader;
using js::ReadableStreamReader;
using js::UnwrapAndTypeCheckThis;

/*** 3.7. Class ReadableStreamBYOBReader *********************************/

/**
 * Stream spec, 3.7.3. new ReadableStreamBYOBReader ( stream )
 * Steps 2-5.
 */
ReadableStreamBYOBReader* js::CreateReadableStreamBYOBReader(
    JSContext* cx, Handle<ReadableStream*> unwrappedStream,
    ForAuthorCodeBool forAuthorCode, Handle<JSObject*> proto /* = nullptr */) {
  Rooted<ReadableStreamBYOBReader*> reader(
      cx, NewObjectWithClassProto<ReadableStreamBYOBReader>(cx, proto));
  if (!reader) {
    return nullptr;
  }

  // Step 1: If ! IsReadableStreamLocked(stream) is true, throw a TypeError
  //         exception.
  if (unwrappedStream->locked()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAM_LOCKED);
    return nullptr;
  }

  // Step 2: If ! IsReadableByteStreamController(
  //                  stream.[[readableStreamController]]) is false, throw a
  //         TypeError exception.
  if (!unwrappedStream->controller()->is<js::ReadableByteStreamController>()) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr,
        JSMSG_READABLESTREAM_BYOB_READER_FOR_NON_BYTE_STREAM);
    return nullptr;
  }

  // Step 3: Perform ! ReadableStreamReaderGenericInitialize(this, stream).
  // Step 4: Set this.[[readRequests]] to a new empty List.
  if (!ReadableStreamReaderGenericInitialize(cx, reader, unwrappedStream,
                                             forAuthorCode)) {
    return nullptr;
  }

  return reader;
}

/**
 * Stream spec, 3.6.3. new ReadableStreamBYOBReader ( stream )
 */
bool ReadableStreamBYOBReader::constructor(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "ReadableStreamBYOBReader")) {
    return false;
  }

  // Implicit in the spec: Find the prototype object to use.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Null, &proto)) {
    return false;
  }

  // Step 1: If ! IsReadableStream(stream) is false, throw a TypeError
  //         exception.
  Rooted<ReadableStream*> unwrappedStream(
      cx, UnwrapAndTypeCheckArgument<ReadableStream>(
              cx, args, "ReadableStreamBYOBReader constructor", 0));
  if (!unwrappedStream) {
    return false;
  }

  Rooted<JSObject*> reader(
      cx, CreateReadableStreamBYOBReader(cx, unwrappedStream,
                                         ForAuthorCodeBool::Yes, proto));
  if (!reader) {
    return false;
  }

  args.rval().setObject(*reader);
  return true;
}

/**
 * Streams spec, 3.6.4.1 get closed
 */
static bool ReadableStreamBYOBReader_closed(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsReadableStreamBYOBReader(this) is false, return a promise
  //         rejected with a TypeError exception.
  Rooted<ReadableStreamBYOBReader*> unwrappedReader(
      cx,
      UnwrapAndTypeCheckThis<ReadableStreamBYOBReader>(cx, args, "get closed"));
  if (!unwrappedReader) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 2: Return this.[[closedPromise]].
  Rooted<JSObject*> closedPromise(cx, unwrappedReader->closedPromise());
  if (!cx->compartment()->wrap(cx, &closedPromise)) {
    return false;
  }

  args.rval().setObject(*closedPromise);
  return true;
}

/**
 * https://streams.spec.whatwg.org/#rs-cancel
 */
static bool ReadableStreamBYOBReader_cancel(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsReadableStreamBYOBReader(this) is false, return a promise
  //         rejected with a TypeError exception.
  Rooted<ReadableStreamBYOBReader*> unwrappedReader(
      cx, UnwrapAndTypeCheckThis<ReadableStreamBYOBReader>(cx, args, "cancel"));
  if (!unwrappedReader) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 2: If this.[[ownerReadableStream]] is undefined, return a promise
  //         rejected with a TypeError exception.
  if (!unwrappedReader->hasStream()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMREADER_NOT_OWNED, "cancel");
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 3: Return ! ReadableStreamReaderGenericCancel(this, reason).
  JSObject* cancelPromise =
      ReadableStreamReaderGenericCancel(cx, unwrappedReader, args.get(0));
  if (!cancelPromise) {
    return false;
  }
  args.rval().setObject(*cancelPromise);
  return true;
}

/**
 * https://streams.spec.whatwg.org/#byob-reader-read
 */
static bool ReadableStreamBYOBReader_read(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Handle<Value> view = args.get(0);

  if (!view.isObject() || !(JS_IsArrayBufferViewObject(&view.toObject()) ||
                            JS::IsArrayBufferObject(&view.toObject()))) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMBYOBREADER_READ_NOT_BUFFER);
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  RootedObject viewBuffer(cx, &view.toObject());
  uint8_t* data;
  bool is_shared;
  bool detached;
  size_t len = 0;

  if (JS_IsArrayBufferViewObject(viewBuffer)) {
    js::GetArrayBufferViewLengthAndData(viewBuffer, &len, &is_shared, &data);
    detached = viewBuffer->maybeUnwrapAs<js::ArrayBufferViewObject>()
                   ->hasDetachedBuffer();
  } else {
    JS::GetArrayBufferLengthAndData(viewBuffer, &len, &is_shared, &data);
    detached = viewBuffer->maybeUnwrapAs<js::ArrayBufferObject>()->isDetached();
  }

  if (len == 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMBYOBREADER_READ_EMPTY_VIEW);
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  if (detached) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMBYOBREADER_READ_DETACHED);
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  Rooted<ReadableStreamBYOBReader*> unwrappedReader(
      cx, UnwrapAndTypeCheckThis<ReadableStreamBYOBReader>(cx, args, "read"));
  if (!unwrappedReader) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  if (!unwrappedReader->hasStream()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMREADER_NOT_OWNED, "read");
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  PromiseObject* readPromise =
      js::ReadableStreamBYOBReaderRead(cx, unwrappedReader, viewBuffer);
  if (!readPromise) {
    return false;
  }
  args.rval().setObject(*readPromise);

  return true;
}

/**
 * https://streams.spec.whatwg.org/#abstract-opdef-readablestreambyobreaderrelease
 */
static bool ReadableStreamBYOBReader_releaseLock(JSContext* cx, unsigned argc,
                                                 Value* vp) {
  // If ! IsReadableStreamBYOBReader(this) is false, throw a TypeError
  // exception.
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableStreamBYOBReader*> reader(
      cx, UnwrapAndTypeCheckThis<ReadableStreamBYOBReader>(cx, args,
                                                           "releaseLock"));
  if (!reader) {
    return false;
  }

  // If this.[[ownerReadableStream]] is undefined, return.
  if (!reader->hasStream()) {
    args.rval().setUndefined();
    return true;
  }

  // Perform ! ReadableStreamReaderGenericRelease(reader).
  if (!js::ReadableStreamReaderGenericRelease(cx, reader)) {
    return false;
  }

  // Let e be a new TypeError exception.
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_READABLESTREAMREADER_RELEASED);
  JS::RootedValue e(cx);
  Rooted<js::SavedFrame*> stack(cx);
  if (!cx->isExceptionPending() ||
      !GetAndClearExceptionAndStack(cx, &e, &stack)) {
    // Uncatchable error. Die immediately without erroring the
    // stream.
    return false;
  }

  // Perform ! ReadableStreamBYOBReaderErrorReadIntoRequests(reader, e).
  if (!js::ReadableStreamReaderErrorReadOrReadIntoRequests(cx, reader, e)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * https://streams.spec.whatwg.org/#readable-stream-byob-reader-read
 */
PromiseObject* js::ReadableStreamBYOBReaderRead(
    JSContext* cx, Handle<ReadableStreamReader*> unwrappedReader,
    Handle<JSObject*> view) {
  Rooted<ReadableStream*> unwrappedStream(
      cx, UnwrapStreamFromReader(cx, unwrappedReader));
  if (!unwrappedStream) {
    return nullptr;
  }

  unwrappedStream->setDisturbed();

  if (unwrappedStream->errored()) {
    Rooted<Value> storedError(cx, unwrappedStream->storedError());
    if (!cx->compartment()->wrap(cx, &storedError)) {
      return nullptr;
    }
    return PromiseObject::unforgeableReject(cx, storedError);
  }

  Rooted<ReadableByteStreamController*> unwrappedController(
      cx, &unwrappedStream->controller()->as<ReadableByteStreamController>());

  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-pull-into

  // Let stream be controller.[[stream]].

  // Let elementSize be 1.
  uint32_t elementSize = 1;

  // Let ctor be %DataView%.
  Rooted<JSObject*> ctor(cx);

  // If view has a [[TypedArrayName]] internal slot (i.e., it is not a
  // DataView),
  if (JS_IsTypedArrayObject(view)) {
    // Set elementSize to the element size specified in the typed array
    // constructors table for view.[[TypedArrayName]].
    JS::Scalar::Type type = JS_GetArrayBufferViewType(view);
    elementSize = JS::Scalar::byteSize(type);

    // Set ctor to the constructor specified in the typed array constructors
    // table for view.[[TypedArrayName]].
    switch (type) {
      case JS::Scalar::Type::Uint8Clamped:
        ctor.set(cx->global()->getOrCreateConstructor(cx, JSProto_Uint8ClampedArray));
        break;
      case JS::Scalar::Type::Int8:
        ctor.set(cx->global()->getOrCreateConstructor(cx, JSProto_Int8Array));
        break;
      case JS::Scalar::Type::Uint8:
        ctor.set(cx->global()->getOrCreateConstructor(cx, JSProto_Uint8Array));
        break;
      case JS::Scalar::Type::Int16:
        ctor.set(cx->global()->getOrCreateConstructor(cx, JSProto_Int16Array));
        break;
      case JS::Scalar::Type::Uint16:
        ctor.set(cx->global()->getOrCreateConstructor(cx, JSProto_Uint16Array));
        break;
      case JS::Scalar::Type::Int32:
        ctor.set(cx->global()->getOrCreateConstructor(cx, JSProto_Int32Array));
        break;
      case JS::Scalar::Type::Uint32:
        ctor.set(cx->global()->getOrCreateConstructor(cx, JSProto_Uint32Array));
        break;
      case JS::Scalar::Type::Float32:
        ctor.set(cx->global()->getOrCreateConstructor(cx, JSProto_Float32Array));
        break;
      case JS::Scalar::Type::Float64:
        ctor.set(cx->global()->getOrCreateConstructor(cx, JSProto_Float64Array));
        break;
      case JS::Scalar::Type::BigInt64:
        ctor.set(cx->global()->getOrCreateConstructor(cx, JSProto_BigInt64Array));
        break;
      case JS::Scalar::Type::BigUint64:
        ctor.set(cx->global()->getOrCreateConstructor(cx, JSProto_BigUint64Array));
        break;

      case JS::Scalar::Type::MaxTypedArrayViewType:
      case JS::Scalar::Type::Int64:
      case JS::Scalar::Type::Simd128:
        return nullptr;
    }
  }

  // Let byteOffset be view.[[ByteOffset]].
  size_t byteOffset = JS_GetArrayBufferViewByteOffset(view);

  // Let byteLength be view.[[ByteLength]].
  size_t byteLength = JS_GetArrayBufferViewByteLength(view);

  // Let bufferResult be TransferArrayBuffer(view.[[ViewedArrayBuffer]]).
  bool isShared;
  Rooted<JSObject*> viewedArrayBuffer(
      cx, JS_GetArrayBufferViewBuffer(cx, view, &isShared));
  if (!viewedArrayBuffer) {
    return PromiseRejectedWithPendingError(cx);
  }
  Rooted<JSObject*> bufferResult(cx,
                                 TransferArrayBuffer(cx, viewedArrayBuffer));

  // If bufferResult is an abrupt completion,
  if (!bufferResult) {
    return PromiseRejectedWithPendingError(cx);
  }

  // Let buffer be bufferResult.[[Value]].

  // Let pullIntoDescriptor be a new pull-into descriptor with buffer buffer,
  // buffer byte length buffer.[[ArrayBufferByteLength]], byte offset
  // byteOffset, byte length byteLength, bytes filled 0, element size
  // elementSize, view constructor ctor, and reader type "byob".
  Rooted<ArrayBufferObject*> buffer(cx, &bufferResult->as<ArrayBufferObject>());
  Rooted<PullIntoDescriptor*> pullIntoDescriptor(
      cx, PullIntoDescriptor::create(cx, buffer, byteOffset, byteLength, 0,
                                     elementSize, ctor, ReaderType::BYOB));
  if (!pullIntoDescriptor) {
    return PromiseRejectedWithPendingError(cx);
  }

  // If controller.[[pendingPullIntos]] is not empty,
  if (unwrappedController->pendingPullIntos()->length() > 0) {
    // Append pullIntoDescriptor to controller.[[pendingPullIntos]].
    Rooted<Value> pullIntoDescriptorVal(cx);
    pullIntoDescriptorVal.setObject(*pullIntoDescriptor);
    if (!unwrappedController->pendingPullIntos()->append(
            cx, pullIntoDescriptorVal)) {
      return nullptr;
    }

    // Perform ! ReadableStreamAddReadIntoRequest(stream, readIntoRequest).
    // Return.
    return ReadableStreamAddReadOrReadIntoRequest(cx, unwrappedStream);
  }

  // If stream.[[state]] is "closed",
  if (unwrappedStream->closed()) {
    // Let emptyView be ! Construct(ctor, « pullIntoDescriptor’s buffer,
    // pullIntoDescriptor’s byte offset, 0 »).
    RootedValueArray<3> args(cx);
    args[0].setObject(*pullIntoDescriptor->buffer());
    args[1].setInt32(pullIntoDescriptor->byteOffset());
    args[2].setInt32(0);

    Rooted<JSObject*> obj(cx);
    RootedValue ctorVal(cx);
    ctorVal.setObject(*ctor);
    if (!JS::Construct(cx, ctorVal, args, &obj)) {
      return PromiseRejectedWithPendingError(cx);
    }
    if (!obj) {
      return nullptr;
    }

    // Perform readIntoRequest’s close steps, given emptyView.
    RootedValue objVal(cx);
    objVal.setObject(*obj);
    PlainObject* iterResult = ReadableStreamCreateReadResult(
        cx, objVal, true, unwrappedReader->forAuthorCode());
    if (!iterResult) {
      return nullptr;
    }

    Rooted<Value> iterResultVal(cx, JS::ObjectValue(*iterResult));
    return PromiseObject::unforgeableResolveWithNonPromise(cx, iterResultVal);
  }

  // If controller.[[queueTotalSize]] > 0,
  if (unwrappedController->queueTotalSize() > 0) {
    // If !
    // ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(controller,
    // pullIntoDescriptor) is true,
    bool ready;
    if (!js::ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(
            cx, unwrappedController, pullIntoDescriptor, &ready)) {
      return nullptr;
    }
    if (ready) {
      // Let filledView be !
      // ReadableByteStreamControllerConvertPullIntoDescriptor(pullIntoDescriptor).
      Rooted<JSObject*> view(
          cx, ReadableByteStreamControllerConvertPullIntoDescriptor(
                  cx, pullIntoDescriptor));

      // Perform ! ReadableByteStreamControllerHandleQueueDrain(controller).
      if (!ReadableByteStreamControllerHandleQueueDrain(cx,
                                                        unwrappedController)) {
        return nullptr;
      }

      // Perform readIntoRequest’s chunk steps, given filledView.
      RootedValue objVal(cx);
      objVal.setObject(*view);
      PlainObject* iterResult = ReadableStreamCreateReadResult(
          cx, objVal, false, unwrappedReader->forAuthorCode());
      if (!iterResult) {
        return nullptr;
      }

      // Return.
      Rooted<Value> iterResultVal(cx, JS::ObjectValue(*iterResult));
      return PromiseObject::unforgeableResolveWithNonPromise(cx, iterResultVal);
    }
    // If controller.[[closeRequested]] is true,
    if (unwrappedController->closeRequested()) {
      // Let e be a TypeError exception.
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_READABLESTREAMCONTROLLER_CLOSED, "read");
      Rooted<Value> exn(cx);
      Rooted<SavedFrame*> stack(cx);
      if (!cx->isExceptionPending() ||
          !GetAndClearExceptionAndStack(cx, &exn, &stack)) {
        return nullptr;
      }
      // Perform ! ReadableByteStreamControllerError(controller, e).
      if (!ReadableStreamControllerError(cx, unwrappedController, exn)) {
        return nullptr;
      }

      // Perform readIntoRequest’s error steps, given e.
      // Return.
      Rooted<Value> storedError(cx, unwrappedStream->storedError());
      if (!cx->compartment()->wrap(cx, &storedError)) {
        return nullptr;
      }
      return PromiseObject::unforgeableReject(cx, storedError);
    }
  }

  // Append pullIntoDescriptor to controller.[[pendingPullIntos]].
  Rooted<Value> pullIntoDescriptorVal(cx);
  pullIntoDescriptorVal.setObject(*pullIntoDescriptor);
  if (!unwrappedController->pendingPullIntos()->append(cx,
                                                       pullIntoDescriptorVal)) {
    return nullptr;
  }

  // Perform ! ReadableStreamAddReadIntoRequest(stream, readIntoRequest).
  PromiseObject* promise =
      ReadableStreamAddReadOrReadIntoRequest(cx, unwrappedStream);

  // Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
  if (!ReadableStreamControllerCallPullIfNeeded(cx, unwrappedController)) {
    return nullptr;
  }

  return promise;
}

static const JSFunctionSpec ReadableStreamBYOBReader_methods[] = {
    JS_FN("cancel", ReadableStreamBYOBReader_cancel, 1, 0),
    JS_FN("read", ReadableStreamBYOBReader_read, 0, 0),
    JS_FN("releaseLock", ReadableStreamBYOBReader_releaseLock, 0, 0),
    JS_FS_END};

static const JSPropertySpec ReadableStreamBYOBReader_properties[] = {
    JS_PSG("closed", ReadableStreamBYOBReader_closed, 0), JS_PS_END};

JS_STREAMS_CLASS_SPEC(ReadableStreamBYOBReader, 1,
                      ReadableByteStreamController::Slots::SlotCount, 0, 0,
                      JS_NULL_CLASS_OPS);
