/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Weval_h
#define vm_Weval_h

#ifdef ENABLE_JS_PBL_WEVAL
#  include <weval.h>
#endif

namespace js {

#ifdef ENABLE_JS_PBL_WEVAL

struct Weval {
  weval_func_t func;
  weval_req_t* req;

 public:
  Weval() : func(nullptr), req(nullptr) {}
  ~Weval() {
    if (req) {
      weval_free(req);
      req = nullptr;
    }
  }
};

#endif
}  // namespace js

#endif /* vm_Weval_h */
