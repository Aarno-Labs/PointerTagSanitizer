// RUN: rm -rf %t && mkdir -p %t && cd %t

// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ! ./%exe 2> out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: PTsan found invalid (maybe double, maybe something else?) free!

#include <stdlib.h>

typedef struct {
  char buffer[256];
  unsigned int value;
} GlobalBuffer;

GlobalBuffer _global_buffer;

__attribute__((noinline))
char *get_buffer() {
  return _global_buffer.buffer + 128;
}

int main(int argc, char *argv[]) {
  char *g_buffer = get_buffer();
  free(g_buffer);
}
