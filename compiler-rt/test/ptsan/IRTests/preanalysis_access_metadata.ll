; RUN: %clang_ptsan_plain -x ir -O2 -fno-discard-value-names \
; RUN:   -mllvm -print-before=loop-vectorize \
; RUN:   -mllvm -print-after=loop-vectorize \
; RUN:   -mllvm -filter-print-funcs=copy \
; RUN:   -mllvm -ptsan-stop-after=optimization \
; RUN:   -S -emit-llvm -o /dev/null %s 2>&1 | FileCheck %s
;
; Exercise the gather phase through its production Clang pipeline. Access
; memberships originate only on memory operations. Their address and
; control-flow calculations must remain free of PTSan access metadata, while
; the memory operations keep membership when vectorized.
target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

define void @copy(ptr noalias %dst, ptr noalias %src) {
; CHECK-LABEL: IR Dump Before LoopVectorizePass on copy
; CHECK: define void @copy(
; CHECK: call void @llvm.sideeffect() {{.*}}!ptsan.loop.bounds
; CHECK: %i = phi i64 [ 0, %entry ], [ %next, %loop ]{{$}}
; CHECK: %p = getelementptr inbounds i32, ptr %src, i64 %i{{$}}
; CHECK: %x = load i32, ptr %p, align 4, !ptsan.loop.ids ![[ID:[0-9]+]]
; CHECK: %y = add i32 %x, 1{{$}}
; CHECK: %q = getelementptr inbounds i32, ptr %dst, i64 %i{{$}}
; CHECK: store i32 %y, ptr %q, align 4, !ptsan.loop.ids ![[ID]]
; CHECK: %next = add nuw nsw i64 %i, 1{{$}}
; CHECK: %done = icmp eq i64 %next, 64{{$}}
; CHECK: br i1 %done, label %exit, label %loop{{$}}
; CHECK-LABEL: IR Dump After LoopVectorizePass on copy
; CHECK: define void @copy(
; CHECK: load <4 x i32>, {{.*}}!ptsan.loop.ids ![[VID:[0-9]+]]
; CHECK: store <4 x i32> {{.*}}!ptsan.loop.ids ![[VID]]
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %loop]
  %p = getelementptr inbounds i32, ptr %src, i64 %i
  %x = load i32, ptr %p, align 4
  %y = add i32 %x, 1
  %q = getelementptr inbounds i32, ptr %dst, i64 %i
  store i32 %y, ptr %q, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, 64
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
