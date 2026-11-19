//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: split-file %s %t
// RUN: %ptsan_musl_nonstrict -I%S/.. -fno-sanitize=ptsan -fno-stack-protector -c %t/probe.c -o %t/probe.o
// RUN: %ptsan_musl_nonstrict -I%S/.. %t/main.c %t/data.c %t/probe.o -Wl,--wrap=__init_tls -Wl,--gc-sections -o %t/test
// RUN: %run %t/test
// RUN: %ptsan_musl_strict -I%S/.. %t/main.c %t/data.c %t/probe.o -Wl,--wrap=__init_tls -Wl,--gc-sections -o %t/strict
// RUN: %run %t/strict

//--- data.c
int values[2] = {17, 29};
extern int *selected;
int **indirect = &selected;
_Thread_local int thread_value = 41;

//--- main.c
#include <assert.h>
#include <stdint.h>

extern int values[2];
extern int **indirect;
extern _Thread_local int thread_value;
int *selected = &values[1];
int *const readonly_pointer = &values[0];
extern int globals_ready_before_tls(void);
#include "Inputs/ptsan_test_interface.h"
static int constructor_ran;

__attribute__((constructor(101))) static void check_constructor(void) {
  assert(globals_ready_before_tls());
  assert(*selected == 29 && **indirect == 29 && *readonly_pointer == 17);
  assert(ptsan_get_id(selected) == ptsan_get_id(values));
  assert(ptsan_get_id(&thread_value) != 0 && thread_value == 41);
  // Early initializer replay must not repeat and overwrite later mutations.
  selected = &values[0];
  constructor_ran = 1;
}

int main(void) {
  assert(constructor_ran);
  assert(*selected == 17 && **indirect == 17);
  return 0;
}

//--- probe.c
#include <stdint.h>

extern int values[2];
extern int *selected;
extern int **indirect;
extern int *const readonly_pointer;
extern void *values_proxy asm("values.ptsan.ptr");
extern void *selected_proxy asm("selected.ptsan.ptr");
#include "Inputs/ptsan_test_interface.h"
void __real___init_tls(uintptr_t *);
static int ready;

void __wrap___init_tls(uintptr_t *aux) {
  unsigned long fs = ~0UL;
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(158L), "D"(0x1003L), "S"(&fs)
                   : "rcx", "r11", "memory");
  ready = result == 0 && fs == 0 && ptsan_get_id(values_proxy) != 0 &&
          ptsan_get_id(selected_proxy) != 0 &&
          ptsan_get_id(selected) == ptsan_get_id(values_proxy) &&
          ptsan_get_id(readonly_pointer) == ptsan_get_id(values_proxy) &&
          ptsan_get_id(indirect) == ptsan_get_id(selected_proxy) &&
          ptsan_get_address(selected) == (uintptr_t)&values[1] &&
          ptsan_get_address(indirect) == (uintptr_t)&selected;
  __real___init_tls(aux);
}

int globals_ready_before_tls(void) { return ready; }
