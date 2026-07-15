// Runtime characterization for cmp/arith ID policy. The same-object mode
// should not pass mismatched IDs into arithmetic validation even when ID
// clearing is disabled and strip-ID hoisting has profitable stripped siblings.
// The different-object mode is a negative control proving the validator would
// complain if mismatched IDs actually reach pointer subtraction.
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -O2 -g -fno-discard-value-names \
// RUN:   -mllvm -ptsan-runtime-validate \
// RUN:   -mllvm -ptsan-runtime-validate-arith \
// RUN:   -mllvm -ptsan-clear-ids-on-cmp=0 \
// RUN:   -mllvm -ptsan-clear-ids-on-arith=0 \
// RUN:   -I%crt_src/include -o no_clear %s
// RUN: ./no_clear same > same.out 2> same.err
// RUN: FileCheck %s --check-prefix=SAME-OUT < same.out
// RUN: FileCheck %s --check-prefix=SAME-ERR --allow-empty < same.err
// RUN: ./no_clear different > different.out 2> different.err
// RUN: FileCheck %s --check-prefix=DIFF-OUT < different.out
// RUN: FileCheck %s --check-prefix=DIFF-ERR < different.err
//
// SAME-OUT: same-object done
// SAME-ERR-NOT: WARNING: Pointer-arith-or-compare
// DIFF-OUT: different-object done
// DIFF-ERR: WARNING: Pointer-arith-or-compare pointers with different IDs

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Inputs/ptsan_test_interface.h"

static volatile uintptr_t Sink;

__attribute__((noinline)) static int direct_cmp(char *A, char *B) {
  return A < B;
}

__attribute__((noinline)) static intptr_t ptr_sub(char *A, char *B) {
  return A - B;
}

__attribute__((noinline)) static void check_same_object(char *Base) {
  int Total = 0;
  for (int I = 0; I < 64; ++I) {
    char *A = Base + (I & 15);
    char *B = Base + ((I + 3) & 15);

    // These memory uses create stripped siblings for the same values that also
    // feed no-clear cmp/arith. If hoisting leaves only one cmp/arith operand
    // stripped, runtime arithmetic validation will report mismatched IDs.
    Sink += (unsigned char)*A;
    Sink += (unsigned char)*B;

    Total += direct_cmp(A, B);
    Total += (int)ptr_sub(B, A);
  }

  Sink += Total;
}

__attribute__((noinline)) static void check_different_objects(char *A,
                                                              char *B) {
  Sink += (unsigned char)*A;
  Sink += (unsigned char)*B;

  // Direct pointer compares are currently stripped even with clear-on-cmp off,
  // so the negative control uses pointer subtraction, where no-clear arithmetic
  // leaves tagged IDs visible to runtime validation.
  Sink += (uintptr_t)ptr_sub(B, A);
}

int main(int Argc, char **Argv) {
  char *A = (char *)malloc(64);
  char *B = (char *)malloc(64);
  if (!A || !B)
    return 1;

  memset(A, 1, 64);
  memset(B, 2, 64);

  if (Argc > 1 && strcmp(Argv[1], "different") == 0) {
    check_different_objects(A + 8, B + 16);
    puts("different-object done");
  } else {
    check_same_object(A);
    puts("same-object done");
  }

  free(A);
  free(B);
  return 0;
}
