//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Preserve tagged buffers through cancellation dispatch, and check at the
// outgoing syscall only. Without pthread_cancel linked, musl uses its weak
// fallback; that path must work even in strict mode. Linking pthread_cancel
// selects the assembly path (enabled) or the raw fallback (disabled).
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -DWITH_CANCELLATION -Wl,-u,pthread_cancel %s -o %t.cp
// RUN: %run %t.cp
// RUN: not %run %t.cp bad 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -DWITH_CANCELLATION -DDISABLE_CANCELLATION -Wl,-u,pthread_cancel %s -o %t.disabled
// RUN: %run %t.disabled
// RUN: not %run %t.disabled bad 2>&1 | FileCheck %s
//
// CHECK: Found bad

#include <pthread.h>
#include <unistd.h>

__attribute__((noinline)) static ssize_t send_buffer(const char *p, size_t n) {
  return write(1, p, n);
}

int main(int argc, char **argv) {
#ifdef WITH_CANCELLATION
#ifdef DISABLE_CANCELLATION
  int state = PTHREAD_CANCEL_DISABLE;
#else
  int state = PTHREAD_CANCEL_ENABLE;
#endif
  if (pthread_setcancelstate(state, 0))
    return 1;
#endif
  char buffer[4] = {'t', 'e', 's', 't'};
  size_t size = argc == 1 ? sizeof(buffer) : sizeof(buffer) + 1;
  return send_buffer(buffer, size) == (ssize_t)size ? 0 : 2;
}
