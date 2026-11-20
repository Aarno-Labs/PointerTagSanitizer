// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt

// CHECK-DAG: Hello world 42
// CHECK-DAG: 7 x -17
// CHECK-DAG: [    x]-[3.142]-(-9)-(12345)
// CHECK-DAG: ptr=0x{{[0-9A-Fa-f]+}}
// CHECK-DAG: {{^BEGIN:.*:END$}}
// CHECK: OK

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <locale.h>

__attribute__((noinline))
static int call_vasprintf(char **out, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vasprintf(out, fmt, ap);
  va_end(ap);
  return n;
}

__attribute__((noinline))
static void test_asprintf_basic(void) {
  char *s = NULL;
  if (asprintf(&s, "Hello %s %d", "world", 42) < 0 || s == NULL) {
    fprintf(stderr, "asprintf_basic failed\n");
    exit(1);
  }
  printf("%s\n", s);
  free(s);
}

__attribute__((noinline))
static void test_asprintf_positional(void) {
  char *s = NULL;
  if (asprintf(&s, "%2$d %1$s %3$jd", "x", 7, (intmax_t)-17) < 0 || s == NULL) {
    fprintf(stderr, "asprintf_positional failed\n");
    exit(1);
  }
  printf("%s\n", s);
  free(s);
}

__attribute__((noinline))
static void test_vasprintf_width_precision(void) {
  char *s = NULL;
  int width = 5;
  int prec = 3;
  if (call_vasprintf(&s, "[%*s]-[%.*f]-(%jd)-(%zu)",
                     width, "x", prec, 3.14159, (intmax_t)-9, (size_t)12345) < 0 || s == NULL) {
    fprintf(stderr, "vasprintf_width_precision failed\n");
    exit(1);
  }
  printf("%s\n", s);
  free(s);
}

__attribute__((noinline))
static void test_vasprintf_pointer(void) {
  char *s = NULL;
  void *p = &s;
  if (call_vasprintf(&s, "ptr=%p", p) < 0 || s == NULL) {
    fprintf(stderr, "vasprintf_pointer failed\n");
    exit(1);
  }
  printf("%s\n", s);
  free(s);
}

__attribute__((noinline))
static void test_asprintf_large_output(void) {
  size_t N = 8192;  // 8 KiB payload
  char *src = (char *)malloc(N + 1);
  if (src == NULL) {
    fprintf(stderr, "malloc failed\n");
    exit(1);
  }
  for (size_t i = 0; i < N; i++) {
    src[i] = (char)('a' + (i % 26));
  }
  src[N] = '\0';

  char *s = NULL;
  if (asprintf(&s, "BEGIN:%s:END", src) < 0 || s == NULL) {
    fprintf(stderr, "asprintf_large_output failed\n");
    free(src);
    exit(1);
  }
  printf("%s\n", s);
  free(s);
  free(src);
}

__attribute__((noinline))
int main(void) {
  /* Ensure decimal point is '.' for predictable "%.3f" */
  setlocale(LC_ALL, "C");

  test_asprintf_basic();
  test_asprintf_positional();
  test_vasprintf_width_precision();
  test_vasprintf_pointer();
  test_asprintf_large_output();
  puts("OK");
  return 0;
}
