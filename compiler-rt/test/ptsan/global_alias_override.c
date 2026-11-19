//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Alias pointers and bounds must follow the definition selected by the linker,
// including a strong definition replacing a weak alias in another translation
// unit. Unoverridden aliases must still share their target's identity. Check
// both valid accesses and the selected object's out-of-bounds diagnostic.
// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O2 -g -mllvm -ptsan-abort-on-error %t.dir/aliases.c %t.dir/main.c -o %t 2> %t.compile.log
// RUN: FileCheck %s --check-prefix=COMPILE --allow-empty < %t.compile.log
// RUN: %run %t
// RUN: not %run %t overflow 2>&1 | FileCheck %s
// RUN: %clang_ptsan_id32 -O2 -g -mllvm -ptsan-abort-on-error %t.dir/aliases.c %t.dir/main.c -o %t.id32 2> %t.id32.compile.log
// RUN: FileCheck %s --check-prefix=COMPILE --allow-empty < %t.id32.compile.log
// RUN: %run %t.id32
// RUN: not %run %t.id32 overflow 2>&1 | FileCheck %s

// COMPILE-NOT: WARNING: found non-expr, non-aggregate global-using constant:

// CHECK: Found bad
// CHECK: Alloc size  0x10
// CHECK: Access size 0x4
// CHECK: Min-Base = 16

//--- aliases.c
static int fallback[4] = {17, 18, 19, 20};
extern int overridden[4] __attribute__((weak, alias("fallback")));
extern int retained[4] __attribute__((weak, alias("fallback")));
static int strong_fallback = 23;
extern int strong __attribute__((alias("strong_fallback")));

int *get_overridden(void) { return overridden; }
int *get_retained(void) { return retained; }
int *get_retained_element(void) { return retained + 2; }
int *get_strong(void) { return &strong; }
int *get_strong_fallback(void) { return &strong_fallback; }
int *get_fallback(void) { return fallback; }
int read_overridden(int index) { return overridden[index]; }

//--- main.c
#include <assert.h>

int overridden[4] = {42, 43, 44, 45};
int *get_overridden(void);
int *get_retained(void);
int *get_retained_element(void);
int *get_strong(void);
int *get_strong_fallback(void);
int *get_fallback(void);
int read_overridden(int index);

int main(int argc, char **argv) {
  if (argc > 1) {
    // Keep the index unknown in the separately compiled accessor.
    volatile int value = read_overridden(4);
    (void)value;
    assert(0 && "missing bounds complaint");
  }
  assert(read_overridden(0) == 42);
  assert(get_overridden() == overridden);
  assert(*get_overridden() == 42);
  assert(get_retained() == get_fallback());
  assert(*get_retained() == 17);
  assert(get_retained_element() == get_fallback() + 2);
  assert(*get_retained_element() == 19);
  assert(get_strong() == get_strong_fallback());
  assert(*get_strong() == 23);
  return 0;
}
