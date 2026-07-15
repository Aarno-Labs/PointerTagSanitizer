; RUN: rm -rf %t && mkdir -p %t && cd %t
; RUN: %opt_ptsan %s -S > out.ll
; RUN: cat out.ll | FileCheck %s

; Check should be fully hoisted
; CHECK-LABEL: define{{.*}}@case_alloca_ptr
; CHECK-LABEL: entry:
; CHECK: %local = alloca [8 x i32]
; CHECK: getelementptr inbounds [65536 x %__ptsan_ObjectBounds], ptr @ptsan_bounds, i64 0, i64
; Exactly one gep for the memset at the end
; CHECK: = getelementptr inbounds [65536 x %__ptsan_ObjectBounds], ptr @ptsan_bounds
; CHECK-NEXT: call void @llvm.memset


; ModuleID = '../hoisting.c'
source_filename = "../hoisting.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@.str = private unnamed_addr constant [10 x i8] c"case8 %d\0A\00", align 1

; Function Attrs: nounwind uwtable
define dso_local noundef i32 @main() local_unnamed_addr #0 !callees !5 !function_def_sig !6 !ir_simhash !7 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %call1 = tail call fastcc i32 @case_alloca_ptr()
  %call2 = tail call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @.str, i32 noundef %call1)
  ret i32 0
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(i64 immarg, ptr nocapture) #1

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr nocapture noundef readonly, ...) local_unnamed_addr #2

; Function Attrs: noinline nounwind memory(inaccessiblemem: readwrite) uwtable
define internal fastcc i32 @case_alloca_ptr() unnamed_addr #3 !callees !10 !function_def_sig !6 !ir_simhash !11 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %local = alloca [8 x i32], align 16
  call void @llvm.lifetime.start.p0(i64 32, ptr nonnull %local) #6
  br label %for.body

for.cond.cleanup:                                 ; preds = %for.body
  call fastcc void @maybe_free(ptr noundef nonnull %local)
  %arrayidx1 = getelementptr inbounds i8, ptr %local, i64 8
  %0 = load i32, ptr %arrayidx1, align 8, !tbaa !12
  %arrayidx2 = getelementptr inbounds i8, ptr %local, i64 20
  %1 = load i32, ptr %arrayidx2, align 4, !tbaa !12
  %add = add nsw i32 %1, %0
  call void @llvm.lifetime.end.p0(i64 32, ptr nonnull %local) #6
  ret i32 %add

for.body:                                         ; preds = %entry, %for.body
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ]
  %2 = mul nuw nsw i64 %indvars.iv, %indvars.iv
  %arrayidx = getelementptr inbounds [8 x i32], ptr %local, i64 0, i64 %indvars.iv
  %3 = trunc nuw i64 %2 to i32
  store i32 %3, ptr %arrayidx, align 4, !tbaa !12
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond.not = icmp eq i64 %indvars.iv.next, 8
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !llvm.loop !16
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(i64 immarg, ptr nocapture) #1

; Function Attrs: mustprogress noinline nounwind willreturn memory(argmem: readwrite, inaccessiblemem: readwrite) uwtable
define internal fastcc void @maybe_free(ptr nocapture noundef %p) unnamed_addr #4 !callees !19 !function_def_sig !20 !ir_simhash !21 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %0 = load i32, ptr %p, align 4, !tbaa !12
  %cmp = icmp slt i32 %0, -97
  br i1 %cmp, label %if.then, label %if.end

if.then:                                          ; preds = %entry
  tail call void @free(ptr noundef %p) #6
  br label %if.end

if.end:                                           ; preds = %if.then, %entry
  ret void
}

; Function Attrs: mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite)
declare void @free(ptr allocptr nocapture noundef) local_unnamed_addr #5

attributes #0 = { nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { nofree nounwind "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #3 = { noinline nounwind memory(inaccessiblemem: readwrite) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #4 = { mustprogress noinline nounwind willreturn memory(argmem: readwrite, inaccessiblemem: readwrite) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #5 = { mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite) "alloc-family"="malloc" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #6 = { nounwind }

!llvm.module.flags = !{!0, !1, !2, !3}
!llvm.ident = !{!4}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{!"clang version 19.1.0 (git@github.com:Aarno-Labs/drift-llvm.git b8cac853f193c6dc69d9b5bb139822ed0da01639)"}
!5 = !{!"printf|case_alloca_ptr|calloc"}
!6 = !{!"int ()"}
!7 = !{i64 1299290950778628216}
!8 = !{!""}
!9 = !{i1 false}
!10 = !{!"maybe_free"}
!11 = !{i64 4473675781468260394}
!12 = !{!13, !13, i64 0}
!13 = !{!"int", !14, i64 0}
!14 = !{!"omnipotent char", !15, i64 0}
!15 = !{!"Simple C/C++ TBAA"}
!16 = distinct !{!16, !17, !18}
!17 = !{!"llvm.loop.mustprogress"}
!18 = !{!"llvm.loop.unroll.disable"}
!19 = !{!"free"}
!20 = !{!"void (int *, int)"}
!21 = !{i64 5838947480210149081}
