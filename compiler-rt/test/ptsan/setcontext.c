// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s --check-prefix=OUT

// ERR: Unreclaimed IDs at exit: 0

// OUT: main: before swapcontext(main_ctx, child_ctx)
// OUT: child: in child context, calling setcontext(main_ctx)
// OUT: main: back in main after setcontext from child


#define _XOPEN_SOURCE 700
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static ucontext_t main_ctx;
static ucontext_t child_ctx;

__attribute__((noinline))
static void child(void) {
  puts("child: in child context, calling setcontext(main_ctx)");
  if (setcontext(&main_ctx) == -1) {
    perror("setcontext");
    exit(1);
  }

  /* We should never get here if setcontext succeeds. */
  puts("child: ERROR: returned from setcontext");
  exit(2);
}

inline void* clear_id(void* ptr) {
  return (void *)((uint64_t)ptr & 0x0000ffffffffffff);
}

__attribute__((noinline))
int main(void) {
  /* Separate stack for the child context. */
  char child_stack[64 * 1024];

  /* Initialize child_ctx with current context. */
  if (getcontext(&child_ctx) == -1) {
    perror("getcontext(child)");
    return 1;
  }

  child_ctx.uc_link = NULL;
  child_ctx.uc_stack.ss_sp = child_stack;
  child_ctx.uc_stack.ss_size = sizeof(child_stack);
  child_ctx.uc_stack.ss_flags = 0;

  /* Build a context that starts at child(). */
  makecontext(&child_ctx, child, 0);

  /* Also exercise getcontext on main_ctx explicitly. */
  if (getcontext(&main_ctx) == -1) {
    perror("getcontext(main)");
    return 1;
  }

  puts("main: before swapcontext(main_ctx, child_ctx)");

  /* Save current context into main_ctx and switch to child_ctx. */
  if (swapcontext(&main_ctx, &child_ctx) == -1) {
    perror("swapcontext");
    return 1;
  }

  /* Execution returns here after child calls setcontext(&main_ctx). */
  puts("main: back in main after setcontext from child");

  return 0;
}
