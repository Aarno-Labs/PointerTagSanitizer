; Tests late strip-ID hoisting after check metadata has been materialized.
;
; RUN: %opt_ptsan -ptsan-stop-after=late-optimization -ptsan-nochecks %s | FileCheck %s --check-prefix=LATE
; RUN: %opt_ptsan -ptsan-stop-after=late-optimization %s | FileCheck %s --check-prefix=CHECKS

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @escape(ptr)
declare ptr @opaque()
declare ptr @__ptsan_strip(ptr)
declare i64 @__ptsan_strip_i64(i64)
declare i64 @__ptsan_get_id(ptr)

@global_buf = global [1024 x i8] zeroinitializer

; The loop load creates a leaf strip on %q. With checks disabled, there are no
; tagged exits, so late optimization hoists that strip to the loop-invariant
; base %p and rewrites the GEP to use the stripped base.
define i8 @simple_loop(ptr %p, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [0, %entry], [%inc, %loop]
  ; Important: %q is the derived pointer whose strip should disappear.
  %q = getelementptr i8, ptr %p, i64 %i
  ; Important: the dereference requires a stripped pointer.
  %v = load i8, ptr %q, align 1
  %inc = add nuw i64 %i, 1
  %done = icmp eq i64 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i8 %v
}

; LATE-LABEL: define i8 @simple_loop(
; LATE: [[BASE_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %p)
; LATE: %q = getelementptr i8, ptr [[BASE_STRIP]], i64 %i
; LATE-NOT: call ptr @__ptsan_strip(ptr %q)
; LATE-LABEL: define i8 @mixed_escape(

; The call needs a tagged reference to %p, while the loop load needs stripped
; derived pointers. Late optimization hoists the base strip and leaves the
; direct tagged call use on the original %p.
define i8 @mixed_escape(ptr %p, i64 %n) {
entry:
  ; Important: this use must remain tagged.
  call void @escape(ptr %p)
  br label %loop

loop:
  %i = phi i64 [0, %entry], [%inc, %loop]
  ; Important: this subtree can use the hoisted stripped base.
  %q = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %q, align 1
  %inc = add nuw i64 %i, 1
  %done = icmp eq i64 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i8 %v
}

; LATE: [[MIXED_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %p)
; LATE-NOT: call ptr @__ptsan_tag(ptr [[MIXED_STRIP]]
; LATE: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %p)
; LATE: call void @escape(ptr [[MIXED_TAGGED]])
; LATE: %q = getelementptr i8, ptr [[MIXED_STRIP]], i64 %i
; LATE-LABEL: define i64 @get_id_sibling(

; get_id needs a tagged reference, but get_id(tag(value, id)) folds to id.
; The loop strip can still hoist without leaving a redundant __ptsan_tag.
define i64 @get_id_sibling(ptr %p, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [0, %entry], [%inc, %loop]
  ; Important: explicit strip leaf for the late optimizer to remove.
  %q = getelementptr i8, ptr %p, i64 %i
  %s = call ptr @__ptsan_strip(ptr %q)
  ; Important: tagged demand with zero retag cost after folding.
  %id = call i64 @__ptsan_get_id(ptr %p)
  %inc = add nuw i64 %i, 1
  %done = icmp eq i64 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i64 %id
}

; LATE: [[GID_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %p)
; LATE: [[GID_ID:%.*]] = call i64 @__ptsan_get_id(ptr %p)
; LATE-NOT: call ptr @__ptsan_tag(
; LATE: %q = getelementptr i8, ptr [[GID_STRIP]], i64 %i
; LATE-NOT: call ptr @__ptsan_strip(ptr %q)
; LATE-LABEL: define i8 @mixed_phi_stays_tagged_with_independent_strip(

; One PHI input is derived from %p and another comes from an opaque source. The
; mixed PHI itself stays tagged, while the independent loop subtree uses a
; stripped base.
define i8 @mixed_phi_stays_tagged_with_independent_strip(ptr %p, i1 %c,
                                                         i64 %n) {
entry:
  br i1 %c, label %left, label %right

left:
  ; Important: one PHI arm is derived from %p.
  %left.gep = getelementptr i8, ptr %p, i64 1
  br label %join

right:
  ; Important: the other PHI arm is not derived from %p.
  %other = call ptr @opaque()
  br label %join

join:
  %phi = phi ptr [%left.gep, %left], [%other, %right]
  ; Important: this tagged escape keeps the mixed PHI tagged.
  call void @escape(ptr %phi)
  br label %loop

loop:
  %i = phi i64 [0, %join], [%inc, %loop]
  %q = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %q, align 1
  %inc = add nuw i64 %i, 1
  %done = icmp eq i64 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i8 %v
}

; LATE: [[PHI_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %p)
; LATE: %left.gep = getelementptr i8, ptr %p, i64 1
; LATE-NOT: call ptr @__ptsan_tag(ptr %left.gep
; LATE: %phi = phi ptr [ %left.gep, %left ], [ %other, %right ]
; LATE: [[PHI_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %phi)
; LATE: call void @escape(ptr [[PHI_TAGGED]])
; LATE: %q = getelementptr i8, ptr [[PHI_STRIP]], i64 %i
; LATE-LABEL: define i64 @integer_strip_unchanged(

; V1 only hoists pointer strip anchors. Integer strips are left alone.
define i64 @integer_strip_unchanged(i64 %x) {
entry:
  ; Important: this is the non-pointer strip variant.
  %s = call i64 @__ptsan_strip_i64(i64 %x)
  ret i64 %s
}

; LATE: call i64 @__ptsan_strip_i64(i64 %x)

; A subtree already derived from a strip anchor cannot use its local stripped
; descendants as retag ID sources. If this shape ever becomes profitable, retags
; need to use the original top base; they must not use get_id(%g) or get_id(%h).
define void @stripped_descendant_is_not_retag_base(ptr %p, ptr %other,
                                                   i1 %cond, i64 %n) {
entry:
  ; Important: everything below %p.stripped is already untagged.
  %p.stripped = call ptr @__ptsan_strip(ptr %p)
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %inc, %join ]
  %g = getelementptr i8, ptr %p.stripped, i64 %i
  %h = getelementptr i8, ptr %g, i64 1
  ; Important: this leaf strip alone should not make %g or %h valid retag ID
  ; sources, because both are derived from the already-stripped %p.stripped.
  %leaf = getelementptr i8, ptr %h, i64 2
  %leaf.s = call ptr @__ptsan_strip(ptr %leaf)
  call void @escape(ptr %leaf.s)
  br i1 %cond, label %left, label %right

left:
  br label %join

right:
  br label %join

join:
  ; Important: this uncontained PHI would need retagging if %h were chosen as a
  ; base. That retag must never source its ID from %h itself.
  %mixed = phi ptr [ %h, %left ], [ %other, %right ]
  call void @escape(ptr %mixed)
  %inc = add nuw i64 %i, 1
  %done = icmp eq i64 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; LATE-LABEL: define void @stripped_descendant_is_not_retag_base(
; LATE: %g = getelementptr i8, ptr %p, i64 %i
; LATE: %h = getelementptr i8, ptr %g, i64 1
; LATE-NOT: call i64 @__ptsan_get_id(ptr %g)
; LATE-NOT: call i64 @__ptsan_get_id(ptr %h)
; LATE: %leaf.s = call ptr @__ptsan_strip(ptr %leaf)
; LATE: %mixed = phi ptr [ %h, %left ], [ %other, %right ]
; LATE-NEXT: [[MIXED_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %mixed)
; LATE-NEXT: call void @escape(ptr [[MIXED_TAGGED]])
; LATE-LABEL: define i8 @global_constexpr_phi_base(

; A global base can have constant-expression users. Late strip-ID hoisting
; materializes those users before graph construction, so the constant-expression
; GEP arm is visible even if the cheapest cut is a local strip of the PHI.
define i8 @global_constexpr_phi_base(i1 %cond, i64 %n) {
entry:
  br i1 %cond, label %left, label %right

left:
  br label %join

right:
  br label %join

join:
  ; Important: this PHI has a direct global arm and a constant-expression GEP
  ; arm. The GEP arm should be materialized so the PHI can stay in the stripped
  ; closure with one stripped global base.
  %sink = phi ptr [ @global_buf, %left ],
                  [ getelementptr inbounds (i8, ptr @global_buf, i64 4),
                    %right ]
  br label %loop

loop:
  %i = phi i64 [ 0, %join ], [ %inc, %loop ]
  ; Important: this loop strip should hoist through the materialized PHI.
  %q = getelementptr i8, ptr %sink, i64 %i
  %s = call ptr @__ptsan_strip(ptr %q)
  %v = load volatile i8, ptr %s, align 1
  %inc = add nuw i64 %i, 1
  %done = icmp eq i64 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i8 %v
}

; LATE: [[RIGHT:%.*]] = getelementptr inbounds i8, ptr @global_buf, i64 4
; LATE: %sink = phi ptr [ @global_buf, %left ], [ [[RIGHT]], %right ]
; LATE-NEXT: [[SINK_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %sink)
; LATE: %q = getelementptr i8, ptr [[SINK_STRIP]], i64 %i
; LATE-NEXT: load volatile i8, ptr %q

; A tagged SSA value remains available when we create a stripped representative
; for its other uses. The store can keep using the original %sub while both
; stripped descendants share one strip inserted immediately after %sub.
define void @subbase_tagged_and_stripped_uses(ptr %p, ptr %slot) {
entry:
  ; Important: %sub has both tagged and stripped consumers.
  %sub = getelementptr i8, ptr %p, i64 8
  ; Important: this use can consume the original tagged %sub for free.
  store ptr %sub, ptr %slot
  ; Important: the two stripped branches should share one conversion.
  %a = getelementptr i8, ptr %sub, i64 1
  %a.s = call ptr @__ptsan_strip(ptr %a)
  %a.v = load volatile i8, ptr %a.s, align 1
  %b = getelementptr i8, ptr %sub, i64 2
  %b.s = call ptr @__ptsan_strip(ptr %b)
  %b.v = load volatile i8, ptr %b.s, align 1
  ret void
}

; LATE-LABEL: define void @subbase_tagged_and_stripped_uses(
; LATE: %sub = getelementptr i8, ptr %p, i64 8
; LATE-NEXT: [[SUB_STRIPPED:%.*]] = call ptr @__ptsan_strip(ptr %sub)
; LATE-NEXT: [[SUB_TAGGED:%.*]] = call ptr @__ptsan_tagged(ptr %sub)
; LATE-NEXT: store ptr [[SUB_TAGGED]], ptr {{%.*}}
; LATE: %a = getelementptr i8, ptr [[SUB_STRIPPED]], i64 1
; LATE-NOT: call ptr @__ptsan_strip(ptr %a)
; LATE: %b = getelementptr i8, ptr [[SUB_STRIPPED]], i64 2
; LATE-NOT: call ptr @__ptsan_strip(ptr %b)
; LATE-NOT: call ptr @__ptsan_tag(ptr %sub

; With checks enabled, materialized check range-min computations create tagged
; demands. The global solver can still hoist the loop strip when the stripped
; loop body savings beat the tagged check-min boundary.
; CHECKS-LABEL: define i8 @simple_loop(
; CHECKS: call void (...) @__ptsan_access(
; CHECKS: [[P_STRIP:%.*]] = call ptr @__ptsan_strip(ptr %p)
; CHECKS: %q = getelementptr i8, ptr [[P_STRIP]], i64 %i
; CHECKS-NEXT: load i8, ptr %q
