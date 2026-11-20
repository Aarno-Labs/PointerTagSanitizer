; This is a regression test for strip-ID hoists that split SCEV-equivalent
; address pairs into different lowered address shapes.
;
; XFAIL: *
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr)
declare void @escape(ptr)

; The load and store addresses are SCEV-equivalent, and both sides are ordinary
; SSA values:
;
;   %load.base  = %p + 4096
;   %store.base = gep [256 x float], %p, 5, 0
;
; The load-side subtree has two loop strip leaves and is profitable to hoist.
; The store-side strip also has many tagged descendant uses, making that side
; unprofitable. Hoisting only the load side still splits the equivalence class.
define void @profitable_subbase_unprofitable_peer(ptr %p, i64 %i) {
; CHECK-LABEL: define void @profitable_subbase_unprofitable_peer(
; CHECK-NOT: call ptr @__ptsan_strip(ptr %load.base)
; CHECK: %load.addr = getelementptr [256 x float], ptr %load.base, i64 %j.next, i64 %i
; CHECK-NEXT: %load.strip = call ptr @__ptsan_strip(ptr %load.addr)
; CHECK: %load.addr.2 = getelementptr [256 x float], ptr %load.base, i64 %j.next, i64 %i
; CHECK-NEXT: %load.strip.2 = call ptr @__ptsan_strip(ptr %load.addr.2)
; CHECK: %store.addr = getelementptr [256 x float], ptr %store.base, i64 %j, i64 %i
; CHECK-NEXT: %store.strip = call ptr @__ptsan_strip(ptr %store.addr)
entry:
  %load.base = getelementptr i8, ptr %p, i64 4096
  %store.base = getelementptr [256 x float], ptr %p, i64 5, i64 0
  br label %loop

loop:
  %j = phi i64 [ 1, %entry ], [ %j.next, %loop ]
  %j.next = add nuw nsw i64 %j, 1

  ; Two equivalent hot strip leaves make the load-side subtree profitable.
  %load.addr =
      getelementptr [256 x float], ptr %load.base, i64 %j.next, i64 %i
  %load.strip = call ptr @__ptsan_strip(ptr %load.addr)
  %load.addr.2 =
      getelementptr [256 x float], ptr %load.base, i64 %j.next, i64 %i
  %load.strip.2 = call ptr @__ptsan_strip(ptr %load.addr.2)
  %v = load float, ptr %load.strip.2, align 4

  ; This strip is SCEV-equivalent to the load-side strips.
  %store.addr =
      getelementptr [256 x float], ptr %store.base, i64 %j, i64 %i
  %store.strip = call ptr @__ptsan_strip(ptr %store.addr)
  store float %v, ptr %store.strip, align 4

  ; Tagged descendant uses make the store-side subtree unprofitable to hoist.
  call void @escape(ptr %store.addr)
  call void @escape(ptr %store.addr)
  call void @escape(ptr %store.addr)
  call void @escape(ptr %store.addr)
  call void @escape(ptr %store.addr)
  call void @escape(ptr %store.addr)
  call void @escape(ptr %store.addr)
  call void @escape(ptr %store.addr)
  call void @escape(ptr %store.addr)
  call void @escape(ptr %store.addr)
  call void @escape(ptr %store.addr)
  call void @escape(ptr %store.addr)

  %done = icmp eq i64 %j.next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
