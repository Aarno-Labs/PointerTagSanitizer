// REQUIRES: ptsan-lam-u48-runtime
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -fno-discard-value-names -mllvm -ptsan-lam -pthread -I%crt_src/include -o %exe %s
// RUN: ./%exe

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include "Inputs/ptsan_test_interface.h"

#undef get_id
static uint16_t get_id(void *ptr) {
  return (uint16_t)((uint64_t)ptr >> 48);
}

static void assert_lam_ptr(void *ptr) {
  assert(((uint64_t)ptr >> 63) == 0);
}

static void assert_lam_stack_ptr(void *ptr) {
  uint16_t id = get_id(ptr);
  assert(id != 0);
  assert(id != ptsan_default_id());
  assert(id <= ptsan_default_id());
  assert_lam_ptr(ptr);
  assert((ptsan_get_base(id) >> 63) == 0);
}

static int __attribute__((noinline)) use(int *ptr) {
  return ptr[0];
}

static void __attribute__((noinline)) check_stack_ids(void) {
  int a[8] = {1};
  int b[8] = {2};
  int c[8] = {3};
  int d[8] = {4};

  use(a);
  use(b);
  use(c);
  use(d);

  assert_lam_stack_ptr(a);
  assert_lam_stack_ptr(b);
  assert_lam_stack_ptr(c);
  assert_lam_stack_ptr(d);
}

static void *thread_main(void *arg) {
  (void)arg;
  check_stack_ids();
  return 0;
}

int main(void) {
  assert(ptsan_id_bits() == 15);
  assert(ptsan_default_id() == 0x7fff);
  assert((ptsan_get_base(ptsan_default_id()) >> 63) == 0);

  check_stack_ids();

  pthread_t thread;
  assert(pthread_create(&thread, 0, thread_main, 0) == 0);
  assert(pthread_join(thread, 0) == 0);

  return 0;
}
