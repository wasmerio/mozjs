/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Class ReadableStreamDefaultReader. */

#include "builtin/streams/ClassSpecMacro.h"  // JS_STREAMS_CLASS_SPEC
#include "builtin/streams/MiscellaneousOperations.h"  // js::ReturnPromiseRejectedWithPendingError
#include "builtin/streams/ReadableStream.h"            // js::ReadableStream
#include "builtin/streams/ReadableStreamController.h"  // js::ReadableStreamController
#include "builtin/streams/ReadableStreamInternals.h"  // js::ReadableStream{Cancel,CreateReadResult}
#include "builtin/streams/ReadableStreamReader.h"  // js::ForAuthorCodeBool, js::ReadableStream{,Default}Reader
#include "js/CallArgs.h"              // JS::CallArgs{,FromVp}
#include "js/Class.h"                 // JSClass, JS_NULL_CLASS_OPS
#include "js/ErrorReport.h"           // JS_ReportErrorNumberASCII
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/RootingAPI.h"            // JS::Handle, JS::Rooted
#include "vm/Interpreter.h"
#include "vm/PromiseObject.h"  // js::PromiseObject

#include "builtin/streams/ReadableStreamReader-inl.h"  // js::UnwrapStreamFromReader

#include "vm/Compartment-inl.h"   // js::UnwrapAndTypeCheckThis
#include "vm/JSObject-inl.h"      // js::NewObjectWithClassProto
#include "vm/NativeObject-inl.h"  // js::ThrowIfNotConstructin

using JS::Handle;
using JS::Rooted;
using JS::Value;

using js::PromiseObject;
using js::ReadableStreamController;

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Handle;
using JS::Rooted;
using JS::Value;

using js::ForAuthorCodeBool;
using js::GetErrorMessage;
using js::ListObject;
using js::NewObjectWithClassProto;
using js::PromiseObject;
using js::ReadableStream;
using js::ReadableStreamDefaultReader;
using js::ReadableStreamReader;
using js::UnwrapAndTypeCheckThis;
using js::UnwrapStreamFromReader;

/*** 3.6. Class ReadableStreamDefaultReader *********************************/

/**
 * Stream spec, 3.6.3. new ReadableStreamDefaultReader ( stream )
 * Steps 2-4.
 */
