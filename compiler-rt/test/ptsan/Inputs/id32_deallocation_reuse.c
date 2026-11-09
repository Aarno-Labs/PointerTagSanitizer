//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Simulate replacement metadata at the retirement boundary.
// The caller must have saved the raw address before retirement, since decoding
// the old pointer again would use the replacement object's metadata.

#include "ptsan_test_interface.h"
#define PTSAN_32_BIT_IDS
#include "../../../lib/ptsan/ptsan_bounds_abi.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>

static void *victim;
static uintptr_t native_address;
static uint32_t victim_id;
static bool reused;

void prepare_reuse(void *ptr) {
  victim = ptr;
  native_address = ptsan_get_address(ptr);
  victim_id = ptsan_get_id(ptr);
  assert(victim_id != 0 && victim_id != ptsan_default_id());
}

static void reuse_retired_id(void *ptr) {
  if (ptr != victim) {
    return;
  }
  victim = NULL;
  assert(ptsan_get_size(victim_id) == 0);

  // Another thread could reuse this ID as soon as retirement publishes it.
  // Change its decode bias directly to model that interleaving, without
  // depending on cache sizes or allocating after __unmapself's TLS teardown.
  ptsan_bounds[victim_id].base += UINT64_C(0x100000000000);
  assert(ptsan_get_address(ptr) != native_address);
  reused = true;
}

#ifdef TEST_UNMAPSELF
void __real_ptsan_retire_mmap_if_exact_direct(void *, uint64_t);

void __wrap_ptsan_retire_mmap_if_exact_direct(void *ptr, uint64_t size) {
  __real_ptsan_retire_mmap_if_exact_direct(ptr, size);
  reuse_retired_id(ptr);
}

// Stand in for the noreturn assembly boundary, checking the address before it
// could unmap the wrong storage. The runtime's real __wrap___unmapself runs.
__attribute__((noreturn)) void __unmapself(void *ptr, size_t size) {
  assert(reused);
  assert((uintptr_t)ptr == native_address);
  assert(size == 4096);
  _Exit(0);
}
#else
void __real_ptsan_mark_ptr_freed(void *, uint8_t);

void __wrap_ptsan_mark_ptr_freed(void *ptr, uint8_t type) {
  __real_ptsan_mark_ptr_freed(ptr, type);
  reuse_retired_id(ptr);
}

void check_deallocation(int unmapped) {
  assert(reused);
  if (unmapped) {
    unsigned char resident;
    errno = 0;
    assert(mincore((void *)native_address, 4096, &resident) == -1);
    assert(errno == ENOMEM);
  }
}
#endif
