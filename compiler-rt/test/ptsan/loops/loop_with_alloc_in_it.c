// Test to make sure this compiles and doesn't explode
// because backedge_base doesn't dominate its use

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s

#include <stdint.h>
#include <stdlib.h>

__attribute__((noinline))
static char *preheader_base(size_t n) {
  // no cast in C
  char *p = malloc(n);
  // force something observable so the call stays
  if (!p) abort();
  return p;
}

__attribute__((noinline))
static char *backedge_base(size_t n) {
  char *p = malloc(n);
  if (!p) abort();
  return p;
}

__attribute__((noinline))
static void escape_char(volatile char x) {
  (void)x;
}

int main(int argc, char **argv) {
  int N = (argc > 1) ? (int)(uintptr_t)argv[1] : 1000;

  char *p = preheader_base(1024);     // preheader incoming (dominates)
  // i is the IV; we use it in the address calc
  for (int i = 0; i < N; i++) {
    // ----- "Start": use derived from PHI’d pointer and IV -----
    // Mix in IV to keep a GEP with a dynamic index.
    // volatile read to keep the GEP/use alive under -O2.
    escape_char(*(volatile char *)(p + (i & 15)));

    // Rebind p on the back-edge to a *new* base that does NOT dominate header.
    // Placing this after the use ensures the latch edge carries a different base.
    if ((i & 1) == 0) {
      char *q = backedge_base(2048);
      // make the branch unpredictable so the compiler keeps both paths
      if (((uintptr_t)q & 1u) == 0u) {
        p = q;                        // latch incoming = fresh malloc (non-dominating)
      }
    }
  }

  // prevent early free from simplifying anything
  return 0;
}
