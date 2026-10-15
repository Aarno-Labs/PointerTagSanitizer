;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; Strict mode rejects unsupported targets even without varargs or globals.
; RUN: not %opt_ptsan -ptsan-strict-mode -mtriple=riscv64-unknown-linux-musl %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ARCH
; RUN: not %opt_ptsan -ptsan-strict-mode -mtriple=x86_64-unknown-freebsd %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=OS
; RUN: not %opt_ptsan -ptsan-strict-mode -mtriple=x86_64-unknown-linux-musl-coff %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=FORMAT
; RUN: %opt_ptsan -ptsan-strict-mode -mtriple=x86_64-unknown-linux-musl %s -o /dev/null
; RUN: %opt_ptsan -ptsan-strict-mode -mtriple=aarch64-unknown-linux-musl %s -o /dev/null
;
; Pointer width is a requirement in ordinary mode too. Explicitly reject x32
; even if a hand-written module omits its 32-bit data layout.
; RUN: not %opt_ptsan -data-layout=e-p:32:32 -mtriple=i386-unknown-linux-gnu %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=PTR32
; RUN: not %opt_ptsan -ptsan-strict-mode -data-layout=e-p:32:32 -mtriple=i386-unknown-linux-musl %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=PTR32-MUSL
; RUN: not %opt_ptsan -mtriple=x86_64-unknown-linux-gnux32 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=X32
; RUN: not %opt_ptsan -ptsan-strict-mode -mtriple=x86_64-unknown-linux-muslx32 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=X32-MUSL
;
; Ordinary mode retains its platform fallbacks. Disabled instrumentation still
; publishes runtime flags, but does not impose instrumentation requirements.
; RUN: %opt_ptsan -mtriple=aarch64-unknown-linux-gnu %s -o /dev/null
; RUN: %opt_ptsan -mtriple=x86_64-unknown-freebsd %s -o /dev/null
; RUN: %opt_ptsan -mtriple=x86_64-apple-darwin %s -o /dev/null
; RUN: %opt_ptsan -ptsan-disable -ptsan-strict-mode -data-layout=e-p:32:32 -mtriple=i386-unknown-linux-musl %s -o /dev/null
; RUN: not %opt_ptsan -ptsan-lam -mtriple=aarch64-unknown-linux-gnu %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=LAM
;
; ARCH: PTSan strict mode requires an x86-64 or AArch64 Linux ELF target; target 'riscv64-unknown-linux-musl' is unsupported
; OS: PTSan strict mode requires an x86-64 or AArch64 Linux ELF target; target 'x86_64-unknown-freebsd' is unsupported
; FORMAT: PTSan strict mode requires an x86-64 or AArch64 Linux ELF target; target 'x86_64-unknown-linux-musl-coff' is unsupported
; PTR32: PTSan requires 64-bit pointers; target 'i386-unknown-linux-gnu' is unsupported
; PTR32-MUSL: PTSan requires 64-bit pointers; target 'i386-unknown-linux-musl' is unsupported
; X32: PTSan requires 64-bit pointers; target 'x86_64-unknown-linux-gnux32' is unsupported
; X32-MUSL: PTSan requires 64-bit pointers; target 'x86_64-unknown-linux-muslx32' is unsupported
; LAM: PTSan LAM mode requires an x86-64 target; target 'aarch64-unknown-linux-gnu' is unsupported

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

define void @f() {
  ret void
}
