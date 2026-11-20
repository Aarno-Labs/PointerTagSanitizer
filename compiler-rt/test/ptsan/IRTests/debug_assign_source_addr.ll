; RUN: %opt_ptsan -ptsan-nochecks -ptsan-stop-after=lowering %s | FileCheck %s
; RUN: %opt_ptsan -ptsan-nochecks -ptsan-reoptimize %s -o /dev/null

; Explicit tag anchors tag escaping uses of stack sources, but debug
; assignment addresses describe storage locations and must stay on the original
; pointer. If they are rewritten to the generated tag value, later optimization
; can simplify that debug address to an integer and DSE will trip over it.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @escape(ptr)
declare void @llvm.dbg.assign(metadata, metadata, metadata, metadata, metadata, metadata)

define void @debug_assign_source_addr() !dbg !4 {
entry:
  %buf = alloca [4 x i8], align 1, !DIAssignID !9
  call void @llvm.dbg.assign(metadata i1 undef, metadata !8, metadata !DIExpression(), metadata !9, metadata ptr %buf, metadata !DIExpression()), !dbg !10
  call void @escape(ptr %buf), !dbg !10
  ret void, !dbg !10
}

; CHECK-LABEL: define void @debug_assign_source_addr(
; CHECK: %buf = alloca
; CHECK: #dbg_assign(i1 undef, !{{[0-9]+}}, !DIExpression(), !{{[0-9]+}}, ptr %buf, !DIExpression()
; CHECK-NOT: #dbg_assign{{.*}}i64
; CHECK: [[TAGGED:%.*]] = inttoptr i64 {{.*}} to ptr
; CHECK: invoke void @escape(ptr [[TAGGED]])

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "ptsan-test", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "debug-assign-source.c", directory: "/")
!2 = !{i32 7, !"debug-info-assignment-tracking", i1 true}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = distinct !DISubprogram(name: "debug_assign_source_addr", scope: !1, file: !1, line: 1, type: !5, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!5 = !DISubroutineType(types: !6)
!6 = !{null}
!7 = !DIBasicType(name: "char", size: 8, encoding: DW_ATE_signed_char)
!8 = !DILocalVariable(name: "buf", scope: !4, file: !1, line: 2, type: !7)
!9 = distinct !DIAssignID()
!10 = !DILocation(line: 2, column: 3, scope: !4)
