//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Strict mode must give TLS objects IDs before their constructors can expose
// their addresses. Run with and without PTSan optimizations so correctness
// cannot depend on permissive zero-ID accesses or an optimization repairing it.
// RUN: split-file %S/../tls_dynamic_initializer.cpp %t
// RUN: %ptsanxx_musl_strict -O2 -I%S/.. -pthread -mllvm -ptsan-abort-on-error -mllvm -ptsan-reoptimize %t/main.cpp %t/value.cpp -o %t/strict
// RUN: %run %t/strict
// RUN: not %run %t/strict bad 2>&1 | FileCheck %S/../tls_dynamic_initializer.cpp
// RUN: %ptsanxx_musl_strict -O2 -I%S/.. -pthread -mllvm -ptsan-abort-on-error -mllvm -ptsan-no-opts %t/main.cpp %t/value.cpp -o %t/noopts
// RUN: %run %t/noopts

// TLS constructors must expose tagged ID32 objects with usable bounds.
// RUN: %ptsanxx_musl_id32_strict -I%S/.. -pthread %t/main.cpp %t/value.cpp -o %t/id32
// RUN: %run %t/id32
// RUN: not %run %t/id32 bad 2>&1 | FileCheck %S/../tls_dynamic_initializer.cpp
