//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise a user-provided pthread stack across musl's uninstrumented
// __copy_tls boundary.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -pthread -o %t %s
// RUN: %run %t

#include <pthread.h>
#include <stddef.h>
#include <sys/mman.h>

static const size_t stack_size = 1024 * 1024;

static void *pthreadChild(void *opaque) {
  int *value = (int *)opaque;
  *value = 42;
  return opaque;
}

int main(void) {
  void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED) {
    return 1;
  }

  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) {
    return 2;
  }
  if (pthread_attr_setstack(&attributes, stack, stack_size) != 0) {
    return 3;
  }

  int value = 0;
  pthread_t thread;
  if (pthread_create(&thread, &attributes, pthreadChild, &value) != 0) {
    return 4;
  }
  pthread_attr_destroy(&attributes);

  void *result = NULL;
  if (pthread_join(thread, &result) != 0 || result != &value || value != 42) {
    return 5;
  }

  if (munmap(stack, stack_size) != 0) {
    return 6;
  }
  return 0;
}
