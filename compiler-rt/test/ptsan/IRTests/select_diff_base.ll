; RUN: rm -rf %t && mkdir -p %t && cd %t
; RUN: %opt_ptsan %s -S > out.ll
; RUN: cat out.ll | FileCheck %s

;CHECK: %cond = select i1 %tobool.not, ptr %call1, ptr %call
;CHECK-DAG: {{%.*}} = getelementptr inbounds [65536 x %__ptsan_ObjectBounds], ptr @ptsan_bounds, i64 0, i64 %cond.id{{[0-9]*}}, i32 0
;CHECK-DAG: {{%.*}} = getelementptr inbounds [65536 x %__ptsan_ObjectBounds], ptr @ptsan_bounds, i64 0, i64 %cond.id{{[0-9]*}}, i32 1


; ModuleID = '../hoisting.c'
source_filename = "../hoisting.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@.str = private unnamed_addr constant [11 x i8] c"case10 %d\0A\00", align 1

; Function Attrs: nounwind uwtable
define dso_local noundef i32 @main() local_unnamed_addr #0 !callees !5 !function_def_sig !6 !ir_simhash !7 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %call1 = tail call fastcc i32 @case_select_diff_base()
  %call2 = tail call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @.str, i32 noundef %call1)
  ret i32 0
}

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr nocapture noundef readonly, ...) local_unnamed_addr #1

; Function Attrs: noinline nounwind uwtable
define internal fastcc i32 @case_select_diff_base() unnamed_addr #2 !callees !10 !function_def_sig !6 !ir_simhash !11 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %call = tail call noalias dereferenceable_or_null(64) ptr @malloc(i64 noundef 64) #6
  %call1 = tail call noalias dereferenceable_or_null(64) ptr @malloc(i64 noundef 64) #6
  store i32 1, ptr %call, align 4, !tbaa !12
  store i32 2, ptr %call1, align 4, !tbaa !12
  %call2 = tail call i32 @rand() #7
  %and = and i32 %call2, 1
  %tobool.not = icmp eq i32 %and, 0
  %cond = select i1 %tobool.not, ptr %call1, ptr %call
  %0 = load i32, ptr %cond, align 4, !tbaa !12
  tail call void @free(ptr noundef %call) #7
  tail call void @free(ptr noundef %call1) #7
  ret i32 %0
}

; Function Attrs: mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0) memory(inaccessiblemem: readwrite)
declare noalias noundef ptr @malloc(i64 noundef) local_unnamed_addr #3

; Function Attrs: nounwind
declare i32 @rand() local_unnamed_addr #4

; Function Attrs: mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite)
declare void @free(ptr allocptr nocapture noundef) local_unnamed_addr #5

attributes #0 = { nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { nofree nounwind "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { noinline nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #3 = { mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0) memory(inaccessiblemem: readwrite) "alloc-family"="malloc" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #4 = { nounwind "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #5 = { mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite) "alloc-family"="malloc" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #6 = { nounwind allocsize(0) }
attributes #7 = { nounwind }

!llvm.module.flags = !{!0, !1, !2, !3}
!llvm.ident = !{!4}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{!"clang version 19.1.0 (git@github.com:Aarno-Labs/drift-llvm.git b8cac853f193c6dc69d9b5bb139822ed0da01639)"}
!5 = !{!"case_select_diff_base|printf|calloc"}
!6 = !{!"int ()"}
!7 = !{i64 2470298702238610174}
!8 = !{!""}
!9 = !{i1 false}
!10 = !{!"free|malloc|rand"}
!11 = !{i64 -6256604338146486998}
!12 = !{!13, !13, i64 0}
!13 = !{!"int", !14, i64 0}
!14 = !{!"omnipotent char", !15, i64 0}
!15 = !{!"Simple C/C++ TBAA"}
