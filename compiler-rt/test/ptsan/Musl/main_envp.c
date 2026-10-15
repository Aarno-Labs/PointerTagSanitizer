//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: env -i PTSAN_ENVP=alpha %run %t
// RUN: env -i %run %t
// RUN: %ptsan_musl_strict -o %t.strict %s
// RUN: env -i PTSAN_ENVP=alpha %run %t.strict
// RUN: env -i %run %t.strict
// RUN: %ptsan_musl_strict -DCLEAR_ENVIRONMENT \
// RUN:   -o %t.clear %s
// RUN: env -i PTSAN_ENVP=alpha %run %t.clear
// RUN: %ptsan_musl_strict -DREPLACE_ENVIRONMENT \
// RUN:   -o %t.replace %s
// RUN: env -i PTSAN_ENVP=alpha %run %t.replace

// main's third argument must retain the initial environment array and its ID,
// even when a constructor changes environ. Avoid allocations here: their
// auxv strict-mode failure is independent of this startup-pointer boundary.
#define _GNU_SOURCE
#include "../Inputs/ptsan_test_interface.h"
#include <stdlib.h>

extern char **environ;
static char **initial_environment;
static size_t initial_count;

#ifdef REPLACE_ENVIRONMENT
static char replacement_string[] = "PTSAN_ENVP=replaced";
static char *replacement[] = {replacement_string, NULL};
#endif

__attribute__((constructor)) static void saveEnvironment(void) {
  initial_environment = environ;
  while (environ[initial_count])
    ++initial_count;
#ifdef CLEAR_ENVIRONMENT
  if (clearenv() != 0)
    _Exit(2);
#elif defined(REPLACE_ENVIRONMENT)
  environ = replacement;
#endif
}

int main(int argc, char **argv, char **envp) {
  unsigned id = get_id(envp);
  if (!id || id == ptsan_default_id() ||
      id != get_id(initial_environment) || envp != initial_environment)
    _Exit(3);
  if (ptsan_get_size(id) != (initial_count + 1) * sizeof(char *))
    _Exit(4);

#ifdef CLEAR_ENVIRONMENT
  if (environ != NULL)
    _Exit(5);
#elif defined(REPLACE_ENVIRONMENT)
  if (environ != replacement || envp == environ)
    _Exit(6);
#else
  if (envp != environ)
    _Exit(7);
#endif

  char *volatile *entries = envp;
  if (entries[initial_count] != NULL)
    _Exit(8);
  if (initial_count) {
    volatile char *entry = entries[0];
    if (initial_count != 1 || entry[0] != 'P' || entry[11] != 'a' ||
        entry[sizeof("PTSAN_ENVP=alpha") - 1] != '\0')
      _Exit(9);
    entry[11] = 'A';
    if (initial_environment[0][11] != 'A')
      _Exit(10);
  }
  // The normal musl exit path has a separate strict-mode TLS limitation.
  _Exit(0);
}
