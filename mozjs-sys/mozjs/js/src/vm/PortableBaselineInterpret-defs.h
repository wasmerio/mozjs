/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef PortableBaselineInerpret_defs_h
#define PortableBaselineInerpret_defs_h

/* Basic definitions for PBL's internals that can be swapped out as
 * needed to handle interpreter details differently.
 *
 * Meant to be included only from PortableBaselineInterpret.cpp. */

#define PBL_HYBRID_ICS_DEFAULT true

#define PBL_CALL_IC(jitcode, ctx, stubvalue, result, arg0, arg1, arg2value, \
                    hasarg2)                                                \
  do {                                                                      \
    ctx.arg2 = arg2value;                                                   \
    if (stubvalue->isFallback()) {                                          \
      ICStubFunc func = reinterpret_cast<ICStubFunc>(jitcode);              \
      result = func(arg0, arg1, stubvalue, ctx);                            \
    } else {                                                                \
      result = ICInterpretOps<false>(arg0, arg1, stubvalue, ctx);           \
    }                                                                       \
  } while (0)

#define PBL_CALL_INTERP(result, script, interp, ...) \
  result = interp(__VA_ARGS__);

#define PBL_ESTABLISH_STUBINFO_CODE(Specialized, stubInfo, code) \
  stubInfo = cstub->stubInfo();                                  \
  code = stubInfo->code();

#define READ_REG(reg)                                       \
  ({                                                        \
    TRACE_PRINTF("READ_REG(%d): %" PRIx64 "\n", int((reg)), \
                 ctx.icregs.icVals[(reg)]);                 \
    ctx.icregs.icVals[(reg)];                               \
  })
#define WRITE_REG(reg, value, tagtype)                                \
  do {                                                                \
    uint64_t write_value = (value);                                   \
    TRACE_PRINTF("WRITE_REG(%d, " #tagtype "): %" PRIx64 "\n", (reg), \
                 write_value);                                        \
    ctx.icregs.icVals[(reg)] = write_value;                           \
    ctx.icregs.icTags[(reg)] = uint64_t(JSVAL_TAG_##tagtype)          \
                               << JSVAL_TAG_SHIFT;                    \
  } while (0)
#define READ_VALUE_REG(reg)                                              \
  ({                                                                     \
    uint64_t bits = ctx.icregs.icVals[(reg)] | ctx.icregs.icTags[(reg)]; \
    TRACE_PRINTF("READ_VALUE_REG(%d): %" PRIx64 "\n", (reg), bits);      \
    Value::fromRawBits(bits);                                            \
  })
#define WRITE_VALUE_REG(reg, value)                                         \
  do {                                                                      \
    uint64_t write_value = (value).asRawBits();                             \
    TRACE_PRINTF("WRITE_VALUE_REG(%d): %" PRIx64 "\n", (reg), write_value); \
    ctx.icregs.icVals[(reg)] = write_value;                                 \
    ctx.icregs.icTags[(reg)] = 0;                                           \
  } while (0)

#define PBL_PUSH_CTX(ctx) ;
#define PBL_UPDATE_CTX(ctx) ;
#define PBL_POP_CTX() ;

#define VIRTPUSH(value) PUSH(value)
#define VIRTPOP() POP()
#define VIRTSP(index) sp[(index)]
#define VIRTSPWRITE(index, value) sp[(index)] = (value)
#define SYNCSP()
#define SETLOCAL(i, value) frame->unaliasedLocal(i) = value
#define GETLOCAL(i) frame->unaliasedLocal(i)

#define PBL_SETUP_INTERP_INPUTS(argsObjAliasesFormals, nfixed)      \
  argsObjAliasesFormals = frame->script()->argsObjAliasesFormals(); \
  nfixed = frame->script()->nfixed();

#define PBL_SPECIALIZE_VALUE(i, low, high) i

#define PBL_SCRIPT_HAS_SPECIALIZATION(script) false

#endif  /* PortableBaselineInerpret_defs_h */
