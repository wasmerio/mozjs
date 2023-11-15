/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ReadableStream reader abstract operations. */

#include "builtin/streams/ReadableStreamReader-inl.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT{,_IF}

#include "jsfriendapi.h"  // JS_ReportErrorNumberASCII

#include "builtin/streams/PullIntoDescriptor.h"        // js::PullIntoDescriptor
#include "builtin/streams/ReadableStream.h"            // js::ReadableStream
#include "builtin/streams/ReadableStreamController.h"  // js::ReadableStreamController
#include "builtin/streams/ReadableStreamInternals.h"  // js::ReadableStream{Cancel,CreateReadResult}
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/RootingAPI.h"            // JS::Handle, JS::Rooted
#include "js/Value.h"                 // JS::Value, JS::UndefinedHandleValue
#include "vm/Interpreter.h"           // js::GetAndClearException
#include "vm/JSContext.h"             // JSContext
#include "vm/PlainObject.h"           // js::PlainObject
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseResolvedWithUndefined
#include "vm/Runtime.h"        // JSRuntime

#include "builtin/Promise-inl.h"  // js::SetSettledPromiseIsHandled
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap, js::UnwrapInternalSlot
#include "vm/List-inl.h"         // js::StoreNewListInFixedSlot
#include "vm/Realm-inl.h"        // js::AutoRealm

using JS::Handle;
using JS::Rooted;
using JS::Value;

using js::PromiseObject;
using js::PullIntoDescriptor;
using js::ReadableStreamController;
using js::ReaderType;
using js::UnwrapStreamFromReader;

/*** 3.8. Readable stream reader abstract operations ************************/

// Streams spec, 3.8.1. IsReadableStreamDefaultReader ( x )
// Implemented via is<ReadableStreamDefaultReader>()

// Streams spec, 3.8.2. IsReadableStreamBYOBReader ( x )
// Implemented via is<ReadableStreamBYOBReader>()

/**
 * Streams spec, 3.8.3. ReadableStreamReaderGenericCancel ( reader, reason )
 */
[[nodiscard]] JSObject* js::ReadableStreamReaderGenericCancel(
    JSContext* cx, Handle<ReadableStreamReader*> unwrappedReader,
    Handle<Value> reason) {
  // Step 1: Let stream be reader.[[ownerReadableStream]].
  // Step 2: Assert: stream is not undefined (implicit).
  Rooted<ReadableStream*> unwrappedStream(
      cx, UnwrapStreamFromReader(cx, unwrappedReader));
  if (!unwrappedStream) {
    return nullptr;
  }

  // Step 3: Return ! ReadableStreamCancel(stream, reason).
  return js::ReadableStreamCancel(cx, unwrappedStream, reason);
}

/**
 * Streams spec, 3.8.4.
 *      ReadableStreamReaderGenericInitialize ( reader, stream )
 */
[[nodiscard]] bool js::ReadableStreamReaderGenericInitialize(
    JSContext* cx, Handle<ReadableStreamReader*> reader,
    Handle<ReadableStream*> unwrappedStream, ForAuthorCodeBool forAuthorCode) {
  cx->check(reader);

  // Step 1: Set reader.[[forAuthorCode]] to true.
  reader->setForAuthorCode(forAuthorCode);

  // Step 2: Set reader.[[ownerReadableStream]] to stream.
  {
    Rooted<JSObject*> readerCompartmentStream(cx, unwrappedStream);
    if (!cx->compartment()->wrap(cx, &readerCompartmentStream)) {
      return false;
    }
    reader->setStream(readerCompartmentStream);
  }

  // Step 3 is moved to the end.

  // Step 4: If stream.[[state]] is "readable",
  Rooted<PromiseObject*> promise(cx);
  if (unwrappedStream->readable()) {
    // Step a: Set reader.[[closedPromise]] to a new promise.
    promise = PromiseObject::createSkippingExecutor(cx);
  } else if (unwrappedStream->closed()) {
    // Step 5: Otherwise, if stream.[[state]] is "closed",
    // Step a: Set reader.[[closedPromise]] to a promise resolved with
    //         undefined.
    promise = PromiseResolvedWithUndefined(cx);
  } else {
    // Step 6: Otherwise,
    // Step a: Assert: stream.[[state]] is "errored".
    MOZ_ASSERT(unwrappedStream->errored());

    // Step b: Set reader.[[closedPromise]] to a promise rejected with
    //         stream.[[storedError]].
    Rooted<Value> storedError(cx, unwrappedStream->storedError());
    if (!cx->compartment()->wrap(cx, &storedError)) {
      return false;
    }
    promise = PromiseObject::unforgeableReject(cx, storedError);
    if (!promise) {
      return false;
    }

    // Step c. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
    js::SetSettledPromiseIsHandled(cx, promise);
  }

  if (!promise) {
    return false;
  }

  reader->setClosedPromise(promise);

  // Step 4 of caller 3.6.3. new ReadableStreamDefaultReader(stream):
  // Step 5 of caller 3.7.3. new ReadableStreamBYOBReader(stream):
  //     Set this.[[read{Into}Requests]] to a new empty List.
  if (!StoreNewListInFixedSlot(cx, reader,
                               ReadableStreamReader::Slot_Requests)) {
    return false;
  }

  // Step 3: Set stream.[[reader]] to reader.
  // Doing this last prevents a partially-initialized reader from being
  // attached to the stream (and possibly left there on OOM).
  {
    AutoRealm ar(cx, unwrappedStream);
    Rooted<JSObject*> streamCompartmentReader(cx, reader);
    if (!cx->compartment()->wrap(cx, &streamCompartmentReader)) {
      return false;
    }
    unwrappedStream->setReader(streamCompartmentReader);
  }

  return true;
}

