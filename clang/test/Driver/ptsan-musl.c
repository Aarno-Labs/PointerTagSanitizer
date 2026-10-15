//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang -### --target=x86_64-linux-musl -static -fsanitize=ptsan %s \
// RUN:   2>&1 | FileCheck %s --check-prefix=STATIC
// RUN: %clang -### --target=x86_64-linux-musl -fsanitize=ptsan %s \
// RUN:   2>&1 | FileCheck %s --check-prefix=DYNAMIC
// RUN: %clang -### --target=aarch64-linux-musl -static -fsanitize=ptsan %s \
// RUN:   2>&1 | FileCheck %s --check-prefix=STATIC
// RUN: %clang -### --target=aarch64-linux-musl -fsanitize=ptsan %s \
// RUN:   2>&1 | FileCheck %s --check-prefix=DYNAMIC

// STATIC: "--wrap=malloc"
// STATIC-SAME: "--wrap=calloc"
// STATIC-SAME: "--wrap=__libc_malloc"
// STATIC-SAME: "--wrap=__libc_calloc"
// STATIC-SAME: "--wrap=aligned_alloc"
// STATIC-SAME: "--wrap=posix_memalign"
// STATIC-SAME: "--wrap=realloc"
// STATIC-SAME: "--wrap=__libc_realloc"
// STATIC-SAME: "--wrap=free"
// STATIC-SAME: "--wrap=__libc_free"
// STATIC-SAME: "--wrap=malloc_usable_size"
// STATIC-SAME: "--wrap=pthread_create"
// STATIC-SAME: "--wrap=__pthread_create"
// STATIC-SAME: "--wrap=dl_iterate_phdr"
// STATIC-NOT: "--wrap=memcpy"
// STATIC-NOT: "--wrap=memmove"
// STATIC-NOT: "--wrap=memset"
// STATIC-SAME: "--wrap=memcmp"
// STATIC-SAME: "--wrap=memchr"
// STATIC-SAME: "--wrap=memccpy"
// STATIC-SAME: "--wrap=strlen"
// STATIC-SAME: "--wrap=strcmp"
// STATIC-SAME: "--wrap=strncmp"
// STATIC-SAME: "--wrap=stpcpy"
// STATIC-SAME: "--wrap=__stpcpy"
// STATIC-SAME: "--wrap=strchrnul"
// STATIC-SAME: "--wrap=__strchrnul"
// STATIC-SAME: "--wrap=strlcpy"
// STATIC-SAME: "--wrap=stpncpy"
// STATIC-SAME: "--wrap=__stpncpy"
// STATIC-NOT: "--wrap=mmap"
// STATIC-SAME: "--wrap=munmap"
// STATIC-SAME: "--wrap=longjmp"
// STATIC-SAME: "--wrap=_longjmp"
// STATIC-SAME: "--wrap=siglongjmp"
// STATIC-SAME: "--wrap=__sigsetjmp_tail"
// STATIC-SAME: "--wrap=__clone"
// STATIC-SAME: "--wrap=__copy_tls"
// STATIC-SAME: "--wrap=__unmapself"
// STATIC-SAME: "--wrap=vfork"
// STATIC-SAME: "--wrap=__libc_start_main"
// STATIC-SAME: "--wrap=__syscall_cp_asm"

// DYNAMIC-NOT: "--wrap="
