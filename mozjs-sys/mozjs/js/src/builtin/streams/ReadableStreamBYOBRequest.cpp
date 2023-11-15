/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Class ReadableStreamBYOBRequest.
 */

#include "builtin/streams/ReadableStreamBYOBRequest.h"  // js::ReadableStream
#include "builtin/streams/ClassSpecMacro.h"             // JS_STREAMS_CLASS_SPEC
#include "builtin/streams/MiscellaneousOperations.h"  // js::TransferArrayBuffer
#include "builtin/streams/PullIntoDescriptor.h"       // js::PullIntoDescriptor
#include "builtin/streams/ReadableByteStreamControllerOperations.h"  // js::ReadableByteStreamControllerRespondInternal
#include "builtin/streams/ReadableStreamController.h"  // js::ReadableByteStreamController
#include "js/ErrorReport.h"             // JS_ReportErrorNumberASCII
#include "js/experimental/TypedData.h"  // JS_GetArrayBufferViewData, JS_IsUint8Array
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "vm/ArrayBufferViewObject.h"  // js::UnwrapArrayBufferView
#include "vm/PromiseObject.h"          // js::UnwrapAndTypeCheckThis

#include "builtin/streams/ReadableStreamReader-inl.h"
#include "vm/Compartment-inl.h"  // js::UnwrapAndTypeCheckThis
#include "vm/Compartment-inl.h"  // JS::Compartment::wrap, js::UnwrapAnd{DowncastObject,TypeCheckThis}
#include "vm/JSObject-inl.h"      // js::NewObjectWithClassProto
#include "vm/NativeObject-inl.h"  // js::ThrowIfNotConstructing

using JS::CallArgs;
using JS::Handle;
using JS::Rooted;
using JS::RootedObject;
using JS::RootedValue;
using JS::Value;

using js::GetErrorMessage;
using js::ListObject;
using js::NewObjectWithClassProto;
using js::PromiseObject;
using js::PullIntoDescriptor;
using js::ReadableByteStreamController;
using js::ReadableStream;
using js::ReadableStreamBYOBRequest;
using js::ReadableStreamReader;
using js::TransferArrayBuffer;
using js::UnwrapAndDowncastObject;
using js::UnwrapAndTypeCheckThis;

bool ReadableStreamBYOBRequest::constructor(JSContext* cx, unsigned argc,
                                            Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_BOGUS_CONSTRUCTOR,
                            "ReadableStreamBYOBRequest");
  return false;
}

// https://streams.spec.whatwg.org/#rs-byob-request-view
static bool ReadableStreamBYOBRequest_view(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<ReadableStreamBYOBRequest*> unwrappedRequest(
      cx,
      UnwrapAndTypeCheckThis<ReadableStreamBYOBRequest>(cx, args, "get view"));
  if (!unwrappedRequest) {
    return false;
  }
  args.rval().set(unwrappedRequest->view());
  return true;
}

