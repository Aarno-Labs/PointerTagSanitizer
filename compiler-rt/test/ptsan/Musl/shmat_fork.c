//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Fork preserves inherited attachment metadata in each process while parent
// and child detach their copies independently. This does not exercise the
// unsupported case where fork discards other threads and their PTSan state.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#define _GNU_SOURCE

#include "../Inputs/ptsan_test_interface.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

static int childProcess(char *mapping, uint16_t mapping_id, size_t size) {
  assert(get_id(mapping) == mapping_id);
  assert(ptsan_get_base(mapping_id) == (uint64_t)(uintptr_t)mapping);
  assert(ptsan_get_size(mapping_id) == size);
  if (mapping[0] != 42) {
    return 1;
  }
  mapping[0] = 7;

  if (shmdt(mapping) != 0) {
    return 2;
  }
  assert(ptsan_get_base(mapping_id) == 0);
  assert(ptsan_get_size(mapping_id) == 0);
  return 0;
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
  mapping[0] = 42;

  pid_t pid = fork();
  if (pid < 0) {
    return 5;
  }
  if (pid == 0) {
    _exit(childProcess(mapping, mapping_id, page_size));
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    return 6;
  }

  assert(mapping[0] == 7);
  assert(ptsan_get_base(mapping_id) == (uint64_t)(uintptr_t)mapping);
  assert(ptsan_get_size(mapping_id) == page_size);
  if (shmdt(mapping) != 0) {
    return 7;
  }
  assert(ptsan_get_base(mapping_id) == 0);
  assert(ptsan_get_size(mapping_id) == 0);
  return 0;
}
