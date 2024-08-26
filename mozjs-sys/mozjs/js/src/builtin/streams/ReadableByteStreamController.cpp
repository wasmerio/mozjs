/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Class ReadableByteStreamController. */

#include "mozilla/Assertions.h"  // MOZ_ASSERT{,_IF}

#include "jsfriendapi.h"  // js::AssertSameCompartment

#include "builtin/streams/ClassSpecMacro.h"           // JS_STREAMS_CLASS_SPEC
#include "builtin/streams/MiscellaneousOperations.h"  // js::IsMaybeWrapped
#include "builtin/streams/PullIntoDescriptor.h"       // js::PullIntoDescriptor
#include "builtin/streams/QueueWithSizes.h"  // js::{DequeueValue,ResetQueue}
#include "builtin/streams/ReadableByteStreamControllerOperations.h"  // js::ReadableStreamController{CallPullIfNeeded,ClearAlgorithms,Error,GetDesiredSizeUnchecked}, js::ReadableByteStreamController{Close,Enqueue}
#include "builtin/streams/ReadableStream.h"  // js::ReadableStream, js::SetUpExternalReadableByteStreamController
#include "builtin/streams/ReadableStreamBYOBRequest.h"  // js::ReadableStreamBYOBRequest
#include "builtin/streams/ReadableStreamController.h"  // js::ReadableStream{,Default}Controller, js::ReadableByteStreamController, js::CheckReadableStreamControllerCanCloseOrEnqueue, js::ReadableStreamControllerCancelSteps, js::ReadableByteStreamControllerPullSteps, js::ReadableStreamControllerStart{,Failed}Handler
#include "builtin/streams/ReadableStreamInternals.h"  // js::ReadableStream{AddReadOrReadIntoRequest,CloseInternal,CreateReadResult}
#include "builtin/streams/ReadableStreamOperations.h"  // js::ReadableStreamTee_Cancel
#include "builtin/streams/ReadableStreamReader.h"  // js::ReadableStream{,Default}Reader
#include "builtin/streams/StreamController.h"  // js::StreamController
#include "builtin/streams/TeeState.h"          // js::TeeState
#include "js/ArrayBuffer.h"                    // JS::NewArrayBuffer
#include "js/Class.h"                          // js::ClassSpec
#include "js/ErrorReport.h"                    // JS_ReportErrorNumberASCII
#include "js/friend/ErrorMessages.h"           // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/Stream.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseResolvedWithUndefined
#include "vm/SelfHosting.h"

#include "builtin/HandlerFunction-inl.h"  // js::TargetFromHandler
#include "builtin/streams/MiscellaneousOperations-inl.h"  // js::PromiseCall
#include "builtin/streams/ReadableStreamReader-inl.h"  // js::UnwrapReaderFromStream
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap, js::UnwrapAnd{DowncastObject,TypeCheckThis}
#include "vm/JSContext-inl.h"  // JSContext::check
#include "vm/JSObject-inl.h"   // js::NewBuiltinClassInstance
#include "vm/List-inl.h"       // js::ListObject, js::StoreNewListInFixedSlot
#include "vm/Realm-inl.h"      // js::AutoRealm

using js::ClassSpec;
using js::PromiseObject;
using js::ReadableByteStreamController;
using js::ReadableByteStreamControllerEnqueue;
using js::ReadableStream;
using js::ReadableStreamController;
using js::ReadableStreamControllerCallPullIfNeeded;
using js::ReadableStreamControllerClearAlgorithms;
using js::ReadableStreamControllerError;
using js::ReadableStreamControllerGetDesiredSizeUnchecked;
using js::TargetFromHandler;
using js::UnwrapAndTypeCheckThis;

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Handle;
using JS::ObjectValue;
using JS::Rooted;
using JS::Value;

using namespace js;

/**
 * Streams spec, 3.9.3.
 * new ReadableByteStreamController( stream, underlyingSource, size,
 *                                      highWaterMark )
 */
