; RUN: split-file %s %t
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %t/pointers.ll | FileCheck %t/pointers.ll --check-prefix=ID16
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks -ptsan-stop-after=late-optimization %t/pointers.ll | FileCheck %t/pointers.ll --check-prefix=ID32
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %t/id16.ll | FileCheck %t/id16.ll
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-nochecks -ptsan-stop-after=late-optimization %t/id32.ll | FileCheck %t/id32.ll

; Ordinary pointer inputs are shared. The two anchor-input cases below explicitly
; start with stripped PHI/select arms, and have separate IR for each encoding.

;--- pointers.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @sink(ptr)

; A tagged sibling use must not block stripping the loop's memory accesses.
define void @tagged_sibling_does_not_poison_neutral_path(ptr %b) {
; ID16-LABEL: define void @tagged_sibling_does_not_poison_neutral_path(
; ID16: entry:
; ID16: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b
; ID16: loop:
; ID16: %g = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID16-NOT: call ptr @__ptsan_strip
; ID16: load volatile i8, ptr %g
; ID32-LABEL: define void @tagged_sibling_does_not_poison_neutral_path(
; ID32: entry:
; ID32: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %b
; ID32: loop:
; ID32: %g = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID32-NOT: call ptr @__ptsan_strip
; ID32: load volatile i8, ptr %g
entry:
  call void @sink(ptr %b)
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %g = getelementptr inbounds i8, ptr %b, i64 %iv ; split target subtree
  %v = load volatile i8, ptr %g, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A value shared by a mixed PHI and a memory-access subtree needs both polarities.
define void @tag_result_split_between_phi_and_strip_subtree(ptr %b, ptr %other,
                                                            ptr %slot,
                                                            i1 %cond) {
; ID16-LABEL: define void @tag_result_split_between_phi_and_strip_subtree(
; ID16: entry:
; ID16-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b
; ID16: [[FROM_B_TAGGED:%from.b]] = getelementptr inbounds i8, ptr %b, i64 8
; ID16: [[FROM_B_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %from.b
; ID16: left:
; ID16: join:
; ID16: %mixed = phi ptr [ [[FROM_B_TAGGED]], %left ], [ %other, %right ]
; ID16: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}%mixed)
; ID16: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; ID16: loop:
; ID16: %direct = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID16-NEXT: load volatile i8, ptr %direct
; ID16: %under.tag = getelementptr inbounds i8, ptr [[FROM_B_STRIP]], i64 %iv
; ID16-NEXT: load volatile i8, ptr %under.tag
; ID32-LABEL: define void @tag_result_split_between_phi_and_strip_subtree(
; ID32: entry:
; ID32-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %b
; ID32: [[FROM_B_STRIP:%from.b]] = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 8
; ID32: [[FROM_B_TAGGED:%.*]] = call ptr @__ptsan_tag_id32(ptr %from.b
; ID32: left:
; ID32: join:
; ID32: %mixed = phi ptr [ [[FROM_B_TAGGED]], %left ], [ %other, %right ]
; ID32: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}%mixed)
; ID32: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; ID32: loop:
; ID32: %direct = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID32-NEXT: load volatile i8, ptr %direct
; ID32: %under.tag = getelementptr inbounds i8, ptr [[FROM_B_STRIP]], i64 %iv
; ID32-NEXT: load volatile i8, ptr %under.tag
entry:
  %from.b = getelementptr inbounds i8, ptr %b, i64 8 ; uncontained PHI input
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
  %direct = getelementptr inbounds i8, ptr %b, i64 %iv ; makes the %b hoist profitable
  %direct.v = load volatile i8, ptr %direct, align 1
  %under.tag = getelementptr inbounds i8, ptr %from.b, i64 %iv ; original subtree root
  %under.tag.v = load volatile i8, ptr %under.tag, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Keep a contained PHI's memory uses stripped without corrupting its mixed exit.
define void @contained_phi_feeds_uncontained_phi(ptr %b, ptr %other,
                                                 ptr %slot, i1 %c1, i1 %c2) {
; ID16-LABEL: define void @contained_phi_feeds_uncontained_phi(
; ID16: entry:
; ID16-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b
; ID16: left:
; ID16: %l = getelementptr inbounds i8, ptr %b, i64 8
; ID16: right:
; ID16: %r = getelementptr inbounds i8, ptr %b, i64 16
; ID16: contained.join:
; ID16: [[CONTAINED_STRIP:%contained]] = phi ptr [ %l, %left ], [ %r, %right ]
; ID16: [[CONTAINED_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %contained
; ID16: mixed.left:
; ID16: mixed.join:
; ID16: %mixed = phi ptr [ %contained, %mixed.left ], [ %other, %mixed.right ]
; ID16: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}%mixed)
; ID16: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; ID16: loop:
; ID16: %direct = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID16-NEXT: load volatile i8, ptr %direct
; ID16: %under.contained = getelementptr inbounds i8, ptr [[CONTAINED_STRIP]], i64 %iv
; ID16-NEXT: load volatile i8, ptr %under.contained
; ID32-LABEL: define void @contained_phi_feeds_uncontained_phi(
; ID32: entry:
; ID32-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %b
; ID32: left:
; ID32: %l = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 8
; ID32: right:
; ID32: %r = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 16
; ID32: contained.join:
; ID32: [[CONTAINED_STRIP:%contained]] = phi ptr [ %l, %left ], [ %r, %right ]
; ID32: [[CONTAINED_TAGGED:%.*]] = call ptr @__ptsan_tag_id32(ptr %contained
; ID32: mixed.left:
; ID32: mixed.join:
; ID32: %mixed = phi ptr [ [[CONTAINED_TAGGED]], %mixed.left ], [ %other, %mixed.right ]
; ID32: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}%mixed)
; ID32: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; ID32: loop:
; ID32: %direct = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID32-NEXT: load volatile i8, ptr %direct
; ID32: %under.contained = getelementptr inbounds i8, ptr [[CONTAINED_STRIP]], i64 %iv
; ID32-NEXT: load volatile i8, ptr %under.contained
entry:
  br i1 %c1, label %left, label %right

left:
  %l = getelementptr inbounds i8, ptr %b, i64 8 ; contained PHI arm
  br label %contained.join

right:
  %r = getelementptr inbounds i8, ptr %b, i64 16 ; contained PHI arm
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
  %direct = getelementptr inbounds i8, ptr %b, i64 %iv ; makes the %b hoist profitable
  %direct.v = load volatile i8, ptr %direct, align 1
  %under.contained = getelementptr inbounds i8, ptr %contained, i64 %iv
  %under.contained.v = load volatile i8, ptr %under.contained, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Selects need the same polarity separation as contained and mixed PHIs.
define void @contained_select_feeds_uncontained_select(ptr %b, ptr %other,
                                                       ptr %slot, i1 %c1,
                                                       i1 %c2) {
; ID16-LABEL: define void @contained_select_feeds_uncontained_select(
; ID16: entry:
; ID16-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b
; ID16: %l = getelementptr inbounds i8, ptr %b, i64 8
; ID16-NEXT: %r = getelementptr inbounds i8, ptr %b, i64 16
; ID16-NEXT: [[CONTAINED_STRIP:%contained]] = select i1 %c1, ptr %l, ptr %r
; ID16: [[CONTAINED_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %contained
; ID16: %mixed = select i1 %c2, ptr %contained, ptr %other
; ID16: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}%mixed)
; ID16: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; ID16: loop:
; ID16: %direct = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID16-NEXT: load volatile i8, ptr %direct
; ID16: %under.contained = getelementptr inbounds i8, ptr [[CONTAINED_STRIP]], i64 %iv
; ID16-NEXT: load volatile i8, ptr %under.contained
; ID32-LABEL: define void @contained_select_feeds_uncontained_select(
; ID32: entry:
; ID32-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %b
; ID32: %l = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 8
; ID32-NEXT: %r = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 16
; ID32-NEXT: [[CONTAINED_STRIP:%contained]] = select i1 %c1, ptr %l, ptr %r
; ID32: [[CONTAINED_TAGGED:%.*]] = call ptr @__ptsan_tag_id32(ptr %contained
; ID32: %mixed = select i1 %c2, ptr [[CONTAINED_TAGGED]], ptr %other
; ID32: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}%mixed)
; ID32: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; ID32: loop:
; ID32: %direct = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID32-NEXT: load volatile i8, ptr %direct
; ID32: %under.contained = getelementptr inbounds i8, ptr [[CONTAINED_STRIP]], i64 %iv
; ID32-NEXT: load volatile i8, ptr %under.contained
entry:
  %l = getelementptr inbounds i8, ptr %b, i64 8 ; contained select arm
  %r = getelementptr inbounds i8, ptr %b, i64 16 ; contained select arm
  %contained = select i1 %c1, ptr %l, ptr %r ; common %b-derived select
  %mixed = select i1 %c2, ptr %contained, ptr %other ; must stay tagged
  store ptr %mixed, ptr %slot ; tagged consumer of the uncontained select
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %direct = getelementptr inbounds i8, ptr %b, i64 %iv ; makes the %b hoist profitable
  %direct.v = load volatile i8, ptr %direct, align 1
  %under.contained = getelementptr inbounds i8, ptr %contained, i64 %iv
  %under.contained.v = load volatile i8, ptr %under.contained, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Same-base PHI arms should permit one shared strip for the loop.
define void @phi_same_base_is_valid_base(ptr %b, i1 %cond) {
; ID16-LABEL: define void @phi_same_base_is_valid_base(
; ID16: entry:
; ID16: left:
; ID16: %l = getelementptr inbounds i8, ptr %b, i64 8
; ID16: right:
; ID16: %r = getelementptr inbounds i8, ptr %b, i64 16
; ID16: join:
; ID16: [[P_STRIP:%p]] = phi ptr [ %l, %left ], [ %r, %right ]
; ID16: [[P_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %p
; ID16: loop:
; ID16: %g = getelementptr inbounds i8, ptr [[P_STRIP]], i64 %iv
; ID16-NEXT: load volatile i8, ptr %g
; ID32-LABEL: define void @phi_same_base_is_valid_base(
; ID32: entry:
; ID32: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %b
; ID32: left:
; ID32: %l = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 8
; ID32: right:
; ID32: %r = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 16
; ID32: join:
; ID32: [[P_STRIP:%p]] = phi ptr [ %l, %left ], [ %r, %right ]
; ID32: loop:
; ID32: %g = getelementptr inbounds i8, ptr [[P_STRIP]], i64 %iv
; ID32-NEXT: load volatile i8, ptr %g
entry:
  call void @sink(ptr %b)
  br i1 %cond, label %left, label %right

left:
  %l = getelementptr inbounds i8, ptr %b, i64 8 ; PHI arm derived from %b
  br label %join

right:
  %r = getelementptr inbounds i8, ptr %b, i64 16 ; PHI arm derived from %b
  br label %join

join:
  %p = phi ptr [ %l, %left ], [ %r, %right ] ; valid common base
  br label %loop

loop:
  %iv = phi i64 [ 0, %join ], [ %next, %loop ]
  %g = getelementptr inbounds i8, ptr %p, i64 %iv ; neutral use of PHI base
  %v = load volatile i8, ptr %g, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Nested same-base PHIs should not require a strip for every loop access.
define void @nested_phi_same_base_is_valid_base(ptr %b, i1 %c1, i1 %c2) {
; ID16-LABEL: define void @nested_phi_same_base_is_valid_base(
; ID16: entry:
; ID16: left:
; ID16: %l = getelementptr inbounds i8, ptr %b, i64 8
; ID16: right:
; ID16: %r = getelementptr inbounds i8, ptr %b, i64 16
; ID16: inner.join:
; ID16: %inner = phi ptr [ %l, %left ], [ %r, %right ]
; ID16: %direct = getelementptr inbounds i8, ptr %b, i64 32
; ID16: outer.join:
; ID16: [[OUTER_STRIP:%outer]] = phi ptr [ %inner, %inner.join ], [ %direct, %direct.block ]
; ID16: [[OUTER_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %outer
; ID16: loop:
; ID16: %g = getelementptr inbounds i8, ptr [[OUTER_STRIP]], i64 %iv
; ID16-NEXT: load volatile i8, ptr %g
; ID32-LABEL: define void @nested_phi_same_base_is_valid_base(
; ID32: entry:
; ID32: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %b
; ID32: left:
; ID32: %l = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 8
; ID32: right:
; ID32: %r = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 16
; ID32: inner.join:
; ID32: %inner = phi ptr [ %l, %left ], [ %r, %right ]
; ID32: %direct = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 32
; ID32: outer.join:
; ID32: [[OUTER_STRIP:%outer]] = phi ptr [ %inner, %inner.join ], [ %direct, %direct.block ]
; ID32: loop:
; ID32: %g = getelementptr inbounds i8, ptr [[OUTER_STRIP]], i64 %iv
; ID32-NEXT: load volatile i8, ptr %g
entry:
  call void @sink(ptr %b)
  br i1 %c1, label %left, label %right

left:
  %l = getelementptr inbounds i8, ptr %b, i64 8 ; inner PHI arm
  br label %inner.join

right:
  %r = getelementptr inbounds i8, ptr %b, i64 16 ; inner PHI arm
  br label %inner.join

inner.join:
  %inner = phi ptr [ %l, %left ], [ %r, %right ] ; not final strip base
  br i1 %c2, label %outer.join, label %direct.block

direct.block:
  %direct = getelementptr inbounds i8, ptr %b, i64 32 ; second outer PHI arm
  br label %outer.join

outer.join:
  %outer = phi ptr [ %inner, %inner.join ], [ %direct, %direct.block ] ; final common base
  br label %loop

loop:
  %iv = phi i64 [ 0, %outer.join ], [ %next, %loop ]
  %g = getelementptr inbounds i8, ptr %outer, i64 %iv ; neutral use of outer base
  %v = load volatile i8, ptr %g, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; An unrelated mixed PHI must not block hoisting the loop's strip.
define void @mixed_phi_tagged_sibling_not_on_strip_path(ptr %b, ptr %other,
                                                        i1 %cond) {
; ID16-LABEL: define void @mixed_phi_tagged_sibling_not_on_strip_path(
; ID16: entry:
; ID16: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b
; ID16: left:
; ID16: [[FROM_B_TAGGED:%from.b]] = getelementptr inbounds i8, ptr %b, i64 8
; ID16: join:
; ID16: %mixed = phi ptr [ [[FROM_B_TAGGED]], %left ], [ %other, %right ]
; ID16: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}%mixed)
; ID16: call void @sink(ptr [[MIXED_TAGGED]])
; ID16: loop:
; ID16: %g = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID16-NOT: call ptr @__ptsan_strip
; ID16: load volatile i8, ptr %g
; ID32-LABEL: define void @mixed_phi_tagged_sibling_not_on_strip_path(
; ID32: entry:
; ID32: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %b
; ID32: left:
; ID32: %from.b = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 8
; ID32: [[FROM_B_TAGGED:%.*]] = call ptr @__ptsan_tag_id32(ptr %from.b
; ID32: join:
; ID32: %mixed = phi ptr [ [[FROM_B_TAGGED]], %left ], [ %other, %right ]
; ID32: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}%mixed)
; ID32: call void @sink(ptr [[MIXED_TAGGED]])
; ID32: loop:
; ID32: %g = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID32-NOT: call ptr @__ptsan_strip
; ID32: load volatile i8, ptr %g
entry:
  br i1 %cond, label %left, label %right

left:
  %from.b = getelementptr inbounds i8, ptr %b, i64 8 ; mixed-PHI arm from %b
  br label %join

right:
  br label %join

join:
  %mixed = phi ptr [ %from.b, %left ], [ %other, %right ] ; not on strip path
  call void @sink(ptr %mixed) ; tagged/mixed sibling use
  br label %loop

loop:
  %iv = phi i64 [ 0, %join ], [ %next, %loop ]
  %g = getelementptr inbounds i8, ptr %b, i64 %iv ; independent split target subtree
  %v = load volatile i8, ptr %g, align 1
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Retag the escaping PHI operand without retagging its memory-access subtree.
define void @retag_boundary_use_keeps_original_subtree(ptr %b, ptr %other,
                                                       ptr %slot, i1 %cond) {
; ID16-LABEL: define void @retag_boundary_use_keeps_original_subtree(
; ID16: entry:
; ID16-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b
; ID16: [[FROM_B_TAGGED:%from.b]] = getelementptr inbounds i8, ptr %b, i64 8
; ID16: [[FROM_B_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %from.b
; ID16: left:
; ID16-NOT: call ptr @__ptsan_strip(ptr [[FROM_B_STRIP]]
; ID16: join:
; ID16: %mixed = phi ptr [ [[FROM_B_TAGGED]], %left ], [ %other, %right ]
; ID16: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}%mixed)
; ID16: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; ID16: loop:
; ID16: %direct = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID16-NEXT: load volatile i8, ptr %direct
; ID16: %under.tag = getelementptr inbounds i8, ptr [[FROM_B_STRIP]], i64 %iv
; ID16-NEXT: load volatile i8, ptr %under.tag
; ID32-LABEL: define void @retag_boundary_use_keeps_original_subtree(
; ID32: entry:
; ID32-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %b
; ID32: [[FROM_B_STRIP:%from.b]] = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 8
; ID32: [[FROM_B_TAGGED:%.*]] = call ptr @__ptsan_tag_id32(ptr %from.b
; ID32: left:
; ID32: join:
; ID32: %mixed = phi ptr [ [[FROM_B_TAGGED]], %left ], [ %other, %right ]
; ID32: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}%mixed)
; ID32: store ptr [[MIXED_TAGGED]], ptr {{%.*}}
; ID32-NOT: call ptr @__ptsan_strip_id32(ptr [[FROM_B_STRIP]]
; ID32: loop:
; ID32: %direct = getelementptr inbounds i8, ptr [[BASE_STRIP]], i64 %iv
; ID32-NEXT: load volatile i8, ptr %direct
; ID32: %under.tag = getelementptr inbounds i8, ptr [[FROM_B_STRIP]], i64 %iv
; ID32-NEXT: load volatile i8, ptr %under.tag
entry:
  %from.b = getelementptr inbounds i8, ptr %b, i64 8 ; mixed-PHI input and subtree root
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
  %direct = getelementptr inbounds i8, ptr %b, i64 %iv ; makes the %b hoist profitable
  %direct.v = load volatile i8, ptr %direct, align 1 ; consumes stripped direct path
  %under.tag = getelementptr inbounds i8, ptr %from.b, i64 %iv ; remains in original subtree
  %under.tag.v = load volatile i8, ptr %under.tag, align 1 ; consumes stripped subtree
  %next = add nuw i64 %iv, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; A stripped pointer recurrence must reuse its original ID at the tagged exit.
define void @retag_loop_carried_gep_uses_base_id(ptr %base, ptr %slot) {
; ID16-LABEL: define void @retag_loop_carried_gep_uses_base_id(
; ID16: entry:
; ID16-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %base
; ID16-DAG: [[BASE_ID:%.*]] = call i64 @__ptsan_get_id(ptr %base)
; ID16: loop:
; ID16: %cursor = phi ptr [ %next.ptr, %loop ], [ [[BASE_STRIP]], %loop.preheader ]
; ID16: %next.ptr = getelementptr inbounds i8, ptr %cursor, i64 1
; ID16: %x = getelementptr inbounds i8, ptr %next.ptr, i64 8
; ID16-NEXT: load volatile i8, ptr %x
; ID16: %y = getelementptr inbounds i8, ptr %next.ptr, i64 16
; ID16-NEXT: load volatile i8, ptr %y
; ID16: exit:
; ID16: %exit.ptr = phi ptr [ %next.ptr, %loop ]
; ID16: [[EXIT_TAG:%.*]] = call ptr @__ptsan_tag(ptr %exit.ptr, i64 [[BASE_ID]]
; ID16-NEXT: [[EXIT_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}[[EXIT_TAG]])
; ID16: store ptr [[EXIT_TAGGED]], ptr {{%.*}}
; ID32-LABEL: define void @retag_loop_carried_gep_uses_base_id(
; ID32: entry:
; ID32-DAG: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %base
; ID32-DAG: [[BASE_ID:%.*]] = call i64 @__ptsan_get_id(ptr %base)
; ID32: loop:
; ID32: %cursor = phi ptr [ %next.ptr, %loop ], [ [[BASE_STRIP]], %loop.preheader ]
; ID32: %next.ptr = getelementptr inbounds i8, ptr %cursor, i64 1
; ID32: %x = getelementptr inbounds i8, ptr %next.ptr, i64 8
; ID32-NEXT: load volatile i8, ptr %x
; ID32: %y = getelementptr inbounds i8, ptr %next.ptr, i64 16
; ID32-NEXT: load volatile i8, ptr %y
; ID32: exit:
; ID32: %exit.ptr = phi ptr [ %next.ptr, %loop ]
; ID32: [[EXIT_TAG:%.*]] = call ptr @__ptsan_tag_id32(ptr %exit.ptr, i64 [[BASE_ID]]
; ID32-NEXT: [[EXIT_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}[[EXIT_TAG]])
; ID32: call ptr @__ptsan_strip_id32(ptr %slot, i64 {{%.*}}, i64 {{%.*}})
; ID32: store ptr [[EXIT_TAGGED]], ptr {{%.*}}
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next.i, %loop ]
  %cursor = phi ptr [ %base, %entry ], [ %next.ptr, %loop ]
  %next.ptr = getelementptr inbounds i8, ptr %cursor, i64 1
  %x = getelementptr inbounds i8, ptr %next.ptr, i64 8
  %x.v = load volatile i8, ptr %x, align 1
  %y = getelementptr inbounds i8, ptr %next.ptr, i64 16
  %y.v = load volatile i8, ptr %y, align 1
  %next.i = add nuw i64 %i, 1
  %done = icmp eq i64 %next.i, 4
  br i1 %done, label %exit, label %loop

exit:
  %exit.ptr = phi ptr [ %next.ptr, %loop ]
  store ptr %exit.ptr, ptr %slot ; tagged exit from the stripped recurrence
  ret void
}

;--- id16.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip(ptr) nofree

; Start with stripped inputs to exercise reconstruction of the merged ID.
define void @retag_stripped_phi_uses_id_phi(ptr %a, ptr %b, ptr %slot,
                                            i1 %choose) {
; CHECK-LABEL: define void @retag_stripped_phi_uses_id_phi(
; CHECK: entry:
; CHECK-DAG: [[A_ID:%.*]] = call i64 @__ptsan_get_id(ptr %a)
; CHECK-DAG: [[B_ID:%.*]] = call i64 @__ptsan_get_id(ptr %b)
; CHECK-DAG: [[A_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %a
; CHECK-DAG: [[B_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b
; CHECK: left:
; CHECK: %la = getelementptr inbounds i8, ptr [[A_STRIP]], i64 8
; CHECK: right:
; CHECK: %rb = getelementptr inbounds i8, ptr [[B_STRIP]], i64 16
; CHECK: join:
; CHECK: [[MERGED_ID:%.*]] = phi i64 [ [[A_ID]], %left ], [ [[B_ID]], %right ]
; CHECK: %merged = phi ptr [ %la, %left ], [ %rb, %right ]
; CHECK: [[MERGED_TAG:%.*]] = call ptr @__ptsan_tag(ptr %merged, i64 [[MERGED_ID]]
; CHECK-NEXT: [[MERGED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}[[MERGED_TAG]])
; CHECK: store ptr [[MERGED_TAGGED]], ptr {{%.*}}
; CHECK: loop:
; CHECK: %x = getelementptr inbounds i8, ptr %merged, i64 %iv
; CHECK-NEXT: load volatile i8, ptr %x
; CHECK: %y = getelementptr inbounds i8, ptr %merged, i64 %next
; CHECK-NEXT: load volatile i8, ptr %y
; CHECK: %z = getelementptr inbounds i8, ptr %merged, i64 %next2
; CHECK-NEXT: load volatile i8, ptr %z
; CHECK: %w = getelementptr inbounds i8, ptr %merged, i64 %next3
; CHECK-NEXT: load volatile i8, ptr %w
entry:
  %a.s = call ptr @__ptsan_strip(ptr %a) ; incoming arm is already stripped
  %b.s = call ptr @__ptsan_strip(ptr %b) ; incoming arm is already stripped
  br i1 %choose, label %left, label %right

left:
  %la = getelementptr inbounds i8, ptr %a.s, i64 8
  br label %join

right:
  %rb = getelementptr inbounds i8, ptr %b.s, i64 16
  br label %join

join:
  %merged = phi ptr [ %la, %left ], [ %rb, %right ] ; stripped merged value
  store ptr %merged, ptr %slot ; tagged exit from a stripped PHI
  br label %loop

loop:
  %iv = phi i64 [ 0, %join ], [ %next, %loop ]
  %x = getelementptr inbounds i8, ptr %merged, i64 %iv
  %x.v = load volatile i8, ptr %x, align 1
  %next = add nuw i64 %iv, 1
  %y = getelementptr inbounds i8, ptr %merged, i64 %next
  %y.v = load volatile i8, ptr %y, align 1
  %next2 = add nuw i64 %next, 1
  %z = getelementptr inbounds i8, ptr %merged, i64 %next2
  %z.v = load volatile i8, ptr %z, align 1
  %next3 = add nuw i64 %next2, 1
  %w = getelementptr inbounds i8, ptr %merged, i64 %next3
  %w.v = load volatile i8, ptr %w, align 1
  %done = icmp eq i64 %next3, 8
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Start with stripped inputs to exercise reconstruction of the merged ID.
define void @retag_stripped_select_uses_id_select(ptr %a, ptr %b, ptr %slot,
                                                  i1 %choose) {
; CHECK-LABEL: define void @retag_stripped_select_uses_id_select(
; CHECK: entry:
; CHECK-DAG: [[A_ID:%.*]] = call i64 @__ptsan_get_id(ptr %a)
; CHECK-DAG: [[B_ID:%.*]] = call i64 @__ptsan_get_id(ptr %b)
; CHECK-DAG: [[A_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %a
; CHECK-DAG: [[B_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %b
; CHECK-DAG: %la = getelementptr inbounds i8, ptr [[A_STRIP]], i64 8
; CHECK-DAG: %rb = getelementptr inbounds i8, ptr [[B_STRIP]], i64 16
; CHECK: %merged = select i1 %choose, ptr %la, ptr %rb
; CHECK-NEXT: [[MERGED_ID:%.*]] = select i1 %choose, i64 [[A_ID]], i64 [[B_ID]]
; CHECK: [[MERGED_TAG:%.*]] = call ptr @__ptsan_tag(ptr %merged, i64 [[MERGED_ID]]
; CHECK-NEXT: [[MERGED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}[[MERGED_TAG]])
; CHECK: store ptr [[MERGED_TAGGED]], ptr {{%.*}}
; CHECK: loop:
; CHECK: %x = getelementptr inbounds i8, ptr %merged, i64 %iv
; CHECK-NEXT: load volatile i8, ptr %x
; CHECK: %y = getelementptr inbounds i8, ptr %merged, i64 %next
; CHECK-NEXT: load volatile i8, ptr %y
; CHECK: %z = getelementptr inbounds i8, ptr %merged, i64 %next2
; CHECK-NEXT: load volatile i8, ptr %z
; CHECK: %w = getelementptr inbounds i8, ptr %merged, i64 %next3
; CHECK-NEXT: load volatile i8, ptr %w
entry:
  %a.s = call ptr @__ptsan_strip(ptr %a) ; incoming arm is already stripped
  %b.s = call ptr @__ptsan_strip(ptr %b) ; incoming arm is already stripped
  %la = getelementptr inbounds i8, ptr %a.s, i64 8
  %rb = getelementptr inbounds i8, ptr %b.s, i64 16
  %merged = select i1 %choose, ptr %la, ptr %rb ; stripped merged value
  store ptr %merged, ptr %slot ; tagged exit from a stripped select
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %x = getelementptr inbounds i8, ptr %merged, i64 %iv
  %x.v = load volatile i8, ptr %x, align 1
  %next = add nuw i64 %iv, 1
  %y = getelementptr inbounds i8, ptr %merged, i64 %next
  %y.v = load volatile i8, ptr %y, align 1
  %next2 = add nuw i64 %next, 1
  %z = getelementptr inbounds i8, ptr %merged, i64 %next2
  %z.v = load volatile i8, ptr %z, align 1
  %next3 = add nuw i64 %next2, 1
  %w = getelementptr inbounds i8, ptr %merged, i64 %next3
  %w.v = load volatile i8, ptr %w, align 1
  %done = icmp eq i64 %next3, 8
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
;--- id32.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @__ptsan_strip_id32(ptr, i64, i64) nofree
declare i64 @__ptsan_load_base(i64) nofree
declare i64 @__ptsan_get_id(ptr) nofree

; Start with stripped inputs to exercise reconstruction of the merged ID.
define void @retag_stripped_phi_uses_id_phi(ptr %a, ptr %b, ptr %slot,
                                            i1 %choose) {
; CHECK-LABEL: define void @retag_stripped_phi_uses_id_phi(
; CHECK: entry:
; CHECK-DAG: [[A_ID:%.*]] = call i64 @__ptsan_get_id(ptr %a)
; CHECK-DAG: [[B_ID:%.*]] = call i64 @__ptsan_get_id(ptr %b)
; CHECK: left:
; CHECK: [[A_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %a, i64 [[A_ID]], i64 {{%.*}})
; CHECK: %la = getelementptr inbounds i8, ptr [[A_STRIP]], i64 8
; CHECK: right:
; CHECK: [[B_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %b, i64 [[B_ID]], i64 {{%.*}})
; CHECK: %rb = getelementptr inbounds i8, ptr [[B_STRIP]], i64 16
; CHECK: join:
; CHECK: [[MERGED_ID:%.*]] = phi i64 [ [[A_ID]], %left ], [ [[B_ID]], %right ]
; CHECK: %merged = phi ptr [ %la, %left ], [ %rb, %right ]
; CHECK: [[MERGED_TAG:%.*]] = call ptr @__ptsan_tag_id32(ptr %merged, i64 [[MERGED_ID]]
; CHECK-NEXT: [[MERGED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}[[MERGED_TAG]])
; CHECK: store ptr [[MERGED_TAGGED]], ptr {{%.*}}
; CHECK: loop:
; CHECK: %x = getelementptr inbounds i8, ptr %merged, i64 %iv
; CHECK-NEXT: load volatile i8, ptr %x
; CHECK: %y = getelementptr inbounds i8, ptr %merged, i64 %next
; CHECK-NEXT: load volatile i8, ptr %y
; CHECK: %z = getelementptr inbounds i8, ptr %merged, i64 %next2
; CHECK-NEXT: load volatile i8, ptr %z
; CHECK: %w = getelementptr inbounds i8, ptr %merged, i64 %next3
; CHECK-NEXT: load volatile i8, ptr %w
entry:
  %a.s.id = call i64 @__ptsan_get_id(ptr %a)
  %a.s.base = call i64 @__ptsan_load_base(i64 %a.s.id)
  %a.s = call ptr @__ptsan_strip_id32(ptr %a, i64 %a.s.id, i64 %a.s.base) ; incoming arm is already stripped
  %b.s.id = call i64 @__ptsan_get_id(ptr %b)
  %b.s.base = call i64 @__ptsan_load_base(i64 %b.s.id)
  %b.s = call ptr @__ptsan_strip_id32(ptr %b, i64 %b.s.id, i64 %b.s.base) ; incoming arm is already stripped
  br i1 %choose, label %left, label %right

left:
  %la = getelementptr inbounds i8, ptr %a.s, i64 8
  br label %join

right:
  %rb = getelementptr inbounds i8, ptr %b.s, i64 16
  br label %join

join:
  %merged = phi ptr [ %la, %left ], [ %rb, %right ] ; stripped merged value
  store ptr %merged, ptr %slot ; tagged exit from a stripped PHI
  br label %loop

loop:
  %iv = phi i64 [ 0, %join ], [ %next, %loop ]
  %x = getelementptr inbounds i8, ptr %merged, i64 %iv
  %x.v = load volatile i8, ptr %x, align 1
  %next = add nuw i64 %iv, 1
  %y = getelementptr inbounds i8, ptr %merged, i64 %next
  %y.v = load volatile i8, ptr %y, align 1
  %next2 = add nuw i64 %next, 1
  %z = getelementptr inbounds i8, ptr %merged, i64 %next2
  %z.v = load volatile i8, ptr %z, align 1
  %next3 = add nuw i64 %next2, 1
  %w = getelementptr inbounds i8, ptr %merged, i64 %next3
  %w.v = load volatile i8, ptr %w, align 1
  %done = icmp eq i64 %next3, 8
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Start with stripped inputs to exercise reconstruction of the merged ID.
define void @retag_stripped_select_uses_id_select(ptr %a, ptr %b, ptr %slot,
                                                  i1 %choose) {
; CHECK-LABEL: define void @retag_stripped_select_uses_id_select(
; CHECK: entry:
; CHECK-DAG: [[A_ID:%.*]] = call i64 @__ptsan_get_id(ptr %a)
; CHECK-DAG: [[B_ID:%.*]] = call i64 @__ptsan_get_id(ptr %b)
; CHECK-DAG: [[A_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %a
; CHECK-DAG: [[B_STRIP:%.*]] = call ptr @__ptsan_strip_id32(ptr %b
; CHECK-DAG: %la = getelementptr inbounds i8, ptr [[A_STRIP]], i64 8
; CHECK-DAG: %rb = getelementptr inbounds i8, ptr [[B_STRIP]], i64 16
; CHECK: %merged = select i1 %choose, ptr %la, ptr %rb
; CHECK-NEXT: [[MERGED_ID:%.*]] = select i1 %choose, i64 [[A_ID]], i64 [[B_ID]]
; CHECK: [[MERGED_TAG:%.*]] = call ptr @__ptsan_tag_id32(ptr %merged, i64 [[MERGED_ID]]
; CHECK-NEXT: [[MERGED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr {{.*}}[[MERGED_TAG]])
; CHECK: store ptr [[MERGED_TAGGED]], ptr {{%.*}}
; CHECK: loop:
; CHECK: %x = getelementptr inbounds i8, ptr %merged, i64 %iv
; CHECK-NEXT: load volatile i8, ptr %x
; CHECK: %y = getelementptr inbounds i8, ptr %merged, i64 %next
; CHECK-NEXT: load volatile i8, ptr %y
; CHECK: %z = getelementptr inbounds i8, ptr %merged, i64 %next2
; CHECK-NEXT: load volatile i8, ptr %z
; CHECK: %w = getelementptr inbounds i8, ptr %merged, i64 %next3
; CHECK-NEXT: load volatile i8, ptr %w
entry:
  %a.s.id = call i64 @__ptsan_get_id(ptr %a)
  %a.s.base = call i64 @__ptsan_load_base(i64 %a.s.id)
  %a.s = call ptr @__ptsan_strip_id32(ptr %a, i64 %a.s.id, i64 %a.s.base) ; incoming arm is already stripped
  %b.s.id = call i64 @__ptsan_get_id(ptr %b)
  %b.s.base = call i64 @__ptsan_load_base(i64 %b.s.id)
  %b.s = call ptr @__ptsan_strip_id32(ptr %b, i64 %b.s.id, i64 %b.s.base) ; incoming arm is already stripped
  %la = getelementptr inbounds i8, ptr %a.s, i64 8
  %rb = getelementptr inbounds i8, ptr %b.s, i64 16
  %merged = select i1 %choose, ptr %la, ptr %rb ; stripped merged value
  store ptr %merged, ptr %slot ; tagged exit from a stripped select
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %x = getelementptr inbounds i8, ptr %merged, i64 %iv
  %x.v = load volatile i8, ptr %x, align 1
  %next = add nuw i64 %iv, 1
  %y = getelementptr inbounds i8, ptr %merged, i64 %next
  %y.v = load volatile i8, ptr %y, align 1
  %next2 = add nuw i64 %next, 1
  %z = getelementptr inbounds i8, ptr %merged, i64 %next2
  %z.v = load volatile i8, ptr %z, align 1
  %next3 = add nuw i64 %next2, 1
  %w = getelementptr inbounds i8, ptr %merged, i64 %next3
  %w.v = load volatile i8, ptr %w, align 1
  %done = icmp eq i64 %next3, 8
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
