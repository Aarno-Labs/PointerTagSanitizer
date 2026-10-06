//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise public musl APIs which intentionally use its hidden allocator
// entry points rather than the replaceable malloc API.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#include <assert.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>

static void exitHandler(void) {}

int main(void) {
  // Musl keeps the first 32 exit handlers in static storage. The next one
  // makes __cxa_atexit allocate another block through __libc_calloc.
  for (int index = 0; index != 33; ++index) {
    assert(atexit(exitHandler) == 0);
  }

  // Musl owns this file-action node and allocates/frees it through
  // __libc_malloc and __libc_free.
  posix_spawn_file_actions_t actions;
  assert(posix_spawn_file_actions_init(&actions) == 0);
  assert(posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY,
                                          0) == 0);
  assert(posix_spawn_file_actions_destroy(&actions) == 0);
  return 0;
}
