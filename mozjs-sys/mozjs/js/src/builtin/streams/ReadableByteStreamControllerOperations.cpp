/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Readable stream default controller abstract operations. */

#include "builtin/streams/ReadableByteStreamControllerOperations.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT{,_IF}

#include "jsfriendapi.h"  // js::AssertSameCompartment

#include "builtin/streams/MiscellaneousOperations.h"  // js::CanTransferArrayBuffer, js::CreateAlgorithmFromUnderlyingMethod, js::InvokeOrNoop, js::IsMaybeWrapped
#include "builtin/streams/PullIntoDescriptor.h"       // js::PullIntoDescriptor
#include "builtin/streams/QueueWithSizes.h"  // js::EnqueueValueWithSize, js::ResetQueue
#include "builtin/streams/ReadableStreamController.h"  // js::ReadableStream{,Default}Controller, js::ReadableByteStreamController, js::ReadableStreamControllerStart{,Failed}Handler
#include "builtin/streams/ReadableStreamInternals.h"  // js::ReadableStream{CloseInternal,ErrorInternal,FulfillReadOrReadIntoRequest,GetNumReadRequests}
#include "builtin/streams/ReadableStreamOperations.h"  // js::ReadableStreamTee_Pull, js::SetUpReadableByteStreamController
#include "builtin/streams/TeeState.h"  // js::TeeState
#include "js/CallAndConstruct.h"       // JS::IsCallable
#include "js/CallArgs.h"               // JS::CallArgs{,FromVp}
#include "js/Promise.h"                // JS::AddPromiseReactions
#include "js/RootingAPI.h"             // JS::Handle, JS::Rooted
#include "js/Stream.h"                 // JS::ReadableStreamUnderlyingSource
#include "js/Value.h"  // JS::{,Int32,Object}Value, JS::UndefinedHandleValue
#include "vm/Compartment.h"  // JS::Compartment
#include "vm/Interpreter.h"  // js::Call, js::GetAndClearExceptionAndStack
#include "vm/JSContext.h"    // JSContext
#include "vm/JSObject.h"     // JSObject
#include "vm/List.h"         // js::ListObject
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseResolvedWithUndefined
#include "vm/Runtime.h"        // JSAtomState
#include "vm/SavedFrame.h"  // js::SavedFrame

#include "builtin/HandlerFunction-inl.h"                  // js::NewHandler
#include "builtin/streams/MiscellaneousOperations-inl.h"  // js::PromiseCall
#include "builtin/streams/ReadableStreamReader-inl.h"  // js::UnwrapReaderFromStreamNoThrow
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap, js::UnwrapCalleeSlot
#include "vm/JSContext-inl.h"    // JSContext::check
#include "vm/JSObject-inl.h"     // js::IsCallable, js::NewBuiltinClassInstance
#include "vm/List-inl.h"         // js::ListObject, js::StoreNewListInFixedSlot
#include "vm/Realm-inl.h"        // js::AutoRealm

using js::ByteStreamChunk;
using js::CanTransferArrayBuffer;
using js::PullIntoDescriptor;
using js::ReadableByteStreamController;
using js::ReadableByteStreamControllerInvalidateBYOBRequest;
using js::ReadableStream;
using js::ReadableStreamController;
using js::ReadableStreamControllerCallPullIfNeeded;
using js::ReadableStreamControllerError;
using js::ReadableStreamGetNumReadRequests;
using js::ReaderType;
using js::SavedFrame;
using js::UnwrapCalleeSlot;
using js::UnwrapReaderFromStreamNoThrow;

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Handle;
using JS::Rooted;
using JS::RootedValueArray;
using JS::UndefinedHandleValue;
using JS::Value;

