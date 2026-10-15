//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Alternate signal stacks remain unsupported until PTSan can track the
// lifetime of the stack pointer retained by the kernel.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t register
// RUN: %run %t query
// RUN: %run %t disable
// RUN: %run %t syscall

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }

  if (!strcmp(argv[1], "query")) {
    stack_t old_stack;
    return sigaltstack(NULL, &old_stack) != 0;
  }

  if (!strcmp(argv[1], "disable")) {
    stack_t disabled_stack = {
        .ss_sp = NULL,
        .ss_flags = SS_DISABLE,
        .ss_size = 0,
    };
    return sigaltstack(&disabled_stack, NULL) != 0;
  }

  void *memory = malloc(SIGSTKSZ);
  if (!memory) {
    return 2;
  }

  stack_t stack = {
      .ss_sp = memory,
      .ss_flags = 0,
      .ss_size = SIGSTKSZ,
  };

  int result;
  if (!strcmp(argv[1], "register")) {
    result = sigaltstack(&stack, NULL);
  } else if (!strcmp(argv[1], "syscall")) {
    result = syscall(SYS_sigaltstack, &stack, NULL);
  } else {
    free(memory);
    return 3;
  }

  if (result == 0) {
    stack_t disabled_stack = {
        .ss_sp = NULL,
        .ss_flags = SS_DISABLE,
        .ss_size = 0,
    };
    if (!strcmp(argv[1], "register")) {
      result = sigaltstack(&disabled_stack, NULL);
    } else {
      result = syscall(SYS_sigaltstack, &disabled_stack, NULL);
    }
  }

  free(memory);
  return result != 0;
}