// https://streams.spec.whatwg.org/#rs-byob-request-respond
[[nodiscard]] static bool ReadableStreamBYOBRequest_respond(JSContext* cx,
                                                            unsigned argc,
                                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.get(0).isNumber() || !(args.get(0).toNumber() >= 0)) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr,
        JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_INVALID_WRITTEN_LEN);
    return false;
  }

  Rooted<ReadableStreamBYOBRequest*> unwrappedRequest(
      cx,
      UnwrapAndTypeCheckThis<ReadableStreamBYOBRequest>(cx, args, "respond"));
  if (!unwrappedRequest) {
    return false;
  }

  // If this.[[controller]] is undefined, throw a TypeError exception.
  if (!unwrappedRequest->hasController()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMBYOBREQUEST_NO_CONTROLLER,
                              "respond()");
    return false;
  }

  uint64_t bytesWritten = args.get(0).toNumber();
  if (args.get(0).toNumber() - bytesWritten != 0) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr,
        JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_INVALID_WRITTEN_LEN);
    return false;
  }

  bool isShared;
  RootedObject viewObj(cx, &unwrappedRequest->view().toObject());
  JS::Rooted<JSObject*> buffer(
      cx, JS_GetArrayBufferViewBuffer(cx, viewObj, &isShared));

  if (!buffer) {
    return false;
  }

  size_t len = 0;
  uint8_t* data;
  JS::GetArrayBufferLengthAndData(buffer, &len, &isShared, &data);

  // If ! IsDetachedBuffer(this.[[view]].[[ArrayBuffer]]) is true, throw a
  // TypeError exception.
  if (buffer->maybeUnwrapAs<js::ArrayBufferObject>()->isDetached()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_DETACHED);
    return false;
  }

  Rooted<ReadableByteStreamController*> controller(
      cx, unwrappedRequest->controller());

  // Assert: this.[[view]].[[ByteLength]] > 0.
  // Assert: this.[[view]].[[ViewedArrayBuffer]].[[ByteLength]] > 0.
  // MOZ_ASSERT(len > 0);

  Rooted<ListObject*> unwrappedPendingPullIntos(cx,
                                                controller->pendingPullIntos());

  // Assert: controller.[[pendingPullIntos]] is not empty.
  MOZ_ASSERT(unwrappedPendingPullIntos->length() > 0);

  // Let firstDescriptor be controller.[[pendingPullIntos]][0].
  Rooted<PullIntoDescriptor*> firstDescriptor(
      cx, UnwrapAndDowncastObject<PullIntoDescriptor>(
              cx, &unwrappedPendingPullIntos->get(0).toObject()));

  // If state is "closed",
  if (controller->stream()->closed()) {
    // If bytesWritten is not 0, throw a TypeError exception.
    if (bytesWritten != 0) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_CLOSED);
      return false;
    }
  } else {
    // Assert: state is "readable".
    MOZ_ASSERT(controller->stream()->readable());

    // If bytesWritten is 0, throw a TypeError exception.
    if (bytesWritten == 0) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_ZERO);
      return false;
    }

    // If firstDescriptor’s bytes filled + bytesWritten > firstDescriptor’s byte
    // length, throw a RangeError exception.
    if (firstDescriptor->bytesFilled() + bytesWritten >
        firstDescriptor->byteLength()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_EXCEED);
      return false;
    }
  }

  // Set firstDescriptor’s buffer to ?
  // TransferArrayBuffer(view.[[ViewedArrayBuffer]]).
  JS::Rooted<JSObject*> transferredBuffer(cx, TransferArrayBuffer(cx, buffer));
  if (!transferredBuffer) {
    return false;
  }
  firstDescriptor->setBuffer(transferredBuffer);

  // Perform ? ReadableByteStreamControllerRespondInternal(controller,
  // bytesWritten).
  return ReadableByteStreamControllerRespondInternal(cx, controller,
                                                     bytesWritten);
}

// https://streams.spec.whatwg.org/#rs-byob-request-respond-with-new-view
static bool ReadableStreamBYOBRequest_respondWithNewView(JSContext* cx,
                                                         unsigned argc,
                                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.get(0).isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLEBYTESTREAMCONTROLLER_NO_VIEW,
                              "respondWithNewView");
    return false;
  }

  Rooted<JSObject*> view(cx, &args.get(0).toObject());

  if (!JS_IsArrayBufferViewObject(view)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLEBYTESTREAMCONTROLLER_NO_VIEW,
                              "respondWithNewView");
    return false;
  }

  Rooted<ReadableStreamBYOBRequest*> unwrappedRequest(
      cx, UnwrapAndTypeCheckThis<ReadableStreamBYOBRequest>(
              cx, args, "respondWithNewView"));
  if (!unwrappedRequest) {
    return false;
  }

  // If this.[[controller]] is undefined, throw a TypeError exception.
  if (!unwrappedRequest->hasController()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMBYOBREQUEST_NO_CONTROLLER,
                              "respondWithNewView()");
    return false;
  }

  Rooted<ReadableByteStreamController*> controller(
      cx, unwrappedRequest->controller());

  // If ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is true, throw a
  // TypeError exception.
  bool isShared;
  Rooted<JSObject*> viewedArrayBuffer(
      cx, JS_GetArrayBufferViewBuffer(cx, view, &isShared));

  if (viewedArrayBuffer->maybeUnwrapAs<js::ArrayBufferObject>()->isDetached()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_READABLESTREAMBYOBREQUEST_RESPOND_DETACHED);
    return false;
  }

  // Return ?
  // ReadableByteStreamControllerRespondWithNewView(this.[[controller]], view).
  if (!ReadableByteStreamControllerRespondWithNewView(cx, controller, view)) {
    return false;
  }
  return true;
}

static const JSPropertySpec ReadableStreamBYOBRequest_properties[] = {
    JS_PSG("view", ReadableStreamBYOBRequest_view, 0), JS_PS_END};

static const JSFunctionSpec ReadableStreamBYOBRequest_methods[] = {
    JS_FN("respond", ReadableStreamBYOBRequest_respond, 1, 0),
    JS_FN("respondWithNewView", ReadableStreamBYOBRequest_respondWithNewView, 1,
          0),
    JS_FS_END};

JS_STREAMS_CLASS_SPEC(ReadableStreamBYOBRequest, 0,
                      ReadableStreamBYOBRequest::SlotCount, 0, 0,
                      JS_NULL_CLASS_OPS);
