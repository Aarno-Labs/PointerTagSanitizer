; RUN: rm -rf %t && mkdir -p %t && cd %t
; RUN: %opt_ptsan %s -S > out.ll
; RUN: cat out.ll | FileCheck %s

; Check should be hoisted to invoke.cont, but no higher
; CHECK-LABEL: define{{.*}}@_Z11test_invokePi
; CHECK-LABEL: invoke.cont:
; CHECK-DAG: [[BASEGEP:%[0-9A-Za-z_.]+]] = getelementptr inbounds [65536 x %__ptsan_ObjectBounds], ptr @ptsan_bounds, i64 0, i64 %buf.id{{[0-9]*}}, i32 0
; CHECK-DAG: [[BOUNDGEP:%[0-9A-Za-z_.]+]] = getelementptr inbounds [65536 x %__ptsan_ObjectBounds], ptr @ptsan_bounds, i64 0, i64 %buf.id{{[0-9]*}}, i32 1
; CHECK-DAG: [[BASEVAL:%[0-9A-Za-z_.]+]] = load i64, ptr [[BASEGEP]], align 8
; CHECK-DAG: [[BOUNDVAL:%[0-9A-Za-z_.]+]] = load i64, ptr [[BOUNDGEP]], align 8
; CHECK: br i1


; ModuleID = 'invoke.cpp'
source_filename = "invoke.cpp"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@_ZTIi = external constant ptr
@.str = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

; Function Attrs: mustprogress noinline uwtable
define dso_local noundef i32 @_Z9may_throwPii(ptr nocapture noundef %buf, i32 noundef %idx) local_unnamed_addr #0 !callees !5 !function_def_sig !6 !ir_simhash !7 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %cmp = icmp slt i32 %idx, 0
  br i1 %cmp, label %if.then, label %if.end

if.then:                                          ; preds = %entry
  tail call void @free(ptr noundef %buf) #6
  %exception = tail call ptr @__cxa_allocate_exception(i64 4) #6
  store i32 %idx, ptr %exception, align 16, !tbaa !10
  tail call void @__cxa_throw(ptr nonnull %exception, ptr nonnull @_ZTIi, ptr null) #7
  unreachable

if.end:                                           ; preds = %entry
  %idxprom = zext nneg i32 %idx to i64
  %arrayidx = getelementptr inbounds i32, ptr %buf, i64 %idxprom
  %0 = load i32, ptr %arrayidx, align 4, !tbaa !10
  ret i32 %0
}

; Function Attrs: mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite)
declare void @free(ptr allocptr nocapture noundef) local_unnamed_addr #1

declare ptr @__cxa_allocate_exception(i64) local_unnamed_addr

declare void @__cxa_throw(ptr, ptr, ptr) local_unnamed_addr

; Function Attrs: mustprogress noinline uwtable
define dso_local noundef i32 @_Z11test_invokePi(ptr nocapture noundef %buf) local_unnamed_addr #0 personality ptr @__gxx_personality_v0 !callees !14 !function_def_sig !15 !ir_simhash !16 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %call = invoke noundef i32 @_Z9may_throwPii(ptr noundef %buf, i32 noundef 3)
          to label %invoke.cont unwind label %lpad

invoke.cont:                                      ; preds = %entry
  %arrayidx = getelementptr inbounds i8, ptr %buf, i64 20
  %0 = load i32, ptr %arrayidx, align 4, !tbaa !10
  %add = add nsw i32 %0, %call
  tail call void @free(ptr noundef %buf) #6
  br label %cleanup

lpad:                                             ; preds = %entry
  %1 = landingpad { ptr, i32 }
          cleanup
          catch ptr @_ZTIi
  %2 = extractvalue { ptr, i32 } %1, 1
  %3 = tail call i32 @llvm.eh.typeid.for.p0(ptr nonnull @_ZTIi) #6
  %matches = icmp eq i32 %2, %3
  br i1 %matches, label %catch, label %ehcleanup

catch:                                            ; preds = %lpad
  %4 = extractvalue { ptr, i32 } %1, 0
  %5 = tail call ptr @__cxa_begin_catch(ptr %4) #6
  tail call void @__cxa_end_catch() #6
  br label %cleanup

cleanup:                                          ; preds = %invoke.cont, %catch
  %retval.0 = phi i32 [ %add, %invoke.cont ], [ -1, %catch ]
  ret i32 %retval.0

ehcleanup:                                        ; preds = %lpad
  resume { ptr, i32 } %1
}

declare i32 @__gxx_personality_v0(...)

; Function Attrs: nofree nosync nounwind memory(none)
declare i32 @llvm.eh.typeid.for.p0(ptr) #2

declare ptr @__cxa_begin_catch(ptr) local_unnamed_addr

declare void @__cxa_end_catch() local_unnamed_addr

; Function Attrs: mustprogress norecurse uwtable
define dso_local noundef i32 @main() local_unnamed_addr #3 !callees !17 !function_def_sig !18 !ir_simhash !19 !indirect_call_sigs !8 !addr_taken !9 !vcallees !8 {
entry:
  %call = tail call noalias dereferenceable_or_null(32) ptr @malloc(i64 noundef 32) #8
  store <4 x i32> <i32 0, i32 1, i32 4, i32 9>, ptr %call, align 4, !tbaa !10
  %arrayidx.4 = getelementptr inbounds i8, ptr %call, i64 16
  store <4 x i32> <i32 16, i32 25, i32 36, i32 49>, ptr %arrayidx.4, align 4, !tbaa !10
  %call1 = tail call noundef i32 @_Z11test_invokePi(ptr noundef nonnull %call)
  %call2 = tail call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @.str, i32 noundef %call1)
  ret i32 0
}

; Function Attrs: mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0) memory(inaccessiblemem: readwrite)
declare noalias noundef ptr @malloc(i64 noundef) local_unnamed_addr #4

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr nocapture noundef readonly, ...) local_unnamed_addr #5

attributes #0 = { mustprogress noinline uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite) "alloc-family"="malloc" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { nofree nosync nounwind memory(none) }
attributes #3 = { mustprogress norecurse uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #4 = { mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0) memory(inaccessiblemem: readwrite) "alloc-family"="malloc" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #5 = { nofree nounwind "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #6 = { nounwind }
attributes #7 = { noreturn }
attributes #8 = { nounwind allocsize(0) }

!llvm.module.flags = !{!0, !1, !2, !3}
!llvm.ident = !{!4}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{!"clang version 19.1.0 (git@github.com:Aarno-Labs/drift-llvm.git b8cac853f193c6dc69d9b5bb139822ed0da01639)"}
!5 = !{!"free|__cxa_throw|__cxa_allocate_exception"}
!6 = !{!"int (int *, int)"}
!7 = !{i64 5188170719477382105}
!8 = !{!""}
!9 = !{i1 false}
!10 = !{!11, !11, i64 0}
!11 = !{!"int", !12, i64 0}
!12 = !{!"omnipotent char", !13, i64 0}
!13 = !{!"Simple C++ TBAA"}
!14 = !{!"__cxa_end_catch|__cxa_begin_catch|free|_Z9may_throwPii"}
!15 = !{!"int (int *)"}
!16 = !{i64 -6863140976928219285}
!17 = !{!"_Z11test_invokePi|printf|malloc"}
!18 = !{!"int ()"}
!19 = !{i64 -5908346762796844050}
