//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#include <stddef.h>
#include <stdint.h>

extern void *__tls_get_addr(size_t *index);
extern _Thread_local int resolver_tls_value;

uintptr_t native_tls_base(void) {
  size_t index[2] = {1, 0};
  return (uintptr_t)__tls_get_addr(index);
}

int native_tls_value(void) {
  return resolver_tls_value;
}
