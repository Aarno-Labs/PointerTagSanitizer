//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise the public musl interfaces that reach its hidden __clone assembly
// boundary with tagged child-stack memory.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -pthread -o %t %s
// RUN: %run %t clone-vfork
// RUN: %run %t clone-tids
// RUN: %run %t posix-spawn

#define _GNU_SOURCE

#include <sched.h>
#include <spawn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const size_t stack_size = 1024 * 1024;

static void *mapStack(void) {
  void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED) {
    return NULL;
  }
  return stack;
}

struct CloneArgument {
  int descriptor;
  int value;
};

static int cloneChild(void *opaque) {
  struct CloneArgument *argument = (struct CloneArgument *)opaque;
  if (write(argument->descriptor, &argument->value, sizeof(argument->value)) !=
      sizeof(argument->value)) {
    return 1;
  }
  return 0;
}

static int testClone(int clone_flags, bool use_tid_cells) {
  int descriptors[2];
  if (pipe(descriptors) != 0) {
    return 1;
  }

  void *stack = mapStack();
  if (stack == NULL) {
    return 2;
  }

  struct CloneArgument argument = {
      .descriptor = descriptors[1],
      .value = 42,
  };
  void *stack_top = (char *)stack + stack_size;
  pid_t parent_tid = -1;
  pid_t child_tid = -1;
  pid_t pid;
  if (use_tid_cells) {
    pid = clone(cloneChild, stack_top, clone_flags | SIGCHLD, &argument,
                &parent_tid, NULL, &child_tid);
  } else {
    pid = clone(cloneChild, stack_top, clone_flags | SIGCHLD, &argument);
  }
  if (pid < 0) {
    return 3;
  }

  close(descriptors[1]);
  int child_value = 0;
  if (read(descriptors[0], &child_value, sizeof(child_value)) !=
      sizeof(child_value)) {
    return 4;
  }
  close(descriptors[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0 || child_value != argument.value) {
    return 5;
  }
  if (use_tid_cells && (parent_tid != pid || child_tid != pid)) {
    return 6;
  }

  if (munmap(stack, stack_size) != 0) {
    return 7;
  }
  return 0;
}

static int testPosixSpawn(const char *program) {
  char *const child_argv[] = {(char *)program, (char *)"spawn-child", NULL};
  pid_t pid = -1;
  int error = posix_spawn(&pid, program, NULL, NULL, child_argv, environ);
  if (error != 0) {
    return 1;
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    return 2;
  }
  return 0;
}

static int testRawClone(void) {
  // Supply all six slots read by musl's syscall wrapper. AArch64's supported
  // fork form requires the unused slots to be zero as well.
  pid_t pid = (pid_t)syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, NULL, 0L);
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
  if (!strcmp(argv[1], "spawn-child")) {
    return 0;
  }
  if (!strcmp(argv[1], "clone-process")) {
    return testClone(0, false);
  }
  if (!strcmp(argv[1], "clone-vfork")) {
    return testClone(CLONE_VM | CLONE_VFORK, false);
  }
  if (!strcmp(argv[1], "clone-vm")) {
    return testClone(CLONE_VM, false);
  }
  if (!strcmp(argv[1], "syscall-clone")) {
    return testRawClone();
  }
  if (!strcmp(argv[1], "clone-tids")) {
    return testClone(CLONE_VM | CLONE_VFORK | CLONE_PARENT_SETTID |
                         CLONE_CHILD_SETTID,
                     true);
  }
  if (!strcmp(argv[1], "posix-spawn")) {
    return testPosixSpawn(argv[0]);
  }
  return 2;
}
