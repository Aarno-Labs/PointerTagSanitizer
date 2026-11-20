// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD

// RUN: %clang_ptsan_id32 -g -O2 -o %exe %s
// RUN: ./%exe 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD

// BAD: Found bad
// GOOD-NOT: Found bad
// Don't care exaclty how many here, but it should be small
// GOOD: Total checks called: {{[1-5]}}

// XXX: should clean this up a bit

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

static const size_t REPS = 1;
// Exercise the quadratic recurrence without requiring an object above 2 GiB.
static const unsigned N    = 10000u;

// Include triangular(N), read by the final scatter/gather iteration.
static const size_t TSIZE = ((size_t)N * ((size_t)N + 1u)) / 2u + 1;

static int sink;

__attribute__((noinline))
void tri_fill(int *a, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    // Non-affine (quadratic) index: i*(i+1)/2
    unsigned idx = (i * (i + 1u)) / 2u;
    a[idx] = (int)i;
  }
}


__attribute__((noinline))
int tri_sum(const int *a, unsigned n) {
  int s = 0;
  for (unsigned i = 0; i < n; i++) {
    unsigned idx = (i * (i + 1u)) / 2u;
    s += a[idx];
  }
  return s;
}

__attribute__((noinline))
int tri_scatter_gather(int *a, unsigned n) {
  int s = 0;
  for (unsigned i = 0; i < n; i++) {
    unsigned widx = (i * (i + 1u)) / 2u;      // write at triangular(i)
    unsigned ridx = ((i + 1u) * (i + 2u)) / 2u; // read at triangular(i+1)
    a[widx] = (int)(i * 3u + 1u);
    s += a[ridx];
  }
  return s;
}
int main(void) {
  // Allocate a big triangular array; TSIZE fits in size_t by construction.
  int *A = (int *)malloc(TSIZE * sizeof(*A));
  if (!A) {
    return 1;
  }

  tri_fill(A, N);
  // tri_fill stops at triangular(N - 1).
  A[TSIZE - 1] = 0;
  for (int i = 0; i < REPS; i++) {
    int s1 = tri_sum(A, N);
    int s2 = tri_scatter_gather(A, N);

    sink += s1;
    sink += s2;
  }

  free(A);

  printf("Got %d\n", sink);
  return 0;
}
