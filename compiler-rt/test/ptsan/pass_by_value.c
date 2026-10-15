// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 0 > first.txt 2>&1
// RUN: FileCheck %s --check-prefix=GOOD < first.txt
// RUN: ./%exe 2047 > last.txt 2>&1
// RUN: FileCheck %s --check-prefix=GOOD < last.txt

// GOOD-NOT: Found bad {{.*}} access
// GOOD: value =

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  char payload[2048];
} large_blob_t;

__attribute__((noinline))
unsigned char read_blob(large_blob_t blob, int index) {
  return blob.payload[index];
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;

  large_blob_t blob;
  for (size_t i = 0; i < sizeof(blob.payload); ++i)
    blob.payload[i] = (char)i;

  int index = atoi(argv[1]);
  printf("value = %u\n", read_blob(blob, index));
  return 0;
}
