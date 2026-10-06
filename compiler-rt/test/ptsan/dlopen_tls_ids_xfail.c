//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// REQUIRES: x86_64-target-arch
// RUN: split-file %S/dlopen_tls_initialization.c %t
// RUN: %clang_ptsan -O2 -fPIC -shared %t/library.c -o %t/library.so
// RUN: %clang_ptsan -O2 -DCHECK_TLS_IDS -mllvm -ptsan-abort-on-error %t/main.c -pthread -ldl -o %t/test
// RUN: %t/test %t/library.so

// Every thread must get a stable, nonzero, nondefault ID for its own instance
// of the dlopened TLS object. Live instances in different threads need distinct
// IDs. Exercise the loading thread, a thread that predates dlopen, and one
// created afterward, using the same program as the loader-behavior test.
//
// Eager TLS initialization in module constructors and at thread startup still
// misses existing non-loading threads. Inspect IDs in ordinary mode so unrelated
// strict-mode glibc limitations do not obscure this requirement.
// The loader/thread harness is uninstrumented because the glibc barrier
// wrapper is missing. All TLS reads and writes take place in the instrumented
// library; the harness only inspects pointer IDs and passes pointers back.
