// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: not --crash ./%exe 4300 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 4097 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 4096 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD
// RUN: ./%exe 0 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD

// BAD: Found bad
// GOOD-NOT: Found bad
// Don't care exaclty how many here, but it should be small
// GOOD: Total checks called: {{[1-5]}}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

void __attribute__((noinline)) func(float *buf, const int L)
{
	int n;
	for (n = 0; n < L; n++)
		buf[n] = 1.0f;
}

void __attribute__((noinline)) use(float *buf)
{
  printf("got %f\n", buf[43]);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <size>\n", argv[0]);
    return 1;
  }

  int L = atoi(argv[1]);

  float *buf = (float *)malloc(4096 * sizeof(float));
  func(buf, L);
  use(buf);
  free(buf);
}