bool ReadableByteStreamController::constructor(JSContext* cx, unsigned argc,
                                               Value* vp) {
  // Step 1: Throw a TypeError.
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_BOGUS_CONSTRUCTOR,
                            "ReadableByteStreamController");
  return false;
}

const JSClass ByteStreamChunk::class_ = {"ByteStreamChunk",
                                         JSCLASS_HAS_RESERVED_SLOTS(SlotCount)};

ByteStreamChunk* ByteStreamChunk::create(JSContext* cx, HandleObject buffer,
                                         uint32_t byteOffset,
                                         uint32_t byteLength) {
  MOZ_ASSERT(buffer);
  Rooted<ByteStreamChunk*> chunk(cx,
                                 NewBuiltinClassInstance<ByteStreamChunk>(cx));
  if (!chunk) {
    return nullptr;
  }

  chunk->setFixedSlot(Slot_Buffer, ObjectValue(*buffer));
  chunk->setFixedSlot(Slot_ByteOffset, Int32Value(byteOffset));
  chunk->setFixedSlot(Slot_ByteLength, Int32Value(byteLength));
  return chunk;
}

// Disconnect the source from a controller without calling finalize() on it,
// unless this class is reset(). This ensures that finalize() will not be called
// on the source if setting up the controller fails.
class MOZ_RAII AutoClearUnderlyingSource {
  Rooted<ReadableStreamController*> controller_;

 public:
  AutoClearUnderlyingSource(JSContext* cx, ReadableStreamController* controller)
      : controller_(cx, controller) {}

  ~AutoClearUnderlyingSource() {
    if (controller_) {
      ReadableStreamController::clearUnderlyingSource(
          controller_, /* finalizeSource */ false);
    }
  }

  void reset() { controller_ = nullptr; }
};

/**
 * Version of SetUpReadableByteStreamController that's specialized for handling
 * external, embedding-provided, underlying sources.
 */
[[nodiscard]] bool js::SetUpExternalReadableByteStreamController(
    JSContext* cx, Handle<ReadableStream*> stream,
    JS::ReadableStreamUnderlyingSource* source) {
  // Done elsewhere in the standard: Create the controller object.
  Rooted<ReadableByteStreamController*> controller(
      cx, NewBuiltinClassInstance<ReadableByteStreamController>(cx));
  if (!controller) {
    return false;
  }

  AutoClearUnderlyingSource autoClear(cx, controller);

  // Step 1: Assert: stream.[[readableStreamController]] is undefined.
  MOZ_ASSERT(!stream->hasController());

  // Step 2: If autoAllocateChunkSize is not undefined, [...]
  // (It's treated as undefined.)

  // Step 3: Set controller.[[controlledReadableByteStream]] to stream.
  controller->setStream(stream);

  // Step 4: Set controller.[[pullAgain]] and controller.[[pulling]] to false.
  controller->setFlags(0);
  MOZ_ASSERT(!controller->pullAgain());
  MOZ_ASSERT(!controller->pulling());

  // Step 5: Perform
  //         ! ReadableByteStreamControllerClearPendingPullIntos(controller).
  // Omitted. This step is apparently redundant; see
  // <https://github.com/whatwg/streams/issues/975>.

  // Step 6: Perform ! ResetQueue(this).
  controller->setQueueTotalSize(0);

  // Step 7: Set controller.[[closeRequested]] and controller.[[started]] to
  //         false (implicit).
  MOZ_ASSERT(!controller->closeRequested());
  MOZ_ASSERT(!controller->started());

  // Step 8: Set controller.[[strategyHWM]] to
  //         ? ValidateAndNormalizeHighWaterMark(highWaterMark).
  controller->setStrategyHWM(0);

  // Step 9: Set controller.[[pullAlgorithm]] to pullAlgorithm.
  // Step 10: Set controller.[[cancelAlgorithm]] to cancelAlgorithm.
  // (These algorithms are given by source's virtual methods.)
  controller->setExternalSource(source);

  // Step 11: Set controller.[[autoAllocateChunkSize]] to
  //          autoAllocateChunkSize (implicit).
  MOZ_ASSERT(controller->autoAllocateChunkSize());

  // Step 12: Set this.[[pendingPullIntos]] to a new empty List.
  if (!StoreNewListInFixedSlot(
          cx, controller,
          ReadableByteStreamController::Slot_PendingPullIntos)) {
    return false;
  }

  // Step 13: Set stream.[[readableStreamController]] to controller.
  stream->setController(controller);

  // Step 14: Let startResult be the result of performing startAlgorithm.
  // (For external sources, this algorithm does nothing and returns undefined.)
  // Step 15: Let startPromise be a promise resolved with startResult.
  Rooted<PromiseObject*> startPromise(cx, PromiseResolvedWithUndefined(cx));
  if (!startPromise) {
    return false;
  }

  // Step 16: Upon fulfillment of startPromise, [...]
  // Step 17: Upon rejection of startPromise with reason r, [...]
  RootedObject onStartFulfilled(
      cx, NewHandler(cx, ReadableStreamControllerStartHandler, controller));
  if (!onStartFulfilled) {
    return false;
  }
  RootedObject onStartRejected(
      cx,
      NewHandler(cx, ReadableStreamControllerStartFailedHandler, controller));
  if (!onStartRejected) {
    return false;
  }
  if (!JS::AddPromiseReactions(cx, startPromise, onStartFulfilled,
                               onStartRejected)) {
    return false;
  }

  autoClear.reset();
  return true;
}

