// REQUIRES: ptsan-lam-u48-runtime
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -fno-discard-value-names -I%crt_src/include -o normal %s
// RUN: ./normal normal
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -fno-discard-value-names -mllvm -ptsan-lam -I%crt_src/include -o lam %s
// RUN: ./lam lam

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "Inputs/ptsan_test_interface.h"

static void assert_top_bit_clear(uint64_t value) {
  assert((value >> 63) == 0);
}

int main(int argc, char **argv) {
  assert(argc == 2);

  if (strcmp(argv[1], "normal") == 0) {
    assert(ptsan_id_bits() == 16);
    assert(ptsan_default_id() == 0xffff);
    assert(ptsan_get_base(ptsan_default_id()) == 0xffff000000000000ULL);
    return 0;
  }

  assert(strcmp(argv[1], "lam") == 0);
  assert(ptsan_id_bits() == 15);
  assert(ptsan_default_id() == 0x7fff);
  assert(ptsan_get_base(ptsan_default_id()) == 0x7fff000000000000ULL);
  assert_top_bit_clear(ptsan_get_base(ptsan_default_id()));
  return 0;
}
