// We don't catch this because:
// - va_list always allocates a chunk of space for arguments, even
//   if some of them aren't filled
// - So the load of the va_list address is valid
// - Then the actual value (ptr) being loaded is garbage. But the garbage
//   has 0x0000 top, so we just accept it
// XFAIL: *
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// ERR: Unreclaimed IDs at exit: 0

// CHECK: Found bad access

// Thanks ChatGPT!
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static __attribute__((noinline)) void use_va(va_list ap) {
  const char *p1 = va_arg(ap, const char *);
  printf("Got p1: %s\n", p1);
}

/* ────────────────────────────────────────────── *
 *  (2) Var-arg wrapper: packs … into a va_list   *
 *      and immediately forwards it.              *
 * ────────────────────────────────────────────── */
static __attribute__((noinline)) void
checksum_twice_vararg(int dummy /* keeps prototype visible */, ...) {
  va_list ap;
  va_start(ap, dummy);
  use_va(ap);
  use_va(ap);
  va_end(ap);
}

/* ────────────────────────────────────────────── *
 *  (3) Main driver: uses a writable buffer so    *
 *      PTSan will tag it.                        *
 * ────────────────────────────────────────────── */
int main(void) {
  char buf[32];
  strcpy(buf, "PointerTag(in)Sanity-VA");

  checksum_twice_vararg(0, buf);

  return 0;
}
