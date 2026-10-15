// Regression test for the espresso failure mode. A loop-preanalysis trip-count
// SCEV contains loads from a tagged struct pointer. Those loads must not be
// rebuilt unsafely in generated preanalysis code; exact capture and safe
// fallback are both valid.
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -O3 -g -fno-discard-value-names \
// RUN:   -mllvm -ptsan-reoptimize \
// RUN:   -mllvm -ptsan-runtime-validate \
// RUN:   -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -o %exe %s
// RUN: ./%exe

#include <stdlib.h>
#include <stdio.h>

typedef struct family {
  int wsize;
  int sf_size;
  int capacity;
  int count;
  int active_count;
  unsigned *data;
} family;

static int copy_words(unsigned *d, unsigned *s, long n) {
  for (long i = 0; i < n; ++i)
    *d++ = *s++;
  return 0;
}

__attribute__((noinline)) family *copy_family(family *r, family *a) {
  r->sf_size = a->sf_size;
  r->wsize = a->wsize;
  r->count = a->count;
  r->active_count = a->active_count;
  copy_words(r->data, a->data, (long)a->wsize * a->count);
  return r;
}

int main(void) {
  family *a = malloc(sizeof(*a));
  family *r = malloc(sizeof(*r));

  a->wsize = 4;
  a->sf_size = 4;
  a->capacity = 4;
  a->count = 64;
  a->active_count = 64;
  a->data = malloc(sizeof(unsigned) * a->wsize * a->count);
  r->data = malloc(sizeof(unsigned) * a->wsize * a->count);

  for (int i = 0; i < a->wsize * a->count; ++i)
    a->data[i] = (unsigned)i;

  copy_family(r, a);

  unsigned sum = 0;
  for (int i = 0; i < a->wsize * a->count; ++i)
    sum += r->data[i];

  printf("%u\n", sum);
  return sum == 32640 ? 0 : 1;
}
