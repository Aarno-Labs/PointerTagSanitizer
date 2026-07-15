; RUN: rm -rf %t && mkdir -p %t && cd %t
; RUN: %opt_ptsan %s -S > out.ll
; RUN: cat out.ll | FileCheck %s

; Check should be fully hoisted
; CHECK-LABEL: define{{.*}}@case_call_after_use
; CHECK-LABEL: entry
; CHECK-DAG: [[ASINT:%[0-9A-Za-z_.]+]] = ptrtoint ptr %buf to i64
; CHECK-DAG: [[ID:%[0-9A-Za-z_.]+]] = lshr i64 [[ASINT]], 48
; CHECK-DAG: [[BASEGEP:%[0-9A-Za-z_.]+]] = getelementptr inbounds [65536 x %__ptsan_ObjectBounds], ptr @ptsan_bounds, i64 0, i64 [[ID]], i32 0
; CHECK-DAG: [[BOUNDGEP:%[0-9A-Za-z_.]+]] = getelementptr inbounds [65536 x %__ptsan_ObjectBounds], ptr @ptsan_bounds, i64 0, i64 [[ID]], i32 1
; CHECK-DAG: [[BASEVAL:%[0-9A-Za-z_.]+]] = load i64, ptr [[BASEGEP]], align 8
; CHECK-DAG: [[BOUNDVAL:%[0-9A-Za-z_.]+]] = load i64, ptr [[BOUNDGEP]], align 8
; CHECK: br i1

; ModuleID = '../hoisting.c'
source_filename = "../hoisting.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@.str = private unnamed_addr constant [10 x i8] c"case2 %d\0A\00", align 1

; Function Attrs: nounwind uwtable
define dso_local noundef i32 @main() local_unnamed_addr #0 !callees !5 !function_def_sig !6 !ir_simhash !7 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %call = tail call noalias dereferenceable_or_null(128) ptr @calloc(i64 noundef 32, i64 noundef 4) #6
  %call1 = tail call fastcc i32 @case_call_after_use(ptr noundef %call)
  %call2 = tail call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @.str, i32 noundef %call1)
  ret i32 0
}

; Function Attrs: mustprogress nofree nounwind willreturn allockind("alloc,zeroed") allocsize(0,1) memory(inaccessiblemem: readwrite)
declare noalias noundef ptr @calloc(i64 noundef, i64 noundef) local_unnamed_addr #1

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr nocapture noundef readonly, ...) local_unnamed_addr #2

; Function Attrs: noinline nounwind memory(argmem: readwrite, inaccessiblemem: readwrite) uwtable
define internal fastcc i32 @case_call_after_use(ptr nocapture noundef %buf) unnamed_addr #3 !callees !10 !function_def_sig !11 !ir_simhash !12 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  br label %for.body

for.cond.cleanup:                                 ; preds = %for.body
  %arrayidx1 = getelementptr inbounds i8, ptr %buf, i64 76
  %0 = load i32, ptr %arrayidx1, align 4, !tbaa !13
  %arrayidx2 = getelementptr inbounds i8, ptr %buf, i64 72
  %1 = load i32, ptr %arrayidx2, align 4, !tbaa !13
  %add = add nsw i32 %1, %0
  tail call fastcc void @maybe_free(ptr noundef %buf)
  ret i32 %add

for.body:                                         ; preds = %entry, %for.body
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ]
  %arrayidx = getelementptr inbounds i32, ptr %buf, i64 %indvars.iv
  %2 = trunc nuw nsw i64 %indvars.iv to i32
  store i32 %2, ptr %arrayidx, align 4, !tbaa !13
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond.not = icmp eq i64 %indvars.iv.next, 16
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !llvm.loop !17
}

; Function Attrs: mustprogress noinline nounwind willreturn memory(argmem: readwrite, inaccessiblemem: readwrite) uwtable
define internal fastcc void @maybe_free(ptr nocapture noundef %p) unnamed_addr #4 !callees !20 !function_def_sig !21 !ir_simhash !22 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %0 = load i32, ptr %p, align 4, !tbaa !13
  %cmp = icmp slt i32 %0, -99
  br i1 %cmp, label %if.then, label %if.end

if.then:                                          ; preds = %entry
  tail call void @free(ptr noundef %p) #7
  br label %if.end

if.end:                                           ; preds = %if.then, %entry
  ret void
}

; Function Attrs: mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite)
declare void @free(ptr allocptr nocapture noundef) local_unnamed_addr #5

attributes #0 = { nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { mustprogress nofree nounwind willreturn allockind("alloc,zeroed") allocsize(0,1) memory(inaccessiblemem: readwrite) "alloc-family"="malloc" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { nofree nounwind "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #3 = { noinline nounwind memory(argmem: readwrite, inaccessiblemem: readwrite) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #4 = { mustprogress noinline nounwind willreturn memory(argmem: readwrite, inaccessiblemem: readwrite) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #5 = { mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite) "alloc-family"="malloc" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #6 = { nounwind allocsize(0,1) }
attributes #7 = { nounwind }

!llvm.module.flags = !{!0, !1, !2, !3}
!llvm.ident = !{!4}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{!"clang version 19.1.0 (git@github.com:Aarno-Labs/drift-llvm.git b8cac853f193c6dc69d9b5bb139822ed0da01639)"}
!5 = !{!"case_call_after_use|printf|calloc"}
!6 = !{!"int ()"}
!7 = !{i64 3690788939453049001}
!8 = !{!""}
!9 = !{i1 false}
!10 = !{!"maybe_free"}
!11 = !{!"int (int *)"}
!12 = !{i64 -5006323634403337878}
!13 = !{!14, !14, i64 0}
!14 = !{!"int", !15, i64 0}
!15 = !{!"omnipotent char", !16, i64 0}
!16 = !{!"Simple C/C++ TBAA"}
!17 = distinct !{!17, !18, !19}
!18 = !{!"llvm.loop.mustprogress"}
!19 = !{!"llvm.loop.unroll.disable"}
!20 = !{!"free"}
!21 = !{!"void (int *, int)"}
!22 = !{i64 5838947480210149081}
