//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: ptsan-instrumented-musl
// RUN: split-file %s %t
// RUN: %ptsan_musl_id32_strict_indicator -mllvm -ptsan-stack-temporal-safety=0 -fno-sanitize=ptsan -c %t/native.c -o %t/native.o
// RUN: %ptsan_musl_id32_strict_indicator -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-may-decode-raw -mllvm -ptsan-nochecks -fno-builtin -c %t/mixed.c -o %t/mixed.o
// RUN: %ptsan_musl_id32_strict_indicator -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-may-decode-raw -c %t/checked.c -o %t/checked.o
// RUN: %ptsan_musl_id32_strict_indicator -mllvm -ptsan-stack-temporal-safety=0 %t/main.c %t/native.o %t/mixed.o %t/checked.o -pthread -o %t/test
// RUN: %run %t/test
// RUN: not %run %t/test raw 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %run %t/test oob extra 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %ptsan_musl_id32_strict_indicator -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-no-stack-id-fast-path %t/main.c %t/native.o %t/mixed.o %t/checked.o -pthread -o %t/slow
// RUN: %run %t/slow
// RUN: %ptsan_musl_id32_strict_indicator -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-stack-id-thread-buckets=0 %t/main.c %t/native.o %t/mixed.o %t/checked.o -pthread -o %t/no-buckets
// RUN: %run %t/no-buckets
// RUN: not %ptsan_musl_id32_strict_indicator -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-stack-id-thread-buckets=129 -mllvm -ptsan-id-bucket-size=16777216 %t/main.c %t/native.o %t/mixed.o %t/checked.o -pthread -o %t/oversized 2>&1 | FileCheck %s --check-prefix=CONFIG
// BAD: Found bad POINT access
// CONFIG: PTSan reserved stack ID buckets exceed ID space
//
// The file-local decoder policy is independent of checking. Native buffers
// remain raw through arithmetic/storage; encoded buffers cross both policies.
//--- main.c
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

uintptr_t native_bits(void);
void native_verify(char *p);
char *mixed(char *raw, char *encoded);
int checked(const char *p);
static int global = 23;
static _Thread_local int tls = 41;
__attribute__((noinline)) static void inspect(int *p) {
  assert(((uintptr_t)p >> 63) == 1);
  assert(*p != 0);
}
static void *worker(void *arg) {
  int local = 7;
  inspect(&local);
  inspect(&tls);
  inspect(&global);
  inspect(arg);
  return arg;
}
int main(int argc, char **argv) {
  (void)argv;
  char *p = malloc(8);
  assert(p && ((uintptr_t)p >> 63));
  char *raw = (char *)native_bits();
  if (argc == 2)
    return checked(raw);
  if (argc > 2)
    return checked(p + 8);
  native_verify(mixed(raw, p));
  assert(p[0] == 42 && checked(p) == 42);
  int local = 17;
  pthread_t t;
  assert(pthread_create(&t, 0, worker, &local) == 0);
  void *ret;
  assert(pthread_join(t, &ret) == 0);
  assert(ret == &local);
  free(p);
  return 0;
}
//--- mixed.c
#include <assert.h>
#include <string.h>
static char *volatile saved;
char *mixed(char *raw, char *encoded) {
  memset(raw, 42, 8);
  assert(memcpy(encoded, raw, 8) == encoded);
  assert(memmove(raw, encoded, 8) == raw);
  saved = raw + 1;
  assert(saved[0] == 42);
  return saved;
}
//--- checked.c
int checked(const char *p) { return *p; }
//--- native.c
#include <assert.h>
#include <stdint.h>
static char buffer[8];
uintptr_t native_bits(void) { return (uintptr_t)buffer; }
void native_verify(char *p) {
  assert(p == buffer + 1);
  assert(buffer[0] == 42 && buffer[7] == 42);
}
