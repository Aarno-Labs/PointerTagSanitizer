//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %S/tls_threads.c %t
// RUN: %ptsan_musl_strict -pthread -DREPEAT_TLS_DTOR -o %t/test \
// RUN:   %t/main.c %t/access.c
// RUN: %t/test

// A valid TLS access in the final key-destructor iteration must still work.
// Musl's final exit boundary must retire IDs only after every destructor.
