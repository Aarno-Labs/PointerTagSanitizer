;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; REQUIRES: x86_64-target-arch
; RUN: split-file %s %t
; RUN: %opt_ptsan -ptsan-runtime-validate -ptsan-stop-after=lowering %t/new.ll | FileCheck %s --check-prefix=DEFAULT
; RUN: %opt_ptsan -ptsan-runtime-validate -ptsan-stop-after=lowering -ptsan-lam %t/new.ll | FileCheck %s --check-prefix=DEFAULT
; RUN: %opt_ptsan -ptsan-runtime-validate -ptsan-stop-after=lowering %t/existing.ll | FileCheck %s --check-prefix=DEFAULT
; RUN: %opt_ptsan -ptsan-runtime-validate -ptsan-stop-after=lowering -ptsan-lam %t/existing.ll | FileCheck %s --check-prefix=DEFAULT
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-runtime-validate -ptsan-stop-after=lowering %t/existing32.ll | FileCheck %s --check-prefix=ID32

; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-runtime-validate -ptsan-stop-after=lowering %t/new.ll | FileCheck %s --check-prefix=ID32

; Match the runtime's unsigned short and bool argument ABI on declarations and
; calls, including when the input already declares the helpers without attrs.
; DEFAULT-DAG: declare void @ptsan_log_alloca_info(ptr, i16 zeroext, i1 zeroext, i1 zeroext)
; DEFAULT-DAG: call void @ptsan_log_alloca_info(ptr {{[^,]+}}, i16 zeroext {{[^,]+}}, i1 zeroext {{[^,]+}}, i1 zeroext {{[^)]+}})
; ID32-DAG: %stack.ids = alloca i32, i64 1
; ID32-DAG: call void @ptsan_get_stack_ids(ptr %stack.ids, i64 1)
; ID32-DAG: call void @ptsan_free_stack_ids(i64
; ID32-DAG: declare void @ptsan_log_alloca_info(ptr, i32, i1 zeroext, i1 zeroext)
; ID32-DAG: call void @ptsan_log_alloca_info(ptr {{[^,]+}}, i32 {{[^,]+}}, i1 zeroext {{[^,]+}}, i1 zeroext {{[^)]+}})

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
declare void @ptsan_log_alloca_info(ptr, i16, i1, i1)

define void @probe() {
entry:
  %a = alloca [8 x i32], align 16
  call void @escape(ptr %a)
  ret void
}

;--- existing32.ll
target datalayout = "e-m:e-p:64:64-i64:64-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @escape(ptr)
declare void @ptsan_free_stack_ids(i64)
declare void @ptsan_log_alloca_info(ptr, i32, i1, i1)

define void @probe() {
entry:
  %a = alloca [8 x i32], align 16
  call void @escape(ptr %a)
  ret void
}
