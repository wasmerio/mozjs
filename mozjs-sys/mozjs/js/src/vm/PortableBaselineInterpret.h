/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PortableBaselineInterpret_h
#define vm_PortableBaselineInterpret_h

/*
 * Portable Baseline Interpreter: a portable interpreter that supports
 * ICs.
 */

#include "jspubtd.h"

#include "jit/JitContext.h"
#include "jit/JitScript.h"
#include "vm/Interpreter.h"
#include "vm/Stack.h"

namespace js {

bool PortableBaselineTrampoline(JSContext* cx, size_t argc, Value* argv,
                                size_t numActuals, size_t numFormals,
                                jit::CalleeToken calleeToken,
                                JSObject* envChain, Value* result);
jit::MethodStatus CanEnterPortableBaselineInterpreter(JSContext* cx,
                                                      RunState& state);
bool PortablebaselineInterpreterStackCheck(JSContext* cx, RunState& state,
                                           size_t numActualArgs);

} /* namespace js */

#endif /* vm_PortableBaselineInterpret_h */
