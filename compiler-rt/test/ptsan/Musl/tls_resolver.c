//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_nonstrict -fno-sanitize=ptsan -c %S/Inputs/tls_resolver_native.c -o %t.native.o
// RUN: %ptsan_musl_nonstrict -pthread %s %t.native.o -o %t
// RUN: %run %t
// RUN: %ptsan_musl_strict -pthread %s %t.native.o -o %t.strict
// RUN: %run %t.strict
// RUN: not %run %t.strict short 2>&1 | FileCheck %s
// RUN: not %run %t.strict raw 2>&1 | FileCheck %s
// CHECK: Found bad

#define _GNU_SOURCE
#include <assert.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

extern void *__tls_get_addr(size_t *index);
extern uintptr_t native_tls_base(void);
extern int native_tls_value(void);
_Thread_local int resolver_tls_value = 17;

static int visit(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size;
  (void)data;
  assert(info->dlpi_tls_modid == 1);
  assert((uintptr_t)info->dlpi_tls_data == native_tls_base());
  return 1;
}

static void exercise(void) {
  size_t index[2] = {1, 0};
  assert((uintptr_t)__tls_get_addr(index) == native_tls_base());
  index[1] = 8;
  assert((uintptr_t)__tls_get_addr(index) == native_tls_base() + 8);
  assert(dl_iterate_phdr(visit, NULL) == 1);
  assert(resolver_tls_value == 17);
  resolver_tls_value = 42;
  assert(native_tls_value() == 42);
}

static void *worker(void *arg) {
  (void)arg;
  exercise();
  return NULL;
}

int main(int argc, char **argv) {
  if (argc > 1) {
    size_t *index = argv[1][0] == 'r' ? (size_t *)(uintptr_t)16
                                    : malloc(sizeof(size_t));
    (void)__tls_get_addr(index);
    abort();
  }
  exercise();
  pthread_t thread;
  assert(pthread_create(&thread, NULL, worker, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(resolver_tls_value == 42);
  return 0;
}