static void ReadableByteStreamControllerFinalize(JS::GCContext* gcx,
                                                 JSObject* obj) {
  ReadableByteStreamController& controller =
      obj->as<ReadableByteStreamController>();

  if (controller.getFixedSlot(ReadableStreamController::Slot_Flags)
          .isUndefined()) {
    return;
  }

  if (!controller.hasExternalSource()) {
    return;
  }

  controller.externalSource()->finalize();
}

// Streams spec, 3.11.5.1. [[CancelSteps]] ()
// Unified with 3.9.5.1 above.

/**
 * https://streams.spec.whatwg.org/#rbs-controller-private-pull
 */
[[nodiscard]] extern PromiseObject* js::ReadableByteStreamControllerPullSteps(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController) {
  // Let stream be this.[[stream]].
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Assert: ! ReadableStreamHasDefaultReader(stream) is true.
  bool result;
  if (!ReadableStreamHasDefaultReader(cx, unwrappedStream, &result)) {
    return nullptr;
  }
  MOZ_ASSERT(result);

  RootedValue val(cx);
  // If this.[[queueTotalSize]] > 0,
  double queueTotalSize = unwrappedController->queueTotalSize();

  if (queueTotalSize > 0) {
    // Assert: ! ReadableStreamGetNumReadRequests(stream) is 0.
    MOZ_ASSERT(ReadableStreamGetNumReadRequests(unwrappedStream) == 0);

    RootedObject view(cx);

    // Internal stream functions
    if (unwrappedStream->mode() == JS::ReadableStreamMode::ExternalSource) {
      JS::ReadableStreamUnderlyingSource* source =
          unwrappedController->externalSource();

      view = JS_NewUint8Array(cx, queueTotalSize);
      if (!view) {
        return nullptr;
      }

      size_t bytesWritten;
      {
        AutoRealm ar(cx, unwrappedStream);
        source->writeIntoReadRequestBuffer(cx, unwrappedStream, view,
                                           queueTotalSize, &bytesWritten);
      }

      queueTotalSize = queueTotalSize - bytesWritten;
    } else {
      // Assert: ! ReadableStreamGetNumReadRequests(stream) is 0.
      MOZ_ASSERT(ReadableStreamGetNumReadRequests(unwrappedStream) == 0);

      // Perform ! ReadableByteStreamControllerFillReadRequestFromQueue(this,
      // readRequest).
      // (inlined here, per initial implementation)
      // allows external path above to share end instructions

      // Step 3.b: Let entry be the first element of this.[[queue]].
      // Step 3.c: Remove entry from this.[[queue]], shifting all other
      //           elements downward (so that the second becomes the
      //           first, and so on).
      Rooted<ListObject*> unwrappedQueue(cx, unwrappedController->queue());
      Rooted<ByteStreamChunk*> unwrappedEntry(
          cx, UnwrapAndDowncastObject<ByteStreamChunk>(
                  cx, &unwrappedQueue->popFirstAs<JSObject>(cx)));
      if (!unwrappedEntry) {
        return nullptr;
      }

      queueTotalSize = queueTotalSize - unwrappedEntry->byteLength();

      // Step 3.f: Let view be ! Construct(%Uint8Array%,
      //                                   « entry.[[buffer]],
      //                                     entry.[[byteOffset]],
      //                                     entry.[[byteLength]] »).
      // (reordered)
      RootedObject buffer(cx, unwrappedEntry->buffer());
      if (!cx->compartment()->wrap(cx, &buffer)) {
        return nullptr;
      }

      uint32_t byteOffset = unwrappedEntry->byteOffset();
      view = JS_NewUint8ArrayWithBuffer(cx, buffer, byteOffset,
                                        unwrappedEntry->byteLength());
      if (!view) {
        return nullptr;
      }
    }

    // Step 3.d: Set this.[[queueTotalSize]] to
    //           this.[[queueTotalSize]] − entry.[[byteLength]].
    // (reordered)
    unwrappedController->setQueueTotalSize(queueTotalSize);

    // Step 3.e: Perform ! ReadableByteStreamControllerHandleQueueDrain(this).
    // (reordered)
    if (!ReadableByteStreamControllerHandleQueueDrain(cx,
                                                      unwrappedController)) {
      return nullptr;
    }

    // Step 3.g: Return a promise resolved with
    //           ! ReadableStreamCreateReadResult(view, false, forAuthorCode).
    val.setObject(*view);
    ReadableStreamReader* unwrappedReader =
        UnwrapReaderFromStream(cx, unwrappedStream);
    if (!unwrappedReader) {
      return nullptr;
    }
    Rooted<PlainObject*> readResult(
        cx, ReadableStreamCreateReadResult(cx, val, false,
                                           unwrappedReader->forAuthorCode()));
    if (!readResult) {
      return nullptr;
    }
    val.setObject(*readResult);

    return PromiseObject::unforgeableResolveWithNonPromise(cx, val);
  }

  // Let autoAllocateChunkSize be this.[[autoAllocateChunkSize]].
  // If autoAllocateChunkSize is not undefined,
  if (unwrappedController->autoAllocateChunkSize().isSome()) {
    uint64_t autoAllocateChunkSize =
        unwrappedController->autoAllocateChunkSize().value();

    // Let buffer be Construct(%ArrayBuffer%, « autoAllocateChunkSize »).
    JSObject* bufferObj = JS::NewArrayBuffer(cx, autoAllocateChunkSize);

    // If buffer is an abrupt completion,
    if (!bufferObj) {
      // Perform readRequest’s error steps, given buffer.[[Value]].
      // Return
      return PromiseRejectedWithPendingError(cx);
    }

    Rooted<ArrayBufferObject*> buffer(cx, &bufferObj->as<ArrayBufferObject>());

    // Let pullIntoDescriptor be a new pull-into descriptor with buffer
    // buffer.[[Value]], buffer byte length autoAllocateChunkSize, byte offset
    // 0, byte length autoAllocateChunkSize, bytes filled 0, element size 1,
    // view constructor %Uint8Array%, and reader type "default".
    RootedObject uint8Array(cx,
                            &cx->global()->getConstructor(JSProto_Uint8Array));
    RootedObject pullIntoDescriptor(
        cx, PullIntoDescriptor::create(cx, buffer, 0, autoAllocateChunkSize, 0,
                                       1, uint8Array, ReaderType::Default));
    if (!pullIntoDescriptor) {
      return PromiseRejectedWithPendingError(cx);
    }

    // Append pullIntoDescriptor to this.[[pendingPullIntos]].
    if (!AppendToListInFixedSlot(
            cx, unwrappedController,
            ReadableByteStreamController::Slot_PendingPullIntos,
            pullIntoDescriptor)) {
      return nullptr;
    }
  }

  // Perform ! ReadableStreamAddReadRequest(stream, readRequest).
  Rooted<PromiseObject*> promise(
      cx, ReadableStreamAddReadOrReadIntoRequest(cx, unwrappedStream));
  if (!promise) {
    return nullptr;
  }

  // Perform ! ReadableByteStreamControllerCallPullIfNeeded(this).
  if (!ReadableStreamControllerCallPullIfNeeded(cx, unwrappedController)) {
    return nullptr;
  }

  return promise;
}

