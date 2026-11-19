//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Use an empty nodemask with MPOL_DEFAULT (0), requiring no particular NUMA node.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
  unsigned long *mask = calloc(2, sizeof(*mask));
  assert(mask);
  if (argc > 1) {
    void *short_mask = calloc(1, sizeof(*mask) - 1);
    assert(short_mask);
    syscall(SYS_set_mempolicy, 0, short_mask, 2UL);
    return 1;
  }

  assert(syscall(SYS_set_mempolicy, 0, mask, 2UL) == 0);
  // maxnode - 1 bits are read, rounded up to whole words.
  unsigned long *word = calloc(1, sizeof(*word));
  assert(word);
  assert(syscall(SYS_set_mempolicy, 0, word, 65UL) == 0);
  assert(syscall(SYS_set_mempolicy, 0, mask, 66UL) == 0);
  // No mask access for maxnode == 1 or a null mask, including maxnode == 0.
  assert(syscall(SYS_set_mempolicy, 0, word + 1, 1UL) == 0);
  assert(syscall(SYS_set_mempolicy, 0, NULL, 0UL) == 0);
  assert(syscall(SYS_set_mempolicy, 0, NULL, ULONG_MAX) == 0);
  // These sizes are rejected before the kernel reads the nonnull mask.
  assert(syscall(SYS_set_mempolicy, 0, word, 0UL) == -1);
  assert(errno == EINVAL);
  assert(syscall(SYS_set_mempolicy, 0, word, ULONG_MAX) == -1);
  assert(errno == EINVAL);
  free(word);
  free(mask);
  return 0;
}
