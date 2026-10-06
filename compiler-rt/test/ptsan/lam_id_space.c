// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -fno-discard-value-names -mllvm -ptsan-lam -I%crt_src/include -o %exe %s
// RUN: ./%exe

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "Inputs/ptsan_test_interface.h"

static void assert_lam_ptr(void *ptr) {
  assert((((uint64_t)ptr) >> 63) == 0);
}

static void assert_lam_id(uint16_t id) {
  assert(id <= ptsan_default_id());
}

static void assert_lam_base(uint16_t id) {
  assert((ptsan_get_base(id) >> 63) == 0);
}

int main(void) {
  assert(ptsan_id_bits() == 15);
  assert(ptsan_default_id() == 0x7fff);

  char *buf = (char *)malloc(16);
  assert(buf);
  assert_lam_id(get_id(buf));
  assert_lam_ptr(buf);
  assert_lam_base(get_id(buf));
  free(buf);

  assert(ptsan_get_base(ptsan_default_id()) == 0x7fff000000000000ULL);
  assert_lam_base(ptsan_default_id());
  return 0;
}
