//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang -### --target=x86_64-linux-gnu -fsanitize=ptsan %s 2>&1 | FileCheck %s
// RUN: %clang -### --target=x86_64-linux-musl -static -fsanitize=ptsan %s 2>&1 | FileCheck %s
// RUN: %clang -### --target=aarch64-linux-gnu -fsanitize=ptsan %s 2>&1 | FileCheck %s
// RUN: %clang -### --target=x86_64-linux-gnu -shared -fsanitize=ptsan %s 2>&1 | FileCheck %s
// CHECK: "-T" "{{[^"]*}}ptsan-init-ro.ld"

// RUN: not %clang -### --target=x86_64-linux-gnu -fsanitize=ptsan -fuse-ld=gold \
// RUN:   -B%S/Inputs/basic_freebsd_tree/usr/bin %s 2>&1 | FileCheck %s --check-prefix=GOLD
// RUN: not %clang -### --target=x86_64-linux-musl -static -fsanitize=ptsan -fuse-ld=gold \
// RUN:   -B%S/Inputs/basic_freebsd_tree/usr/bin %s 2>&1 | FileCheck %s --check-prefix=GOLD
// RUN: not %clang -### --target=x86_64-linux-gnu -fsanitize=ptsan \
// RUN:   --ld-path=%S/Inputs/basic_freebsd_tree/usr/bin/ld.gold %s 2>&1 | FileCheck %s --check-prefix=GOLD
// GOLD: error: the gold linker is not supported with '-fsanitize=ptsan'; use '-fuse-ld=bfd' or '-fuse-ld=lld'

// --ld-path takes precedence over -fuse-ld, and compile-only jobs do not link.
// RUN: %clang -### --target=x86_64-linux-gnu -fsanitize=ptsan -fuse-ld=gold \
// RUN:   --ld-path=%S/Inputs/basic_freebsd_tree/usr/bin/ld.bfd %s 2>&1 | FileCheck %s --check-prefixes=CHECK,NO-ERROR
// RUN: %clang -### --target=x86_64-linux-gnu -fsanitize=ptsan -fuse-ld=gold \
// RUN:   -c %s 2>&1 | FileCheck %s --check-prefix=NO-ERROR
// NO-ERROR-NOT: error:

// RUN: not %clang -### --target=x86_64-linux-gnu -fsanitize=ptsan -static %s \
// RUN:   2>&1 | FileCheck %s --check-prefix=STATIC-GLIBC
// RUN: not %clang -### --target=aarch64-linux-gnu -fsanitize=ptsan -static-pie %s \
// RUN:   2>&1 | FileCheck %s --check-prefix=STATIC-GLIBC
// STATIC-GLIBC: error: static glibc linking is not supported with '-fsanitize=ptsan'; use a PTSan musl configuration

// Compile-only jobs and a static C++ library do not imply static libc linking.
// RUN: %clang -### --target=x86_64-linux-gnu -fsanitize=ptsan -static -c %s \
// RUN:   2>&1 | FileCheck %s --check-prefix=NO-ERROR
// RUN: %clang -### --target=x86_64-linux-gnu -fsanitize=ptsan -static-libstdc++ %s \
// RUN:   2>&1 | FileCheck %s --check-prefixes=CHECK,NO-ERROR
