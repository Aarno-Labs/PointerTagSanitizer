//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// RUN: %ptsan_musl_strict -o %t %S/frexp_libcall.c
// RUN: %run %t

// This valid computation should also succeed in strict mode. The backend's
// exponent temporary is created after PTSan and has no ID, so instrumented
// musl rejects frexp's write to it. One possible fix is to expand llvm.frexp
// into an explicit call and alloca before PTSan's discovery, letting ordinary
// source handling tag that temporary. No backend change would be needed.
