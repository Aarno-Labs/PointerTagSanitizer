// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt

//CHECK: width=10 prec=0 val=12.950000
//CHECK:   %*.*f : "        13"
//CHECK:   %#*.*f: "       13."
//CHECK: width=10 prec=2 val=12.950000
//CHECK:   %*.*f : "     12.95"
//CHECK:   %#*.*f: "     12.95"

// %#*.*f_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <errno.h>

__attribute__((noinline))
static char *heap_string(const char *src) {
  size_t n = strlen(src) + 1;
  char *p = malloc(n);
  if (p == NULL) {
    perror("malloc");
    exit(1);
  }
  memcpy(p, src, n);
  return p;
}

__attribute__((noinline))
static int parse_int(const char *s) {
  errno = 0;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') {
    fprintf(stderr, "bad int: \"%s\"\n", s);
    exit(2);
  }
  if (v < -2147483648L || v > 2147483647L) {
    fprintf(stderr, "int out of range: \"%s\"\n", s);
    exit(3);
  }
  return (int)v;
}

__attribute__((noinline))
static double parse_double(const char *s) {
  errno = 0;
  char *end = NULL;
  double v = strtod(s, &end);
  if (errno != 0 || end == s || *end != '\0') {
    fprintf(stderr, "bad double: \"%s\"\n", s);
    exit(4);
  }
  return v;
}

__attribute__((noinline))
static void run_case(int width, int prec, double val) {


  printf("width=%d prec=%d val=%f\n", width, prec, val);

  /* Without '#' (baseline) */
  printf("  %%*.*f : \"");
  printf("%*.*f", width, prec, val);
  printf("\"\n");

  /* With '#' — forces decimal point when prec==0 (for %%f) */
  printf("  %%#*.*f: \"");
  printf("%#*.*f", width, prec, val);
  printf("\"\n");
}

int main(void) {
  /* Ensure '.' as decimal separator regardless of host locale */
  setlocale(LC_ALL, "C");

  /* Case 1: precision 0 — '#' forces a trailing '.' */
  run_case(10, 0, 12.95);

  /* Case 2: precision > 0 — '#' has no visible effect for %f */
  run_case(10, 2, 12.95);

  return 0;
}
