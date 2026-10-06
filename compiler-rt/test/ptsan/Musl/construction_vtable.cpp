//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t
// RUN: %ptsanxx_musl_strict -O2 -mllvm -ptsan-abort-on-error %t/base.cpp %t/main.cpp -o %t/test
// RUN: %t/test

// The separately compiled base constructor uses the VTT's construction-vtable
// pointer to locate its virtual base. Initializer replay must tag that pointer.
//--- base.h
struct Base {
  int value;
};

struct Intermediate : virtual Base {
  Intermediate(int n);
  virtual int get() { return value; }
};

//--- base.cpp
#include "base.h"
Intermediate::Intermediate(int n) { value = n; }

//--- main.cpp
#include "base.h"
struct Derived : Intermediate {
  Derived(int n) : Intermediate(n) {}
};

int main(int argc, char **) {
  Derived object(argc);
  return object.get() != argc;
}
