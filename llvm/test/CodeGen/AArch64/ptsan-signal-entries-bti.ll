;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: opt -passes=ptsan -S %s -o %t
; RUN: FileCheck %s --check-prefix=IR < %t
; RUN: llc < %t | FileCheck %s --check-prefix=ASM

target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-unknown-linux-musl"

; The kernel enters the generated wrapper, so all signal-handler forms must
; preserve the handler's BTI landing pad. Keep BTI disabled for plain handlers.
define dso_local void @ordinary(i32 %sig) #0 { ret void }
define dso_local void @info(i32 %sig, ptr %info, ptr %context) #0 { ret void }
define dso_local void @no_args() #0 { ret void }
define dso_local void @plain(i32 %sig) #1 { ret void }

attributes #0 = { "branch-target-enforcement" "target-cpu"="generic" "target-features"="+bti,+neon" }
attributes #1 = { "target-cpu"="generic" "target-features"="+bti,+neon" }

; IR-LABEL: define internal void @__ptsan_signal_entry.ordinary(
; IR-SAME: #[[BTI:[0-9]+]] {
; IR-LABEL: define internal void @__ptsan_signal_entry.info(
; IR-SAME: #[[BTI]] {
; IR-LABEL: define internal void @__ptsan_signal_entry.no_args(
; IR-SAME: #[[BTI]] {
; IR-LABEL: define internal void @__ptsan_signal_entry.plain(
; IR-SAME: #[[PLAIN:[0-9]+]] {
; IR: attributes #[[BTI]] = { disable_sanitizer_instrumentation "branch-target-enforcement" "target-cpu"="generic" "target-features"="+bti,+neon" }
; IR: attributes #[[PLAIN]] = { disable_sanitizer_instrumentation "target-cpu"="generic" "target-features"="+bti,+neon" }

; ASM-LABEL: __ptsan_signal_entry.ordinary:
; ASM:       bti c
; ASM-NEXT:  adrp x1, ordinary
; ASM:       b ptsan_signal_entry_1
; ASM-LABEL: __ptsan_signal_entry.info:
; ASM:       bti c
; ASM-NEXT:  adrp x3, info
; ASM:       b ptsan_signal_entry_3
; ASM-LABEL: __ptsan_signal_entry.no_args:
; ASM:       bti c
; ASM-NEXT:  adrp x0, no_args
; ASM:       b ptsan_signal_entry_0
; ASM-LABEL: __ptsan_signal_entry.plain:
; ASM-NOT:   bti
; ASM:       b ptsan_signal_entry_1
