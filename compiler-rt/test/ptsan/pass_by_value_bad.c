// A pass-by-value struct is a new object in the callee and must be checked
// against the bounds of that copy.

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: FileCheck %s --check-prefix=BAD < err.txt

// BAD: Found bad POINT access
// BAD: Found bad POINT access

#include <stdio.h>

typedef struct {
  char payload[2048];
} large_blob_t;

__attribute__((noinline))
unsigned char read_blob(large_blob_t blob, int index) {
  return blob.payload[index];
}

int main(void) {
  large_blob_t blob;
  for (size_t i = 0; i < sizeof(blob.payload); ++i)
    blob.payload[i] = (char)i;

  volatile unsigned char value = read_blob(blob, -1);
  value ^= read_blob(blob, 2048);
  printf("value = %u\n", value);
  return 0;
}
