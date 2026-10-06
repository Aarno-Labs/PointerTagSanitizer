//===-- ptsan_drift.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// Drift-specific PTSan machinery.
//
//===----------------------------------------------------------------------===//

/* Drift-specific ptsan machinery

This lives in a seperate file so I can easily remove it for the paper.
*/
#include "ptsan.h"
#include "ptsan_drift.h"

#include "sanitizer_common/sanitizer_stacktrace.h"


void __sanitizer::BufferedStackTrace::UnwindImpl(uptr pc, uptr bp,
                                                 void *context,
                                                 bool request_fast,
                                                 u32 max_depth) {
#ifdef PTSAN_INSTRUMENTED_LIBC
  // The initial static instrumented-libc runtime deliberately omits the
  // symbolizer and platform unwinder.  PTSan does not currently report this
  // trace, but sanitizer_common still requires an UnwindImpl definition.
  top_frame_bp = max_depth ? bp : 0;
  size = max_depth ? 1 : 0;
  if (size)
    trace_buffer[0] = pc;
#else
  return Unwind(max_depth, pc, bp, context, 0, 0, false);
#endif
}

extern "C" {
  SANITIZER_INTERFACE_ATTRIBUTE
  thread_local jmp_buf ptsan_jmp_env;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
jmp_buf *ptsan_get_jmp_env() {
  static bool announced_patch = false;
  if (!announced_patch) {
    Printf("Ptsan's get jmp env called. This is the patched version!\n");
    announced_patch = true;
  }

  return &ptsan_jmp_env;
}


void ptsan_handle_error(u32 check_id, u64 function_id, u64 ptr_id,
                  const char * type) {
  switch (ClPatchAllWith){
    case 0:
      // By default we don't do anything here
      return;
    case 1:
      Printf("Patch applied. Exiting!\n");
      Die();
    case 2:
      Printf("Patch applied. Jumping!\n");
      longjmp(ptsan_jmp_env, 1);
    default:
      Printf("Unknown patch type!. Exiting!\n");
      internal__exit(1);
  }
}