/**
 * https://streams.spec.whatwg.org/#readable-stream-reader-generic-release
 */
[[nodiscard]] bool js::ReadableStreamReaderGenericRelease(
    JSContext* cx, Handle<ReadableStreamReader*> unwrappedReader) {
  // Let stream be reader.[[stream]].
  // Assert: stream is not undefined.
  Rooted<ReadableStream*> unwrappedStream(
      cx, UnwrapStreamFromReader(cx, unwrappedReader));
  if (!unwrappedStream) {
    return false;
  }

  // Assert: stream.[[reader]] is reader.
#ifdef DEBUG
  // The assertion is weakened a bit to allow for nuked wrappers.
  ReadableStreamReader* unwrappedReader2 =
      UnwrapReaderFromStreamNoThrow(unwrappedStream);
  MOZ_ASSERT_IF(unwrappedReader2, unwrappedReader2 == unwrappedReader);
#endif

  // Create an exception to reject promises with below. We don't have a
  // clean way to do this, unfortunately.
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_READABLESTREAMREADER_RELEASED);
  Rooted<Value> exn(cx);
  if (!cx->isExceptionPending() || !GetAndClearException(cx, &exn)) {
    // Uncatchable error. Die immediately without resolving
    // reader.[[closedPromise]].
    return false;
  }

  // If stream.[[state]] is "readable", reject reader.[[closedPromise]] with a
  // TypeError exception.
  Rooted<PromiseObject*> unwrappedClosedPromise(cx);
  if (unwrappedStream->readable()) {
    unwrappedClosedPromise = UnwrapInternalSlot<PromiseObject>(
        cx, unwrappedReader, ReadableStreamReader::Slot_ClosedPromise);
    if (!unwrappedClosedPromise) {
      return false;
    }

    AutoRealm ar(cx, unwrappedClosedPromise);
    if (!cx->compartment()->wrap(cx, &exn)) {
      return false;
    }
    if (!PromiseObject::reject(cx, unwrappedClosedPromise, exn)) {
      return false;
    }
  } else {
    // Otherwise, set reader.[[closedPromise]] to a promise rejected with a
    // TypeError exception.
    Rooted<JSObject*> closedPromise(cx,
                                    PromiseObject::unforgeableReject(cx, exn));
    if (!closedPromise) {
      return false;
    }
    unwrappedClosedPromise = &closedPromise->as<PromiseObject>();

    AutoRealm ar(cx, unwrappedReader);
    if (!cx->compartment()->wrap(cx, &closedPromise)) {
      return false;
    }
    unwrappedReader->setClosedPromise(closedPromise);
  }

  // Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
  js::SetSettledPromiseIsHandled(cx, unwrappedClosedPromise);

  // Perform ! stream.[[controller]].[[ReleaseSteps]]().
  // https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontroller-releasesteps
  if (unwrappedStream->controller()->is<ReadableByteStreamController>()) {
    Rooted<ReadableByteStreamController*> unwrappedController(
        cx, &unwrappedStream->controller()->as<ReadableByteStreamController>());

    // If this.[[pendingPullIntos]] is not empty,
    if (unwrappedController->pendingPullIntos()->length() > 0) {
      // Let firstPendingPullInto be this.[[pendingPullIntos]][0].
      Rooted<PullIntoDescriptor*> firstPendingPullInto(
          cx, UnwrapAndDowncastObject<PullIntoDescriptor>(
                  cx, &unwrappedController->pendingPullIntos()
                           ->popFirstAs<JSObject>(cx)));

      // Set firstPendingPullInto’s reader type to "none".
      firstPendingPullInto->setReaderType(ReaderType::None);

      // Set this.[[pendingPullIntos]] to the list « firstPendingPullInto ».
      if (!StoreNewListInFixedSlot(
              cx, unwrappedController,
              ReadableByteStreamController::Slot_PendingPullIntos)) {
        return false;
      }
      Rooted<Value> firstPendingPullIntoValue(cx);
      firstPendingPullIntoValue.setObject(*firstPendingPullInto);
      if (!unwrappedController->pendingPullIntos()->append(
              cx, firstPendingPullIntoValue)) {
        return false;
      }
    }
  }

  // Set stream.[[reader]] to undefined.
  unwrappedStream->clearReader();

  // Set reader.[[stream]] to undefined.
  unwrappedReader->clearStream();

  return true;
}