extern bool js::ReadableByteStreamControllerEnqueueClonedChunkToQueue(
    JSContext* cx, JS::Handle<ReadableByteStreamController*> controller,
    JS::Handle<JSObject*> buffer, uint32_t byteOffset, uint32_t byteLength) {
  // Let cloneResult be CloneArrayBuffer(buffer, byteOffset, byteLength,
  // %ArrayBuffer%).
  JS::Rooted<JSObject*> cloneResult(
      cx, JS::ArrayBufferClone(cx, buffer, byteOffset, byteLength));

  // If cloneResult is an abrupt completion,
  if (!cloneResult) {
    Rooted<Value> exn(cx);
    Rooted<SavedFrame*> stack(cx);
    if (!cx->isExceptionPending() ||
        !GetAndClearExceptionAndStack(cx, &exn, &stack)) {
      return false;
    }
    // Perform ! ReadableByteStreamControllerError(controller,
    // cloneResult.[[Value]]).
    if (!ReadableStreamControllerError(cx, controller, exn)) {
      return false;
    }

    return false;
  }

  // Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller,
  // cloneResult.[[Value]], 0, byteLength).
  if (!ReadableByteStreamControllerEnqueueChunkToQueue(
          cx, controller, cloneResult, 0, byteLength)) {
    return false;
  }
  return true;
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue-chunk-to-queue
 */
extern bool js::ReadableByteStreamControllerEnqueueChunkToQueue(
    JSContext* cx, JS::Handle<ReadableByteStreamController*> controller,
    JS::Handle<JSObject*> buffer, uint32_t byteOffset, uint32_t byteLength) {
  MOZ_ASSERT(controller);
  MOZ_ASSERT(buffer);
  // Append a new readable byte stream queue entry with buffer buffer, byte
  // offset byteOffset, and byte length byteLength to controller.[[queue]].
  Rooted<Value> chunkVal(cx);
  chunkVal.setObject(
      *ByteStreamChunk::create(cx, buffer, byteOffset, byteLength));

  if (!controller->queue()->append(cx, chunkVal)) {
    return false;
  }

  // Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] +
  // byteLength.
  controller->setQueueTotalSize(controller->queueTotalSize() + byteLength);
  return true;
}

extern bool js::ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(
    JSContext* cx, JS::Handle<ReadableByteStreamController*> controller,
    JS::Handle<PullIntoDescriptor*> pullIntoDescriptor) {
  // Assert: pullIntoDescriptor’s reader type is "none".
  MOZ_ASSERT(pullIntoDescriptor->readerType() == ReaderType::None);

  // If pullIntoDescriptor’s bytes filled > 0, perform ?
  // ReadableByteStreamControllerEnqueueClonedChunkToQueue(controller,
  // pullIntoDescriptor’s buffer, pullIntoDescriptor’s byte offset,
  // pullIntoDescriptor’s bytes filled).
  Rooted<JSObject*> bufferObj(cx, pullIntoDescriptor->buffer());
  if (pullIntoDescriptor->bytesFilled() > 0) {
    if (!ReadableByteStreamControllerEnqueueClonedChunkToQueue(
            cx, controller, bufferObj, pullIntoDescriptor->byteOffset(),
            pullIntoDescriptor->bytesFilled())) {
      return false;
    }
  }

  // Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
  ReadableByteStreamControllerShiftPendingPullInto(cx, controller);
  return true;
}

/*
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-in-readable-state
 */
extern bool js::ReadableByteStreamControllerRespondInReadableState(
    JSContext* cx, JS::Handle<ReadableByteStreamController*> controller,
    uint64_t bytesWritten, JS::Handle<PullIntoDescriptor*> pullIntoDescriptor) {
  // Assert: pullIntoDescriptor’s bytes filled + bytesWritten ≤
  // pullIntoDescriptor’s byte length.
  MOZ_ASSERT(pullIntoDescriptor->bytesFilled() + bytesWritten <=
             pullIntoDescriptor->byteLength());

  // Perform !
  // ReadableByteStreamControllerFillHeadPullIntoDescriptor(controller,
  // bytesWritten, pullIntoDescriptor). Assert: either
  // controller.[[pendingPullIntos]] is empty, or
  // controller.[[pendingPullIntos]][0] is pullIntoDescriptor.
  MOZ_ASSERT(controller->pendingPullIntos()->length() == 0 ||
             &controller->pendingPullIntos()->get(0).toObject() ==
                 &pullIntoDescriptor.get()->as<JSObject>());

  // Assert: controller.[[byobRequest]] is null.
  MOZ_ASSERT(!controller->hasByobRequest());

  // Set pullIntoDescriptor’s bytes filled to bytes filled + size.
  pullIntoDescriptor->setBytesFilled(pullIntoDescriptor->bytesFilled() +
                                     bytesWritten);

  // If pullIntoDescriptor’s reader type is "none",
  if (pullIntoDescriptor->readerType() == ReaderType::None) {
    // Perform ?
    // ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(controller,
    // pullIntoDescriptor).
    if (!ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(
            cx, controller, pullIntoDescriptor)) {
      return false;
    }

    // Perform !
    // ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller).
    if (!ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(
            cx, controller)) {
      return false;
    }

    // Return.
    return true;
  }

  // If pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s element size,
  // return.
  if (pullIntoDescriptor->bytesFilled() < pullIntoDescriptor->elementSize()) {
    return true;
  }

  // Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
  MOZ_ASSERT(!controller->hasByobRequest());
  Rooted<PullIntoDescriptor*> firstDescriptor(
      cx, UnwrapAndDowncastObject<PullIntoDescriptor>(
              cx, &controller->pendingPullIntos()->popFirst(cx).toObject()));

  // Let remainderSize be pullIntoDescriptor’s bytes filled mod
  // pullIntoDescriptor’s element size.
  uint32_t remainderSize =
      pullIntoDescriptor->bytesFilled() % pullIntoDescriptor->elementSize();

  // If remainderSize > 0,
  if (remainderSize > 0) {
    // Let end be pullIntoDescriptor’s byte offset + pullIntoDescriptor’s bytes
    // filled.
    uint32_t end =
        pullIntoDescriptor->byteOffset() + pullIntoDescriptor->bytesFilled();

    // Perform ?
    // ReadableByteStreamControllerEnqueueClonedChunkToQueue(controller,
    // pullIntoDescriptor’s buffer, end − remainderSize, remainderSize).
    Rooted<JSObject*> bufferObj(cx, pullIntoDescriptor->buffer());
    if (!ReadableByteStreamControllerEnqueueClonedChunkToQueue(
            cx, controller, bufferObj, end - remainderSize, remainderSize)) {
      return false;
    }
  }

  // Set pullIntoDescriptor’s bytes filled to pullIntoDescriptor’s bytes filled
  // − remainderSize.
  pullIntoDescriptor->setBytesFilled(pullIntoDescriptor->bytesFilled() -
                                     remainderSize);

  // Perform !
  // ReadableByteStreamControllerCommitPullIntoDescriptor(controller.[[stream]],
  // pullIntoDescriptor).
  Rooted<ReadableStream*> stream(cx, controller->stream());
  if (!ReadableByteStreamControllerCommitPullIntoDescriptor(
          cx, stream, pullIntoDescriptor)) {
    return false;
  }

  // Perform !
  // ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller).
  if (!ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(
          cx, controller)) {
    return false;
  }

  return true;
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-process-pull-into-descriptors-using-queue
 */
extern bool
js::ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController) {
  // Assert: controller.[[closeRequested]] is false.
  MOZ_ASSERT(unwrappedController->closeRequested() == false);

  Rooted<ListObject*> unwrappedPendingPullIntos(
      cx, unwrappedController->pendingPullIntos());

  // While controller.[[pendingPullIntos]] is not empty,
  while (unwrappedPendingPullIntos->length() > 0) {
    // If controller.[[queueTotalSize]] is 0, return.
    if (unwrappedController->queueTotalSize() == 0) return true;

    // Let pullIntoDescriptor be controller.[[pendingPullIntos]][0].
    Rooted<PullIntoDescriptor*> pullIntoDescriptor(
        cx, UnwrapAndDowncastObject<PullIntoDescriptor>(
                cx, &unwrappedPendingPullIntos->get(0).toObject()));

    // If !
    // ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(controller,
    // pullIntoDescriptor) is true,
    bool ready;
    if (!ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(
            cx, unwrappedController, pullIntoDescriptor, &ready)) {
      return false;
    }
    if (ready) {
      // Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
      ReadableByteStreamControllerShiftPendingPullInto(cx, unwrappedController);

      // Perform !
      // ReadableByteStreamControllerCommitPullIntoDescriptor(controller.[[stream]],
      // pullIntoDescriptor).
      Rooted<ReadableStream*> stream(cx, unwrappedController->stream());
      if (!ReadableByteStreamControllerCommitPullIntoDescriptor(
              cx, stream, pullIntoDescriptor)) {
        return false;
      }
    }
  }
  return true;
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-pull-into-descriptor-from-queue
 */
extern bool js::ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController,
    JS::Handle<PullIntoDescriptor*> pullIntoDescriptor, bool* ready) {
  // Let elementSize be pullIntoDescriptor.[[elementSize]].
  uint32_t elementSize = pullIntoDescriptor->elementSize();

  // Let currentAlignedBytes be pullIntoDescriptor’s bytes filled −
  // (pullIntoDescriptor’s bytes filled mod elementSize).
  uint32_t currentAlignedBytes =
      pullIntoDescriptor->bytesFilled() -
      (pullIntoDescriptor->bytesFilled() % elementSize);

  // Let maxBytesToCopy be min(controller.[[queueTotalSize]],
  // pullIntoDescriptor’s byte length − pullIntoDescriptor’s bytes filled).
  uint32_t maxBytesToCopy =
      pullIntoDescriptor->byteLength() - pullIntoDescriptor->bytesFilled();
  if (unwrappedController->queueTotalSize() < maxBytesToCopy) {
    maxBytesToCopy = unwrappedController->queueTotalSize();
  }

  // Let maxBytesFilled be pullIntoDescriptor’s bytes filled + maxBytesToCopy.
  uint32_t maxBytesFilled = pullIntoDescriptor->bytesFilled() + maxBytesToCopy;

  // Let maxAlignedBytes be maxBytesFilled − (maxBytesFilled mod elementSize).
  uint32_t maxAlignedBytes = maxBytesFilled - (maxBytesFilled % elementSize);

  // Let totalBytesToCopyRemaining be maxBytesToCopy.
  uint32_t totalBytesToCopyRemaining = maxBytesToCopy;

  // Let ready be false.
  *ready = false;

  // If maxAlignedBytes > currentAlignedBytes,
  if (maxAlignedBytes > currentAlignedBytes) {
    // Set totalBytesToCopyRemaining to maxAlignedBytes − pullIntoDescriptor’s
    // bytes filled.
    totalBytesToCopyRemaining =
        maxAlignedBytes - pullIntoDescriptor->bytesFilled();

    // Set ready to true.
    *ready = true;
  }

  // Let queue be controller.[[queue]].
  ListObject* queue = unwrappedController->queue();

  // While totalBytesToCopyRemaining > 0,
  while (totalBytesToCopyRemaining > 0) {
    // Let headOfQueue be queue[0].
    Rooted<ByteStreamChunk*> headOfQueue(
        cx, UnwrapAndDowncastObject<ByteStreamChunk>(
                cx, &queue->get(0).toObject()));

    // Let bytesToCopy be min(totalBytesToCopyRemaining, headOfQueue’s byte
    // length).
    uint32_t bytesToCopy = totalBytesToCopyRemaining < headOfQueue->byteLength()
                               ? totalBytesToCopyRemaining
                               : headOfQueue->byteLength();

    // Let destStart be pullIntoDescriptor’s byte offset + pullIntoDescriptor’s
    // bytes filled.
    uint32_t destStart =
        pullIntoDescriptor->byteOffset() + pullIntoDescriptor->bytesFilled();

    // Perform ! CopyDataBlockBytes(pullIntoDescriptor’s
    // buffer.[[ArrayBufferData]], destStart, headOfQueue’s
    // buffer.[[ArrayBufferData]], headOfQueue’s byte offset, bytesToCopy).
    // CopyDataBlockBytes(pullIntoDescriptor->buffer(), destStart,
    // headOfQueue->buffer(), headOfQueue->byteOffset(), bytesToCopy);
    JS::Rooted<JSObject*> descriptorBuffer(cx, pullIntoDescriptor->buffer());
    JS::Rooted<JSObject*> queueBuffer(cx, headOfQueue->buffer());
    if (!JS::ArrayBufferCopyData(cx, descriptorBuffer, destStart, queueBuffer,
                                 headOfQueue->byteOffset(), bytesToCopy)) {
      return false;
    }

    // If headOfQueue’s byte length is bytesToCopy,
    if (headOfQueue->byteLength() == bytesToCopy) {
      // Remove queue[0].
      queue->popFirst(cx);
    } else {
      // Otherwise,

      // Set headOfQueue’s byte offset to headOfQueue’s byte offset +
      // bytesToCopy.
      headOfQueue->setByteOffset(headOfQueue->byteOffset() + bytesToCopy);

      // Set headOfQueue’s byte length to headOfQueue’s byte length −
      // bytesToCopy.
      headOfQueue->setByteLength(headOfQueue->byteLength() - bytesToCopy);
    }

    // Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] −
    // bytesToCopy.
    unwrappedController->setQueueTotalSize(
        unwrappedController->queueTotalSize() - bytesToCopy);

    // Perform !
    // ReadableByteStreamControllerFillHeadPullIntoDescriptor(controller,
    // bytesToCopy, pullIntoDescriptor).
    ReadableByteStreamControllerFillHeadPullIntoDescriptor(
        cx, unwrappedController, bytesToCopy, pullIntoDescriptor);

    // Set totalBytesToCopyRemaining to totalBytesToCopyRemaining − bytesToCopy.
    totalBytesToCopyRemaining -= bytesToCopy;
  }

  // If ready is false,
  if (*ready == false) {
    // Assert: controller.[[queueTotalSize]] is 0.
    MOZ_ASSERT(unwrappedController->queueTotalSize() == 0);

    // Assert: pullIntoDescriptor’s bytes filled > 0.
    MOZ_ASSERT(pullIntoDescriptor->bytesFilled() > 0);

    // Assert: pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s element
    // size.
    MOZ_ASSERT(pullIntoDescriptor->bytesFilled() <
               pullIntoDescriptor->elementSize());
  }

  // Return ready.
  return true;
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-head-pull-into-descriptor
 */
extern void js::ReadableByteStreamControllerFillHeadPullIntoDescriptor(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController,
    uint32_t size, JS::Handle<PullIntoDescriptor*> pullIntoDescriptor) {
  // Assert: either controller.[[pendingPullIntos]] is empty, or
  // controller.[[pendingPullIntos]][0] is pullIntoDescriptor.
  MOZ_ASSERT(unwrappedController->pendingPullIntos()->length() == 0 ||
             &unwrappedController->pendingPullIntos()->get(0).toObject() ==
                 &pullIntoDescriptor.get()->as<JSObject>());

  // Assert: controller.[[byobRequest]] is null.
  MOZ_ASSERT(!unwrappedController->hasByobRequest());

  // Set pullIntoDescriptor’s bytes filled to bytes filled + size.
  pullIntoDescriptor->setBytesFilled(pullIntoDescriptor->bytesFilled() + size);
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-shift-pending-pull-into
 */
extern PullIntoDescriptor* js::ReadableByteStreamControllerShiftPendingPullInto(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController) {
  // Assert: controller.[[byobRequest]] is null.
  MOZ_ASSERT(!unwrappedController->hasByobRequest());

  // Let descriptor be controller.[[pendingPullIntos]][0].
  // Remove descriptor from controller.[[pendingPullIntos]].
  // Return descriptor.
  Rooted<ListObject*> unwrappedPendingPullIntos(
      cx, unwrappedController->pendingPullIntos());

  return UnwrapAndDowncastObject<PullIntoDescriptor>(
      cx, &unwrappedPendingPullIntos->popFirst(cx).toObject());
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-commit-pull-into-descriptor
 */
extern bool js::ReadableByteStreamControllerCommitPullIntoDescriptor(
    JSContext* cx, JS::Handle<ReadableStream*> stream,
    JS::Handle<PullIntoDescriptor*> pullIntoDescriptor) {
  // Assert: stream.[[state]] is not "errored".
  MOZ_ASSERT(!stream->errored());

  // Assert: pullIntoDescriptor.reader type is not "none".
  ReaderType readerType = pullIntoDescriptor->readerType();
  MOZ_ASSERT(readerType != ReaderType::None);

  // Let done be false.
  bool done = false;

  // If stream.[[state]] is "closed",
  if (stream->closed()) {
    // Assert: pullIntoDescriptor’s bytes filled is 0.
    MOZ_ASSERT(pullIntoDescriptor->bytesFilled() == 0);

    // Set done to true.
    done = true;
  }

  // Let filledView be !
  // ReadableByteStreamControllerConvertPullIntoDescriptor(pullIntoDescriptor).
  Rooted<JSObject*> filledView(
      cx, ReadableByteStreamControllerConvertPullIntoDescriptor(
              cx, pullIntoDescriptor));
  if (!filledView) {
    return false;
  }

  Rooted<Value> filledViewVal(cx);
  filledViewVal.setObject(*filledView.get());

  // If pullIntoDescriptor’s reader type is "default",
  if (readerType == ReaderType::Default) {
    // Perform ! ReadableStreamFulfillReadRequest(stream, filledView, done).
    if (!ReadableStreamFulfillReadOrReadIntoRequest(cx, stream, filledViewVal,
                                                    done)) {
      return false;
    }
  } else {
    // Otherwise,
    // Assert: pullIntoDescriptor’s reader type is "byob".
    MOZ_ASSERT(readerType == ReaderType::BYOB);
    // Perform ! ReadableStreamFulfillReadIntoRequest(stream, filledView, done).
    if (!ReadableStreamFulfillReadOrReadIntoRequest(cx, stream, filledViewVal,
                                                    done)) {
      return false;
    }
  }
  return true;
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-with-new-view
 */
extern bool js::ReadableByteStreamControllerRespondWithNewView(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController,
    JS::Handle<JSObject*> view) {
  // Assert: controller.[[pendingPullIntos]] is not empty.
  MOZ_ASSERT(unwrappedController->pendingPullIntos()->length() > 0);

  // Assert: ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is false.

  // Let firstDescriptor be controller.[[pendingPullIntos]][0].
  Rooted<PullIntoDescriptor*> firstDescriptor(
      cx, UnwrapAndDowncastObject<PullIntoDescriptor>(
              cx, &unwrappedController->pendingPullIntos()->get(0).toObject()));

  // Let state be controller.[[stream]].[[state]].
  // If state is "closed",
  if (unwrappedController->stream()->closed()) {
    // If view.[[ByteLength]] is not 0, throw a TypeError exception.
    if (JS_GetArrayBufferViewByteLength(view) != 0) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_CLOSED);
      return false;
    }
  } else {
    // Otherwise,

    // Assert: state is "readable".
    MOZ_ASSERT(unwrappedController->stream()->readable());

    // If view.[[ByteLength]] is 0, throw a TypeError exception.
    if (JS_GetArrayBufferViewByteLength(view) == 0) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_ZERO);
      return false;
    }
  }

  // Let viewByteLength be view.[[ByteLength]].
  uint32_t viewByteLength = JS_GetArrayBufferViewByteLength(view);

  // If firstDescriptor’s byte offset + firstDescriptor’ bytes filled is not
  // view.[[ByteOffset]], throw a RangeError exception.
  if (firstDescriptor->byteOffset() + firstDescriptor->bytesFilled() !=
      JS_GetArrayBufferViewByteOffset(view)) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr,
        JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_BAD_OFFSET);
    return false;
  }

  bool isShared;
  Rooted<JSObject*> viewedArrayBuffer(
      cx, JS_GetArrayBufferViewBuffer(cx, view, &isShared));

  uint8_t* data;
  bool is_shared;
  size_t len = 0;

  JS::GetArrayBufferLengthAndData(viewedArrayBuffer, &len, &is_shared, &data);

  // If firstDescriptor’s buffer byte length is not
  // view.[[ViewedArrayBuffer]].[[ByteLength]], throw a RangeError exception.
  if (JS_IsArrayBufferViewObject(firstDescriptor->buffer())) {
    if (JS_GetArrayBufferViewByteLength(firstDescriptor->buffer()) != len) {
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr,
          JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_BAD_LEN);
      return false;
    }
  } else {
    size_t len2 = 0;
    JS::GetArrayBufferLengthAndData(firstDescriptor->buffer(), &len2,
                                    &is_shared, &data);
    if (len2 != 0 && len2 != len) {
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr,
          JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_BAD_LEN);
      return false;
    }
  }

  // If firstDescriptor’s bytes filled + view.[[ByteLength]] > firstDescriptor’s
  // byte length, throw a RangeError exception.
  if (firstDescriptor->bytesFilled() + viewByteLength >
      firstDescriptor->byteLength()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_BAD_LEN);
    return false;
  }

  // Set firstDescriptor’s buffer to ?
  // TransferArrayBuffer(view.[[ViewedArrayBuffer]]).
  firstDescriptor->setBuffer(TransferArrayBuffer(cx, viewedArrayBuffer));

  // Perform ? ReadableByteStreamControllerRespondInternal(controller,
  // viewByteLength).
  return ReadableByteStreamControllerRespondInternal(cx, unwrappedController,
                                                     viewByteLength);
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-convert-pull-into-descriptor
 */
