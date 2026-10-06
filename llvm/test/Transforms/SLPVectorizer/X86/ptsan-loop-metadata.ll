;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: opt < %s -passes=slp-vectorizer,dce -S -mtriple=x86_64 -mcpu=haswell | FileCheck %s

; Vector loads and stores may inherit only loop IDs shared by all their scalar
; accesses. A common outer summary can cover lanes from different inner loops;
; an inner summary or a summary covering only one lane cannot cover the vector.
define void @common_outer(ptr %src, ptr %dst) {
; CHECK-LABEL: define void @common_outer(
; CHECK: load <2 x double>, ptr %src, align 8, !ptsan.loop.ids ![[OUTER:[0-9]+]]
; CHECK: store <2 x double> {{.*}}, ptr %dst, align 8, !ptsan.loop.ids ![[OUTER]]
; CHECK: ret void
  %src1 = getelementptr double, ptr %src, i64 1
  %dst1 = getelementptr double, ptr %dst, i64 1
  %a = load double, ptr %src, align 8, !ptsan.loop.ids !0
  %b = load double, ptr %src1, align 8, !ptsan.loop.ids !1
  store double %a, ptr %dst, align 8, !ptsan.loop.ids !0
  store double %b, ptr %dst1, align 8, !ptsan.loop.ids !1
  ret void
}

; Disjoint memberships leave an empty intersection, not either input's ID.
define void @disjoint(ptr %src, ptr %dst) {
; CHECK-LABEL: define void @disjoint(
; CHECK: load <2 x double>, ptr %src, align 8, !ptsan.loop.ids ![[EMPTY:[0-9]+]]
; CHECK: store <2 x double> {{.*}}, ptr %dst, align 8, !ptsan.loop.ids ![[EMPTY]]
; CHECK: ret void
  %src1 = getelementptr double, ptr %src, i64 1
  %dst1 = getelementptr double, ptr %dst, i64 1
  %a = load double, ptr %src, align 8, !ptsan.loop.ids !2
  %b = load double, ptr %src1, align 8, !ptsan.loop.ids !3
  store double %a, ptr %dst, align 8, !ptsan.loop.ids !2
  store double %b, ptr %dst1, align 8, !ptsan.loop.ids !3
  ret void
}

; Missing metadata on either input prevents attribution: the second load and
; first store have no membership. Check both input orders.
define void @missing(ptr %src, ptr %dst) {
; CHECK-LABEL: define void @missing(
; CHECK: load <2 x double>, ptr %src, align 8{{$}}
; CHECK: store <2 x double> {{.*}}, ptr %dst, align 8{{$}}
; CHECK: ret void
  %src1 = getelementptr double, ptr %src, i64 1
  %dst1 = getelementptr double, ptr %dst, i64 1
  %a = load double, ptr %src, align 8, !ptsan.loop.ids !0
  %b = load double, ptr %src1, align 8
  store double %a, ptr %dst, align 8
  store double %b, ptr %dst1, align 8, !ptsan.loop.ids !0
  ret void
}

; CHECK-DAG: ![[OUTER]] = !{i64 10}
; CHECK-DAG: ![[EMPTY]] = !{}
!0 = !{i64 10, i64 20}
!1 = !{i64 10, i64 30}
!2 = !{i64 20}
!3 = !{i64 30}
