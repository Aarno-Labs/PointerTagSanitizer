; XFAIL: *
; This needs a more compliex "If it's in both branches it's okay" analysis.
; RUN: rm -rf %t && mkdir -p %t && cd %t
; RUN: %opt_ptsan %s -S > out.ll
; RUN: cat out.ll | FileCheck %s

; There should be exactly two checks for each branch in the loop,
; in the preheaders

; CHECK-LABEL: for.body13.preheader:
; CHECK: call void @ptsan_complain(i32 0,
; CHECK: call void @ptsan_complain(i32 1,
; CHECK-LABEL: vector.body.preheader:
; CHECK: call void @ptsan_complain(i32 2,
; CHECK: call void @ptsan_complain(i32 3,
; CHECK-LABEL: vector.body:


; ModuleID = 'mc_chroma.c'
source_filename = "mc_chroma.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct.timespec = type { i64, i64 }

@stderr = external local_unnamed_addr global ptr, align 8
@.str = private unnamed_addr constant [14 x i8] c"alloc failed\0A\00", align 1, !dbg !0
@g_sink = internal global i64 0, align 8, !dbg !7
@.str.1 = private unnamed_addr constant [57 x i8] c"iters=%d elapsed=%.3f s throughput=%.2f Mpx/s sink=%llu\0A\00", align 1, !dbg !23


; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(i64 immarg, ptr nocapture) #1

; Function Attrs: mustprogress nofree nounwind willreturn allockind("alloc,uninitialized,aligned") allocsize(1) memory(inaccessiblemem: readwrite)
declare !dbg !130 noalias noundef ptr @aligned_alloc(i64 allocalign noundef, i64 noundef) local_unnamed_addr #2

; Function Attrs: nofree nounwind
declare !dbg !137 noundef i32 @fprintf(ptr nocapture noundef, ptr nocapture noundef readonly, ...) local_unnamed_addr #3

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(i64 immarg, ptr nocapture) #1

; Function Attrs: mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite)
declare !dbg !319 void @free(ptr allocptr nocapture noundef) local_unnamed_addr #8

; Function Attrs: nounwind
declare !dbg !322 i32 @clock_gettime(i32 noundef, ptr noundef) local_unnamed_addr #9

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare double @llvm.fmuladd.f64(double, double, double) #10

; Function Attrs: nofree noinline norecurse nosync nounwind memory(readwrite, inaccessiblemem: none) uwtable
define fastcc void @mc_chroma(ptr nocapture noundef writeonly %dst, ptr nocapture noundef readonly %src, i32 noundef %mvx, i32 noundef %mvy) unnamed_addr #7 !dbg !330 !drift.function_def_sig !355 !drift.ir_simhash !356 {
entry:
    #dbg_value(ptr %dst, !334, !DIExpression(), !357)
    #dbg_value(i32 64, !335, !DIExpression(), !357)
    #dbg_value(ptr %src, !336, !DIExpression(), !357)
    #dbg_value(i32 80, !337, !DIExpression(), !357)
    #dbg_value(i32 %mvx, !338, !DIExpression(), !357)
    #dbg_value(i32 %mvy, !339, !DIExpression(), !357)
    #dbg_value(i32 64, !340, !DIExpression(), !357)
    #dbg_value(i32 64, !341, !DIExpression(), !357)
  %and = and i32 %mvx, 7, !dbg !358
    #dbg_value(i32 %and, !343, !DIExpression(), !357)
  %and1 = and i32 %mvy, 7, !dbg !359
    #dbg_value(i32 %and1, !344, !DIExpression(), !357)
  %sub = sub nuw nsw i32 8, %and, !dbg !360
  %sub2 = sub nuw nsw i32 8, %and1, !dbg !361
    #dbg_value(!DIArgList(i32 %sub2, i32 %sub), !345, !DIExpression(DW_OP_LLVM_arg, 0, DW_OP_LLVM_arg, 1, DW_OP_mul, DW_OP_stack_value), !357)
    #dbg_value(!DIArgList(i32 %sub2, i32 %and), !346, !DIExpression(DW_OP_LLVM_arg, 0, DW_OP_LLVM_arg, 1, DW_OP_mul, DW_OP_stack_value), !357)
    #dbg_value(!DIArgList(i32 %sub, i32 %and1), !347, !DIExpression(DW_OP_LLVM_arg, 0, DW_OP_LLVM_arg, 1, DW_OP_mul, DW_OP_stack_value), !357)
    #dbg_value(!DIArgList(i32 %and1, i32 %and), !348, !DIExpression(DW_OP_LLVM_arg, 0, DW_OP_LLVM_arg, 1, DW_OP_mul, DW_OP_stack_value), !357)
    #dbg_value(ptr %src, !342, !DIExpression(DW_OP_plus_uconst, 80, DW_OP_stack_value), !357)
    #dbg_value(i32 0, !349, !DIExpression(), !362)
  %0 = insertelement <4 x i32> poison, i32 %and, i64 0
  %1 = insertelement <4 x i32> %0, i32 %sub, i64 1
  %2 = shufflevector <4 x i32> %1, <4 x i32> poison, <4 x i32> <i32 0, i32 0, i32 1, i32 1>
  %3 = insertelement <4 x i32> poison, i32 %sub2, i64 0
  %4 = insertelement <4 x i32> %3, i32 %and1, i64 1
  %5 = shufflevector <4 x i32> %4, <4 x i32> poison, <4 x i32> <i32 0, i32 1, i32 0, i32 1>
  %6 = mul nuw nsw <4 x i32> %2, %5
  %scevgep = getelementptr i8, ptr %dst, i64 4096, !dbg !363
  %scevgep1 = getelementptr i8, ptr %src, i64 5185, !dbg !363
  %bound0 = icmp ugt ptr %scevgep1, %dst
  %bound1 = icmp ugt ptr %scevgep, %src
  %found.conflict = and i1 %bound0, %bound1
  %broadcast.splat = shufflevector <4 x i32> %6, <4 x i32> poison, <16 x i32> <i32 2, i32 2, i32 2, i32 2, i32 2, i32 2, i32 2, i32 2, i32 2, i32 2, i32 2, i32 2, i32 2, i32 2, i32 2, i32 2>
  %broadcast.splat4 = shufflevector <4 x i32> %6, <4 x i32> poison, <16 x i32> zeroinitializer
  %broadcast.splat7 = shufflevector <4 x i32> %6, <4 x i32> poison, <16 x i32> <i32 3, i32 3, i32 3, i32 3, i32 3, i32 3, i32 3, i32 3, i32 3, i32 3, i32 3, i32 3, i32 3, i32 3, i32 3, i32 3>
  %broadcast.splat10 = shufflevector <4 x i32> %6, <4 x i32> poison, <16 x i32> <i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1>
  br label %for.cond10.preheader, !dbg !363

for.cond10.preheader:                             ; preds = %entry, %for.cond.cleanup12
  %y.079 = phi i32 [ 0, %entry ], [ %inc44, %for.cond.cleanup12 ]
  %dst.addr.078 = phi ptr [ %dst, %entry ], [ %add.ptr40, %for.cond.cleanup12 ]
  %src.addr.077 = phi ptr [ %src, %entry ], [ %srcp.080, %for.cond.cleanup12 ]
  %srcp.080 = getelementptr inbounds i8, ptr %src.addr.077, i64 80, !dbg !357
    #dbg_value(i32 %y.079, !349, !DIExpression(), !362)
    #dbg_value(ptr %dst.addr.078, !334, !DIExpression(), !357)
    #dbg_value(ptr %src.addr.077, !336, !DIExpression(), !357)
    #dbg_value(i32 0, !351, !DIExpression(), !364)
  br i1 %found.conflict, label %for.body13, label %vector.body, !dbg !365

vector.body:                                      ; preds = %for.cond10.preheader, %vector.body
  %index = phi i64 [ %index.next, %vector.body ], [ 0, %for.cond10.preheader ], !dbg !366
  %7 = getelementptr inbounds i8, ptr %src.addr.077, i64 %index, !dbg !369
  %wide.load = load <16 x i8>, ptr %7, align 1, !dbg !369, !tbaa !236, !alias.scope !370
  %8 = zext <16 x i8> %wide.load to <16 x i32>, !dbg !369
  %9 = mul nuw nsw <16 x i32> %broadcast.splat, %8
  %10 = or disjoint i64 %index, 1, !dbg !366
  %11 = getelementptr inbounds i8, ptr %src.addr.077, i64 %10, !dbg !373
  %wide.load2 = load <16 x i8>, ptr %11, align 1, !dbg !373, !tbaa !236, !alias.scope !370
  %12 = zext <16 x i8> %wide.load2 to <16 x i32>, !dbg !373
  %13 = mul nuw nsw <16 x i32> %broadcast.splat4, %12
  %14 = getelementptr inbounds i8, ptr %srcp.080, i64 %index, !dbg !374
  %wide.load5 = load <16 x i8>, ptr %14, align 1, !dbg !374, !tbaa !236, !alias.scope !370
  %15 = zext <16 x i8> %wide.load5 to <16 x i32>, !dbg !374
  %16 = mul nuw nsw <16 x i32> %broadcast.splat7, %15
  %17 = getelementptr inbounds i8, ptr %srcp.080, i64 %10, !dbg !375
  %wide.load8 = load <16 x i8>, ptr %17, align 1, !dbg !375, !tbaa !236, !alias.scope !370
  %18 = zext <16 x i8> %wide.load8 to <16 x i32>, !dbg !375
  %19 = mul nuw nsw <16 x i32> %broadcast.splat10, %18
  %20 = add nuw nsw <16 x i32> %19, %16
  %21 = add nuw nsw <16 x i32> %13, %9
  %22 = add nuw nsw <16 x i32> %21, <i32 32, i32 32, i32 32, i32 32, i32 32, i32 32, i32 32, i32 32, i32 32, i32 32, i32 32, i32 32, i32 32, i32 32, i32 32, i32 32>, !dbg !376
  %23 = add nuw nsw <16 x i32> %22, %20, !dbg !377
  %24 = lshr <16 x i32> %23, <i32 6, i32 6, i32 6, i32 6, i32 6, i32 6, i32 6, i32 6, i32 6, i32 6, i32 6, i32 6, i32 6, i32 6, i32 6, i32 6>, !dbg !378
  %25 = trunc <16 x i32> %24 to <16 x i8>, !dbg !379
  %26 = getelementptr inbounds i8, ptr %dst.addr.078, i64 %index, !dbg !380
  store <16 x i8> %25, ptr %26, align 1, !dbg !381, !tbaa !236, !alias.scope !382, !noalias !370
  %index.next = add nuw i64 %index, 16, !dbg !366
  %27 = icmp eq i64 %index.next, 64, !dbg !366
  br i1 %27, label %for.cond.cleanup12, label %vector.body, !dbg !366, !llvm.loop !384

for.cond.cleanup:                                 ; preds = %for.cond.cleanup12
  ret void, !dbg !388

for.cond.cleanup12:                               ; preds = %vector.body, %for.body13
  %add.ptr40 = getelementptr inbounds i8, ptr %dst.addr.078, i64 64, !dbg !389
    #dbg_value(ptr %add.ptr40, !334, !DIExpression(), !357)
    #dbg_value(ptr %srcp.080, !336, !DIExpression(), !357)
    #dbg_value(ptr %srcp.080, !342, !DIExpression(DW_OP_plus_uconst, 80, DW_OP_stack_value), !357)
  %inc44 = add nuw nsw i32 %y.079, 1, !dbg !390
    #dbg_value(i32 %inc44, !349, !DIExpression(), !362)
  %exitcond82.not = icmp eq i32 %inc44, 64, !dbg !391
  br i1 %exitcond82.not, label %for.cond.cleanup, label %for.cond10.preheader, !dbg !363, !llvm.loop !392

for.body13:                                       ; preds = %for.cond10.preheader, %for.body13
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.body13 ], [ 0, %for.cond10.preheader ]
    #dbg_value(i64 %indvars.iv, !351, !DIExpression(), !364)
  %arrayidx15 = getelementptr inbounds i8, ptr %src.addr.077, i64 %indvars.iv, !dbg !369
  %28 = load i8, ptr %arrayidx15, align 1, !dbg !369, !tbaa !236
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1, !dbg !366
  %arrayidx19 = getelementptr inbounds i8, ptr %src.addr.077, i64 %indvars.iv.next, !dbg !373
  %29 = load i8, ptr %arrayidx19, align 1, !dbg !373, !tbaa !236
  %arrayidx24 = getelementptr inbounds i8, ptr %srcp.080, i64 %indvars.iv, !dbg !374
  %30 = load i8, ptr %arrayidx24, align 1, !dbg !374, !tbaa !236
  %arrayidx30 = getelementptr inbounds i8, ptr %srcp.080, i64 %indvars.iv.next, !dbg !375
  %31 = load i8, ptr %arrayidx30, align 1, !dbg !375, !tbaa !236
  %32 = insertelement <4 x i8> poison, i8 %29, i64 0, !dbg !373
  %33 = insertelement <4 x i8> %32, i8 %31, i64 1, !dbg !373
  %34 = insertelement <4 x i8> %33, i8 %28, i64 2, !dbg !373
  %35 = insertelement <4 x i8> %34, i8 %30, i64 3, !dbg !373
  %36 = zext <4 x i8> %35 to <4 x i32>, !dbg !373
  %37 = mul nuw nsw <4 x i32> %6, %36
  %38 = tail call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %37), !dbg !377
  %op.rdx = add i32 %38, 32, !dbg !376
  %shr35 = lshr i32 %op.rdx, 6, !dbg !378
  %conv36 = trunc i32 %shr35 to i8, !dbg !379
  %arrayidx38 = getelementptr inbounds i8, ptr %dst.addr.078, i64 %indvars.iv, !dbg !380
  store i8 %conv36, ptr %arrayidx38, align 1, !dbg !381, !tbaa !236
    #dbg_value(i64 %indvars.iv.next, !351, !DIExpression(), !364)
  %exitcond.not = icmp eq i64 %indvars.iv.next, 64, !dbg !394
  br i1 %exitcond.not, label %for.cond.cleanup12, label %for.body13, !dbg !365, !llvm.loop !395
}