extern JSObject* js::ReadableByteStreamControllerConvertPullIntoDescriptor(
    JSContext* cx, JS::Handle<PullIntoDescriptor*> pullIntoDescriptor) {
  // Let bytesFilled be pullIntoDescriptor’s bytes filled.
  uint32_t bytesFilled = pullIntoDescriptor->bytesFilled();

  // Let elementSize be pullIntoDescriptor’s element size.
  uint32_t elementSize = pullIntoDescriptor->elementSize();

  // Assert: bytesFilled ≤ pullIntoDescriptor’s byte length.
  MOZ_ASSERT(bytesFilled <= pullIntoDescriptor->byteLength());

  // Assert: bytesFilled mod elementSize is 0.
  MOZ_ASSERT(bytesFilled % elementSize == 0);

  JS::Rooted<JSObject*> buffer(cx, pullIntoDescriptor->buffer());

  // Let buffer be ! TransferArrayBuffer(pullIntoDescriptor’s buffer).
  JS::Rooted<JSObject*> transferredBuffer(cx, TransferArrayBuffer(cx, buffer));

  // Return ! Construct(pullIntoDescriptor’s view constructor, « buffer,
  // pullIntoDescriptor’s byte offset, bytesFilled ÷ elementSize »).
  JS::RootedValueArray<3> args(cx);
  args[0].setObject(*transferredBuffer);
  args[1].setInt32(pullIntoDescriptor->byteOffset());
  args[2].setDouble(bytesFilled == 0 ? 0 : bytesFilled / elementSize);

  Rooted<Value> ctor(cx, pullIntoDescriptor->ctor());

  Rooted<JSString*> str(cx, JS::ToString(cx, ctor));
  JS::UniqueChars specChars = JS_EncodeStringToUTF8(cx, str);

  JS::Rooted<JSObject*> obj(cx);

  if (!JS::Construct(cx, ctor, args, &obj)) {
    return nullptr;
  }
  if (!obj) {
    return nullptr;
  }
  return obj;
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-should-call-pull
 */
[[nodiscard]] bool ReadableByteStreamControllerShouldCallPull(
    JSContext* cx, ReadableByteStreamController* unwrappedController) {
  // Let stream be controller.[[stream]].
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());

  // If stream.[[state]] is not "readable", return false.
  if (!unwrappedStream->readable()) {
    return false;
  }

  // If controller.[[closeRequested]] is true, return false.
  if (unwrappedController->closeRequested()) {
    return false;
  }

  // If controller.[[started]] is false, return false.
  if (!unwrappedController->started()) {
    return false;
  }

  // If ! ReadableStreamHasDefaultReader(stream) is true and !
  // ReadableStreamGetNumReadRequests(stream) > 0, return true.
  bool hasDefaultReader;
  if (!ReadableStreamHasDefaultReader(cx, unwrappedStream, &hasDefaultReader)) {
    abort();
  }
  if (hasDefaultReader &&
      ReadableStreamGetNumReadRequests(unwrappedStream) > 0) {
    return true;
  }

  // If ! ReadableStreamHasBYOBReader(stream) is true and !
  // ReadableStreamGetNumReadIntoRequests(stream) > 0, return true.
  bool hasBYOBReader;
  if (!ReadableStreamHasBYOBReader(cx, unwrappedStream, &hasBYOBReader)) {
    abort();
  }
  if (hasBYOBReader && ReadableStreamGetNumReadRequests(unwrappedStream) > 0) {
    return true;
  }

  // Let desiredSize be !
  // ReadableByteStreamControllerGetDesiredSize(controller).
  double desiredSize =
      ReadableStreamControllerGetDesiredSizeUnchecked(unwrappedController);

  // Assert: desiredSize is not null. (implicit).
  // If desiredSize > 0, return true.
  // Return false.
  return desiredSize > 0;
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-in-closed-state
 */
extern bool js::ReadableByteStreamControllerRespondInClosedState(
    JSContext* cx, JS::Handle<ReadableByteStreamController*> controller,
    Handle<PullIntoDescriptor*> firstDescriptor) {
  // Step 1. Assert: firstDescriptor ’s bytes filled is 0.
  MOZ_ASSERT(firstDescriptor->bytesFilled() == 0);

  // Step 2. If firstDescriptor’s reader type is "none",
  // perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
  if (firstDescriptor->readerType() == ReaderType::None) {
    ReadableByteStreamControllerShiftPendingPullInto(cx, controller);
  }

  // Step 3. Let stream be controller.[[stream]].
  Rooted<ReadableStream*> stream(cx, controller->stream());

  // Step 4. If ! ReadableStreamHasBYOBReader(stream) is true,
  bool result;
  if (!ReadableStreamHasBYOBReader(cx, stream, &result)) {
    return false;
  }
  if (result) {
    // Step 4.1. While ! ReadableStreamGetNumReadIntoRequests(stream) > 0,
    while (ReadableStreamGetNumReadRequests(stream) > 0) {
      // Step 4.1.1. Let pullIntoDescriptor be !
      // ReadableByteStreamControllerShiftPendingPullInto(controller).
      Rooted<PullIntoDescriptor*> pullIntoDescriptor(
          cx, ReadableByteStreamControllerShiftPendingPullInto(cx, controller));

      // Step 4.1.2. Perform !
      // ReadableByteStreamControllerCommitPullIntoDescriptor(stream,
      // pullIntoDescriptor).
      if (!ReadableByteStreamControllerCommitPullIntoDescriptor(
              cx, stream, pullIntoDescriptor)) {
        return false;
      }
    }
  }
  return true;
}

/*
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-internal
 */
[[nodiscard]] bool js::ReadableByteStreamControllerRespondInternal(
    JSContext* cx, Handle<ReadableByteStreamController*> controller,
    uint64_t bytesWritten) {
  // Let firstDescriptor be controller.[[pendingPullIntos]][0].
  Rooted<ListObject*> unwrappedPendingPullIntos(cx,
                                                controller->pendingPullIntos());
  Rooted<PullIntoDescriptor*> firstDescriptor(
      cx, UnwrapAndDowncastObject<PullIntoDescriptor>(
              cx, &unwrappedPendingPullIntos->get(0).toObject()));

  // Assert: ! CanTransferArrayBuffer(firstDescriptor’s buffer) is true.
  RootedObject bufferObj(cx, firstDescriptor->buffer());
  MOZ_ASSERT(CanTransferArrayBuffer(cx, bufferObj));

  // Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
  if (!ReadableByteStreamControllerInvalidateBYOBRequest(cx, controller)) {
    return false;
  }

  // Let state be controller.[[stream]].[[state]].
  // If state is "closed",
  if (controller->stream()->closed()) {
    // Assert: bytesWritten is 0.
    MOZ_ASSERT(bytesWritten == 0);

    // Perform ! ReadableByteStreamControllerRespondInClosedState(controller,
    // firstDescriptor).
    if (!ReadableByteStreamControllerRespondInClosedState(cx, controller,
                                                          firstDescriptor)) {
      return false;
    }
    // Otherwise,
  } else {
    // Assert: state is "readable".
    MOZ_ASSERT(controller->stream()->readable());

    // Assert: bytesWritten > 0.
    MOZ_ASSERT(bytesWritten > 0);

    // Perform ? ReadableByteStreamControllerRespondInReadableState(controller,
    // bytesWritten, firstDescriptor).
    if (!ReadableByteStreamControllerRespondInReadableState(
            cx, controller, bytesWritten, firstDescriptor)) {
      return false;
    }
  }

  // Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
  if (!ReadableStreamControllerCallPullIfNeeded(cx, controller)) {
    return false;
  }

  return true;
}

/**
 * https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue
 */
[[nodiscard]] bool js::ReadableByteStreamControllerEnqueue(
    JSContext* cx, Handle<ReadableByteStreamController*> unwrappedController,
    Handle<JSObject*> chunkObj) {
  // Let stream be controller.[[stream]].
  Rooted<ReadableStream*> unwrappedStream(cx, unwrappedController->stream());

  // If controller.[[closeRequested]] is true or stream.[[state]] is not
  // "readable", return.
  if (unwrappedController->closeRequested() || !unwrappedStream->readable()) {
    return true;
  }

  // Let buffer be chunk.[[ViewedArrayBuffer]].
  bool isShared;
  JS::Rooted<JSObject*> buffer(
      cx, JS_GetArrayBufferViewBuffer(cx, chunkObj, &isShared));

  // Let byteOffset be view.[[ByteOffset]].
  size_t byteOffset = JS_GetArrayBufferViewByteOffset(chunkObj);

  // Let byteLength be view.[[ByteLength]].
  size_t byteLength = JS_GetArrayBufferViewByteLength(chunkObj);

  // If ! IsDetachedBuffer(this.[[view]].[[ArrayBuffer]]) is true, throw a
  // TypeError exception.
  if (buffer->maybeUnwrapAs<js::ArrayBufferObject>()->isDetached()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLEBYTESTREAMCONTROLLER_DETACHED);
    return false;
  }

  // Let transferredBuffer be ? TransferArrayBuffer(buffer).
  Rooted<JSObject*> transferredBuffer(cx, TransferArrayBuffer(cx, buffer));

  // If controller.[[pendingPullIntos]] is not empty,
  if (unwrappedController->pendingPullIntos()->length() > 0) {
    // Let firstPendingPullInto be controller.[[pendingPullIntos]][0].
    Rooted<PullIntoDescriptor*> firstPendingPullInto(
        cx,
        UnwrapAndDowncastObject<PullIntoDescriptor>(
            cx, &unwrappedController->pendingPullIntos()->get(0).toObject()));

    Rooted<JSObject*> buffer(cx, firstPendingPullInto->buffer());

    // If ! IsDetachedBuffer(firstPendingPullInto’s buffer) is true, throw a
    // TypeError exception.
    if (buffer->maybeUnwrapAs<js::ArrayBufferObject>()->isDetached()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_READABLEBYTESTREAMCONTROLLER_DETACHED);
      return false;
    }

    // Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
    if (!ReadableByteStreamControllerInvalidateBYOBRequest(
            cx, unwrappedController)) {
      return false;
    }

    // Set firstPendingPullInto’s buffer to !
    // TransferArrayBuffer(firstPendingPullInto’s buffer). firstPendingPullInto
    firstPendingPullInto->setBuffer(TransferArrayBuffer(cx, buffer));

    // If firstPendingPullInto’s reader type is "none", perform ?
    // ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(controller,
    // firstPendingPullInto).
    if (firstPendingPullInto->readerType() == ReaderType::None) {
      if (!ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(
              cx, unwrappedController, firstPendingPullInto)) {
        return false;
      }
    }
  }

  // If ! ReadableStreamHasDefaultReader(stream) is true,
  bool hasDefaultReader;
  if (!ReadableStreamHasDefaultReader(cx, unwrappedStream, &hasDefaultReader)) {
    return false;
  }
  if (hasDefaultReader) {
    // Perform !
    // ReadableByteStreamControllerProcessReadRequestsUsingQueue(controller).

    // Let reader be controller.[[stream]].[[reader]].
    ReadableStreamReader* reader =
        UnwrapReaderFromStreamNoThrow(unwrappedStream);

    // Assert: reader implements ReadableStreamDefaultReader.
    MOZ_ASSERT(reader->is<ReadableStreamDefaultReader>());

    // While reader.[[readRequests]] is not empty,
    while (reader->requests()->length() > 0) {
      uint32_t queueTotalSize = unwrappedController->queueTotalSize();

      // If controller.[[queueTotalSize]] is 0, return.
      if (queueTotalSize == 0) {
        break;
      }

      // Perform !
      // ReadableByteStreamControllerFillReadRequestFromQueue(controller,
      // readRequest).
      // (inlined here, as with other call site)

      MOZ_ASSERT(unwrappedStream->mode() !=
                 JS::ReadableStreamMode::ExternalSource);
      // Let entry be controller.[[queue]][0].
      // Remove entry from controller.[[queue]].
      Rooted<ListObject*> unwrappedQueue(cx, unwrappedController->queue());
      Rooted<ByteStreamChunk*> unwrappedEntry(
          cx, UnwrapAndDowncastObject<ByteStreamChunk>(
                  cx, &unwrappedQueue->popFirstAs<JSObject>(cx)));
      if (!unwrappedEntry) {
        return false;
      }

      // Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] −
      // entry’s byte length.
      unwrappedController->setQueueTotalSize(queueTotalSize -
                                             unwrappedEntry->byteLength());

      // Perform ! ReadableByteStreamControllerHandleQueueDrain(controller).
      if (!ReadableByteStreamControllerHandleQueueDrain(cx,
                                                        unwrappedController)) {
        return false;
      }

      // Let view be ! Construct(%Uint8Array%, « entry’s buffer, entry’s byte
      // offset, entry’s byte length »).
      RootedObject buffer(cx, unwrappedEntry->buffer());
      if (!cx->compartment()->wrap(cx, &buffer)) {
        return false;
      }
      uint32_t byteOffset = unwrappedEntry->byteOffset();
      RootedObject view(
          cx, JS_NewUint8ArrayWithBuffer(cx, buffer, byteOffset,
                                         unwrappedEntry->byteLength()));
      if (!view) {
        return false;
      }

      // Perform readRequest’s chunk steps, given view.
      RootedValue val(cx);
      val.setObject(*view);

      if (!ReadableStreamFulfillReadOrReadIntoRequest(cx, unwrappedStream, val,
                                                      false)) {
        return false;
      }
    }

    // If ! ReadableStreamGetNumReadRequests(stream) is 0,
    if (ReadableStreamGetNumReadRequests(unwrappedStream) == 0) {
      // Assert: controller.[[pendingPullIntos]] is empty.
      MOZ_ASSERT(unwrappedController->pendingPullIntos()->length() == 0);

      // Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller,
      // transferredBuffer, byteOffset, byteLength).
      if (!ReadableByteStreamControllerEnqueueChunkToQueue(
              cx, unwrappedController, transferredBuffer, byteOffset,
              byteLength)) {
        return false;
      }
    } else {
      // Otherwise,

      // Assert: controller.[[queue]] is empty.
      MOZ_ASSERT(unwrappedController->queueTotalSize() == 0);

      // If controller.[[pendingPullIntos]] is not empty,
      if (unwrappedController->pendingPullIntos()->length() > 0) {
        // Assert: controller.[[pendingPullIntos]][0]'s reader type is
        // "default".
        Rooted<PullIntoDescriptor*> firstPendingPullInto(
            cx,
            UnwrapAndDowncastObject<PullIntoDescriptor>(
                cx,
                &unwrappedController->pendingPullIntos()->get(0).toObject()));
        MOZ_ASSERT(firstPendingPullInto->readerType() == ReaderType::Default);

        // Perform !
        // ReadableByteStreamControllerShiftPendingPullInto(controller).
        ReadableByteStreamControllerShiftPendingPullInto(cx,
                                                         unwrappedController);
      }

      // Let transferredView be ! Construct(%Uint8Array%, « transferredBuffer,
      // byteOffset, byteLength »).
      Rooted<JSObject*> transferredViewObj(
          cx, JS_NewUint8ArrayWithBuffer(cx, transferredBuffer, byteOffset,
                                         byteLength));
      RootedValue transferredView(cx);
      transferredView.setObject(*transferredViewObj);

      // Perform ! ReadableStreamFulfillReadRequest(stream, transferredView,
      // false).
      if (!ReadableStreamFulfillReadOrReadIntoRequest(cx, unwrappedStream,
                                                      transferredView, false)) {
        return false;
      }
    }
  } else {
    bool hasBYOBReader;
    if (!ReadableStreamHasBYOBReader(cx, unwrappedStream, &hasBYOBReader)) {
      return false;
    }
    // Otherwise, if ! ReadableStreamHasBYOBReader(stream) is true,
    if (hasBYOBReader) {
      // Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller,
      // transferredBuffer, byteOffset, byteLength).
      if (!ReadableByteStreamControllerEnqueueChunkToQueue(
              cx, unwrappedController, transferredBuffer, byteOffset,
              byteLength)) {
        return false;
      }

      // Perform !
      // ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller).
      if (!ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(
              cx, unwrappedController)) {
        return false;
      }
    } else {
      // Otherwise,

      // Assert: ! IsReadableStreamLocked(stream) is false.
      MOZ_ASSERT(unwrappedStream->locked() == false);

      // Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller,
      // transferredBuffer, byteOffset, byteLength).
      if (!ReadableByteStreamControllerEnqueueChunkToQueue(
              cx, unwrappedController, transferredBuffer, byteOffset,
              byteLength)) {
        return false;
      }
    }
  }

  // Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
  return ReadableStreamControllerCallPullIfNeeded(cx, unwrappedController);
}

