//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#include "ptsan_origins.h"

#ifdef PTSAN_32_BIT_IDS
#include "ptsan_thread.h"

SANITIZER_INTERFACE_ATTRIBUTE
u32 *ptsan_origins;

SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u32 *ptsan_stack_origin_next;

namespace __ptsan {

static RandomStream StartupOriginRandom;

void initializeOrigins() {
  StartupOriginRandom.initialize();
}

// Keep TLS address calculation out of the startup path, which runs before
// instrumented musl has initialized TLS.
RandomStream &origin_random() {
  // Startup is single-threaded and can register globals before TLS exists.
  if (UNLIKELY(!ptsan_initialized)) {
    return StartupOriginRandom;
  }
  PTSanThread *Thread = GetCurrentThread();
  return Thread->Random;
}

} // namespace __ptsan

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u32 ptsan_choose_stack_origin(u64 alignment) {
  return __ptsan::choose_id32_origin(alignment);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE u32
ptsan_refill_stack_origins() {
  using namespace __ptsan;
  // Only the inline fast-ID path calls this, after thread state is installed.
  PTSanThread *thread = GetCurrentThread();
  for (u32 i = 0; i != thread->StackOriginCount; ++i) {
    thread->StackOrigins[i] = choose_id32_origin(4096, &thread->Random);
  }
  // Reset origin cursor
  ptsan_stack_origin_next = thread->StackOrigins;
  return choose_id32_origin(4096, &thread->Random);
}

#ifndef PTSAN_PRODUCTION_MODE
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u32 ptsan_get_origin(PTSanID id) {
  return ptsan_origins[id];
}
#endif
#endif
