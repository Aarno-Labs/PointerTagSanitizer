; RUN: %opt_ptsan -ptsan-stop-after=late-optimization %s -o /dev/null
;
; This is reduced from Ptrdist/bc's bc_divide. The first loop produces a
; pointer used both by memset and by a second pointer recurrence. Min-cut makes
; the shared pointer chain stripped and retags its tagged exits.
;
; PTSan preparation creates an LCSSA PHI for %first.ptr in %first.exit. The
; solved polarities then make the %second.start GEP a tagged exit from that
; stripped LCSSA value. Rewriting that boundary produces this shape:
;
; first.exit:
;   %first.ptr.lcssa = phi ptr [ %first.ptr, %first.loop ]
;   %first.ptr.tag = call ptr @__ptsan_tag(ptr %first.ptr.lcssa, i64 %id)
;   ...
; second.preheader:
;   %second.start = getelementptr i8, ptr %first.ptr.tag, i64 %extended.index
;
; Rewriting later needs an ID for the second loop's own LCSSA pointer. Its
; provenance walk follows %second.ptr.lcssa -> %second.ptr -> %second.start and
; reaches %first.ptr.tag. That TagAnchor was inserted after min-cut solving, so
; it has no node in the immutable solution; querying its polarity crashes.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i32 @retag_rewrite_order(i64 %memset_len) {
entry:
  br label %first.loop

first.loop:
  %index = phi i32 [ %next.index, %first.loop ], [ 0, %entry ]
  %first.ptr = getelementptr i8, ptr null, i64 1
  %next.index = add i32 %index, 1
  load i8, ptr %first.ptr, align 1
  br i1 false, label %first.loop, label %first.exit

first.exit:
  ; Preparation inserts %first.ptr.lcssa here because %first.ptr is used by
  ; both successor regions. The problematic TagAnchor is inserted immediately
  ; after that generated PHI and is used as %second.start's pointer operand.
  ; %first.ptr.tag = call ptr @__ptsan_tag(ptr %first.ptr.lcssa,
  ;                                       i64 %first.ptr.lcssa.ptsan.id)
  %extended.index = sext i32 %index to i64
  br i1 false, label %memset.exit, label %second.preheader

memset.exit:
  tail call void @llvm.memset.p0.i64(ptr %first.ptr, i8 0,
                                    i64 %memset_len, i1 false)
  ret i32 0

second.preheader:
  %second.start = getelementptr i8, ptr %first.ptr, i64 %extended.index
  br label %second.loop

second.loop:
  %second.ptr = phi ptr [ %second.start, %second.preheader ],
                          [ %second.next, %second.loop ]
  store i8 0, ptr %second.ptr, align 1
  %second.next = getelementptr i8, ptr %second.ptr, i64 -2
  br i1 true, label %second.exit, label %second.loop

second.exit:
  store i8 0, ptr %second.ptr, align 1
  ret i32 0
}

declare void @llvm.memset.p0.i64(ptr nocapture writeonly, i8, i64, i1 immarg)
