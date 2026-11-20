//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Dynamically initialized TLS needs an ID before its constructor receives this,
// including constructors of subobjects. Check that IDs are stable within each
// thread, distinct between threads, and still enforce the object's bounds.
// The constructor is in another translation unit so its ID checks cannot fold
// into the initialization code.
// RUN: split-file %s %t
// RUN: %clangxx_ptsan -O2 -I%S -pthread -mllvm -ptsan-abort-on-error %t/main.cpp %t/value.cpp -o %t/ordinary
// RUN: %run %t/ordinary
// RUN: not %run %t/ordinary bad 2>&1 | FileCheck %s
// RUN: %clangxx_ptsan -O0 -I%S -pthread -mllvm -ptsan-abort-on-error %t/main.cpp %t/value.cpp -o %t/o0
// RUN: %run %t/o0
// RUN: %clangxx_ptsan_id32 -O2 -I%S -pthread %t/main.cpp %t/value.cpp -o %t/id32
// RUN: %run %t/id32
// RUN: not %run %t/id32 bad 2>&1 | FileCheck %s
// CHECK: Found bad POINT access

//--- value.h
struct Value {
  Value();
  int number;
  unsigned constructor_id;
};

int read_number(const volatile int *p, int index);

//--- value.cpp
#include "value.h"
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>

// Keep the constructor opaque to the defining translation unit. The TLS
// address must carry its ID across the call, not just at later access sites.
Value::Value() {
  unsigned id = get_id(this);
  assert(id != 0 && id != ptsan_default_id());
  number = 42;
  constructor_id = id;
}

int read_number(const volatile int *p, int index) { return p[index]; }

//--- main.cpp
#include "value.h"
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <pthread.h>

thread_local Value value;

struct Holder {
  int prefix = 7;
  Value member;
};
thread_local Holder holder;

static unsigned check_value() {
  assert(value.number == 42);
  unsigned id = get_id(&value);
  assert(id == value.constructor_id);
  assert(holder.prefix == 7 && holder.member.number == 42);
  assert(get_id(&holder) == holder.member.constructor_id);
  assert(get_id(&holder) != id);
  return id;
}

static void *worker(void *arg) {
  unsigned *id = (unsigned *)arg;
  *id = check_value();
  return nullptr;
}

int main(int argc, char **argv) {
  unsigned main_id = check_value();
  unsigned worker_id = 0;
  pthread_t thread;
  assert(pthread_create(&thread, nullptr, worker, &worker_id) == 0);
  assert(pthread_join(thread, nullptr) == 0);
  assert(worker_id != main_id);
  assert(check_value() == main_id);
  if (argc > 1) {
    (void)read_number(&value.number, sizeof(Value) / sizeof(int));
    assert(0 && "TLS bounds must still be checked");
  }
}
