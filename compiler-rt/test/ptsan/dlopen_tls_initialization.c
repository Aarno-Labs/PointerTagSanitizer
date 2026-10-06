//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t
// RUN: %clang_raw -O2 -fPIC -shared %t/library.c -o %t/library.so
// RUN: %clang_raw -O2 %t/main.c -pthread -ldl -o %t/test
// RUN: %t/test %t/library.so

// Loader-behavior test, deliberately without PTSan: distinguish creation of
// each thread's TLS storage from execution of the library's constructor.
// The constructor changes only the loading thread's instance. Both an already
// existing thread and a subsequently created thread get the original template.
// This is why a constructor alone cannot initialize every thread's PTSan proxy.

//--- library.c
static _Thread_local int value = 7;
static int constructor_calls;

__attribute__((constructor)) static void initialize(void) {
  ++constructor_calls;
  value = 19;
}

int *get_value(void) {
  return &value;
}

int read_value(int *p) {
  return *p;
}

void write_value(int *p, int n) {
  *p = n;
}

int get_constructor_calls(void) {
  return constructor_calls;
}

//--- main.c
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>

static pthread_barrier_t barrier;
static pthread_barrier_t existing_ready;
static int *(*get_value)(void);
static int (*read_value)(int *);
static void (*write_value)(int *, int);

#ifdef CHECK_TLS_IDS
__attribute__((disable_sanitizer_instrumentation))
static unsigned check_tls_id(int *value) {
  unsigned id = (uintptr_t)value >> 48;
  assert(id && id != 0xffff);
  assert(((uintptr_t)get_value() >> 48) == id);
  return id;
}
#endif

__attribute__((disable_sanitizer_instrumentation))
static void *worker(void *arg) {
  pthread_barrier_wait(&barrier);
  int *value = get_value();
#ifdef CHECK_TLS_IDS
  check_tls_id(value);
#endif
  assert(read_value(value) == 7);
  write_value(value, 31);
  *(int **)arg = value;
  pthread_barrier_wait(&barrier);
  // Keep both TLS instances alive until main has compared their addresses.
  pthread_barrier_wait(&barrier);
  assert(read_value(value) == 31);
  return 0;
}

__attribute__((disable_sanitizer_instrumentation))
static void *existing_worker(void *arg) {
  // Ensure the thread has finished runtime startup before main loads the DSO.
  pthread_barrier_wait(&existing_ready);
  return worker(arg);
}

__attribute__((disable_sanitizer_instrumentation))
int main(int argc, char **argv) {
  assert(argc == 2);
  assert(pthread_barrier_init(&barrier, 0, 3) == 0);
  assert(pthread_barrier_init(&existing_ready, 0, 2) == 0);
  pthread_t existing, subsequent;
  int *existing_value, *subsequent_value;
  assert(pthread_create(&existing, 0, existing_worker, &existing_value) == 0);
  pthread_barrier_wait(&existing_ready);

  void *library = dlopen(argv[1], RTLD_NOW);
  assert(library);
  get_value = (int *(*)(void))dlsym(library, "get_value");
  read_value = (int (*)(int *))dlsym(library, "read_value");
  write_value = (void (*)(int *, int))dlsym(library, "write_value");
  int (*get_constructor_calls)(void) =
      (int (*)(void))dlsym(library, "get_constructor_calls");
  assert(get_value && read_value && write_value && get_constructor_calls);
  int *loading_value = get_value();
#ifdef CHECK_TLS_IDS
  unsigned loading_id = check_tls_id(loading_value);
#endif
  assert(read_value(loading_value) == 19);
  assert(get_constructor_calls() == 1);

  assert(pthread_create(&subsequent, 0, worker, &subsequent_value) == 0);
  pthread_barrier_wait(&barrier);
  pthread_barrier_wait(&barrier);
  assert(existing_value != subsequent_value);
  assert(existing_value != loading_value && subsequent_value != loading_value);
#ifdef CHECK_TLS_IDS
  unsigned existing_id = (uintptr_t)existing_value >> 48;
  unsigned subsequent_id = (uintptr_t)subsequent_value >> 48;
  assert(existing_id != subsequent_id);
  assert(existing_id != loading_id && subsequent_id != loading_id);
#endif
  assert(read_value(loading_value) == 19);
  assert(get_constructor_calls() == 1);
  pthread_barrier_wait(&barrier);
  assert(pthread_join(existing, 0) == 0);
  assert(pthread_join(subsequent, 0) == 0);
  assert(pthread_barrier_destroy(&barrier) == 0);
  assert(pthread_barrier_destroy(&existing_ready) == 0);
  return 0;
}