[[nodiscard]] ReadableStreamDefaultReader*
js::CreateReadableStreamDefaultReader(JSContext* cx,
                                      Handle<ReadableStream*> unwrappedStream,
                                      ForAuthorCodeBool forAuthorCode,
                                      Handle<JSObject*> proto /* = nullptr */) {
  Rooted<ReadableStreamDefaultReader*> reader(
      cx, NewObjectWithClassProto<ReadableStreamDefaultReader>(cx, proto));
  if (!reader) {
    return nullptr;
  }

  // Step 2: If ! IsReadableStreamLocked(stream) is true, throw a TypeError
  //         exception.
  if (unwrappedStream->locked()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAM_LOCKED);
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
 * Stream spec, 3.6.3. new ReadableStreamDefaultReader ( stream )
 */
bool ReadableStreamDefaultReader::constructor(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "ReadableStreamDefaultReader")) {
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
              cx, args, "ReadableStreamDefaultReader constructor", 0));
  if (!unwrappedStream) {
    return false;
  }

  Rooted<JSObject*> reader(
      cx, CreateReadableStreamDefaultReader(cx, unwrappedStream,
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
[[nodiscard]] static bool ReadableStreamDefaultReader_closed(JSContext* cx,
                                                             unsigned argc,
                                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsReadableStreamDefaultReader(this) is false, return a promise
  //         rejected with a TypeError exception.
  Rooted<ReadableStreamDefaultReader*> unwrappedReader(
      cx, UnwrapAndTypeCheckThis<ReadableStreamDefaultReader>(cx, args,
                                                              "get closed"));
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
 * Streams spec, 3.6.4.2. cancel ( reason )
 */
[[nodiscard]] static bool ReadableStreamDefaultReader_cancel(JSContext* cx,
                                                             unsigned argc,
                                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsReadableStreamDefaultReader(this) is false, return a promise
  //         rejected with a TypeError exception.
  Rooted<ReadableStreamDefaultReader*> unwrappedReader(
      cx,
      UnwrapAndTypeCheckThis<ReadableStreamDefaultReader>(cx, args, "cancel"));
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
 * Streams spec, 3.6.4.3 read ( )
 */
[[nodiscard]] static bool ReadableStreamDefaultReader_read(JSContext* cx,
                                                           unsigned argc,
                                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: If ! IsReadableStreamDefaultReader(this) is false, return a promise
  //         rejected with a TypeError exception.
  Rooted<ReadableStreamDefaultReader*> unwrappedReader(
      cx,
      UnwrapAndTypeCheckThis<ReadableStreamDefaultReader>(cx, args, "read"));
  if (!unwrappedReader) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 2: If this.[[ownerReadableStream]] is undefined, return a promise
  //         rejected with a TypeError exception.
  if (!unwrappedReader->hasStream()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMREADER_NOT_OWNED, "read");
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  // Step 3: Return ! ReadableStreamDefaultReaderRead(this, true).
  PromiseObject* readPromise =
      js::ReadableStreamDefaultReaderRead(cx, unwrappedReader);
  if (!readPromise) {
    return false;
  }
  args.rval().setObject(*readPromise);
  return true;
}

/**
 * https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaultreadererrorreadrequests
 * AND
 * https://streams.spec.whatwg.org/#abstract-opdef-readablestreambyobreadererrorreadintorequests
 */
[[nodiscard]] bool js::ReadableStreamReaderErrorReadOrReadIntoRequests(
    JSContext* cx, Handle<ReadableStreamReader*> reader, Handle<Value> err) {
  // Let readRequests be reader.[[readRequests]].
  ListObject* readIntoRequests = reader->requests();

  if (!readIntoRequests) {
    return true;
  }

  // Set reader.[[readRequests]] to a new empty list.
  reader->clearRequests();

  // For each readRequest of readRequests,
  uint32_t len = readIntoRequests->length();
  Rooted<JSObject*> readRequest(cx);
  Rooted<JSObject*> resultObj(cx);
  Rooted<Value> resultVal(cx);
  for (uint32_t i = 0; i < len; i++) {
    // Perform readRequest’s error steps, given e.
    readRequest = &readIntoRequests->getAs<JSObject>(i);
    if (!RejectPromise(cx, readRequest, err)) {
      return false;
    }
  }
  return true;
}

/**
 * https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaultreaderrelease
 */
static bool ReadableStreamDefaultReader_releaseLock(JSContext* cx,
                                                    unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableStreamDefaultReader*> reader(
      cx, UnwrapAndTypeCheckThis<ReadableStreamDefaultReader>(cx, args,
                                                              "releaseLock"));
  if (!reader) {
    return false;
  }

  // If this.[[stream]] is undefined, return.
  if (!reader->hasStream()) {
    args.rval().setUndefined();
    return true;
  }

  // Perform ! ReadableStreamDefaultReaderRelease(this).
  // (inlined)

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

  // Perform ! ReadableStreamDefaultReaderErrorReadRequests(reader, e).
  if (!ReadableStreamReaderErrorReadOrReadIntoRequests(cx, reader, e)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 3.8.7.
 *      ReadableStreamDefaultReaderRead ( reader [, forAuthorCode ] )
 */
[[nodiscard]] PromiseObject* js::ReadableStreamDefaultReaderRead(
    JSContext* cx, Handle<ReadableStreamReader*> unwrappedReader) {
  // Step 1: If forAuthorCode was not passed, set it to false (implicit).

  // Step 2: Let stream be reader.[[ownerReadableStream]].
  // Step 3: Assert: stream is not undefined.
  Rooted<ReadableStream*> unwrappedStream(
      cx, UnwrapStreamFromReader(cx, unwrappedReader));
  if (!unwrappedStream) {
    return nullptr;
  }

  // Step 4: Set stream.[[disturbed]] to true.
  unwrappedStream->setDisturbed();

  // Step 5: If stream.[[state]] is "closed", return a promise resolved with
  //         ! ReadableStreamCreateReadResult(undefined, true, forAuthorCode).
  if (unwrappedStream->closed()) {
    PlainObject* iterResult = ReadableStreamCreateReadResult(
        cx, UndefinedHandleValue, true, unwrappedReader->forAuthorCode());
    if (!iterResult) {
      return nullptr;
    }

    Rooted<Value> iterResultVal(cx, JS::ObjectValue(*iterResult));
    return PromiseObject::unforgeableResolveWithNonPromise(cx, iterResultVal);
  }

  // Step 6: If stream.[[state]] is "errored", return a promise rejected
  //         with stream.[[storedError]].
  if (unwrappedStream->errored()) {
    Rooted<Value> storedError(cx, unwrappedStream->storedError());
    if (!cx->compartment()->wrap(cx, &storedError)) {
      return nullptr;
    }
    return PromiseObject::unforgeableReject(cx, storedError);
  }

  // Step 7: Assert: stream.[[state]] is "readable".
  MOZ_ASSERT(unwrappedStream->readable());

  // Step 8: Return ! stream.[[readableStreamController]].[[PullSteps]]().
  Rooted<ReadableStreamController*> unwrappedController(
      cx, unwrappedStream->controller());

  if (unwrappedController->is<ReadableStreamDefaultController>()) {
    Rooted<ReadableStreamDefaultController*> unwrappedDefaultController(
        cx, &unwrappedController->as<ReadableStreamDefaultController>());
    return ReadableStreamDefaultControllerPullSteps(cx,
                                                    unwrappedDefaultController);
  } else {
    MOZ_ASSERT(unwrappedController->is<ReadableByteStreamController>());
    Rooted<ReadableByteStreamController*> unwrappedByteController(
        cx, &unwrappedController->as<ReadableByteStreamController>());
    return ReadableByteStreamControllerPullSteps(cx, unwrappedByteController);
  }
}

static const JSFunctionSpec ReadableStreamDefaultReader_methods[] = {
    JS_FN("cancel", ReadableStreamDefaultReader_cancel, 1, 0),
    JS_FN("read", ReadableStreamDefaultReader_read, 0, 0),
    JS_FN("releaseLock", ReadableStreamDefaultReader_releaseLock, 0, 0),
    JS_FS_END};

static const JSPropertySpec ReadableStreamDefaultReader_properties[] = {
    JS_PSG("closed", ReadableStreamDefaultReader_closed, 0), JS_PS_END};

const JSClass ReadableStreamReader::class_ = {"ReadableStreamReader"};

JS_STREAMS_CLASS_SPEC(ReadableStreamDefaultReader, 1, SlotCount, 0, 0,
                      JS_NULL_CLASS_OPS);
