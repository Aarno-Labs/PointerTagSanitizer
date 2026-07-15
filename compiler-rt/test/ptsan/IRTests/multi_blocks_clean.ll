; RUN: rm -rf %t && mkdir -p %t && cd %t
; RUN: %opt_ptsan %s -S > out.ll
; RUN: cat out.ll | FileCheck %s

; Check should be fully hoisted
; CHECK-LABEL: define{{.*}}@case_multi_blocks_clean
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

@.str = private unnamed_addr constant [10 x i8] c"case4 %d\0A\00", align 1

; Function Attrs: nofree nounwind uwtable
define dso_local noundef i32 @main() local_unnamed_addr #0 !callees !5 !function_def_sig !6 !ir_simhash !7 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %call = tail call noalias dereferenceable_or_null(128) ptr @calloc(i64 noundef 32, i64 noundef 4) #4
  %call1 = tail call fastcc i32 @case_multi_blocks_clean(ptr noundef %call)
  %call2 = tail call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @.str, i32 noundef %call1)
  ret i32 0
}

; Function Attrs: mustprogress nofree nounwind willreturn allockind("alloc,zeroed") allocsize(0,1) memory(inaccessiblemem: readwrite)
declare noalias noundef ptr @calloc(i64 noundef, i64 noundef) local_unnamed_addr #1

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr nocapture noundef readonly, ...) local_unnamed_addr #2

; Function Attrs: mustprogress nofree noinline norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define internal fastcc i32 @case_multi_blocks_clean(ptr nocapture noundef readonly %buf) unnamed_addr #3 !callees !8 !function_def_sig !10 !ir_simhash !11 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %0 = load i32, ptr %buf, align 4, !tbaa !12
  %cmp = icmp sgt i32 %0, -1
  br i1 %cmp, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  %arrayidx1 = getelementptr inbounds i8, ptr %buf, i64 40
  %1 = load i32, ptr %arrayidx1, align 4, !tbaa !12
  %arrayidx2 = getelementptr inbounds i8, ptr %buf, i64 80
  %2 = load i32, ptr %arrayidx2, align 4, !tbaa !12
  %add = add nsw i32 %2, %1
  br label %if.end

if.else:                                          ; preds = %entry
  %arrayidx3 = getelementptr inbounds i8, ptr %buf, i64 16
  %3 = load i32, ptr %arrayidx3, align 4, !tbaa !12
  br label %if.end

if.end:                                           ; preds = %if.else, %if.then
  %s.0 = phi i32 [ %add, %if.then ], [ %3, %if.else ]
  ret i32 %s.0
}

attributes #0 = { nofree nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { mustprogress nofree nounwind willreturn allockind("alloc,zeroed") allocsize(0,1) memory(inaccessiblemem: readwrite) "alloc-family"="malloc" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { nofree nounwind "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #3 = { mustprogress nofree noinline norecurse nosync nounwind willreturn memory(argmem: read) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #4 = { nounwind allocsize(0,1) }

!llvm.module.flags = !{!0, !1, !2, !3}
!llvm.ident = !{!4}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{!"clang version 19.1.0 (git@github.com:Aarno-Labs/drift-llvm.git b8cac853f193c6dc69d9b5bb139822ed0da01639)"}
!5 = !{!"printf|case_multi_blocks_clean|calloc"}
!6 = !{!"int ()"}
!7 = !{i64 2522105249008327309}
!8 = !{!""}
!9 = !{i1 false}
!10 = !{!"int (int *)"}
!11 = !{i64 -2636863872389408148}
!12 = !{!13, !13, i64 0}
!13 = !{!"int", !14, i64 0}
!14 = !{!"omnipotent char", !15, i64 0}
!15 = !{!"Simple C/C++ TBAA"}
