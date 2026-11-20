//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// vfork remains unsupported until PTSan can preserve the parent's runtime
// state while the child temporarily shares its address space and stack.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t public
// RUN: %run %t syscall

#define _GNU_SOURCE

#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int waitForChild(pid_t pid) {
  if (pid < 0) {
    return 1;
  }
  if (pid == 0) {
    _exit(0);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    return 2;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }

  if (!strcmp(argv[1], "public")) {
    return waitForChild(vfork());
  }
  if (!strcmp(argv[1], "syscall")) {
    return waitForChild((pid_t)syscall(SYS_vfork));
  }
  return 3;
}
