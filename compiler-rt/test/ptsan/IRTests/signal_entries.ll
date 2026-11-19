;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=prepare %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-stop-after=lowering %s | FileCheck %s
; RUN: %opt_ptsan %s | FileCheck %s
; RUN: %opt_ptsan -mtriple=x86_64-unknown-linux-gnu %s | FileCheck %s --check-prefix=HOST

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-musl"

@taken = global ptr @local_handler
@taken_no_args = global ptr @local_no_args

; Include external definitions even without local address-taking: a different
; translation unit can register them. Include explicitly uninstrumented bodies
; too, because their callees may use stack IDs.
define void @no_args() { ret void }
define internal void @local_no_args() { ret void }
define internal void @local_direct_no_args() { ret void }
define i32 @wrong_result_no_args() { ret i32 0 }
define void @variadic_no_args(...) { ret void }
define fastcc void @wrong_cc_no_args() { ret void }
declare void @declaration_no_args()
define void @ordinary(i32 %sig) { ret void }
define void @info(i32 %sig, ptr %info, ptr %context) { ret void }
define void @raw(i32 %sig) disable_sanitizer_instrumentation { ret void }
define internal void @local_handler(i32 %sig) { ret void }
define internal void @local_direct(i32 %sig) { ret void }
define i32 @wrong_result(i32 %sig) { ret i32 0 }
define void @wrong_count(i32 %sig, ptr %info) { ret void }
define void @wrong_type(i64 %sig) { ret void }
define void @wrong_info_type(i32 %sig, i64 %info, ptr %ctx) { ret void }
define void @wrong_context_type(i32 %sig, ptr %info, i64 %ctx) { ret void }
define void @wrong_space(i32 %sig, ptr addrspace(1) %info, ptr %ctx) { ret void }
define void @wrong_context_space(i32 %sig, ptr %info, ptr addrspace(1) %ctx) { ret void }
define void @variadic(i32 %sig, ...) { ret void }
define fastcc void @wrong_cc(i32 %sig) { ret void }
define void @wrong_abi(i32 %sig, ptr byval(i32) %info, ptr %ctx) { ret void }
declare void @declaration(i32)

; The three generated global initializers also qualify as void() functions.
; CHECK: @__ptsan_signal_entries.no_args = {{.*}}ptr @no_args, ptr @__ptsan_signal_entry.no_args
; CHECK-SAME: section "ptsan_signal_entries", align 8
; CHECK: @__ptsan_signal_entries.local_no_args = {{.*}}ptr @local_no_args, ptr @__ptsan_signal_entry.local_no_args
; CHECK: @__ptsan_signal_entries.ordinary = {{.*}}ptr @ordinary, ptr @__ptsan_signal_entry.ordinary
; CHECK: @__ptsan_signal_entries.info = {{.*}}ptr @info, ptr @__ptsan_signal_entry.info
; CHECK: @__ptsan_signal_entries.raw = {{.*}}ptr @raw, ptr @__ptsan_signal_entry.raw
; CHECK: @__ptsan_signal_entries.local_handler = {{.*}}ptr @local_handler, ptr @__ptsan_signal_entry.local_handler
; CHECK: @__ptsan_signal_entries.__ptsan_init_tagged_globals = {{.*}}ptr @__ptsan_init_tagged_globals, ptr @__ptsan_signal_entry.__ptsan_init_tagged_globals
; CHECK: @__ptsan_signal_entries.__ptsan_init_extern_globals = {{.*}}ptr @__ptsan_init_extern_globals, ptr @__ptsan_signal_entry.__ptsan_init_extern_globals
; CHECK: @__ptsan_signal_entries.__ptsan_init_global_initializer_pointers = {{.*}}ptr @__ptsan_init_global_initializer_pointers, ptr @__ptsan_signal_entry.__ptsan_init_global_initializer_pointers
; CHECK-SAME: section "ptsan_signal_entries", align 8
; CHECK: @llvm.used = appending global {{.*}}ptr @__ptsan_signal_entries
; CHECK-LABEL: define internal void @__ptsan_signal_entry.no_args()
; CHECK: tail call void @ptsan_signal_entry_0(ptr {{(nonnull )?}}@no_args)
; CHECK-NEXT: ret void
; CHECK-LABEL: define internal void @__ptsan_signal_entry.local_no_args()
; CHECK: tail call void @ptsan_signal_entry_0(ptr {{(nonnull )?}}@local_no_args)
; CHECK-NEXT: ret void
; CHECK-LABEL: define internal void @__ptsan_signal_entry.ordinary(i32
; CHECK: tail call void @ptsan_signal_entry_1(i32 %{{[^,]+}}, ptr {{(nonnull )?}}@ordinary)
; CHECK-NEXT: ret void
; CHECK-LABEL: define internal void @__ptsan_signal_entry.info(i32
; CHECK: tail call void @ptsan_signal_entry_3(i32 %{{[^,]+}}, ptr %{{[^,]+}}, ptr %{{[^,]+}}, ptr {{(nonnull )?}}@info)
; CHECK-NEXT: ret void
; CHECK-LABEL: define internal void @__ptsan_signal_entry.raw(i32
; CHECK: tail call void @ptsan_signal_entry_1(i32 %{{[^,]+}}, ptr {{(nonnull )?}}@raw)
; CHECK-NEXT: ret void
; CHECK-LABEL: define internal void @__ptsan_signal_entry.local_handler(i32
; CHECK: tail call void @ptsan_signal_entry_1(i32 %{{[^,]+}}, ptr {{(nonnull )?}}@local_handler)
; CHECK-NEXT: ret void
; HOST-NOT: @__ptsan_signal_entry
; HOST-NOT: @ptsan_signal_entry
