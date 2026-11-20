// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 1> out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: Policy -1

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

__attribute__((noinline))
void OS_BSP_ConsoleOutput_Impl(const char *Str, size_t DataLen)
{
    write(1, Str, DataLen);
}
__attribute__((noinline))
void OS_DebugPrintf(const char *Func, uint32_t Line,
                    const char *Format, ...) {
  char buffer[132];
  va_list va;
  va_start(va, Format);
  vsnprintf(buffer, sizeof(buffer), Format, va);
  va_end(va);

  OS_BSP_ConsoleOutput_Impl(buffer, strlen(buffer));
}

int main(void) {
  int sched_policy = -1;

  OS_DebugPrintf("Main ", 36, "Policy %d\n",
  sched_policy);
}