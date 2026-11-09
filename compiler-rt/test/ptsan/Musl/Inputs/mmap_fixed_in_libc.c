//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// This helper is deliberately compiled without PTSan. ptsan_in_libc is
// runtime state, not an application-facing tagged object; mutating it from the
// instrumented test would test strict global access instead of MAP_FIXED.
extern _Thread_local unsigned int ptsan_in_libc;

void ptsan_test_enter_libc(void) { ptsan_in_libc++; }

void ptsan_test_leave_libc(void) { ptsan_in_libc--; }
