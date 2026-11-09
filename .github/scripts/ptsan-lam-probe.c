#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void) {
  unsigned long bits = 0;
  if (syscall(SYS_arch_prctl, 0x4003, &bits) || bits < 15) {
    fprintf(stderr, "LAM_U48 requires at least 15 tag bits (reported %lu)\n", bits);
    return 1;
  }
  if (syscall(SYS_arch_prctl, 0x4002, 15)) { perror("enable LAM_U48"); return 1; }
  volatile unsigned long value = 42;
  volatile unsigned long *tagged = (volatile unsigned long *)((uintptr_t)&value | UINT64_C(0x1234000000000000));
  if (*tagged != 42) return 1;
  *tagged = 43;
  if (value != 43) return 1;
  puts("LAM_U48 enabled; tagged load/store passed");
  return 0;
}
