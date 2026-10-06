//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clangxx_raw -fsanitize=ptsan -O2 -g -S -emit-llvm -o - %s | FileCheck %s
// RUN: %clangxx_ptsan -O2 -o %exe %s
// RUN: ./%exe

// CHECK: @llvm.global_ctors = appending global
// CHECK: @llvm.used = appending global
// CHECK-NOT: store {{.*}}, ptr @llvm.global_ctors
// CHECK-NOT: store {{.*}}, ptr @llvm.used

volatile int initial_value = 42;

__attribute__((noinline)) int make_value() { return initial_value; }

struct GlobalObject {
  GlobalObject() : value(make_value()) {}
  int value;
};

template <class T> struct Holder {
  static GlobalObject object;
};

template <class T> GlobalObject Holder<T>::object;

GlobalObject *use_object() { return &Holder<int>::object; }

int main() { return use_object()->value == 42 ? 0 : 1; }
