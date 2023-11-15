/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Readable stream default controller abstract operations. */

#ifndef builtin_streams_ReadableByteStreamControllerOperations_h
#define builtin_streams_ReadableByteStreamControllerOperations_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "builtin/streams/ReadableStreamDefaultControllerOperations.h"

#include "js/RootingAPI.h"  // JS::Handle
#include "js/Value.h"       // JS::Value

struct JS_PUBLIC_API JSContext;

namespace js {

class PullIntoDescriptor;
class ReadableStream;
class ReadableStreamController;
class ReadableByteStreamController;

[[nodiscard]] extern bool ReadableByteStreamControllerRespondInReadableState(
    JSContext* cx, JS::Handle<ReadableByteStreamController*> controller,
    uint64_t bytesWritten, JS::Handle<PullIntoDescriptor*> firstDescriptor);

[[nodiscard]] extern bool ReadableByteStreamControllerRespondInternal(
    JSContext* cx, JS::Handle<ReadableByteStreamController*> controller,
    uint64_t bytesWritten);

[[nodiscard]] extern bool ReadableByteStreamControllerEnqueue(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController,
    JS::Handle<JSObject*> chunk);

[[nodiscard]] extern bool ReadableByteStreamControllerClose(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController);

[[nodiscard]] extern bool SetUpReadableByteStreamController(
    JSContext* cx, JS::Handle<ReadableStream*> stream,
    SourceAlgorithms sourceAlgorithms, JS::Handle<JS::Value> underlyingSource,
    JS::Handle<JS::Value> pullMethod, JS::Handle<JS::Value> cancelMethod,
    double highWaterMark, uint64_t autoAllocateChunkSize);

[[nodiscard]] extern bool SetUpReadableByteStreamControllerFromUnderlyingSource(
    JSContext* cx, JS::Handle<ReadableStream*> stream,
    JS::Handle<JS::Value> underlyingSource, double highWaterMark,
    uint64_t autoAllocateChunkSize);

[[nodiscard]] extern bool ReadableByteStreamControllerCommitPullIntoDescriptor(
    JSContext* cx, JS::Handle<ReadableStream*> stream,
    JS::Handle<PullIntoDescriptor*> firstDescriptor);

[[nodiscard]] extern bool
ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController);

[[nodiscard]] extern bool
ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController,
    JS::Handle<PullIntoDescriptor*> pullIntoDescriptor, bool* ready);

extern PullIntoDescriptor* ReadableByteStreamControllerShiftPendingPullInto(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController);

[[nodiscard]] extern bool ReadableByteStreamControllerRespondWithNewView(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController,
    JS::Handle<JSObject*> view);

[[nodiscard]] extern JSObject*
ReadableByteStreamControllerConvertPullIntoDescriptor(
    JSContext* cx, JS::Handle<PullIntoDescriptor*> pullIntoDescriptor);

[[nodiscard]] extern bool ReadableByteStreamControllerRespondInClosedState(
    JSContext* cx, JS::Handle<ReadableByteStreamController*> controller,
    JS::Handle<PullIntoDescriptor*> firstDescriptor);

[[nodiscard]] extern bool ReadableByteStreamControllerEnqueueClonedChunkToQueue(
    JSContext* cx, JS::Handle<ReadableByteStreamController*> controller,
    JS::Handle<JSObject*> buffer, uint32_t byteOffset, uint32_t bytesFilled);

[[nodiscard]] extern bool
ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(
    JSContext* cx, JS::Handle<ReadableByteStreamController*> controller,
    JS::Handle<PullIntoDescriptor*> pullIntoDescriptor);

extern void ReadableByteStreamControllerFillHeadPullIntoDescriptor(
    JSContext* cx,
    JS::Handle<ReadableByteStreamController*> unwrappedController,
    uint32_t size, JS::Handle<PullIntoDescriptor*> pullIntoDescriptor);

[[nodiscard]] extern bool ReadableByteStreamControllerEnqueueChunkToQueue(
    JSContext* cx, JS::Handle<ReadableByteStreamController*> controller,
    JS::Handle<JSObject*> buffer, uint32_t byteOffset, uint32_t byteLength);

}  // namespace js

#endif  // builtin_streams_ReadableByteStreamControllerOperations_h
