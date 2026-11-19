//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: split-file %s %t
// RUN: %ptsan_musl_nonstrict -fno-sanitize=ptsan -fno-stack-protector -c %t/probe.c -o %t/probe.o
// RUN: %ptsan_musl_nonstrict %t/main.c %t/probe.o -Wl,--wrap=InitializeIDs -o %t/test
// RUN: %run %t/test
// RUN: %ptsan_musl_strict %t/main.c %t/probe.o -Wl,--wrap=InitializeIDs -o %t/strict
// RUN: %run %t/strict
// RUN: %ptsan_musl_strict -mllvm -ptsan-stack-id-thread-buckets=0 %t/main.c %t/probe.o -Wl,--wrap=InitializeIDs -o %t/no-buckets
// RUN: %run %t/no-buckets

// Observe the real startup path, before musl installs the main thread's TLS.
// The later initialization must not reset the allocator a second time.

//--- probe.c
#include <stdint.h>

static unsigned calls;
static int before_tls;
static int after_tls;
extern unsigned ClStackIDThreadBuckets;
extern struct StackIDBucket *StackIDBuckets;
static int buckets_initialized;
static int strict_bounds_closed;
extern int ClStrictMode;
extern uint64_t ptsan_get_size(uint32_t);
extern uint32_t ptsan_id_bits(void);
void __real_InitializeIDs(void);

static int tls_is_absent(void) {
  unsigned long fs = ~0UL;
  long result;
  // arch_prctl(ARCH_GET_FS, &fs), without libc or errno/TLS access.
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(158L), "D"(0x1003L), "S"(&fs)
                   : "rcx", "r11", "memory");
  return result == 0 && fs == 0;
}

void __wrap_InitializeIDs(void) {
  ++calls;
  before_tls = tls_is_absent();
  __real_InitializeIDs();
  after_tls = tls_is_absent();
  buckets_initialized =
      (StackIDBuckets != 0) == (ClStackIDThreadBuckets != 0);
  uint32_t fallback = (UINT64_C(1) << ptsan_id_bits()) - 1;
  strict_bounds_closed = !ClStrictMode ||
      (ptsan_get_size(0) == 0 && ptsan_get_size(fallback) == 0);
}

int initialized_before_tls(void) {
  return calls == 1 && before_tls && after_tls && buckets_initialized &&
         strict_bounds_closed;
}

//--- main.c
#include <assert.h>
#include <stdlib.h>

int initialized_before_tls(void);

int main(void) {
  assert(initialized_before_tls());
  volatile int *p = malloc(sizeof(*p));
  assert(p != NULL);
  *p = 42;
  assert(*p == 42);
  free((void *)p);
  return 0;
}
