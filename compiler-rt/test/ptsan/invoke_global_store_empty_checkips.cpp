// RUN: %clangxx_raw -fsanitize=ptsan -g -m64 -O3 -std=c++03 -fexceptions \
// RUN:   -mllvm -ptsan-reoptimize \
// RUN:   -mllvm -ptsan-clear-ids-on-arith=0 \
// RUN:   -mllvm -ptsan-clear-ids-on-cmp=0 \
// RUN:   -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 \
// RUN:   -mllvm -ptsan-no-loop-check-hoist \
// RUN:   -mllvm -ptsan-no-check-merge \
// RUN:   -mllvm -ptsan-no-filter-removable-checks \
// RUN:   -c %s -o %t.o

// Reproduces a PTSan CheckIPs assertion for a global store after an invoke.
static bool initialized = false;

extern void may_throw();
extern void init2();

int repro() {
  try {
    may_throw();
    init2();
    initialized = true;
  } catch (...) {
  }

  return initialized ? 0 : -1;
}
