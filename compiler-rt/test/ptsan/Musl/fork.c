//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Ordinary fork is permitted so the child can immediately replace its copied
// runtime state with exec. General fork-and-continue support is not guaranteed.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t fork
// RUN: %run %t underscore-fork
// RUN: %run %t syscall-fork

#define _GNU_SOURCE

#include <signal.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int waitForExecChild(pid_t pid, const char *self) {
  if (pid < 0) {
    return 1;
  }
  if (pid == 0) {
    execl(self, self, "child", NULL);
    _exit(127);
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
  if (!strcmp(argv[1], "child")) {
    return 0;
  }
  if (!strcmp(argv[1], "fork")) {
    return waitForExecChild(fork(), argv[0]);
  }
  if (!strcmp(argv[1], "underscore-fork")) {
    return waitForExecChild(_Fork(), argv[0]);
  }
  if (!strcmp(argv[1], "syscall-fork")) {
#ifdef SYS_fork
    return waitForExecChild((pid_t)syscall(SYS_fork), argv[0]);
#else
    return waitForExecChild(
        (pid_t)syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0), argv[0]);
#endif
  }
  return 3;
}