// https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller
[[nodiscard]] bool js::SetUpReadableByteStreamController(
    JSContext* cx, Handle<ReadableStream*> stream,
    SourceAlgorithms sourceAlgorithms, Handle<Value> underlyingSource,
    Handle<Value> pullMethod, Handle<Value> cancelMethod, double highWaterMark,
    uint64_t autoAllocateChunkSize) {
  MOZ_ASSERT(pullMethod.isUndefined() || IsCallable(pullMethod));
  MOZ_ASSERT(cancelMethod.isUndefined() || IsCallable(cancelMethod));
  MOZ_ASSERT_IF(sourceAlgorithms != SourceAlgorithms::Script,
                pullMethod.isUndefined());
  MOZ_ASSERT_IF(sourceAlgorithms != SourceAlgorithms::Script,
                cancelMethod.isUndefined());
  MOZ_ASSERT(highWaterMark >= 0);
  MOZ_ASSERT(!stream->hasController());

  // Done elsewhere in the standard: Create the new controller.
  Rooted<ReadableByteStreamController*> controller(
      cx, NewBuiltinClassInstance<ReadableByteStreamController>(cx));

  if (!controller) {
    return false;
  }

  controller->setStream(stream);

  controller->clearByobRequest();

  controller->setQueueTotalSize(0);

  // Set controller.[[started]], controller.[[closeRequested]],
  // controller.[[pullAgain]], and controller.[[pulling]] to false.
  controller->setFlags(0);

  controller->setStrategyHWM(highWaterMark);

  // Step 6: Set controller.[[pullAlgorithm]] to pullAlgorithm.
  // (In this implementation, the pullAlgorithm is determined by the
  // underlyingSource in combination with the pullMethod field.)
  controller->setUnderlyingSource(underlyingSource);
  controller->setPullMethod(pullMethod);
  controller->setCancelMethod(cancelMethod);

  if (autoAllocateChunkSize) {
    controller->setAutoAllocateChunkSize(autoAllocateChunkSize);
  }

  if (!StoreNewListInFixedSlot(
          cx, controller,
          ReadableByteStreamController::Slot_PendingPullIntos)) {
    return false;
  }

  if (!StoreNewListInFixedSlot(cx, controller,
                               ReadableByteStreamController::Slot_Queue)) {
    return false;
  }

  // // Step 8: Set stream.[[readableStreamController]] to controller.
  stream->setController(controller);

  Rooted<Value> startResult(cx);
  if (sourceAlgorithms == SourceAlgorithms::Script) {
    Rooted<Value> controllerVal(cx, ObjectValue(*controller));
    if (!InvokeOrNoop(cx, underlyingSource, cx->names().start, controllerVal,
                      &startResult)) {
      return false;
    }
  }

  RootedObject startPromise(cx,
                            PromiseObject::unforgeableResolve(cx, startResult));
  if (!startPromise) {
    return false;
  }

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

  // autoClear.reset();
  return true;
}

