//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// SHM_REMAP should replace destination metadata and return a bounded
// attachment. It remains deliberately fail-closed for now.
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
#include <sys/mman.h>
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
  char *keeper = (char *)shmat(shared_memory_id, NULL, 0);
  if (keeper == (void *)-1) {
    shmctl(shared_memory_id, IPC_RMID, NULL);
    return 3;
  }
  if (shmctl(shared_memory_id, IPC_RMID, NULL) != 0) {
    return 4;
  }

  char *destination = (char *)mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (destination == MAP_FAILED) {
    shmdt(keeper);
    return 5;
  }
  const uint16_t destination_id = get_id(destination);

  char *result =
      (char *)shmat(shared_memory_id, clearPointerID(destination), SHM_REMAP);
  if (result == (void *)-1) {
    shmdt(keeper);
    return 6;
  }

  const uint16_t result_id = get_id(result);
  assert(clearPointerID(result) == clearPointerID(destination));
  assert(result_id != destination_id);
  assert(ptsan_get_base(destination_id) == 0);
  assert(ptsan_get_size(destination_id) == 0);
  assert(ptsan_get_base(result_id) == (uint64_t)(uintptr_t)result);
  assert(ptsan_get_size(result_id) == page_size);
  result[page_size - 1] = 1;

  if (shmdt(result) != 0) {
    return 7;
  }
  if (shmdt(keeper) != 0) {
    return 8;
  }
  assert(ptsan_get_base(result_id) == 0);
  assert(ptsan_get_size(result_id) == 0);
  return 0;
}
