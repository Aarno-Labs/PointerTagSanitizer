//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_raw -O2 %s -o %t.base
// RUN: %t.base 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %clang_ptsan -O2 -mllvm -ptsan-instrument-globals=0 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: %clang_ptsan -O2 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-abort-on-error %s -o %t
// RUN: %t 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %clang_ptsan -O2 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-no-opts -S -emit-llvm %s -o - | \
// RUN:   FileCheck %s --check-prefix=IR
// RUN: %clang_ptsan -O2 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-no-opts \
// RUN:   -mllvm -ptsan-abort-on-error %s -o %t.noopts
// RUN: %t.noopts 2>&1 | FileCheck %s --check-prefix=GOOD

// Protobuf's StrongPointer uses a relocation-only asm operand to retain a
// symbol without executing instructions. Its fallback message traits supply a
// function address. Stripping that address produces a computed pointer which
// the x86 "Ws" constraint cannot accept. Keep the original function symbol,
// just as for an unproxied data global. Also check that returning either to a
// tagged-use context leaves the symbol unchanged and preserves its zero ID.

// IR-LABEL: define {{.*}} @keep_symbols(
// IR-NOT: @llvm.ptrmask
// IR: call void asm sideeffect ".reloc ., BFD_RELOC_NONE, ${0:p}", "^Ws,{{.*}}"(ptr{{[^,)]*}} @target)
// IR-NOT: @llvm.ptrmask
// IR: call void asm sideeffect ".reloc ., BFD_RELOC_NONE, ${0:p}", "^Ws,{{.*}}"(ptr{{[^,)]*}} @scalar)
// IR-NOT: @llvm.ptrmask
// IR: ret void
// IR-LABEL: define {{.*}} @get_target(
// IR: ret ptr @target
// IR-LABEL: define {{.*}} @get_scalar(
// IR: ret ptr @scalar

// GOOD-NOT: Found bad
// GOOD: function-symbol-ok
// GOOD-NOT: Found bad

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef int (*Function)(int);
int scalar;

__attribute__((noinline)) int target(int value) { return value + 1; }

__attribute__((noinline)) void keep_symbols(void) {
  __asm__(".reloc ., BFD_RELOC_NONE, %p0" : : "Ws"(&target));
  __asm__(".reloc ., BFD_RELOC_NONE, %p0" : : "Ws"(&scalar));
}

__attribute__((noinline)) Function get_target(void) { return target; }

__attribute__((noinline)) int *get_scalar(void) { return &scalar; }

int main(void) {
  keep_symbols();
  Function function = get_target();
  assert(((uintptr_t)function >> 48) == 0);
  assert(((uintptr_t)get_scalar() >> 48) == 0);
  assert(function(41) == 42);
  puts("function-symbol-ok");
  return 0;
}