/*** 3.13. Readable stream BYOB controller abstract operations **************/

// Streams spec, 3.13.1. IsReadableStreamBYOBRequest ( x )
// Implemented via is<ReadableStreamBYOBRequest>()

// Streams spec, 3.13.2. IsReadableByteStreamController ( x )
// Implemented via is<ReadableByteStreamController>()

// Streams spec, 3.13.3.
//      ReadableByteStreamControllerCallPullIfNeeded ( controller )
// Unified with 3.9.2 above.

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-clear-pending-pull-intos
 */
[[nodiscard]] bool js::ReadableByteStreamControllerClearPendingPullIntos(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController) {
  // Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
  if (!ReadableByteStreamControllerInvalidateBYOBRequest(cx,
                                                         unwrappedController)) {
    return false;
  }

  // Set controller.[[pendingPullIntos]] to a new empty list.
  return StoreNewListInFixedSlot(
      cx, unwrappedController,
      ReadableByteStreamController::Slot_PendingPullIntos);
}

/**
 * Streams spec, 3.13.6. ReadableByteStreamControllerClose ( controller )
 */
[[nodiscard]] bool js::ReadableByteStreamControllerClose(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController) {
  // Step 1: Let stream be controller.[[controlledReadableByteStream]].
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());

  // Step 2: Assert: controller.[[closeRequested]] is false.
  MOZ_ASSERT(!unwrappedController->closeRequested());

  // Step 3: Assert: stream.[[state]] is "readable".
  MOZ_ASSERT(unwrappedStream->readable());

  // Step 4: If controller.[[queueTotalSize]] > 0,
  if (unwrappedController->queueTotalSize() > 0) {
    // Step a: Set controller.[[closeRequested]] to true.
    unwrappedController->setCloseRequested();

    // Step b: Return.
    return true;
  }

  // Step 5: If controller.[[pendingPullIntos]] is not empty,
  Rooted<ListObject*> unwrappedPendingPullIntos(
      cx, unwrappedController->pendingPullIntos());
  if (unwrappedPendingPullIntos->length() != 0) {
    // Step a: Let firstPendingPullInto be the first element of
    //         controller.[[pendingPullIntos]].
    Rooted<PullIntoDescriptor*> unwrappedFirstPendingPullInto(
        cx, UnwrapAndDowncastObject<PullIntoDescriptor>(
                cx, &unwrappedPendingPullIntos->get(0).toObject()));
    if (!unwrappedFirstPendingPullInto) {
      return false;
    }

    // Step b: If firstPendingPullInto.[[bytesFilled]] > 0,
    if (unwrappedFirstPendingPullInto->bytesFilled() > 0) {
      // Step i: Let e be a new TypeError exception.
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr,
          JSMSG_READABLEBYTESTREAMCONTROLLER_CLOSE_PENDING_PULL);
      RootedValue e(cx);
      Rooted<SavedFrame*> stack(cx);
      if (!cx->isExceptionPending() ||
          !GetAndClearExceptionAndStack(cx, &e, &stack)) {
        // Uncatchable error. Die immediately without erroring the
        // stream.
        return false;
      }

      // Step ii: Perform ! ReadableByteStreamControllerError(controller, e).
      if (!ReadableStreamControllerError(cx, unwrappedController, e)) {
        return false;
      }

      // Step iii: Throw e.
      cx->setPendingException(e, stack);
      return false;
    }
  }

  // Step 6: Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
  ReadableStreamControllerClearAlgorithms(unwrappedController);

  // Step 7: Perform ! ReadableStreamClose(stream).
  return ReadableStreamCloseInternal(cx, unwrappedStream);
}