; Function Attrs: nofree nounwind
declare noundef i64 @fwrite(ptr nocapture noundef, i64 noundef, i64 noundef, ptr nocapture noundef) local_unnamed_addr #11

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>) #12

attributes #0 = { nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { mustprogress nofree nounwind willreturn allockind("alloc,uninitialized,aligned") allocsize(1) memory(inaccessiblemem: readwrite) "alloc-family"="malloc" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #3 = { nofree nounwind "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #4 = { nofree noinline norecurse nosync nounwind memory(argmem: write) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #5 = { mustprogress nocallback nofree nounwind willreturn memory(argmem: write) }
attributes #6 = { noinline nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #7 = { nofree noinline norecurse nosync nounwind memory(readwrite, inaccessiblemem: none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #8 = { mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite) "alloc-family"="malloc" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #9 = { nounwind "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #10 = { mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #11 = { nofree nounwind }
attributes #12 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #13 = { nounwind allocsize(1) }
attributes #14 = { cold }
attributes #15 = { cold nounwind }
attributes #16 = { nounwind }

!llvm.dbg.cu = !{!9}
!llvm.module.flags = !{!29, !30, !31, !32, !33, !34, !35}
!llvm.ident = !{!36}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(scope: null, file: !2, line: 93, type: !3, isLocal: true, isDefinition: true)
!2 = !DIFile(filename: "mc_chroma.c", directory: "/home/eli/llvm/drift-llvm/compiler-rt/test/ptsan/ManualTests", checksumkind: CSK_MD5, checksum: "05af2ae91ded00c3db70ffe384ec500f")
!3 = !DICompositeType(tag: DW_TAG_array_type, baseType: !4, size: 112, elements: !5)
!4 = !DIBasicType(name: "char", size: 8, encoding: DW_ATE_signed_char)
!5 = !{!6}
!6 = !DISubrange(count: 14)
!7 = !DIGlobalVariableExpression(var: !8, expr: !DIExpression())
!8 = distinct !DIGlobalVariable(name: "g_sink", scope: !9, file: !2, line: 10, type: !28, isLocal: true, isDefinition: true)
!9 = distinct !DICompileUnit(language: DW_LANG_C11, file: !2, producer: "clang version 19.1.0 (git@github.com:Aarno-Labs/drift-llvm.git 604797ef731994ca12a7bbb4abc0708c1012bbe4)", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, retainedTypes: !10, globals: !22, splitDebugInlining: false, nameTableKind: None)
!10 = !{!11, !17, !18, !12, !19}
!11 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !12, size: 64)
!12 = !DIDerivedType(tag: DW_TAG_typedef, name: "uint8_t", file: !13, line: 24, baseType: !14)
!13 = !DIFile(filename: "/usr/include/x86_64-linux-gnu/bits/stdint-uintn.h", directory: "", checksumkind: CSK_MD5, checksum: "256fcabbefa27ca8cf5e6d37525e6e16")
!14 = !DIDerivedType(tag: DW_TAG_typedef, name: "__uint8_t", file: !15, line: 38, baseType: !16)
!15 = !DIFile(filename: "/usr/include/x86_64-linux-gnu/bits/types.h", directory: "", checksumkind: CSK_MD5, checksum: "e1865d9fe29fe1b5ced550b7ba458f9e")
!16 = !DIBasicType(name: "unsigned char", size: 8, encoding: DW_ATE_unsigned_char)
!17 = !DIBasicType(name: "double", size: 64, encoding: DW_ATE_float)
!18 = !DIBasicType(name: "unsigned long long", size: 64, encoding: DW_ATE_unsigned)
!19 = !DIDerivedType(tag: DW_TAG_typedef, name: "uint64_t", file: !13, line: 27, baseType: !20)
!20 = !DIDerivedType(tag: DW_TAG_typedef, name: "__uint64_t", file: !15, line: 45, baseType: !21)
!21 = !DIBasicType(name: "unsigned long", size: 64, encoding: DW_ATE_unsigned)
!22 = !{!0, !23, !7}
!23 = !DIGlobalVariableExpression(var: !24, expr: !DIExpression())
!24 = distinct !DIGlobalVariable(scope: null, file: !2, line: 129, type: !25, isLocal: true, isDefinition: true)
!25 = !DICompositeType(tag: DW_TAG_array_type, baseType: !4, size: 456, elements: !26)
!26 = !{!27}
!27 = !DISubrange(count: 57)
!28 = !DIDerivedType(tag: DW_TAG_volatile_type, baseType: !19)
!29 = !{i32 7, !"Dwarf Version", i32 5}
!30 = !{i32 2, !"Debug Info Version", i32 3}
!31 = !{i32 1, !"wchar_size", i32 4}
!32 = !{i32 8, !"PIC Level", i32 2}
!33 = !{i32 7, !"PIE Level", i32 2}
!34 = !{i32 7, !"uwtable", i32 2}
!35 = !{i32 7, !"debug-info-assignment-tracking", i1 true}
!36 = !{!"clang version 19.1.0 (git@github.com:Aarno-Labs/drift-llvm.git 604797ef731994ca12a7bbb4abc0708c1012bbe4)"}
!37 = distinct !DISubprogram(name: "main", scope: !2, file: !2, line: 80, type: !38, scopeLine: 81, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !9, retainedNodes: !41)
!38 = !DISubroutineType(types: !39)
!39 = !{!40}
!40 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!41 = !{!42, !44, !45, !46, !47, !48, !49, !50, !51, !52, !53, !55, !56, !57, !58, !61, !62, !63, !64, !65}
!42 = !DILocalVariable(name: "W", scope: !37, file: !2, line: 83, type: !43)
!43 = !DIDerivedType(tag: DW_TAG_const_type, baseType: !40)
!44 = !DILocalVariable(name: "H", scope: !37, file: !2, line: 84, type: !43)
!45 = !DILocalVariable(name: "SRC_STRIDE", scope: !37, file: !2, line: 85, type: !43)
!46 = !DILocalVariable(name: "DST_STRIDE", scope: !37, file: !2, line: 86, type: !43)
!47 = !DILocalVariable(name: "SRC_ROWS", scope: !37, file: !2, line: 87, type: !43)
!48 = !DILocalVariable(name: "SRC_COLS", scope: !37, file: !2, line: 88, type: !43)
!49 = !DILocalVariable(name: "src", scope: !37, file: !2, line: 90, type: !11)
!50 = !DILocalVariable(name: "dst", scope: !37, file: !2, line: 91, type: !11)
!51 = !DILocalVariable(name: "t0", scope: !37, file: !2, line: 101, type: !17)
!52 = !DILocalVariable(name: "it", scope: !37, file: !2, line: 102, type: !40)
!53 = !DILocalVariable(name: "target", scope: !37, file: !2, line: 108, type: !54)
!54 = !DIDerivedType(tag: DW_TAG_const_type, baseType: !17)
!55 = !DILocalVariable(name: "batch", scope: !37, file: !2, line: 109, type: !43)
!56 = !DILocalVariable(name: "t1", scope: !37, file: !2, line: 110, type: !17)
!57 = !DILocalVariable(name: "iters", scope: !37, file: !2, line: 111, type: !40)
!58 = !DILocalVariable(name: "b", scope: !59, file: !2, line: 113, type: !40)
!59 = distinct !DILexicalBlock(scope: !60, file: !2, line: 113, column: 5)
!60 = distinct !DILexicalBlock(scope: !37, file: !2, line: 112, column: 13)
!61 = !DILocalVariable(name: "t", scope: !60, file: !2, line: 117, type: !17)
!62 = !DILocalVariable(name: "t2", scope: !37, file: !2, line: 122, type: !17)
!63 = !DILocalVariable(name: "elapsed", scope: !37, file: !2, line: 124, type: !54)
!64 = !DILocalVariable(name: "pixels", scope: !37, file: !2, line: 125, type: !54)
!65 = !DILocalVariable(name: "mpix_s", scope: !37, file: !2, line: 126, type: !54)
!66 = !{!"int ()"}
!67 = !{i64 -1203722382849146642}
!68 = !{!"now_secs|fprintf|aligned_alloc|free|fill_src|bench_once"}
!69 = !DILocation(line: 0, scope: !37)
!70 = !DILocation(line: 90, column: 29, scope: !37)
!71 = !DILocation(line: 91, column: 29, scope: !37)
!72 = !DILocation(line: 92, column: 8, scope: !73)
!73 = distinct !DILexicalBlock(scope: !37, file: !2, line: 92, column: 7)
!74 = !DILocation(line: 92, column: 12, scope: !73)
!75 = !DILocation(line: 93, column: 13, scope: !76)
!76 = distinct !DILexicalBlock(scope: !73, file: !2, line: 92, column: 21)
!77 = !{!78, !78, i64 0}
!78 = !{!"any pointer", !79, i64 0}
!79 = !{!"omnipotent char", !80, i64 0}
!80 = !{!"Simple C/C++ TBAA"}
!81 = !DILocation(line: 93, column: 5, scope: !76)
!82 = !DILocation(line: 94, column: 5, scope: !76)
!83 = !DILocation(line: 97, column: 3, scope: !37)
!84 = !DILocation(line: 98, column: 3, scope: !37)
!85 = !DILocation(line: 101, column: 15, scope: !37)
!86 = !DILocation(line: 103, column: 10, scope: !37)
!87 = !DILocation(line: 103, column: 21, scope: !37)
!88 = !DILocation(line: 103, column: 26, scope: !37)
!89 = !DILocation(line: 103, column: 3, scope: !37)
!90 = !DILocation(line: 104, column: 68, scope: !91)
!91 = distinct !DILexicalBlock(scope: !37, file: !2, line: 103, column: 34)
!92 = !DILocation(line: 104, column: 15, scope: !91)
!93 = !DILocation(line: 104, column: 12, scope: !91)
!94 = !{!95, !95, i64 0}
!95 = !{!"long", !79, i64 0}
!96 = distinct !{!96, !89, !97, !98}
!97 = !DILocation(line: 105, column: 3, scope: !37)
!98 = !{!"llvm.loop.mustprogress"}
!99 = !DILocation(line: 110, column: 15, scope: !37)
!100 = !DILocation(line: 112, column: 3, scope: !37)
!101 = !DILocation(line: 111, column: 7, scope: !37)
!102 = !DILocation(line: 102, column: 7, scope: !37)
!103 = !DILocation(line: 0, scope: !59)
!104 = !DILocation(line: 113, column: 5, scope: !59)
!105 = !DILocation(line: 117, column: 16, scope: !60)
!106 = !DILocation(line: 0, scope: !60)
!107 = !DILocation(line: 118, column: 11, scope: !108)
!108 = distinct !DILexicalBlock(scope: !60, file: !2, line: 118, column: 9)
!109 = !DILocation(line: 118, column: 16, scope: !108)
!110 = !DILocation(line: 114, column: 70, scope: !111)
!111 = distinct !DILexicalBlock(scope: !112, file: !2, line: 113, column: 37)
!112 = distinct !DILexicalBlock(scope: !59, file: !2, line: 113, column: 5)
!113 = !DILocation(line: 114, column: 17, scope: !111)
!114 = !DILocation(line: 114, column: 14, scope: !111)
!115 = !DILocation(line: 113, column: 23, scope: !112)
!116 = distinct !{!116, !104, !117, !98}
!117 = !DILocation(line: 116, column: 5, scope: !59)
!118 = !DILocation(line: 122, column: 15, scope: !37)
!119 = !DILocation(line: 124, column: 29, scope: !37)
!120 = !DILocation(line: 125, column: 25, scope: !37)
!121 = !DILocation(line: 125, column: 39, scope: !37)
!122 = !DILocation(line: 126, column: 33, scope: !37)
!123 = !DILocation(line: 126, column: 40, scope: !37)
!124 = !DILocation(line: 129, column: 11, scope: !37)
!125 = !DILocation(line: 130, column: 55, scope: !37)
!126 = !DILocation(line: 129, column: 3, scope: !37)
!127 = !DILocation(line: 132, column: 3, scope: !37)
!128 = !DILocation(line: 133, column: 3, scope: !37)
!129 = !DILocation(line: 135, column: 1, scope: !37)
!130 = !DISubprogram(name: "aligned_alloc", scope: !131, file: !131, line: 724, type: !132, flags: DIFlagPrototyped, spFlags: DISPFlagOptimized)
!131 = !DIFile(filename: "/usr/include/stdlib.h", directory: "", checksumkind: CSK_MD5, checksum: "7fa2ecb2348a66f8b44ab9a15abd0b72")
!132 = !DISubroutineType(types: !133)
!133 = !{!134, !135, !135}
!134 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)
!135 = !DIDerivedType(tag: DW_TAG_typedef, name: "size_t", file: !136, line: 18, baseType: !21)
!136 = !DIFile(filename: "release-build/lib/clang/19/include/__stddef_size_t.h", directory: "/home/eli/llvm", checksumkind: CSK_MD5, checksum: "2c44e821a2b1951cde2eb0fb2e656867")
!137 = !DISubprogram(name: "fprintf", scope: !138, file: !138, line: 357, type: !139, flags: DIFlagPrototyped, spFlags: DISPFlagOptimized)
!138 = !DIFile(filename: "/usr/include/stdio.h", directory: "", checksumkind: CSK_MD5, checksum: "1e435c46987a169d9f9186f63a512303")
!139 = !DISubroutineType(types: !140)
!140 = !{!40, !141, !200, null}
!141 = !DIDerivedType(tag: DW_TAG_restrict_type, baseType: !142)
!142 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !143, size: 64)
!143 = !DIDerivedType(tag: DW_TAG_typedef, name: "FILE", file: !144, line: 7, baseType: !145)
!144 = !DIFile(filename: "/usr/include/x86_64-linux-gnu/bits/types/FILE.h", directory: "", checksumkind: CSK_MD5, checksum: "571f9fb6223c42439075fdde11a0de5d")
!145 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "_IO_FILE", file: !146, line: 49, size: 1728, elements: !147, annotations: !198)
!146 = !DIFile(filename: "/usr/include/x86_64-linux-gnu/bits/types/struct_FILE.h", directory: "", checksumkind: CSK_MD5, checksum: "7a6d4a00a37ee6b9a40cd04bd01f5d00")
!147 = !{!148, !149, !151, !152, !153, !154, !155, !156, !157, !158, !159, !160, !161, !164, !166, !167, !168, !171, !173, !175, !179, !182, !184, !187, !190, !191, !192, !193, !194}
!148 = !DIDerivedType(tag: DW_TAG_member, name: "_flags", scope: !145, file: !146, line: 51, baseType: !40, size: 32)
!149 = !DIDerivedType(tag: DW_TAG_member, name: "_IO_read_ptr", scope: !145, file: !146, line: 54, baseType: !150, size: 64, offset: 64)
!150 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !4, size: 64)
!151 = !DIDerivedType(tag: DW_TAG_member, name: "_IO_read_end", scope: !145, file: !146, line: 55, baseType: !150, size: 64, offset: 128)
!152 = !DIDerivedType(tag: DW_TAG_member, name: "_IO_read_base", scope: !145, file: !146, line: 56, baseType: !150, size: 64, offset: 192)
!153 = !DIDerivedType(tag: DW_TAG_member, name: "_IO_write_base", scope: !145, file: !146, line: 57, baseType: !150, size: 64, offset: 256)
!154 = !DIDerivedType(tag: DW_TAG_member, name: "_IO_write_ptr", scope: !145, file: !146, line: 58, baseType: !150, size: 64, offset: 320)
!155 = !DIDerivedType(tag: DW_TAG_member, name: "_IO_write_end", scope: !145, file: !146, line: 59, baseType: !150, size: 64, offset: 384)
!156 = !DIDerivedType(tag: DW_TAG_member, name: "_IO_buf_base", scope: !145, file: !146, line: 60, baseType: !150, size: 64, offset: 448)
!157 = !DIDerivedType(tag: DW_TAG_member, name: "_IO_buf_end", scope: !145, file: !146, line: 61, baseType: !150, size: 64, offset: 512)
!158 = !DIDerivedType(tag: DW_TAG_member, name: "_IO_save_base", scope: !145, file: !146, line: 64, baseType: !150, size: 64, offset: 576)
!159 = !DIDerivedType(tag: DW_TAG_member, name: "_IO_backup_base", scope: !145, file: !146, line: 65, baseType: !150, size: 64, offset: 640)
!160 = !DIDerivedType(tag: DW_TAG_member, name: "_IO_save_end", scope: !145, file: !146, line: 66, baseType: !150, size: 64, offset: 704)
!161 = !DIDerivedType(tag: DW_TAG_member, name: "_markers", scope: !145, file: !146, line: 68, baseType: !162, size: 64, offset: 768)
!162 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !163, size: 64)
!163 = !DICompositeType(tag: DW_TAG_structure_type, name: "_IO_marker", file: !146, line: 36, flags: DIFlagFwdDecl)
!164 = !DIDerivedType(tag: DW_TAG_member, name: "_chain", scope: !145, file: !146, line: 70, baseType: !165, size: 64, offset: 832)
!165 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !145, size: 64)
!166 = !DIDerivedType(tag: DW_TAG_member, name: "_fileno", scope: !145, file: !146, line: 72, baseType: !40, size: 32, offset: 896)
!167 = !DIDerivedType(tag: DW_TAG_member, name: "_flags2", scope: !145, file: !146, line: 73, baseType: !40, size: 32, offset: 928)
!168 = !DIDerivedType(tag: DW_TAG_member, name: "_old_offset", scope: !145, file: !146, line: 74, baseType: !169, size: 64, offset: 960)
!169 = !DIDerivedType(tag: DW_TAG_typedef, name: "__off_t", file: !15, line: 152, baseType: !170)
!170 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!171 = !DIDerivedType(tag: DW_TAG_member, name: "_cur_column", scope: !145, file: !146, line: 77, baseType: !172, size: 16, offset: 1024)
!172 = !DIBasicType(name: "unsigned short", size: 16, encoding: DW_ATE_unsigned)
!173 = !DIDerivedType(tag: DW_TAG_member, name: "_vtable_offset", scope: !145, file: !146, line: 78, baseType: !174, size: 8, offset: 1040)
!174 = !DIBasicType(name: "signed char", size: 8, encoding: DW_ATE_signed_char)
!175 = !DIDerivedType(tag: DW_TAG_member, name: "_shortbuf", scope: !145, file: !146, line: 79, baseType: !176, size: 8, offset: 1048)
!176 = !DICompositeType(tag: DW_TAG_array_type, baseType: !4, size: 8, elements: !177)
!177 = !{!178}
!178 = !DISubrange(count: 1)
!179 = !DIDerivedType(tag: DW_TAG_member, name: "_lock", scope: !145, file: !146, line: 81, baseType: !180, size: 64, offset: 1088)
!180 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !181, size: 64)
!181 = !DIDerivedType(tag: DW_TAG_typedef, name: "_IO_lock_t", file: !146, line: 43, baseType: null)
!182 = !DIDerivedType(tag: DW_TAG_member, name: "_offset", scope: !145, file: !146, line: 89, baseType: !183, size: 64, offset: 1152)
!183 = !DIDerivedType(tag: DW_TAG_typedef, name: "__off64_t", file: !15, line: 153, baseType: !170)
!184 = !DIDerivedType(tag: DW_TAG_member, name: "_codecvt", scope: !145, file: !146, line: 91, baseType: !185, size: 64, offset: 1216)
!185 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !186, size: 64)
!186 = !DICompositeType(tag: DW_TAG_structure_type, name: "_IO_codecvt", file: !146, line: 37, flags: DIFlagFwdDecl)
!187 = !DIDerivedType(tag: DW_TAG_member, name: "_wide_data", scope: !145, file: !146, line: 92, baseType: !188, size: 64, offset: 1280)
!188 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !189, size: 64)
!189 = !DICompositeType(tag: DW_TAG_structure_type, name: "_IO_wide_data", file: !146, line: 38, flags: DIFlagFwdDecl)
!190 = !DIDerivedType(tag: DW_TAG_member, name: "_freeres_list", scope: !145, file: !146, line: 93, baseType: !165, size: 64, offset: 1344)
!191 = !DIDerivedType(tag: DW_TAG_member, name: "_freeres_buf", scope: !145, file: !146, line: 94, baseType: !134, size: 64, offset: 1408)
!192 = !DIDerivedType(tag: DW_TAG_member, name: "__pad5", scope: !145, file: !146, line: 95, baseType: !135, size: 64, offset: 1472)
!193 = !DIDerivedType(tag: DW_TAG_member, name: "_mode", scope: !145, file: !146, line: 96, baseType: !40, size: 32, offset: 1536)
!194 = !DIDerivedType(tag: DW_TAG_member, name: "_unused2", scope: !145, file: !146, line: 98, baseType: !195, size: 160, offset: 1568)
!195 = !DICompositeType(tag: DW_TAG_array_type, baseType: !4, size: 160, elements: !196)
!196 = !{!197}
!197 = !DISubrange(count: 20)
!198 = !{!199}
!199 = !{!"drift.fully_qualified_name", !"_IO_FILE"}
!200 = !DIDerivedType(tag: DW_TAG_restrict_type, baseType: !201)
!201 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !202, size: 64)
!202 = !DIDerivedType(tag: DW_TAG_const_type, baseType: !4)
!203 = distinct !DISubprogram(name: "fill_src", scope: !2, file: !2, line: 42, type: !204, scopeLine: 43, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagLocalToUnit | DISPFlagDefinition | DISPFlagOptimized, unit: !9, retainedNodes: !206)
!204 = !DISubroutineType(cc: DW_CC_nocall, types: !205)
!205 = !{null, !11, !40, !40, !40}
!206 = !{!207, !208, !209, !210, !211, !213}
!207 = !DILocalVariable(name: "src", arg: 1, scope: !203, file: !2, line: 42, type: !11)
!208 = !DILocalVariable(name: "stride", arg: 2, scope: !203, file: !2, line: 42, type: !40)
!209 = !DILocalVariable(name: "rows", arg: 3, scope: !203, file: !2, line: 42, type: !40)
!210 = !DILocalVariable(name: "cols", arg: 4, scope: !203, file: !2, line: 42, type: !40)
!211 = !DILocalVariable(name: "y", scope: !212, file: !2, line: 44, type: !40)
!212 = distinct !DILexicalBlock(scope: !203, file: !2, line: 44, column: 3)
!213 = !DILocalVariable(name: "x", scope: !214, file: !2, line: 45, type: !40)
!214 = distinct !DILexicalBlock(scope: !215, file: !2, line: 45, column: 5)
!215 = distinct !DILexicalBlock(scope: !216, file: !2, line: 44, column: 34)
!216 = distinct !DILexicalBlock(scope: !212, file: !2, line: 44, column: 3)
!217 = !{!"void (unsigned char *, int, int, int)"}
!218 = !{i64 8724996266571592169}
!219 = !DILocation(line: 0, scope: !203)
!220 = !DILocation(line: 0, scope: !212)
!221 = !DILocation(line: 44, column: 3, scope: !212)
!222 = !DILocation(line: 0, scope: !214)
!223 = !DILocation(line: 45, column: 5, scope: !214)
!224 = !DILocation(line: 49, column: 1, scope: !203)
!225 = !DILocation(line: 44, column: 30, scope: !216)
!226 = !DILocation(line: 44, column: 21, scope: !216)
!227 = distinct !{!227, !221, !228, !98}
!228 = !DILocation(line: 48, column: 3, scope: !212)
!229 = !DILocation(line: 46, column: 51, scope: !230)
!230 = distinct !DILexicalBlock(scope: !231, file: !2, line: 45, column: 36)
!231 = distinct !DILexicalBlock(scope: !214, file: !2, line: 45, column: 5)
!232 = !DILocation(line: 46, column: 47, scope: !230)
!233 = !DILocation(line: 46, column: 29, scope: !230)
!234 = !DILocation(line: 46, column: 7, scope: !230)
!235 = !DILocation(line: 46, column: 27, scope: !230)
!236 = !{!79, !79, i64 0}
!237 = !DILocation(line: 45, column: 32, scope: !231)
!238 = !DILocation(line: 45, column: 23, scope: !231)
!239 = distinct !{!239, !223, !240, !98}
!240 = !DILocation(line: 47, column: 5, scope: !214)
!241 = distinct !DISubprogram(name: "now_secs", scope: !2, file: !2, line: 73, type: !242, scopeLine: 74, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagLocalToUnit | DISPFlagDefinition | DISPFlagOptimized, unit: !9, retainedNodes: !244)
!242 = !DISubroutineType(types: !243)
!243 = !{!17}
!244 = !{!245}
!245 = !DILocalVariable(name: "ts", scope: !241, file: !2, line: 75, type: !246)
!246 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "timespec", file: !247, line: 11, size: 128, elements: !248, annotations: !253)
!247 = !DIFile(filename: "/usr/include/x86_64-linux-gnu/bits/types/struct_timespec.h", directory: "", checksumkind: CSK_MD5, checksum: "55dc154df3f21a5aa944dcafba9b43f6")
!248 = !{!249, !251}
!249 = !DIDerivedType(tag: DW_TAG_member, name: "tv_sec", scope: !246, file: !247, line: 16, baseType: !250, size: 64)
!250 = !DIDerivedType(tag: DW_TAG_typedef, name: "__time_t", file: !15, line: 160, baseType: !170)
!251 = !DIDerivedType(tag: DW_TAG_member, name: "tv_nsec", scope: !246, file: !247, line: 21, baseType: !252, size: 64, offset: 64)
!252 = !DIDerivedType(tag: DW_TAG_typedef, name: "__syscall_slong_t", file: !15, line: 197, baseType: !170)
!253 = !{!254}
!254 = !{!"drift.fully_qualified_name", !"timespec"}
!255 = !{!"double ()"}
!256 = !{i64 -2697757224416595139}
!257 = !{!"clock_gettime"}
!258 = distinct !DIAssignID()
!259 = !DILocation(line: 0, scope: !241)
!260 = !DILocation(line: 75, column: 3, scope: !241)
!261 = !DILocation(line: 76, column: 3, scope: !241)
!262 = !DILocation(line: 77, column: 21, scope: !241)
!263 = !{!264, !95, i64 0}
!264 = !{!"timespec", !95, i64 0, !95, i64 8}
!265 = !DILocation(line: 77, column: 10, scope: !241)
!266 = !DILocation(line: 77, column: 41, scope: !241)
!267 = !{!264, !95, i64 8}
!268 = !DILocation(line: 77, column: 30, scope: !241)
!269 = !DILocation(line: 77, column: 28, scope: !241)
!270 = !DILocation(line: 78, column: 1, scope: !241)
!271 = !DILocation(line: 77, column: 3, scope: !241)
!272 = distinct !DISubprogram(name: "bench_once", scope: !2, file: !2, line: 52, type: !273, scopeLine: 55, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagLocalToUnit | DISPFlagDefinition | DISPFlagOptimized, unit: !9, retainedNodes: !275)
!273 = !DISubroutineType(cc: DW_CC_nocall, types: !274)
!274 = !{!19, !11, !40, !11, !40, !40, !40, !40}
!275 = !{!276, !277, !278, !279, !280, !281, !282, !283, !284, !285, !286, !288}
!276 = !DILocalVariable(name: "dst", arg: 1, scope: !272, file: !2, line: 52, type: !11)
!277 = !DILocalVariable(name: "dst_stride", arg: 2, scope: !272, file: !2, line: 52, type: !40)
!278 = !DILocalVariable(name: "src", arg: 3, scope: !272, file: !2, line: 53, type: !11)
!279 = !DILocalVariable(name: "src_stride", arg: 4, scope: !272, file: !2, line: 53, type: !40)
!280 = !DILocalVariable(name: "w", arg: 5, scope: !272, file: !2, line: 54, type: !40)
!281 = !DILocalVariable(name: "h", arg: 6, scope: !272, file: !2, line: 54, type: !40)
!282 = !DILocalVariable(name: "it", arg: 7, scope: !272, file: !2, line: 54, type: !40)
!283 = !DILocalVariable(name: "mvx", scope: !272, file: !2, line: 57, type: !40)
!284 = !DILocalVariable(name: "mvy", scope: !272, file: !2, line: 58, type: !40)
!285 = !DILocalVariable(name: "s", scope: !272, file: !2, line: 63, type: !19)
!286 = !DILocalVariable(name: "y", scope: !287, file: !2, line: 64, type: !40)
!287 = distinct !DILexicalBlock(scope: !272, file: !2, line: 64, column: 3)
!288 = !DILocalVariable(name: "x", scope: !289, file: !2, line: 65, type: !40)
!289 = distinct !DILexicalBlock(scope: !290, file: !2, line: 65, column: 5)
!290 = distinct !DILexicalBlock(scope: !291, file: !2, line: 64, column: 31)
!291 = distinct !DILexicalBlock(scope: !287, file: !2, line: 64, column: 3)
!292 = !{!"unsigned long (unsigned char *, int, unsigned char *, int, int, int, int)"}
!293 = !{i64 -630835230909917791}
!294 = !{!"mc_chroma"}
!295 = !DILocation(line: 0, scope: !272)
!296 = !DILocation(line: 57, column: 17, scope: !272)
!297 = !DILocation(line: 57, column: 22, scope: !272)
!298 = !DILocation(line: 58, column: 17, scope: !272)
!299 = !DILocation(line: 58, column: 22, scope: !272)
!300 = !DILocation(line: 60, column: 3, scope: !272)
!301 = !DILocation(line: 0, scope: !287)
!302 = !DILocation(line: 64, column: 3, scope: !287)
!303 = !DILocation(line: 0, scope: !289)
!304 = !DILocation(line: 65, column: 5, scope: !289)
!305 = !DILocation(line: 69, column: 3, scope: !272)
!306 = !DILocation(line: 64, column: 27, scope: !291)
!307 = !DILocation(line: 64, column: 21, scope: !291)
!308 = distinct !{!308, !302, !309, !98}
!309 = !DILocation(line: 68, column: 3, scope: !287)
!310 = !DILocation(line: 66, column: 22, scope: !311)
!311 = distinct !DILexicalBlock(scope: !312, file: !2, line: 65, column: 33)
!312 = distinct !DILexicalBlock(scope: !289, file: !2, line: 65, column: 5)
!313 = !DILocation(line: 66, column: 12, scope: !311)
!314 = !DILocation(line: 66, column: 9, scope: !311)
!315 = !DILocation(line: 65, column: 29, scope: !312)
!316 = !DILocation(line: 65, column: 23, scope: !312)
!317 = distinct !{!317, !304, !318, !98}
!318 = !DILocation(line: 67, column: 5, scope: !289)
!319 = !DISubprogram(name: "free", scope: !131, file: !131, line: 687, type: !320, flags: DIFlagPrototyped, spFlags: DISPFlagOptimized)
!320 = !DISubroutineType(types: !321)
!321 = !{null, !134}
!322 = !DISubprogram(name: "clock_gettime", scope: !323, file: !323, line: 289, type: !324, flags: DIFlagPrototyped, spFlags: DISPFlagOptimized)
!323 = !DIFile(filename: "/usr/include/time.h", directory: "", checksumkind: CSK_MD5, checksum: "0f2fb4d8bdeb2539d9a74dd8d835207f")
!324 = !DISubroutineType(types: !325)
!325 = !{!40, !326, !329}
!326 = !DIDerivedType(tag: DW_TAG_typedef, name: "clockid_t", file: !327, line: 7, baseType: !328)
!327 = !DIFile(filename: "/usr/include/x86_64-linux-gnu/bits/types/clockid_t.h", directory: "", checksumkind: CSK_MD5, checksum: "099a80153c2ad48bc7f5f4a188cb6d24")
!328 = !DIDerivedType(tag: DW_TAG_typedef, name: "__clockid_t", file: !15, line: 169, baseType: !40)
!329 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !246, size: 64)
!330 = distinct !DISubprogram(name: "mc_chroma", scope: !2, file: !2, line: 13, type: !331, scopeLine: 17, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagLocalToUnit | DISPFlagDefinition | DISPFlagOptimized, unit: !9, retainedNodes: !333)
!331 = !DISubroutineType(cc: DW_CC_nocall, types: !332)
!332 = !{null, !11, !40, !11, !40, !40, !40, !40, !40}
!333 = !{!334, !335, !336, !337, !338, !339, !340, !341, !342, !343, !344, !345, !346, !347, !348, !349, !351}
!334 = !DILocalVariable(name: "dst", arg: 1, scope: !330, file: !2, line: 13, type: !11)
!335 = !DILocalVariable(name: "i_dst_stride", arg: 2, scope: !330, file: !2, line: 13, type: !40)
!336 = !DILocalVariable(name: "src", arg: 3, scope: !330, file: !2, line: 14, type: !11)
!337 = !DILocalVariable(name: "i_src_stride", arg: 4, scope: !330, file: !2, line: 14, type: !40)
!338 = !DILocalVariable(name: "mvx", arg: 5, scope: !330, file: !2, line: 15, type: !40)
!339 = !DILocalVariable(name: "mvy", arg: 6, scope: !330, file: !2, line: 15, type: !40)
!340 = !DILocalVariable(name: "i_width", arg: 7, scope: !330, file: !2, line: 16, type: !40)
!341 = !DILocalVariable(name: "i_height", arg: 8, scope: !330, file: !2, line: 16, type: !40)
!342 = !DILocalVariable(name: "srcp", scope: !330, file: !2, line: 18, type: !11)
!343 = !DILocalVariable(name: "d8x", scope: !330, file: !2, line: 20, type: !40)
!344 = !DILocalVariable(name: "d8y", scope: !330, file: !2, line: 21, type: !40)
!345 = !DILocalVariable(name: "cA", scope: !330, file: !2, line: 22, type: !40)
!346 = !DILocalVariable(name: "cB", scope: !330, file: !2, line: 23, type: !40)
!347 = !DILocalVariable(name: "cC", scope: !330, file: !2, line: 24, type: !40)
!348 = !DILocalVariable(name: "cD", scope: !330, file: !2, line: 25, type: !40)
!349 = !DILocalVariable(name: "y", scope: !350, file: !2, line: 30, type: !40)
!350 = distinct !DILexicalBlock(scope: !330, file: !2, line: 30, column: 3)
!351 = !DILocalVariable(name: "x", scope: !352, file: !2, line: 31, type: !40)
!352 = distinct !DILexicalBlock(scope: !353, file: !2, line: 31, column: 5)
!353 = distinct !DILexicalBlock(scope: !354, file: !2, line: 30, column: 38)
!354 = distinct !DILexicalBlock(scope: !350, file: !2, line: 30, column: 3)
!355 = !{!"void (unsigned char *, int, unsigned char *, int, int, int, int, int)"}
!356 = !{i64 -7815122219769205097}
!357 = !DILocation(line: 0, scope: !330)
!358 = !DILocation(line: 20, column: 17, scope: !330)
!359 = !DILocation(line: 21, column: 17, scope: !330)
!360 = !DILocation(line: 22, column: 15, scope: !330)
!361 = !DILocation(line: 22, column: 27, scope: !330)
!362 = !DILocation(line: 0, scope: !350)
!363 = !DILocation(line: 30, column: 3, scope: !350)
!364 = !DILocation(line: 0, scope: !352)
!365 = !DILocation(line: 31, column: 5, scope: !352)
!366 = !DILocation(line: 32, column: 42, scope: !367)
!367 = distinct !DILexicalBlock(scope: !368, file: !2, line: 31, column: 39)
!368 = distinct !DILexicalBlock(scope: !352, file: !2, line: 31, column: 5)
!369 = !DILocation(line: 32, column: 22, scope: !367)
!370 = !{!371}
!371 = distinct !{!371, !372}
!372 = distinct !{!372, !"LVerDomain"}
!373 = !DILocation(line: 32, column: 36, scope: !367)
!374 = !DILocation(line: 33, column: 22, scope: !367)
!375 = !DILocation(line: 33, column: 37, scope: !367)
!376 = !DILocation(line: 33, column: 30, scope: !367)
!377 = !DILocation(line: 33, column: 49, scope: !367)
!378 = !DILocation(line: 33, column: 55, scope: !367)
!379 = !DILocation(line: 32, column: 16, scope: !367)
!380 = !DILocation(line: 32, column: 7, scope: !367)
!381 = !DILocation(line: 32, column: 14, scope: !367)
!382 = !{!383}
!383 = distinct !{!383, !372}
!384 = distinct !{!384, !365, !385, !98, !386, !387}
!385 = !DILocation(line: 34, column: 5, scope: !352)
!386 = !{!"llvm.loop.isvectorized", i32 1}
!387 = !{!"llvm.loop.unroll.runtime.disable"}
!388 = !DILocation(line: 39, column: 1, scope: !330)
!389 = !DILocation(line: 35, column: 10, scope: !353)
!390 = !DILocation(line: 30, column: 34, scope: !354)
!391 = !DILocation(line: 30, column: 21, scope: !354)
!392 = distinct !{!392, !363, !393, !98}
!393 = !DILocation(line: 38, column: 3, scope: !350)
!394 = !DILocation(line: 31, column: 23, scope: !368)
!395 = distinct !{!395, !365, !385, !98, !386}
