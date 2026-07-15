; Check the desired strip-ID hoist shape for a global constant-expression
; operand. The constant-expression GEP should be materialized so late strip-ID
; hoisting can strip the raw global once and use ordinary GEPs under it.
;
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@extern_global = external global [16 x i8]

declare ptr @__ptsan_strip(ptr)

define void @global_constexpr_strip_hoists(i64 %n) {
; CHECK-LABEL: define void @global_constexpr_strip_hoists(
; CHECK: entry:
; CHECK: [[GLOBAL_STRIP:%.*]] = call ptr @__ptsan_strip(ptr @extern_global)
; CHECK: loop:
; CHECK-NOT: call ptr @__ptsan_strip
; CHECK: [[BASE:%.*]] = getelementptr inbounds i8, ptr [[GLOBAL_STRIP]], i64 4
; CHECK-NEXT: %addr = getelementptr i8, ptr [[BASE]], i64 %i
; CHECK-NOT: call ptr @__ptsan_strip
; CHECK: load volatile i8, ptr %addr
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %addr =
      getelementptr i8,
                    ptr getelementptr inbounds (i8, ptr @extern_global, i64 4),
                    i64 %i
  %stripped = call ptr @__ptsan_strip(ptr %addr)
  %v = load volatile i8, ptr %stripped, align 1
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; This is the SPEC-style case that used to split equivalent address shapes:
; one side used a global-derived SSA base, while the other side used a raw
; global constant-expression GEP. Materializing the constant expression lets
; both sides derive from the same stripped global.
define void @global_constexpr_counterpart_canonicalizes_both_sides(i64 %i) {
; CHECK-LABEL: define void @global_constexpr_counterpart_canonicalizes_both_sides(
; CHECK: entry:
; CHECK: [[GLOBAL_STRIP:%.*]] = call ptr @__ptsan_strip(ptr @extern_global)
; CHECK: [[LOAD_BASE:%.*]] = getelementptr inbounds i8, ptr [[GLOBAL_STRIP]], i64 4
; CHECK-NEXT: %load.col = getelementptr [256 x float], ptr [[LOAD_BASE]], i64 0, i64 %i
; CHECK: loop:
; CHECK-NOT: call ptr @__ptsan_strip
; CHECK: %load.addr = getelementptr [256 x float], ptr %load.col, i64 %j.next
; CHECK-NEXT: %v = load float, ptr %load.addr
; CHECK-NEXT: [[STORE_COL:%.*]] = getelementptr inbounds i8, ptr [[GLOBAL_STRIP]], i64 1028
; CHECK-NEXT: %store.addr = getelementptr inbounds [256 x float], ptr [[STORE_COL]], i64 %j, i64 %i
; CHECK-NEXT: store float %v, ptr %store.addr
; CHECK-NOT: call ptr @__ptsan_strip
entry:
  %load.col =
      getelementptr [256 x float],
                    ptr getelementptr inbounds (i8, ptr @extern_global, i64 4),
                    i64 0, i64 %i
  br label %loop

loop:
  %j = phi i64 [ 1, %entry ], [ %j.next, %loop ]
  %j.next = add nuw nsw i64 %j, 1

  %load.addr = getelementptr [256 x float], ptr %load.col, i64 %j.next
  %load.strip = call ptr @__ptsan_strip(ptr %load.addr)
  %v = load float, ptr %load.strip, align 4

  %store.addr =
      getelementptr inbounds [256 x float],
                    ptr getelementptr inbounds (i8, ptr @extern_global,
                                                i64 1028),
                    i64 %j, i64 %i
  %store.strip = call ptr @__ptsan_strip(ptr %store.addr)
  store float %v, ptr %store.strip, align 4

  %done = icmp eq i64 %j.next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
