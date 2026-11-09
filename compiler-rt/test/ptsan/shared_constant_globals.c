//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -o %t %s
// RUN: %run %t

// RUN: %clang_ptsan_id32 -o %t %s
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"

#include <assert.h>
#include <stdint.h>

static const char FirstString[] = "first constant";
static const char SecondString[] = "second constant";
static const int ScalarConstant = 42;
static const int ForcedWritableConstant
    __attribute__((section(".data"))) = 43;
static int MutableGlobal;

__attribute__((noinline)) static const char *getLiteral(void) {
  return "compiler-generated literal";
}

int main(void) {
  const char *literal = getLiteral();
  PTSanID first_id = get_id(FirstString);
  PTSanID second_id = get_id(SecondString);
  PTSanID scalar_id = get_id(&ScalarConstant);
  PTSanID forced_writable_id = get_id(&ForcedWritableConstant);
  PTSanID literal_id = get_id(literal);
  PTSanID mutable_id = get_id(&MutableGlobal);

  assert(first_id != 0);
  assert(first_id != ptsan_default_id());
  assert(first_id == second_id);
  assert(first_id == scalar_id);
  assert(first_id == literal_id);

  assert(forced_writable_id != 0);
  assert(forced_writable_id != ptsan_default_id());
  assert(forced_writable_id != first_id);
  assert(ptsan_get_size(forced_writable_id) ==
         sizeof(ForcedWritableConstant));

  assert(mutable_id != 0);
  assert(mutable_id != ptsan_default_id());
  assert(mutable_id != first_id);
  assert(ptsan_get_size(mutable_id) == sizeof(MutableGlobal));

  uint64_t shared_base = ptsan_get_base(first_id);
  uint64_t shared_size = ptsan_get_size(first_id);
  uint64_t address_mask = (UINT64_C(1) << 48) - 1;
  uint64_t first_address = ptsan_get_address(FirstString);
  uint64_t second_address = ptsan_get_address(SecondString);

  shared_base &= address_mask;
  assert(shared_base <= first_address);
  assert(shared_base <= second_address);
  assert(first_address + sizeof(FirstString) <= shared_base + shared_size);
  assert(second_address + sizeof(SecondString) <= shared_base + shared_size);
  assert(shared_size > sizeof(FirstString));
  return 0;
}
