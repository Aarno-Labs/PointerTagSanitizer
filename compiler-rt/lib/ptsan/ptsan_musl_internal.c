//===-- ptsan_musl_internal.c - Access to musl private state --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Keep musl's private headers and layout-dependent operations in C.
// ID allocation and pointer tagging remain in the C++ runtime.
#include "src/include/features.h"
#include "src/internal/libc.h"
#include "src/internal/pthread_impl.h"
#include "ptsan_musl_internal.h"

size_t *ptsan_musl_get_auxv(void) {
  return __libc.auxv;
}

void ptsan_musl_set_auxv(size_t *auxv) {
  __libc.auxv = auxv;
}

// Do not use __tls_get_addr here: musl's resolver itself needs pthread_self.
static __thread pthread_t TaggedSelf __attribute__((tls_model("initial-exec")));

void *ptsan_musl_self(void) {
  // Bootstrap consumers run before IDs exist. The ABI self slot stays raw:
  // x86-64 TLS addressing also reads it via %fs:0.
  return TaggedSelf ? TaggedSelf : __pthread_self();
}

size_t ptsan_musl_thread_size(void) {
  return sizeof(struct pthread);
}

void ptsan_musl_init_main_thread(void *tagged) {
  pthread_t self = __pthread_self();
  TaggedSelf = tagged;
  self->prev = tagged;
  self->next = tagged;
}

void ptsan_musl_init_child_thread(void *raw, void *tagged) {
  // musl defines TLS_ABOVE_TP for layouts such as AArch64. The incoming
  // pointers are TP_ADJ(pthread); undo musl's adjustment to recover the
  // pthread base while preserving the tagged pointer's ID.
#ifdef TLS_ABOVE_TP
  raw = (char *)raw - sizeof(struct pthread) - TP_OFFSET;
  tagged = (char *)tagged - sizeof(struct pthread) - TP_OFFSET;
#endif
  // Static TLS has the same layout in every thread. Seed the child's cache
  // before clone, since musl uses pthread_self before our start trampoline.
  uintptr_t offset = (uintptr_t)&TaggedSelf - (uintptr_t)__pthread_self();
  *(pthread_t *)((uintptr_t)raw + offset) = tagged;

  // Instrumented pthread_create assigned new->self = new. This ABI slot
  // must instead contain the canonical pointer for %fs-based TLS addressing.
  ((pthread_t)raw)->self = raw;
}

void ptsan_musl_thread_stack(void **top, size_t *size) {
  pthread_t self = __pthread_self();
  *top = self->stack;
  *size = self->stack_size;
}

int ptsan_musl_is_c11_thread(const void *attr) {
  return attr == __ATTRP_C11_THREAD;
}
