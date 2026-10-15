//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A tagless exact attachment address should eventually recover and retire its
// mapping metadata. Raw-address lookup is deferred, so this fails closed now.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#define _GNU_SOURCE

#include "../Inputs/ptsan_test_interface.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

static void *clearPointerID(const void *pointer) {
  const uintptr_t address_mask = UINT64_C(0x0000ffffffffffff);
  return (void *)((uintptr_t)pointer & address_mask);
}

int main(void) {
  long page_size_result = sysconf(_SC_PAGESIZE);
  if (page_size_result <= 0) {
    return 1;
  }
  const size_t page_size = (size_t)page_size_result;

  int shared_memory_id = shmget(IPC_PRIVATE, page_size, IPC_CREAT | 0600);
  if (shared_memory_id < 0) {
    return 2;
  }
  char *mapping = (char *)shmat(shared_memory_id, NULL, 0);
  if (mapping == (void *)-1) {
    shmctl(shared_memory_id, IPC_RMID, NULL);
    return 3;
  }
  if (shmctl(shared_memory_id, IPC_RMID, NULL) != 0) {
    return 4;
  }

  const uint16_t mapping_id = get_id(mapping);
  assert(ptsan_get_base(mapping_id) == (uint64_t)(uintptr_t)mapping);
  assert(ptsan_get_size(mapping_id) == page_size);
  if (shmdt(clearPointerID(mapping)) != 0) {
    return 5;
  }
  assert(ptsan_get_base(mapping_id) == 0);
  assert(ptsan_get_size(mapping_id) == 0);
  return 0;
}
