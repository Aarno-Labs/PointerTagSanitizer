
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// CHECK: Clearly inbounds
// CHECK-NOT: Found bad
// CHECK: Over!
// CHECK: Found bad

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((noinline))
static void normalize_crlf(char *wholeFile) {
  char *s, *d;
  for (s = d = wholeFile; d == wholeFile || *(d - 1); ) {
    if (*s == '\r' && *(s + 1) == '\n') {
      s++;
    } else if (*s == '\r') {
      s++;
      *d++ = '\n';
    } else {
      *d++ = *s++;
    }
  }
}

/*
 * Case 1: clearly in-bounds
 *
 * - Buffer is exactly the size of the literal, including the terminating '\0'.
 * - Every *(s+1) stays within the allocated region.
 */
__attribute__((noinline))
static void test_clearly_inbounds(void) {
  const char *src = "line1\r\nline2\r\n";
  size_t len = sizeof("line1\r\nline2\r\n");  /* includes '\0' */

  char *buf = malloc(len);
  memcpy(buf, src, len);

  normalize_crlf(buf);
  puts(buf);

  free(buf);
}

/*
 * Case 2: almost over (touches last valid byte, still in-bounds)
 *
 * - Allocates exactly 2 bytes: {'\r', '\0'}.
 * - On first iteration, s points at buf[0] == '\r'.
 *   *(s+1) reads buf[1] == '\0', which is the last valid byte.
 */
__attribute__((noinline))
static void test_almost_over(void) {
  size_t len = 2;
  char *buf = malloc(len);

  buf[0] = '\r';
  buf[1] = '\0';

  normalize_crlf(buf);
  puts(buf);

  free(buf);
}

/*
 * Case 3: actual off-by-one overflow
 *
 * - Allocates exactly 1 byte: buf[0] == '\r'.
 * - No terminating '\0'.
 * - On first iteration, s points at buf[0] == '\r'.
 *   The condition `*s=='\r' && *(s+1)=='\n'` evaluates *(s+1),
 *   dereferencing buf[1], which is one past the end: off-by-one read.
 */
__attribute__((noinline))
static void test_off_by_one_overflow(void) {
  size_t len = 1;
  char *buf = malloc(len);

  buf[0] = '\r';

  normalize_crlf(buf);

  free(buf);
}

__attribute__((noinline))
int main(void) {
  fprintf(stderr, "Clearly inbounds\n");
  test_clearly_inbounds();
  fprintf(stderr, "Almost over\n");
  test_almost_over();
  fprintf(stderr, "Over!\n");
  test_off_by_one_overflow();
  return 0;
}
