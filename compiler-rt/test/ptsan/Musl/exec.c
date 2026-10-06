//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise exec argument and environment pointer graphs through public musl
// APIs. fexecve reaches the execveat syscall path.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t execve
// RUN: %run %t fexecve
// RUN: not %run %t bad-string 2> %t.string
// RUN: FileCheck %s < %t.string
// RUN: not %run %t bad-vector 2> %t.vector
// RUN: FileCheck %s < %t.vector
//
// CHECK: Found bad

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char Program[] = "/proc/self/exe";
static const char Completed[] = "exec-completed";
static const char Environment[] = "PTSAN_EXEC_FAMILY=1";

static char *copy_string(const char *source) {
  size_t size = strlen(source) + 1;
  char *copy = malloc(size);
  if (copy) {
    memcpy(copy, source, size);
  }
  return copy;
}

static int run_exec(bool use_fexecve) {
  char *program = copy_string(Program);
  char *completed = copy_string(Completed);
  char *environment_entry = copy_string(Environment);
  if (!program || !completed || !environment_entry) {
    free(environment_entry);
    free(completed);
    free(program);
    return 1;
  }

  char *arguments[] = {program, completed, NULL};
  char *environment[] = {environment_entry, NULL};

  if (use_fexecve) {
    int fd = open(program, O_RDONLY);
    if (fd < 0) {
      return 2;
    }
    (void)fexecve(fd, arguments, environment);
    close(fd);
  } else {
    (void)execve(program, arguments, environment);
  }

  free(environment_entry);
  free(completed);
  free(program);
  return 3;
}

static int run_bad_string(void) {
  char *invalid = malloc(1);
  if (!invalid) {
    return 1;
  }

  char *arguments[] = {(char *)Program, invalid + 1, NULL};
  char *environment[] = {NULL};
  (void)execve(Program, arguments, environment);
  free(invalid);
  return 0;
}

static int run_bad_vector(void) {
  char **arguments = malloc(sizeof(char *));
  if (!arguments) {
    return 1;
  }

  // The sole allocated slot is non-NULL, so the vector has no terminator
  // within its bounds.
  arguments[0] = (char *)Program;
  char *environment[] = {NULL};
  (void)execve(Program, arguments, environment);
  free(arguments);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], Completed)) {
    return 0;
  }
  if (!strcmp(argv[1], "execve")) {
    return run_exec(false);
  }
  if (!strcmp(argv[1], "fexecve")) {
    return run_exec(true);
  }
  if (!strcmp(argv[1], "bad-string")) {
    return run_bad_string();
  }
  if (!strcmp(argv[1], "bad-vector")) {
    return run_bad_vector();
  }
  return 2;
}
