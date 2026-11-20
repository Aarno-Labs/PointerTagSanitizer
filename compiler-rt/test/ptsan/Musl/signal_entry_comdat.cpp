//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t
// RUN: %ptsanxx_musl_nonstrict -std=c++17 -O2 %t/other.cpp %t/main.cpp -o %t/id16
// RUN: %t/id16
// RUN: %ptsanxx_musl_id32_nonstrict -std=c++17 -O2 %t/other.cpp %t/main.cpp -o %t/id32
// RUN: %t/id32
// RUN: %ptsanxx_musl_id32_strict -std=c++17 -O2 %t/other.cpp %t/main.cpp -o %t/strict
// RUN: %t/strict

// Both translation units emit the inline variable's initializer and handler.
// Signal wrappers and registration records must be discarded with duplicate
// COMDAT definitions, while the surviving handler remains usable by sigaction.

//--- shared.h
#include <signal.h>

int initial_value();
inline int initialized = initial_value();
extern volatile sig_atomic_t seen;

inline void handler(int sig) { seen = sig; }
using Handler = void (*)(int);
Handler other_handler();

//--- other.cpp
#include "shared.h"

int initial_value() { return 42; }
Handler other_handler() { return handler; }

//--- main.cpp
#include "shared.h"
#include <assert.h>

volatile sig_atomic_t seen;

int main() {
  assert(initialized == 42);
  Handler other = other_handler();
  assert(other == handler);
  Handler previous = signal(SIGUSR1, handler);
  assert(previous != SIG_ERR);
  int result = raise(SIGUSR1);
  assert(result == 0);
  assert(seen == SIGUSR1);
}
