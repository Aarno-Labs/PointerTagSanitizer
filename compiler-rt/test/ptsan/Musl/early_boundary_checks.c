//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Metadata and globals are ready before libc-dependent runtime initialization
// finishes. Early constructors must use checked string and syscall boundaries
// in non-strict mode too, rather than bypassing checks or forwarding tagged
// pointers to the kernel. Priority 0 precedes PTSan's priority-2 finish hook.
// A variadic syscall() also exercises va_start during early startup.
// RUN: %ptsan_musl_nonstrict %s -o %t.nonstrict
// RUN: %run %t.nonstrict
// RUN: %ptsan_musl_nonstrict -DBAD_SCAN %s -o %t.nonstrict.scan
// RUN: not %run %t.nonstrict.scan 2>&1 | FileCheck %s --check-prefix=SCAN
// RUN: %ptsan_musl_nonstrict -DBAD_SYSCALL %s -o %t.nonstrict.syscall
// RUN: not %run %t.nonstrict.syscall 2>&1 | FileCheck %s --check-prefix=SYSCALL
// RUN: %ptsan_musl_strict %s -o %t.strict
// RUN: %run %t.strict
// RUN: %ptsan_musl_strict -DBAD_SCAN %s -o %t.strict.scan
// RUN: not %run %t.strict.scan 2>&1 | FileCheck %s --check-prefix=SCAN
// RUN: %ptsan_musl_strict -DBAD_SYSCALL %s -o %t.strict.syscall
// RUN: not %run %t.strict.syscall 2>&1 | FileCheck %s --check-prefix=SYSCALL
// RUN: %ptsan_musl_id32_nonstrict %s -o %t.id32
// RUN: %run %t.id32
// RUN: %ptsan_musl_id32_nonstrict -DBAD_SCAN %s -o %t.id32.scan
// RUN: not %run %t.id32.scan 2>&1 | FileCheck %s --check-prefix=SCAN
// RUN: %ptsan_musl_id32_nonstrict -DBAD_SYSCALL %s -o %t.id32.syscall
// RUN: not %run %t.id32.syscall 2>&1 | FileCheck %s --check-prefix=SYSCALL
// RUN: %ptsan_musl_id32_strict %s -o %t.id32.strict
// RUN: %run %t.id32.strict
// RUN: %ptsan_musl_id32_strict -DBAD_SCAN %s -o %t.id32.strict.scan
// RUN: not %run %t.id32.strict.scan 2>&1 | FileCheck %s --check-prefix=SCAN
// RUN: %ptsan_musl_id32_strict -DBAD_SYSCALL %s -o %t.id32.strict.syscall
// RUN: not %run %t.id32.strict.syscall 2>&1 | FileCheck %s --check-prefix=SYSCALL
// RUN: %ptsan_musl_id32_strict_indicator %s -o %t.id32.indicator
// RUN: %run %t.id32.indicator
// RUN: %ptsan_musl_id32_strict_indicator -DBAD_SCAN %s -o %t.id32.indicator.scan
// RUN: not %run %t.id32.indicator.scan 2>&1 | FileCheck %s --check-prefix=SCAN
// RUN: %ptsan_musl_id32_strict_indicator -DBAD_SYSCALL %s -o %t.id32.indicator.syscall
// RUN: not %run %t.id32.indicator.syscall 2>&1 | FileCheck %s --check-prefix=SYSCALL
// SCAN: Found bad read access
// SYSCALL: Found bad libc access

#include <assert.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static char text[] = "ok\n";
static int ran;

__attribute__((constructor(0))) static void early(void) {
  size_t (*volatile scan)(const char *) = strlen;
#ifdef BAD_SCAN
  scan(text + sizeof(text));
#elif defined(BAD_SYSCALL)
  write(STDOUT_FILENO, text, sizeof(text) + 1);
#else
  assert(scan(text) == 3);
  assert(write(STDOUT_FILENO, text, 3) == 3);
  assert(syscall(SYS_write, STDOUT_FILENO, text, 3) == 3);
#endif
  ran = 1;
}

int main(void) {
  assert(ran);
  return 0;
}