// Streams spec, 3.13.11. ReadableByteStreamControllerError ( controller, e )
// Unified with 3.10.7 above.

// Streams spec 3.13.14.
//      ReadableByteStreamControllerGetDesiredSize ( controller )
// Unified with 3.10.8 above.

/**
 * Streams spec, 3.13.15.
 *      ReadableByteStreamControllerHandleQueueDrain ( controller )
 */

extern bool js::ReadableByteStreamControllerHandleQueueDrain(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController) {
  // Step 1: Assert: controller.[[controlledReadableStream]].[[state]]
  //                 is "readable".
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());
  MOZ_ASSERT(unwrappedStream->readable());

  // Step 2: If controller.[[queueTotalSize]] is 0 and
  //         controller.[[closeRequested]] is true,
  if (unwrappedController->queueTotalSize() == 0 &&
      unwrappedController->closeRequested()) {
    // Step a: Perform
    //         ! ReadableByteStreamControllerClearAlgorithms(controller).
    ReadableStreamControllerClearAlgorithms(unwrappedController);

    // Step b: Perform
    //         ! ReadableStreamClose(controller.[[controlledReadableStream]]).
    return ReadableStreamCloseInternal(cx, unwrappedStream);
  }

  // Step 3: Otherwise,
  // Step a: Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
  return ReadableStreamControllerCallPullIfNeeded(cx, unwrappedController);
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-invalidate-byob-request
 */
