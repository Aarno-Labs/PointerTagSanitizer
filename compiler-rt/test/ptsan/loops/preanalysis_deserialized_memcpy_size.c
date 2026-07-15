// Regression test for the automotive-susan failure mode. Loop preanalysis
// deserializes SCEVs for a memcpy-heavy border-copy loop that depends on loads
// from tagged size pointers. Those loads must be recovered by lookup or left to
// normal checks rather than rebuilt unsafely in generated preanalysis code.
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
#include <string.h>
#include <stdio.h>

typedef unsigned char uchar;

__attribute__((noinline)) int enlarge_like(uchar **in, uchar *tmp_image,
                                           int *x_size, int *y_size,
                                           int border) {
  int i, j;

  for (i = 0; i < *y_size; i++)
    memcpy(tmp_image + (i + border) * (*x_size + 2 * border) + border,
           *in + i * *x_size, *x_size);

  for (i = 0; i < border; i++) {
    memcpy(tmp_image + (border - 1 - i) * (*x_size + 2 * border) + border,
           *in + i * *x_size, *x_size);
    memcpy(tmp_image + (*y_size + border + i) * (*x_size + 2 * border) +
               border,
           *in + (*y_size - i - 1) * *x_size, *x_size);
  }

  for (i = 0; i < border; i++)
    for (j = 0; j < *y_size + 2 * border; j++) {
      tmp_image[j * (*x_size + 2 * border) + border - 1 - i] =
          tmp_image[j * (*x_size + 2 * border) + border + i];
      tmp_image[j * (*x_size + 2 * border) + *x_size + border + i] =
          tmp_image[j * (*x_size + 2 * border) + *x_size + border - 1 - i];
    }

  *x_size += 2 * border;
  *y_size += 2 * border;
  *in = tmp_image;
  return 0;
}

volatile int global_border = 7;
volatile int global_x = 32;
volatile int global_y = 32;

int main(void) {
  int *x = malloc(sizeof(*x));
  int *y = malloc(sizeof(*y));
  *x = global_x;
  *y = global_y;

  int border = global_border;
  uchar *in = malloc((size_t)*x * *y);
  uchar *tmp = malloc((size_t)(*x + 2 * border) * (*y + 2 * border));
  memset(in, 3, (size_t)*x * *y);

  enlarge_like(&in, tmp, x, y, border);

  unsigned v = in[(size_t)border * *x + border];
  printf("%u\n", v);
  return v == 3 ? 0 : 1;
}
