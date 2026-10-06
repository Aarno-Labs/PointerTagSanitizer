;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; REQUIRES: x86_64-target-arch
; RUN: split-file %s %t
; RUN: %opt_ptsan -ptsan-runtime-validate -ptsan-stop-after=lowering %t/new.ll | FileCheck %s
; RUN: %opt_ptsan -ptsan-runtime-validate -ptsan-stop-after=lowering -ptsan-lam %t/new.ll | FileCheck %s
; RUN: %opt_ptsan -ptsan-runtime-validate -ptsan-stop-after=lowering %t/existing.ll | FileCheck %s
; RUN: %opt_ptsan -ptsan-runtime-validate -ptsan-stop-after=lowering -ptsan-lam %t/existing.ll | FileCheck %s

; Match the runtime's unsigned short and bool argument ABI on declarations and
; calls, including when the input already declares the helpers without attrs.
; CHECK-DAG: declare void @ptsan_free_stack_ids_1(i16 zeroext)
; CHECK-DAG: declare void @ptsan_log_alloca_info(ptr, i16 zeroext, i1 zeroext, i1 zeroext)
; CHECK-DAG: call void @ptsan_free_stack_ids_1(i16 zeroext %{{[^)]+}})
; CHECK-DAG: call void @ptsan_log_alloca_info(ptr {{[^,]+}}, i16 zeroext {{[^,]+}}, i1 zeroext {{[^,]+}}, i1 zeroext {{[^)]+}})

;--- new.ll
target datalayout = "e-m:e-p:64:64-i64:64-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @escape(ptr)

define void @probe() {
entry:
  %a = alloca [8 x i32], align 16
  call void @escape(ptr %a)
  ret void
}

;--- existing.ll
target datalayout = "e-m:e-p:64:64-i64:64-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @escape(ptr)
declare void @ptsan_free_stack_ids_1(i16)
declare void @ptsan_log_alloca_info(ptr, i16, i1, i1)

define void @probe() {
entry:
  %a = alloca [8 x i32], align 16
  call void @escape(ptr %a)
  ret void
}
