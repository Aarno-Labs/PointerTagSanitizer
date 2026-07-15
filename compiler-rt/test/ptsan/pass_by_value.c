// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// ERR: Unreclaimed IDs at exit: 0

// CHECK: checksum = 261120

// Thanks ChatGPT!
#include <stdio.h>
#include <stdint.h>

/* A wrapper struct: raw arrays themselves cannot be passed by value.      */
typedef struct {
  char payload[2048];          /* 2 KiB – big enough to trigger memcpy  */
} large_blob_t;

/* ----------- callee: receives the struct *by value* -------------------- */
__attribute__((noinline))
uint64_t work(large_blob_t blob)      /* ← copy happens here              */
{
  uint64_t sum = 0;
  for (size_t i = 0; i < sizeof(blob.payload); ++i)
    sum += (unsigned char)blob.payload[i];
  return sum;
}

/* ------------------------------ driver --------------------------------- */
int main(void)
{
  large_blob_t blob;
  for (size_t i = 0; i < sizeof(blob.payload); ++i)
    blob.payload[i] = (char)i;

  uint64_t checksum = work(blob);      /* struct passed by value */

  printf("checksum = %llu\n", (unsigned long long)checksum);
  return 0;
}
