//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -DENV_CASE=0 \
// RUN:   -o %t.array %s
// RUN: %ptsan_musl_strict -DENV_CASE=1 \
// RUN:   -o %t.read %s
// RUN: %ptsan_musl_strict -DENV_CASE=2 \
// RUN:   -o %t.write %s
// RUN: %ptsan_musl_strict -DENV_CASE=3 \
// RUN:   -o %t.getenv %s
// RUN: %ptsan_musl_strict -DENV_CASE=4 \
// RUN:   -o %t.empty %s
// RUN: env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t.array
// RUN: env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t.read
// RUN: env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t.write
// RUN: env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t.getenv
// RUN: env -i %run %t.empty

// musl saves the kernel's raw environment array in __environ, aliased as
// environ. Both that array and its string entries need startup IDs. getenv
// traverses the same array and returns an interior pointer into an entry.
//
// Direct writes use environ's backing string, not a getenv return value.
// Keep the name, length and terminator intact, and verify getenv observes the
// changed value. _Exit avoids the unrelated strict-mode musl exit/TLS hole.
#include "../Inputs/ptsan_test_interface.h"
#include <stdlib.h>

extern char **environ;
extern char **__environ;

int main(void) {
  unsigned array_id = get_id(environ);
  if (!array_id || array_id == ptsan_default_id())
    _Exit(9);
#if ENV_CASE == 0
  if (environ != __environ)
    _Exit(2);
  char *volatile *entries = environ;
  if (!entries[0] || !entries[1] || entries[2] != NULL)
    _Exit(3);
  if (ptsan_get_size(array_id) != 3 * sizeof(char *) ||
      ptsan_get_size(get_id(entries[0])) != sizeof("PTSAN_ENV=alpha") ||
      ptsan_get_size(get_id(entries[1])) != sizeof("PTSAN_EMPTY="))
    _Exit(10);
#elif ENV_CASE == 1
  volatile char *entry = environ[0];
  if (entry[0] != 'P' || entry[sizeof("PTSAN_ENV=") - 1] != 'a' ||
      entry[sizeof("PTSAN_ENV=alpha") - 1] != '\0')
    _Exit(4);
#elif ENV_CASE == 2
  volatile char *entry = environ[0];
  entry[sizeof("PTSAN_ENV=") - 1] = 'A';
  entry[sizeof("PTSAN_ENV=alpha") - 1] = '\0';
  if (entry[sizeof("PTSAN_ENV=") - 1] != 'A')
    _Exit(5);
  volatile char *value = getenv("PTSAN_ENV");
  if (!value || value[0] != 'A' || value[5] != '\0')
    _Exit(6);
#elif ENV_CASE == 3
  volatile char *value = getenv("PTSAN_ENV");
  if (!value || value[0] != 'a' || value[4] != 'a' || value[5] != '\0')
    _Exit(7);
  if (get_id(value) != get_id(environ[0]))
    _Exit(11);
  volatile char *empty = getenv("PTSAN_EMPTY");
  if (!empty || empty[0] != '\0' || getenv("PTSAN_MISSING") != NULL)
    _Exit(8);
#elif ENV_CASE == 4
  char *volatile *entries = environ;
  if (entries[0] != NULL || ptsan_get_size(array_id) != sizeof(char *) ||
      getenv("PTSAN_MISSING") != NULL)
    _Exit(12);
#else
#error Select an environment test case.
#endif
  _Exit(0);
}
