; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s --check-prefix=OPT

; Retagging covers mixed tagged/stripped pointer trees. The pass may strip a
; base once, use the stripped tree for memory accesses, and insert __ptsan_tag
; at tagged exits.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr)
declare i64 @__ptsan_get_id(ptr)
declare void @sink(ptr)
declare void @use_id(i64)

; A derived pointer stored outside the loop needs a tagged reference, while the
; loop body needs stripped references. The global cut can keep that independent
; sibling tagged and strip only the loop subtree.
define void @retag_store_sibling(ptr %b, ptr %slot) {
; OPT-LABEL: define void @retag_store_sibling(
; OPT: entry:
; OPT-DAG: [[SLOT_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %slot)
; OPT-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; OPT: %tagged.gep = getelementptr i8, ptr %b, i64 8
; OPT-NEXT: [[TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %tagged.gep)
; OPT-NEXT: store ptr [[TAGGED]], ptr [[SLOT_STRIP]]
; OPT: loop:
; OPT: %strip.gep = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; OPT-NEXT: load volatile i8, ptr %strip.gep
entry:
  %tagged.gep = getelementptr i8, ptr %b, i64 8 ; tagged derived pointer
  store ptr %tagged.gep, ptr %slot ; tagged reference required
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %strip.gep = getelementptr i8, ptr %b, i64 %iv ; stripped loop pointer
  %s = call ptr @__ptsan_strip(ptr %strip.gep) ; stripped reference required
  %v = load volatile i8, ptr %s, align 1 ; consumes the already-stripped reference
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A direct tagged use of the hoist base first becomes
; tag(strip(%b), get_id(%b)), then folds back to the original tagged %b.
define void @direct_base_retag_folds(ptr %b, ptr %slot) {
; OPT-LABEL: define void @direct_base_retag_folds(
; OPT: entry:
; OPT-DAG: [[SLOT_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %slot)
; OPT-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; OPT-NOT: call i64 @__ptsan_get_id(ptr %b)
; OPT-NOT: call ptr @__ptsan_tag(ptr [[BASE_STRIP]]
; OPT: [[TAGGED_BASE:%.*]] = call ptr @__ptsan_tagged(ptr %b)
; OPT-NEXT: store ptr [[TAGGED_BASE]], ptr [[SLOT_STRIP]]
; OPT: loop:
; OPT: %g = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; OPT-NEXT: load volatile i8, ptr %g
entry:
  store ptr %b, ptr %slot ; direct tagged use of the hoist base
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %g = getelementptr i8, ptr %b, i64 %iv ; stripped loop pointer
  %s = call ptr @__ptsan_strip(ptr %g) ; stripped reference required
  %v = load volatile i8, ptr %s, align 1 ; consumes the already-stripped reference
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A direct get_id user and a tagged sibling can both remain tagged while the
; loop subtree uses one stripped base.
define void @get_id_and_tagged_sibling_keep_loop_strip_local(ptr %b,
                                                             ptr %slot) {
; OPT-LABEL: define void @get_id_and_tagged_sibling_keep_loop_strip_local(
; OPT: entry:
; OPT-DAG: [[SLOT_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %slot)
; OPT-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; OPT-DAG: [[ID:%.*]] = call i64 @__ptsan_get_id(ptr %b)
; OPT: call void @use_id(i64 [[ID]])
; OPT: %tagged.gep = getelementptr i8, ptr %b, i64 8
; OPT-NEXT: [[TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %tagged.gep)
; OPT-NEXT: store ptr [[TAGGED]], ptr [[SLOT_STRIP]]
; OPT: loop:
; OPT: %strip.gep = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; OPT-NEXT: load volatile i8, ptr %strip.gep
entry:
  %id = call i64 @__ptsan_get_id(ptr %b) ; tagged base, zero retag cost
  call void @use_id(i64 %id) ; keeps the get_id result observable
  %tagged.gep = getelementptr i8, ptr %b, i64 8 ; tagged derived pointer
  store ptr %tagged.gep, ptr %slot ; independent tagged exit
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %strip.gep = getelementptr i8, ptr %b, i64 %iv ; stripped loop pointer
  %s = call ptr @__ptsan_strip(ptr %strip.gep) ; stripped reference required
  %v = load volatile i8, ptr %s, align 1 ; consumes the already-stripped reference
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Multiple independent tagged exits can remain tagged while the loop subtree is
; stripped once.
define void @independent_tagged_exits_do_not_need_retags(ptr %b, ptr %slot) {
; OPT-LABEL: define void @independent_tagged_exits_do_not_need_retags(
; OPT: entry:
; OPT-DAG: [[SLOT_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %slot)
; OPT-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; OPT: %stored.gep = getelementptr i8, ptr %b, i64 8
; OPT-NEXT: [[STORE_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %stored.gep)
; OPT-NEXT: store ptr [[STORE_TAGGED]], ptr [[SLOT_STRIP]]
; OPT: %called.gep = getelementptr i8, ptr %b, i64 16
; OPT-NEXT: [[CALL_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %called.gep)
; OPT-NEXT: call void @sink(ptr [[CALL_TAGGED]])
; OPT: loop:
; OPT: %strip.gep.0 = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; OPT-NEXT: load volatile i8, ptr %strip.gep.0
; OPT: %strip.gep.1 = getelementptr i8, ptr [[BASE_STRIP]], i64 %next
; OPT-NEXT: load volatile i8, ptr %strip.gep.1
entry:
  %stored.gep = getelementptr i8, ptr %b, i64 8 ; first tagged exit
  store ptr %stored.gep, ptr %slot ; tagged store remains tagged
  %called.gep = getelementptr i8, ptr %b, i64 16 ; second tagged exit
  call void @sink(ptr %called.gep) ; tagged call remains tagged
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %strip.gep.0 = getelementptr i8, ptr %b, i64 %iv ; first strip path
  %s0 = call ptr @__ptsan_strip(ptr %strip.gep.0) ; stripped reference required
  %v0 = load volatile i8, ptr %s0, align 1 ; consumes the already-stripped reference
  %next = add nuw i64 %iv, 1
  %strip.gep.1 = getelementptr i8, ptr %b, i64 %next ; second strip path
  %s1 = call ptr @__ptsan_strip(ptr %strip.gep.1) ; stripped reference required
  %v1 = load volatile i8, ptr %s1, align 1 ; consumes the already-stripped reference
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Returning a derived pointer is a tagged exit. Since it is independent from the
; loop subtree, the global cut leaves it on the original tagged base.
define ptr @retag_return(ptr %b) {
; OPT-LABEL: define ptr @retag_return(
; OPT: entry:
; OPT: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; OPT: loop:
; OPT: %strip.gep = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; OPT-NEXT: load volatile i8, ptr %strip.gep
; OPT: exit:
; OPT-NEXT: %ret.gep = getelementptr i8, ptr %b, i64 8
; OPT: [[TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %ret.gep)
; OPT-NEXT: ret ptr [[TAGGED]]
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %strip.gep = getelementptr i8, ptr %b, i64 %iv ; stripped loop pointer
  %s = call ptr @__ptsan_strip(ptr %strip.gep) ; stripped reference required
  %v = load volatile i8, ptr %s, align 1 ; consumes the already-stripped reference
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  %ret.gep = getelementptr i8, ptr %b, i64 8 ; tagged return value
  ret ptr %ret.gep ; tagged reference required
}

; If a tagged leaf is itself get_id(tag(..., id)), the cleanup pass should fold
; it directly to the dumb retag ID instead of leaving a tag/get-id pair around.
define void @fold_get_id_of_tag(ptr %b) {
; OPT-LABEL: define void @fold_get_id_of_tag(
; OPT: entry:
; OPT-DAG: [[ID:%.*]] = call i64 @__ptsan_get_id(ptr %b)
; OPT-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; OPT: call void @use_id(i64 [[ID]])
; OPT-NOT: call ptr @__ptsan_tag(
; OPT: loop:
; OPT: %g = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; OPT-NEXT: load volatile i8, ptr %g
entry:
  %id = call i64 @__ptsan_get_id(ptr %b) ; tagged reference required
  call void @use_id(i64 %id) ; keeps the folded ID observable
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %g = getelementptr i8, ptr %b, i64 %iv ; stripped loop pointer
  %s = call ptr @__ptsan_strip(ptr %g) ; stripped reference required
  %v = load volatile i8, ptr %s, align 1 ; consumes the already-stripped reference
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A mixed PHI stays tagged, while the local loop strip remains on the derived
; pointer feeding the stripped load.
define void @mixed_phi_stays_tagged_with_local_loop_strip(ptr %b, ptr %other,
                                                          ptr %slot,
                                                          i1 %cond) {
; OPT-LABEL: define void @mixed_phi_stays_tagged_with_local_loop_strip(
; OPT: loop:
; OPT: %g = getelementptr i8, ptr %b, i64 %iv
; OPT-NEXT: %s = call ptr @__ptsan_strip(ptr %g)
; OPT: join:
; OPT: %p = phi ptr [ %g, %left ], [ %other, %right ]
; OPT: [[PHI_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %p)
; OPT: store ptr [[PHI_TAGGED]], ptr {{%.*}}
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %join ]
  %g = getelementptr i8, ptr %b, i64 %iv ; both stripped and PHI-tagged
  %s = call ptr @__ptsan_strip(ptr %g) ; stripped reference required
  %v = load volatile i8, ptr %s, align 1 ; consumes the already-stripped reference
  br i1 %cond, label %left, label %right

left:
  br label %join

right:
  br label %join

join:
  %p = phi ptr [ %g, %left ], [ %other, %right ] ; mixed PHI stays tagged
  store ptr %p, ptr %slot ; tagged reference required
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