[[nodiscard]] bool js::ReadableByteStreamControllerInvalidateBYOBRequest(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController) {
  // If controller.[[byobRequest]] is null, return.
  RootedValue unwrappedBYOBRequestVal(cx, unwrappedController->byobRequest());
  if (unwrappedBYOBRequestVal.isNullOrUndefined()) {
    return true;
  }

  Rooted<ReadableStreamBYOBRequest*> unwrappedBYOBRequest(
      cx, UnwrapAndDowncastValue<ReadableStreamBYOBRequest>(cx, unwrappedBYOBRequestVal));
  if (!unwrappedBYOBRequest) {
    return false;
  }

  // Set controller.[[byobRequest]].[[controller]] to undefined.
  unwrappedBYOBRequest->clearController();

  // Set controller.[[byobRequest]].[[view]] to null.
  unwrappedBYOBRequest->clearView();

  // Set controller.[[byobRequest]] to null.
  unwrappedController->clearByobRequest();

  return true;
}

// Streams spec, 3.13.25.
//      ReadableByteStreamControllerShouldCallPull ( controller )
// Unified with 3.10.3 above.

/**
 * https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollergetbyobrequest
 */
[[nodiscard]] static bool ReadableByteStreamController_byobRequest(
    JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableByteStreamController*> unwrappedController(
      cx, UnwrapAndTypeCheckThis<ReadableByteStreamController>(
              cx, args, "get byobRequest"));
  if (!unwrappedController) {
    return false;
  }

  Rooted<ListObject*> unwrappedPendingPullIntos(
      cx, unwrappedController->pendingPullIntos());

  // If controller.[[byobRequest]] is null and controller.[[pendingPullIntos]]
  // is not empty
  if (!unwrappedController->hasByobRequest() &&
      unwrappedPendingPullIntos->length() > 0) {
    // Let firstDescriptor be controller.[[pendingPullIntos]][0].
    Rooted<PullIntoDescriptor*> firstDescriptor(
        cx, UnwrapAndDowncastObject<PullIntoDescriptor>(
                cx, &unwrappedPendingPullIntos->get(0).toObject()));

    // Let view be ! Construct(%Uint8Array%, « firstDescriptor’s buffer,
    // firstDescriptor’s byte offset + firstDescriptor’s bytes filled,
    // firstDescriptor’s byte length − firstDescriptor’s bytes filled »).
    RootedObject buffer(cx, firstDescriptor->buffer());
    if (!cx->compartment()->wrap(cx, &buffer)) {
      return false;
    }

    uint32_t byteOffset =
        firstDescriptor->byteOffset() + firstDescriptor->bytesFilled();
    uint32_t byteLength =
        firstDescriptor->byteLength() - firstDescriptor->bytesFilled();

    RootedObject viewObj(
        cx, JS_NewUint8ArrayWithBuffer(cx, buffer, byteOffset, byteLength));
    if (!viewObj) {
      return false;
    }

    // Let byobRequest be a new ReadableStreamBYOBRequest.
    Rooted<ReadableStreamBYOBRequest*> byobRequest(
        cx, NewBuiltinClassInstance<ReadableStreamBYOBRequest>(cx));

    if (!byobRequest) {
      return false;
    }

    // Set byobRequest.[[controller]] to controller.
    byobRequest->setController(unwrappedController);

    // Set byobRequest.[[view]] to view.
    RootedValue view(cx);
    view.setObject(*viewObj);
    byobRequest->setView(view);

    // Set controller.[[byobRequest]] to byobRequest.
    unwrappedController->setByobRequest(byobRequest);
  }

  args.rval().set(unwrappedController->byobRequest());
  return true;
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-get-desired-size
 */
static bool ReadableByteStreamController_desiredSize(JSContext* cx,
                                                     unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableByteStreamController*> unwrappedController(
      cx, UnwrapAndTypeCheckThis<ReadableByteStreamController>(
              cx, args, "get desiredSize"));
  if (!unwrappedController) {
    return false;
  }

  // Let state be controller.[[stream]].[[state]].
  ReadableStream* unwrappedStream = unwrappedController->stream();

  // If state is "errored", return null.
  if (unwrappedStream->errored()) {
    args.rval().setNull();
    return true;
  }

  // If state is "closed", return 0.
  if (unwrappedStream->closed()) {
    args.rval().setInt32(0);
    return true;
  }

  // Return controller.[[strategyHWM]] − controller.[[queueTotalSize]].
  args.rval().setNumber(unwrappedController->strategyHWM() -
                        unwrappedController->queueTotalSize());
  return true;
}

/**
 * Streams spec, 3.9.4.2 close()
 */
static bool ReadableByteStreamController_close(JSContext* cx, unsigned argc,
                                               Value* vp) {
  // Step 1: If ! IsReadableByteStreamController(this) is false, throw a
  //         TypeError exception.
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableByteStreamController*> unwrappedController(
      cx,
      UnwrapAndTypeCheckThis<ReadableByteStreamController>(cx, args, "close"));
  if (!unwrappedController) {
    return false;
  }

  // Step 2: If ! ReadableByteStreamControllerCanCloseOrEnqueue(this) is
  //         false, throw a TypeError exception.
  if (!js::CheckReadableStreamControllerCanCloseOrEnqueue(
          cx, unwrappedController, "close")) {
    return false;
  }

  // Step 3: Perform ! ReadableByteStreamControllerClose(this).
  if (!ReadableByteStreamControllerClose(cx, unwrappedController)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * https://streams.spec.whatwg.org/#rbs-controller-enqueue
 */
static bool ReadableByteStreamController_enqueue(JSContext* cx, unsigned argc,
                                                 Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableByteStreamController*> unwrappedController(
      cx, UnwrapAndTypeCheckThis<ReadableByteStreamController>(cx, args,
                                                               "enqueue"));
  if (!unwrappedController) {
    return false;
  }

  if (!args.get(0).isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLEBYTESTREAMCONTROLLER_NO_VIEW,
                              "enqueue");
    return false;
  }

  Rooted<JSObject*> chunkObj(cx, &args.get(0).toObject());

  if (!JS_IsArrayBufferViewObject(chunkObj)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLEBYTESTREAMCONTROLLER_NO_VIEW,
                              "enqueue");
    return false;
  }

  // If chunk.[[ByteLength]] is 0, throw a TypeError exception.
  if (JS_GetArrayBufferViewByteLength(chunkObj) == 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLEBYTESTREAMCONTROLLER_EMPTY_VIEW);
    return false;
  }
  // If chunk.[[ViewedArrayBuffer]].[[ArrayBufferByteLength]] is 0, throw a
  // TypeError exception.
  bool isShared;
  JS::Rooted<JSObject*> buffer(
      cx, JS_GetArrayBufferViewBuffer(cx, chunkObj, &isShared));
  if (!buffer ||
      buffer->maybeUnwrapAs<js::ArrayBufferObject>()->byteLength() == 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLEBYTESTREAMCONTROLLER_EMPTY_VIEW);
    return false;
  }

  // If this.[[closeRequested]] is true, throw a TypeError exception.
  // If this.[[stream]].[[state]] is not "readable", throw a TypeError
  // exception.
  if (!js::CheckReadableStreamControllerCanCloseOrEnqueue(
          cx, unwrappedController, "enqueue")) {
    return false;
  }

  // Return ? ReadableByteStreamControllerEnqueue(this, chunk).
  if (!ReadableByteStreamControllerEnqueue(cx, unwrappedController, chunkObj)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

/**
 * Streams spec, 3.9.4.4. error ( e )
 */
static bool ReadableByteStreamController_error(JSContext* cx, unsigned argc,
                                               Value* vp) {
  // Step 1: If ! IsReadableByteStreamController(this) is false, throw a
  //         TypeError exception.
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ReadableByteStreamController*> unwrappedController(
      cx, UnwrapAndTypeCheckThis<ReadableByteStreamController>(cx, args,
                                                               "enqueue"));
  if (!unwrappedController) {
    return false;
  }

  // Step 2: Perform ! ReadableByteStreamControllerError(this, e).
  if (!ReadableStreamControllerError(cx, unwrappedController, args.get(0))) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static const JSPropertySpec ReadableByteStreamController_properties[] = {
    JS_PSG("byobRequest", ReadableByteStreamController_byobRequest, 0),
    JS_PSG("desiredSize", ReadableByteStreamController_desiredSize, 0),
    JS_PS_END};

static const JSFunctionSpec ReadableByteStreamController_methods[] = {
    JS_FN("close", ReadableByteStreamController_close, 0, 0),
    JS_FN("enqueue", ReadableByteStreamController_enqueue, 1, 0),
    JS_FN("error", ReadableByteStreamController_error, 1, 0), JS_FS_END};

static const JSClassOps ReadableByteStreamControllerClassOps = {
    nullptr,                               // addProperty
    nullptr,                               // delProperty
    nullptr,                               // enumerate
    nullptr,                               // newEnumerate
    nullptr,                               // resolve
    nullptr,                               // mayResolve
    ReadableByteStreamControllerFinalize,  // finalize
    nullptr,                               // call
    nullptr,                               // construct
    nullptr,                               // trace
};

JS_STREAMS_CLASS_SPEC(ReadableByteStreamController, 0, SlotCount, 0,
                      JSCLASS_BACKGROUND_FINALIZE,
                      &ReadableByteStreamControllerClassOps);
