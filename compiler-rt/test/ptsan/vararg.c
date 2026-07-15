// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// ERR: Unreclaimed IDs at exit: 0

// CHECK: checksum_twice = 3698

// Thanks ChatGPT!
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ────────────────────────────────────────────── *
 *  (1) Core consumer that uses **va_copy**.      *
 *      It reads the first pointer twice: once    *
 *      from the original va_list and once from   *
 *      the duplicate produced by va_copy, then   *
 *      returns the combined checksum.            *
 * ────────────────────────────────────────────── */
static __attribute__((noinline)) int checksum_twice_va(va_list ap_orig) {
  va_list ap_dup;
  va_copy(ap_dup, ap_orig); /* <─── here is the va_copy */

  const char *p1 = va_arg(ap_orig, const char *);
  const char *p2 = va_arg(ap_dup, const char *);

  int sum = 0;
  for (size_t i = 0; p1[i]; ++i)
    sum += (unsigned char)p1[i];
  for (size_t i = 0; p2[i]; ++i)
    sum += (unsigned char)p2[i];

  va_end(ap_dup);
  return sum;
}

/* ────────────────────────────────────────────── *
 *  (2) Var-arg wrapper: packs … into a va_list   *
 *      and immediately forwards it.              *
 * ────────────────────────────────────────────── */
static __attribute__((noinline)) int
checksum_twice_vararg(int dummy /* keeps prototype visible */, ...) {
  va_list ap;
  va_start(ap, dummy);
  int result = checksum_twice_va(ap);
  va_end(ap);
  return result;
}

/* ────────────────────────────────────────────── *
 *  (3) Main driver: uses a writable buffer so    *
 *      PTSan will tag it.                        *
 * ────────────────────────────────────────────── */
int main(void) {
  char buf[32];
  strcpy(buf, "PointerTagSanity-VA"); /* 19 bytes incl. NUL   */

  /*  Pass *one* pointer – the helper reads it twice thanks
        to va_copy duplicating the list before consumption.    */
  int cs = checksum_twice_vararg(0, buf);

  printf("checksum_twice = %d\n", cs);

  return 0;
}
