//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_raw -O2 -I%crt_src/lib/ptsan %s -o %t.default
// RUN: %run %t.default
// RUN: %clang_raw -O2 -DPTSAN_32_BIT_IDS -I%crt_src/lib/ptsan %s -o %t.id32
// RUN: %run %t.id32

// Test the decoder shared by the C interceptors and C++ runtime, independently
// of compiler instrumentation. A high payload must be zero-extended.
#include "ptsan_bounds_abi.h"
#include <assert.h>
#include <stddef.h>

#ifdef PTSAN_32_BIT_IDS
#include <sys/mman.h>
struct ObjectBounds *ptsan_bounds;
#else
struct ObjectBounds ptsan_bounds[4];
#endif

int main(void) {
  char object[16];
  uintptr_t base = (uintptr_t)object;
  assert(ptsan_raw_address(base) == base);
  assert(ptsan_raw_address(0) == 0);

#ifdef PTSAN_32_BIT_IDS
  // The indicator is part of the index. Reserve the full table but touch only
  // this high-half entry, just as the runtime does for a marked allocation.
  const size_t table_size = (UINT64_C(1) << 32) * sizeof(*ptsan_bounds);
  ptsan_bounds = mmap(NULL, table_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  assert(ptsan_bounds != MAP_FAILED);
  const uint32_t id = UINT32_C(0x80000002);
  const uint32_t origin = UINT32_C(0x80000010);
  ptsan_bounds[id].base = base - origin;
  ptsan_bounds[id].span = sizeof(object);
  uintptr_t packed = ((uint64_t)id << 32) | origin;
  assert(ptsan_raw_address(packed) == base);
  assert(ptsan_raw_address(packed + 7) == base + 7);
  assert(ptsan_raw_address(packed - 1) == base - 1);
  assert(ptsan_raw_address(UINTPTR_MAX) == UINTPTR_MAX);
  munmap(ptsan_bounds, table_size);
#else
  uintptr_t packed = base | UINT64_C(0x1234000000000000);
  assert(ptsan_raw_address(packed) == base);
  assert(ptsan_raw_address(packed + 7) == base + 7);
#endif
  return 0;
}
