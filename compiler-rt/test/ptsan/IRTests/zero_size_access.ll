; RUN: rm -rf %t && mkdir -p %t && cd %t
; RUN: %opt_ptsan %s -S > out.ll
; RUN: FileCheck %s < out.ll

; CHECK-LABEL: define void @zero_memcpy
; CHECK-NOT: call void @ptsan_log_check_called
; CHECK-NOT: call void @ptsan_complain
; CHECK: ret void

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define void @zero_memcpy(ptr %dst, ptr %src) {
entry:
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %dst, ptr align 1 %src,
                                   i64 0, i1 false)
  ret void
}

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
