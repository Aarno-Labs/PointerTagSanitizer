// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 19 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 20 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD
// RUN: ./%exe 25 2> out.txt
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

int __attribute__((noinline)) func(float *buf, const int L)
{
	int out = 0;
	for (int n = L; n < 4096; n++){
		out += buf[n];
  }
  return out;
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

  int L = atoi(argv[1]) - 20;

  float *buf = (float *)malloc(4096 * sizeof(float));
  int sum = func(buf, L);
  use(buf);
  printf("sum is %d\n", sum);
  free(buf);
}