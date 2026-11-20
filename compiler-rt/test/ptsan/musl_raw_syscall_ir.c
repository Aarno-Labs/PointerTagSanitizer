//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Model the raw syscall helpers in musl's arch/x86_64/syscall_arch.h and
// arch/aarch64/syscall_arch.h.  The kernel must receive untagged addresses,
// even though musl transports pointer arguments through integer registers.
//
// RUN: %clang_raw --target=x86_64-unknown-linux-musl -fsanitize=ptsan -O1 -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefix=X86
// RUN: %clang_raw --target=aarch64-unknown-linux-musl -fsanitize=ptsan -O1 -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefix=AARCH64

typedef unsigned long size_t;

struct kernel_iovec {
  void *iov_base;
  size_t iov_len;
};

static __attribute__((always_inline)) inline long
raw_syscall3(long number, long arg1, long arg2, long arg3) {
#if defined(__x86_64__)
  unsigned long result;
  __asm__ __volatile__("syscall"
                       : "=a"(result)
                       : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
                       : "rcx", "r11", "memory");
  return result;
#elif defined(__aarch64__)
  register long x8 __asm__("x8") = number;
  register long x0 __asm__("x0") = arg1;
  register long x1 __asm__("x1") = arg2;
  register long x2 __asm__("x2") = arg3;
  __asm__ __volatile__("svc 0"
                       : "=r"(x0)
                       : "r"(x8), "0"(x0), "r"(x1), "r"(x2)
                       : "memory", "cc");
  return x0;
#else
#error unsupported test architecture
#endif
}

__attribute__((noinline)) long
musl_shaped_write(const void *buffer, size_t size) {
#if defined(__x86_64__)
  const long write_number = 1;
#elif defined(__aarch64__)
  const long write_number = 64;
#else
#error unsupported test architecture
#endif
  return raw_syscall3(write_number, 1, (long)buffer, size);
}

// X86-LABEL: define {{.*}}i64 @musl_shaped_write(
// X86: [[BUFFER_INT:%[^ ]+]] = ptrtoint ptr %buffer to i64
// X86: [[BUFFER_RAW:%[^ ]+]] = {{.*}}call i64 asm "andq $2, $0", "=r,0,r,~{flags}"(i64 [[BUFFER_INT]], i64 281474976710655)
// X86: call i64 asm sideeffect "syscall", {{.*}}(i64 1, i64 1, i64 [[BUFFER_RAW]], i64 %size)
//
// AARCH64-LABEL: define {{.*}}i64 @musl_shaped_write(
// AARCH64: [[BUFFER_INT:%[^ ]+]] = ptrtoint ptr %buffer to i64
// AARCH64: [[BUFFER_RAW:%[^ ]+]] = and i64 [[BUFFER_INT]], 281474976710655
// AARCH64: call i64 asm sideeffect "svc 0", {{.*}}(i64 64, i64 1, i64 [[BUFFER_RAW]], i64 %size)

__attribute__((noinline)) long
musl_shaped_writev(const struct kernel_iovec *iov, int count) {
#if defined(__x86_64__)
  const long writev_number = 20;
#elif defined(__aarch64__)
  const long writev_number = 66;
#else
#error unsupported test architecture
#endif
  return raw_syscall3(writev_number, 1, (long)iov, count);
}

// PTSan can strip the top-level iovec pointer because it reaches the inline-asm
// boundary directly.  It cannot see or strip iov_base pointers nested in the
// array; handling those requires syscall-specific semantics outside this test.
// X86-LABEL: define {{.*}}i64 @musl_shaped_writev(
// X86: [[IOV_INT:%[^ ]+]] = ptrtoint ptr %iov to i64
// X86: [[IOV_RAW:%[^ ]+]] = {{.*}}call i64 asm "andq $2, $0", "=r,0,r,~{flags}"(i64 [[IOV_INT]], i64 281474976710655)
// X86: call i64 asm sideeffect "syscall", {{.*}}(i64 20, i64 1, i64 [[IOV_RAW]], i64 {{.*}})
//
// AARCH64-LABEL: define {{.*}}i64 @musl_shaped_writev(
// AARCH64: [[IOV_INT:%[^ ]+]] = ptrtoint ptr %iov to i64
// AARCH64: [[IOV_RAW:%[^ ]+]] = and i64 [[IOV_INT]], 281474976710655
// AARCH64: call i64 asm sideeffect "svc 0", {{.*}}(i64 66, i64 1, i64 [[IOV_RAW]], i64 {{.*}})
