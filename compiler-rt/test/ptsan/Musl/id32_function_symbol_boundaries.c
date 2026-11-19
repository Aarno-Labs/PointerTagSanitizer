//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Function symbols must not be tagged as unregistered data when passed or
// returned. In markerless strict ID32, tagging a code address with ID zero
// happens to preserve addresses below 4 GiB, but turns larger ones into null.
// Put the target above 4 GiB and use separate translation units to retain both
// boundaries. Call the resulting pointers rather than comparing them, avoiding
// the separate high-function-pointer comparison limitation.
// REQUIRES: ptsan-instrumented-musl, x86_64-target-arch
// RUN: split-file %s %t.dir
// RUN: %ptsan_musl_id32_strict -mcmodel=large -Wl,--section-start=.hightext=0x1000000000 -Wl,--no-eh-frame-hdr %t.dir/main.c %t.dir/helper.c -o %t
// RUN: llvm-nm %t | FileCheck %s --check-prefix=ADDRESS
// RUN: %t
// RUN: %t pass
// ADDRESS: 0000001000000000 T target

//--- main.c
typedef int (*Callback)(void);
extern int target(void);
extern Callback get_target(void);
extern int invoke(Callback callback);

int main(int argc, char **argv) {
  // The first run tests returning a symbol; the second tests passing one.
  if (argc == 1) {
    return get_target()() != 42;
  }
  return invoke(target) != 42;
}

//--- helper.c
typedef int (*Callback)(void);

__attribute__((section(".hightext"))) int target(void) { return 42; }

Callback get_target(void) { return target; }

int invoke(Callback callback) { return callback(); }
