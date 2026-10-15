; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s | FileCheck %s

; These cover strip-ID hoisting over base-pointer chains involving PHIs.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr)
declare i64 @__ptsan_get_id(ptr)
declare void @sink(ptr)

; Target shape for splitting a tagged direct sibling away from a strip-needed
; neutral path. This is the same core blocker as loop_hoist_tiny's should_hoist
; case, kept here as focused IR.
define void @tagged_sibling_does_not_poison_neutral_path(ptr %b) {
; CHECK-LABEL: define void @tagged_sibling_does_not_poison_neutral_path(
; CHECK: entry:
; CHECK: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; CHECK: loop:
; CHECK: %g = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; CHECK-NOT: call ptr @__ptsan_strip
; CHECK: load volatile i8, ptr %g
entry:
  %id = call i64 @__ptsan_get_id(ptr %b) ; tagged reference required
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %g = getelementptr i8, ptr %b, i64 %iv ; split target subtree
  %s = call ptr @__ptsan_strip(ptr %g) ; stripped reference required
  %v = load volatile i8, ptr %s, align 1 ; consumes the already-stripped reference
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A value may be split between an uncontained PHI input that must stay tagged
; and a contained strip subtree that can remain stripped.
define void @tag_result_split_between_phi_and_strip_subtree(ptr %b, ptr %other,
                                                            ptr %slot,
                                                            i1 %cond) {
; CHECK-LABEL: define void @tag_result_split_between_phi_and_strip_subtree(
; CHECK: entry:
; CHECK-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; CHECK: %from.b = getelementptr i8, ptr %b, i64 8
; CHECK-NEXT: [[FROM_B_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %from.b)
; CHECK: left:
; CHECK: join:
; CHECK: %mixed = phi ptr [ %from.b, %left ], [ %other, %right ]
; CHECK: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %mixed)
; CHECK: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; CHECK: loop:
; CHECK: %direct = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %direct
; CHECK: %under.tag = getelementptr i8, ptr [[FROM_B_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %under.tag
entry:
  %from.b = getelementptr i8, ptr %b, i64 8 ; uncontained PHI input
  br i1 %cond, label %left, label %right

left:
  br label %join

right:
  br label %join

join:
  %mixed = phi ptr [ %from.b, %left ], [ %other, %right ] ; must stay tagged
  store ptr %mixed, ptr %slot ; tagged consumer of the mixed PHI
  br label %loop

loop:
  %iv = phi i64 [ 0, %join ], [ %next, %loop ]
  %direct = getelementptr i8, ptr %b, i64 %iv ; makes the %b hoist profitable
  %direct.s = call ptr @__ptsan_strip(ptr %direct) ; direct strip saving
  %direct.v = load volatile i8, ptr %direct.s, align 1
  %under.tag = getelementptr i8, ptr %from.b, i64 %iv ; original subtree root
  %under.tag.s = call ptr @__ptsan_strip(ptr %under.tag) ; stripped subtree
  %under.tag.v = load volatile i8, ptr %under.tag.s, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A contained PHI can stay tagged for a mixed PHI exit while a separate stripped
; representative feeds the loop subtree.
define void @contained_phi_feeds_uncontained_phi(ptr %b, ptr %other,
                                                 ptr %slot, i1 %c1, i1 %c2) {
; CHECK-LABEL: define void @contained_phi_feeds_uncontained_phi(
; CHECK: entry:
; CHECK-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; CHECK: left:
; CHECK: %l = getelementptr i8, ptr %b, i64 8
; CHECK: right:
; CHECK: %r = getelementptr i8, ptr %b, i64 16
; CHECK: contained.join:
; CHECK: %contained = phi ptr [ %l, %left ], [ %r, %right ]
; CHECK-NEXT: [[CONTAINED_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %contained)
; CHECK: mixed.left:
; CHECK: mixed.join:
; CHECK: %mixed = phi ptr [ %contained, %mixed.left ], [ %other, %mixed.right ]
; CHECK: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %mixed)
; CHECK: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; CHECK: loop:
; CHECK: %direct = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %direct
; CHECK: %under.contained = getelementptr i8, ptr [[CONTAINED_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %under.contained
entry:
  br i1 %c1, label %left, label %right

left:
  %l = getelementptr i8, ptr %b, i64 8 ; contained PHI arm
  br label %contained.join

right:
  %r = getelementptr i8, ptr %b, i64 16 ; contained PHI arm
  br label %contained.join

contained.join:
  %contained = phi ptr [ %l, %left ], [ %r, %right ] ; common %b-derived PHI
  br i1 %c2, label %mixed.left, label %mixed.right

mixed.left:
  br label %mixed.join

mixed.right:
  br label %mixed.join

mixed.join:
  %mixed = phi ptr [ %contained, %mixed.left ], [ %other, %mixed.right ]
  store ptr %mixed, ptr %slot ; tagged consumer of the uncontained PHI
  br label %loop

loop:
  %iv = phi i64 [ 0, %mixed.join ], [ %next, %loop ]
  %direct = getelementptr i8, ptr %b, i64 %iv ; makes the %b hoist profitable
  %direct.s = call ptr @__ptsan_strip(ptr %direct) ; direct strip saving
  %direct.v = load volatile i8, ptr %direct.s, align 1
  %under.contained = getelementptr i8, ptr %contained, i64 %iv
  %under.contained.s = call ptr @__ptsan_strip(ptr %under.contained)
  %under.contained.v = load volatile i8, ptr %under.contained.s, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Selects have the same polarity shape as PHIs: a contained select may stay
; tagged for a mixed exit while a separate stripped representative feeds the
; loop subtree.
define void @contained_select_feeds_uncontained_select(ptr %b, ptr %other,
                                                       ptr %slot, i1 %c1,
                                                       i1 %c2) {
; CHECK-LABEL: define void @contained_select_feeds_uncontained_select(
; CHECK: entry:
; CHECK-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; CHECK: %l = getelementptr i8, ptr %b, i64 8
; CHECK-NEXT: %r = getelementptr i8, ptr %b, i64 16
; CHECK-NEXT: %contained = select i1 %c1, ptr %l, ptr %r
; CHECK-NEXT: [[CONTAINED_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %contained)
; CHECK-NEXT: %mixed = select i1 %c2, ptr %contained, ptr %other
; CHECK-NEXT: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %mixed)
; CHECK-NEXT: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; CHECK: loop:
; CHECK: %direct = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %direct
; CHECK: %under.contained = getelementptr i8, ptr [[CONTAINED_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %under.contained
entry:
  %l = getelementptr i8, ptr %b, i64 8 ; contained select arm
  %r = getelementptr i8, ptr %b, i64 16 ; contained select arm
  %contained = select i1 %c1, ptr %l, ptr %r ; common %b-derived select
  %mixed = select i1 %c2, ptr %contained, ptr %other ; must stay tagged
  store ptr %mixed, ptr %slot ; tagged consumer of the uncontained select
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %direct = getelementptr i8, ptr %b, i64 %iv ; makes the %b hoist profitable
  %direct.s = call ptr @__ptsan_strip(ptr %direct) ; direct strip saving
  %direct.v = load volatile i8, ptr %direct.s, align 1
  %under.contained = getelementptr i8, ptr %contained, i64 %iv
  %under.contained.s = call ptr @__ptsan_strip(ptr %under.contained)
  %under.contained.v = load volatile i8, ptr %under.contained.s, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; When the only profitable stripped use is after the PHI, the global solver
; strips the PHI result once instead of stripping each incoming arm.
define void @phi_same_base_is_valid_base(ptr %b, i1 %cond) {
; CHECK-LABEL: define void @phi_same_base_is_valid_base(
; CHECK: entry:
; CHECK: left:
; CHECK: %l = getelementptr i8, ptr %b, i64 8
; CHECK: right:
; CHECK: %r = getelementptr i8, ptr %b, i64 16
; CHECK: join:
; CHECK: %p = phi ptr [ %l, %left ], [ %r, %right ]
; CHECK-NEXT: [[P_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %p)
; CHECK: loop:
; CHECK: %g = getelementptr i8, ptr [[P_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %g
entry:
  %id = call i64 @__ptsan_get_id(ptr %b) ; tagged sibling use of %b
  br i1 %cond, label %left, label %right

left:
  %l = getelementptr i8, ptr %b, i64 8 ; PHI arm derived from %b
  br label %join

right:
  %r = getelementptr i8, ptr %b, i64 16 ; PHI arm derived from %b
  br label %join

join:
  %p = phi ptr [ %l, %left ], [ %r, %right ] ; valid common base
  br label %loop

loop:
  %iv = phi i64 [ 0, %join ], [ %next, %loop ]
  %g = getelementptr i8, ptr %p, i64 %iv ; neutral use of PHI base
  %s = call ptr @__ptsan_strip(ptr %g) ; stripped reference required
  %v = load volatile i8, ptr %s, align 1 ; consumes the already-stripped reference
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Nested same-base PHIs similarly strip once at the final PHI when that is the
; cheapest cut.
define void @nested_phi_same_base_is_valid_base(ptr %b, i1 %c1, i1 %c2) {
; CHECK-LABEL: define void @nested_phi_same_base_is_valid_base(
; CHECK: entry:
; CHECK: left:
; CHECK: %l = getelementptr i8, ptr %b, i64 8
; CHECK: right:
; CHECK: %r = getelementptr i8, ptr %b, i64 16
; CHECK: inner.join:
; CHECK: %inner = phi ptr [ %l, %left ], [ %r, %right ]
; CHECK: %direct = getelementptr i8, ptr %b, i64 32
; CHECK: outer.join:
; CHECK: %outer = phi ptr [ %inner, %inner.join ], [ %direct, %direct.block ]
; CHECK-NEXT: [[OUTER_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %outer)
; CHECK: loop:
; CHECK: %g = getelementptr i8, ptr [[OUTER_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %g
entry:
  %id = call i64 @__ptsan_get_id(ptr %b) ; tagged sibling use of %b
  br i1 %c1, label %left, label %right

left:
  %l = getelementptr i8, ptr %b, i64 8 ; inner PHI arm
  br label %inner.join

right:
  %r = getelementptr i8, ptr %b, i64 16 ; inner PHI arm
  br label %inner.join

inner.join:
  %inner = phi ptr [ %l, %left ], [ %r, %right ] ; not final strip base
  br i1 %c2, label %outer.join, label %direct.block

direct.block:
  %direct = getelementptr i8, ptr %b, i64 32 ; second outer PHI arm
  br label %outer.join

outer.join:
  %outer = phi ptr [ %inner, %inner.join ], [ %direct, %direct.block ] ; final common base
  br label %loop

loop:
  %iv = phi i64 [ 0, %outer.join ], [ %next, %loop ]
  %g = getelementptr i8, ptr %outer, i64 %iv ; neutral use of outer base
  %s = call ptr @__ptsan_strip(ptr %g) ; stripped reference required
  %v = load volatile i8, ptr %s, align 1 ; consumes the already-stripped reference
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A mixed PHI that is not on the strip path should not block hoisting of a
; separate %b-derived loop path. This documents the intended interaction between
; split bases and unrelated tagged/mixed users.
define void @mixed_phi_tagged_sibling_not_on_strip_path(ptr %b, ptr %other,
                                                        i1 %cond) {
; CHECK-LABEL: define void @mixed_phi_tagged_sibling_not_on_strip_path(
; CHECK: entry:
; CHECK: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; CHECK: left:
; CHECK: %from.b = getelementptr i8, ptr %b, i64 8
; CHECK: join:
; CHECK: %mixed = phi ptr [ %from.b, %left ], [ %other, %right ]
; CHECK: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %mixed)
; CHECK: call void @sink(ptr [[MIXED_TAGGED]])
; CHECK: loop:
; CHECK: %g = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; CHECK-NOT: call ptr @__ptsan_strip
; CHECK: load volatile i8, ptr %g
entry:
  br i1 %cond, label %left, label %right

left:
  %from.b = getelementptr i8, ptr %b, i64 8 ; mixed-PHI arm from %b
  br label %join

right:
  br label %join

join:
  %mixed = phi ptr [ %from.b, %left ], [ %other, %right ] ; not on strip path
  call void @sink(ptr %mixed) ; tagged/mixed sibling use
  br label %loop

loop:
  %iv = phi i64 [ 0, %join ], [ %next, %loop ]
  %g = getelementptr i8, ptr %b, i64 %iv ; independent split target subtree
  %s = call ptr @__ptsan_strip(ptr %g) ; stripped reference required
  %v = load volatile i8, ptr %s, align 1 ; consumes the already-stripped reference
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; If a value is both an uncontained PHI input and the root of a stripped
; subtree, only the PHI operand should be retagged. The original subtree must
; stay stripped so contained users do not inherit the tagged boundary.
define void @retag_boundary_use_keeps_original_subtree(ptr %b, ptr %other,
                                                       ptr %slot, i1 %cond) {
; CHECK-LABEL: define void @retag_boundary_use_keeps_original_subtree(
; CHECK: entry:
; CHECK-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; CHECK: %from.b = getelementptr i8, ptr %b, i64 8
; CHECK-NEXT: [[FROM_B_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %from.b)
; CHECK: left:
; CHECK-NOT: call ptr @__ptsan_strip(ptr [[FROM_B_STRIP]])
; CHECK: join:
; CHECK: %mixed = phi ptr [ %from.b, %left ], [ %other, %right ]
; CHECK: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %mixed)
; CHECK: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; CHECK: loop:
; CHECK: %direct = getelementptr i8, ptr [[BASE_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %direct
; CHECK: %under.tag = getelementptr i8, ptr [[FROM_B_STRIP]], i64 %iv
; CHECK-NEXT: load volatile i8, ptr %under.tag
entry:
  %from.b = getelementptr i8, ptr %b, i64 8 ; mixed-PHI input and subtree root
  br i1 %cond, label %left, label %right

left:
  br label %join

right:
  br label %join

join:
  %mixed = phi ptr [ %from.b, %left ], [ %other, %right ] ; forces %from.b tagged
  store ptr %mixed, ptr %slot ; tagged consumer of the mixed PHI
  br label %loop

loop:
  %iv = phi i64 [ 0, %join ], [ %next, %loop ]
  %direct = getelementptr i8, ptr %b, i64 %iv ; makes the %b hoist profitable
  %direct.s = call ptr @__ptsan_strip(ptr %direct) ; direct strip saving
  %direct.v = load volatile i8, ptr %direct.s, align 1 ; consumes stripped direct path
  %under.tag = getelementptr i8, ptr %from.b, i64 %iv ; remains in original subtree
  %under.tag.s = call ptr @__ptsan_strip(ptr %under.tag) ; subtree strip saving
  %under.tag.v = load volatile i8, ptr %under.tag.s, align 1 ; consumes stripped subtree
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; PHI merges with different possible IDs synthesize a matching i64 ID PHI so
; tagged exits can restore the selected incoming ID.
define void @retag_stripped_phi_uses_id_phi(ptr %a, ptr %b, ptr %slot,
                                            i1 %choose) {
; CHECK-LABEL: define void @retag_stripped_phi_uses_id_phi(
; CHECK: entry:
; CHECK-DAG: [[A_ID:%.*]] = call i64 @__ptsan_get_id(ptr %a)
; CHECK-DAG: [[B_ID:%.*]] = call i64 @__ptsan_get_id(ptr %b)
; CHECK-DAG: [[A_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %a)
; CHECK-DAG: [[B_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; CHECK: left:
; CHECK: %la = getelementptr i8, ptr [[A_STRIP]], i64 8
; CHECK: right:
; CHECK: %rb = getelementptr i8, ptr [[B_STRIP]], i64 16
; CHECK: join:
; CHECK: [[MERGED_ID:%.*]] = phi i64 [ [[A_ID]], %left ], [ [[B_ID]], %right ]
; CHECK: %merged = phi ptr [ %la, %left ], [ %rb, %right ]
; CHECK-NEXT: [[MERGED_TAG:%.*]] = call ptr @__ptsan_tag(ptr %merged, i64 [[MERGED_ID]])
; CHECK-NEXT: [[MERGED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr [[MERGED_TAG]])
; CHECK-NEXT: store ptr [[MERGED_TAGGED]], ptr {{%.*}}
; CHECK: loop:
; CHECK: %x = getelementptr i8, ptr %merged, i64 %iv
; CHECK-NEXT: load volatile i8, ptr %x
; CHECK: %y = getelementptr i8, ptr %merged, i64 %next
; CHECK-NEXT: load volatile i8, ptr %y
; CHECK: %z = getelementptr i8, ptr %merged, i64 %next2
; CHECK-NEXT: load volatile i8, ptr %z
; CHECK: %w = getelementptr i8, ptr %merged, i64 %next3
; CHECK-NEXT: load volatile i8, ptr %w
entry:
  %a.s = call ptr @__ptsan_strip(ptr %a) ; incoming arm is already stripped
  %b.s = call ptr @__ptsan_strip(ptr %b) ; incoming arm is already stripped
  br i1 %choose, label %left, label %right

left:
  %la = getelementptr i8, ptr %a.s, i64 8
  br label %join

right:
  %rb = getelementptr i8, ptr %b.s, i64 16
  br label %join

join:
  %merged = phi ptr [ %la, %left ], [ %rb, %right ] ; stripped merged value
  store ptr %merged, ptr %slot ; tagged exit from a stripped PHI
  br label %loop

loop:
  %iv = phi i64 [ 0, %join ], [ %next, %loop ]
  %x = getelementptr i8, ptr %merged, i64 %iv
  %x.s = call ptr @__ptsan_strip(ptr %x)
  %x.v = load volatile i8, ptr %x.s, align 1
  %next = add nuw i64 %iv, 1
  %y = getelementptr i8, ptr %merged, i64 %next
  %y.s = call ptr @__ptsan_strip(ptr %y)
  %y.v = load volatile i8, ptr %y.s, align 1
  %next2 = add nuw i64 %next, 1
  %z = getelementptr i8, ptr %merged, i64 %next2
  %z.s = call ptr @__ptsan_strip(ptr %z)
  %z.v = load volatile i8, ptr %z.s, align 1
  %next3 = add nuw i64 %next2, 1
  %w = getelementptr i8, ptr %merged, i64 %next3
  %w.s = call ptr @__ptsan_strip(ptr %w)
  %w.v = load volatile i8, ptr %w.s, align 1
  %done = icmp eq i64 %next3, 8
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Selects need the same ID merge as PHIs, except the ID merge is an ordinary
; i64 select placed next to the retag.
define void @retag_stripped_select_uses_id_select(ptr %a, ptr %b, ptr %slot,
                                                  i1 %choose) {
; CHECK-LABEL: define void @retag_stripped_select_uses_id_select(
; CHECK: entry:
; CHECK-DAG: [[A_ID:%.*]] = call i64 @__ptsan_get_id(ptr %a)
; CHECK-DAG: [[B_ID:%.*]] = call i64 @__ptsan_get_id(ptr %b)
; CHECK-DAG: [[A_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %a)
; CHECK-DAG: [[B_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b)
; CHECK: %la = getelementptr i8, ptr [[A_STRIP]], i64 8
; CHECK-NEXT: %rb = getelementptr i8, ptr [[B_STRIP]], i64 16
; CHECK-NEXT: %merged = select i1 %choose, ptr %la, ptr %rb
; CHECK-NEXT: [[MERGED_ID:%.*]] = select i1 %choose, i64 [[A_ID]], i64 [[B_ID]]
; CHECK-NEXT: [[MERGED_TAG:%.*]] = call ptr @__ptsan_tag(ptr %merged, i64 [[MERGED_ID]])
; CHECK-NEXT: [[MERGED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr [[MERGED_TAG]])
; CHECK-NEXT: store ptr [[MERGED_TAGGED]], ptr {{%.*}}
; CHECK: loop:
; CHECK: %x = getelementptr i8, ptr %merged, i64 %iv
; CHECK-NEXT: load volatile i8, ptr %x
; CHECK: %y = getelementptr i8, ptr %merged, i64 %next
; CHECK-NEXT: load volatile i8, ptr %y
; CHECK: %z = getelementptr i8, ptr %merged, i64 %next2
; CHECK-NEXT: load volatile i8, ptr %z
; CHECK: %w = getelementptr i8, ptr %merged, i64 %next3
; CHECK-NEXT: load volatile i8, ptr %w
entry:
  %a.s = call ptr @__ptsan_strip(ptr %a) ; incoming arm is already stripped
  %b.s = call ptr @__ptsan_strip(ptr %b) ; incoming arm is already stripped
  %la = getelementptr i8, ptr %a.s, i64 8
  %rb = getelementptr i8, ptr %b.s, i64 16
  %merged = select i1 %choose, ptr %la, ptr %rb ; stripped merged value
  store ptr %merged, ptr %slot ; tagged exit from a stripped select
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %x = getelementptr i8, ptr %merged, i64 %iv
  %x.s = call ptr @__ptsan_strip(ptr %x)
  %x.v = load volatile i8, ptr %x.s, align 1
  %next = add nuw i64 %iv, 1
  %y = getelementptr i8, ptr %merged, i64 %next
  %y.s = call ptr @__ptsan_strip(ptr %y)
  %y.v = load volatile i8, ptr %y.s, align 1
  %next2 = add nuw i64 %next, 1
  %z = getelementptr i8, ptr %merged, i64 %next2
  %z.s = call ptr @__ptsan_strip(ptr %z)
  %z.v = load volatile i8, ptr %z.s, align 1
  %next3 = add nuw i64 %next2, 1
  %w = getelementptr i8, ptr %merged, i64 %next3
  %w.s = call ptr @__ptsan_strip(ptr %w)
  %w.v = load volatile i8, ptr %w.s, align 1
  %done = icmp eq i64 %next3, 8
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A stripped loop-carried pointer recurrence remains derived from its original
; base, so its tagged exit can reuse the base ID without a recurrent ID PHI.
define void @retag_loop_carried_gep_uses_base_id(ptr %base, ptr %slot) {
; CHECK-LABEL: define void @retag_loop_carried_gep_uses_base_id(
; CHECK: entry:
; CHECK-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %base)
; CHECK-DAG: [[BASE_ID:%.*]] = call i64 @__ptsan_get_id(ptr %base)
; CHECK: loop:
; CHECK: %cursor = phi ptr [ %next.ptr, %loop ], [ [[BASE_STRIP]], %loop.preheader ]
; CHECK: %next.ptr = getelementptr i8, ptr %cursor, i64 1
; CHECK: %x = getelementptr i8, ptr %next.ptr, i64 8
; CHECK-NEXT: load volatile i8, ptr %x
; CHECK: %y = getelementptr i8, ptr %next.ptr, i64 16
; CHECK-NEXT: load volatile i8, ptr %y
; CHECK: exit:
; CHECK: %exit.ptr = phi ptr [ %next.ptr, %loop ]
; CHECK-NEXT: [[EXIT_TAG:%.*]] = call ptr @__ptsan_tag(ptr %exit.ptr, i64 [[BASE_ID]])
; CHECK-NEXT: [[EXIT_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr [[EXIT_TAG]])
; CHECK-NEXT: store ptr [[EXIT_TAGGED]], ptr {{%.*}}
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next.i, %loop ]
  %cursor = phi ptr [ %base, %entry ], [ %next.ptr, %loop ]
  %next.ptr = getelementptr i8, ptr %cursor, i64 1
  %x = getelementptr i8, ptr %next.ptr, i64 8
  %x.s = call ptr @__ptsan_strip(ptr %x)
  %x.v = load volatile i8, ptr %x.s, align 1
  %y = getelementptr i8, ptr %next.ptr, i64 16
  %y.s = call ptr @__ptsan_strip(ptr %y)
  %y.v = load volatile i8, ptr %y.s, align 1
  %next.i = add nuw i64 %i, 1
  %done = icmp eq i64 %next.i, 4
  br i1 %done, label %exit, label %loop

exit:
  %exit.ptr = phi ptr [ %next.ptr, %loop ]
  store ptr %exit.ptr, ptr %slot ; tagged exit from the stripped recurrence
  ret void
}