// https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller-from-underlying-source
[[nodiscard]] extern bool SetUpReadableByteStreamControllerFromUnderlyingSource(
    JSContext* cx, JS::Handle<ReadableStream*> stream,
    JS::Handle<JS::Value> underlyingSource, double highWaterMark) {
  // Step 1: Assert: underlyingSource is not undefined.
  MOZ_ASSERT(!underlyingSource.isUndefined());

  // Step 2: Let controller be ObjectCreate(the original value of
  //         ReadableByteStreamController's prototype property).
  // (Deferred to SetUpReadableByteStreamController.)

  // Step 3: Let startAlgorithm be the following steps:
  //         a. Return ? InvokeOrNoop(underlyingSource, "start",
  //                                  « controller »).
  js::SourceAlgorithms sourceAlgorithms = js::SourceAlgorithms::Script;

  // Step 4: Let pullAlgorithm be
  //         ? CreateAlgorithmFromUnderlyingMethod(underlyingSource, "pull",
  //                                               0, « controller »).
  Rooted<Value> pullMethod(cx);
  if (!CreateAlgorithmFromUnderlyingMethod(cx, underlyingSource,
                                           "ReadableStream source.pull method",
                                           cx->names().pull, &pullMethod)) {
    return false;
  }

  // Step 5. Let cancelAlgorithm be
  //         ? CreateAlgorithmFromUnderlyingMethod(underlyingSource,
  //                                               "cancel", 1, « »).
  Rooted<Value> cancelMethod(cx);
  if (!CreateAlgorithmFromUnderlyingMethod(
          cx, underlyingSource, "ReadableStream source.cancel method",
          cx->names().cancel, &cancelMethod)) {
    return false;
  }

  Rooted<Value> autoAllocateChunkSizeVal(cx);
  if (!GetProperty(cx, underlyingSource, cx->names().autoAllocateChunkSize,
                   &autoAllocateChunkSizeVal)) {
    return false;
  }

  double autoAllocateChunkSize = 0;
  if (!autoAllocateChunkSizeVal.isUndefined()) {
    MOZ_ASSERT(autoAllocateChunkSizeVal.isNumber());
    if (!js::ToInteger(cx, autoAllocateChunkSizeVal, &autoAllocateChunkSize)) {
      MOZ_ASSERT(false);
    }
    if (autoAllocateChunkSize == 0) {
      JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                                JSMSG_STREAM_AUTOALLOCATECHUNKSIZE_ZERO);
      return false;
    }
  }

  return SetUpReadableByteStreamController(
      cx, stream, sourceAlgorithms, underlyingSource, pullMethod, cancelMethod,
      highWaterMark, static_cast<uint64_t>(autoAllocateChunkSize));
}
