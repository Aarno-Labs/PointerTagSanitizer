//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t

// Start with no environment so this exercises libc-created arrays and strings,
// independently of the initial process environment.
//
// setenv copies its input; putenv retains the caller's buffer. Check both
// ownership rules, array growth/compaction, and direct string modification.
#define _GNU_SOURCE
#include "../Inputs/ptsan_test_interface.h"
#include <stdlib.h>
#include <string.h>

extern char **environ;

int main(void) {
  environ = NULL;
  char value[] = "alpha";
  if (setenv("PTSAN_SET", value, 1) != 0)
    _Exit(2);
  value[0] = 'X';
  char *result = getenv("PTSAN_SET");
  if (!result || strcmp(result, "alpha") != 0)
    _Exit(3);
  unsigned array_id = get_id(environ);
  unsigned string_id = get_id(environ[0]);
  if (!array_id || array_id == ptsan_default_id() || !string_id ||
      string_id == ptsan_default_id() || get_id(result) != string_id)
    _Exit(4);

  if (setenv("PTSAN_SET", "ignored", 0) != 0 ||
      strcmp(getenv("PTSAN_SET"), "alpha") != 0)
    _Exit(5);
  if (setenv("PTSAN_SET", "beta", 1) != 0 ||
      strcmp(getenv("PTSAN_SET"), "beta") != 0)
    _Exit(6);

  char entry[] = "PTSAN_PUT=one";
  if (putenv(entry) != 0)
    _Exit(7);
  result = getenv("PTSAN_PUT");
  if (result != entry + sizeof("PTSAN_PUT=") - 1 ||
      get_id(result) != get_id(entry))
    _Exit(8);
  entry[sizeof("PTSAN_PUT=") - 1] = 'O';
  environ[0][sizeof("PTSAN_SET=") - 1] = 'B';
  if (strcmp(getenv("PTSAN_PUT"), "One") != 0 ||
      strcmp(getenv("PTSAN_SET"), "Beta") != 0)
    _Exit(9);

  if (unsetenv("PTSAN_SET") != 0 || getenv("PTSAN_SET") != NULL ||
      environ[0] != entry || environ[1] != NULL)
    _Exit(10);
  if (unsetenv("PTSAN_PUT") != 0 || getenv("PTSAN_PUT") != NULL ||
      environ[0] != NULL || entry[sizeof("PTSAN_PUT=") - 1] != 'O')
    _Exit(11);
  if (setenv("PTSAN_EMPTY", "", 1) != 0)
    _Exit(12);
  result = getenv("PTSAN_EMPTY");
  if (!result || *result != '\0')
    _Exit(13);
  if (clearenv() != 0 || environ != NULL)
    _Exit(14);
  _Exit(0);
}
