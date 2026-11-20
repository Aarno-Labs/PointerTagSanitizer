;
; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: split-file %s %t
; RUN: opt < %t/input.ll -passes=instcombine -S | FileCheck %s
; RUN: not llvm-as < %t/invalid.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=VERIFY-ERROR

;--- input.ll

target datalayout = "e-p:64:64"

%struct.vector_nonnull_object = type { i32, i32 }

@vector_nonnull_objects = internal global [64 x %struct.vector_nonnull_object] zeroinitializer, align 8

declare <2 x ptr> @llvm.ptrmask.v2p0.v2i64(<2 x ptr>, <2 x i64>)

; llvm.ptrmask explicitly supports both scalar pointers and vectors of pointers.
; This case gives InstCombine enough information to prove that every result lane
; is non-null: the global is 8-byte aligned, each structure is 8 bytes, and its
; second i32 is at offset 4. Consequently, every lane of %gep is 4 modulo 8, so
; bit 2 is known to be one. The ptrmask clears only the high 16 bits and preserves
; bit 2, allowing the common vector KnownBits result to prove both non-zero and
; four-byte alignment.
;
; InstCombine used to cache both facts as return attributes. The alignment
; attribute is valid for either a pointer or a vector of pointers, but nonnull is
; valid only for a scalar pointer. Attaching nonnull to this <2 x ptr> result
; therefore created malformed IR, which the verifier rejected with:
;
;   Attribute 'nonnull' applied to incompatible type!
;
; The invalid.ll module below contains that exact malformed call and pins down
; the verifier diagnostic. The input.ll module checks that InstCombine keeps the
; useful vector alignment inference without attaching nonnull. This is
; deliberately a generic reproducer: it uses no sanitizer passes,
; sanitizer-specific intrinsics, metadata, or other PTSan machinery.
define <2 x ptr> @ptrmask_vector_known_nonnull_no_nonnull_attr(<2 x i64> %indices) {
; CHECK-LABEL: define <2 x ptr> @ptrmask_vector_known_nonnull_no_nonnull_attr
; CHECK-SAME: (<2 x i64> [[INDICES:%.*]]) {
; CHECK-NEXT:    [[GEP:%.*]] = getelementptr inbounds [64 x %struct.vector_nonnull_object], ptr @vector_nonnull_objects, i64 0, <2 x i64> [[INDICES]], i32 1
; CHECK-NEXT:    [[R:%.*]] = call align 4 <2 x ptr> @llvm.ptrmask.v2p0.v2i64(<2 x ptr> [[GEP]], <2 x i64> <i64 281474976710652, i64 281474976710652>)
; CHECK-NEXT:    ret <2 x ptr> [[R]]
;
  %gep = getelementptr inbounds [64 x %struct.vector_nonnull_object], ptr @vector_nonnull_objects, i64 0, <2 x i64> %indices, i32 1
  %r = call <2 x ptr> @llvm.ptrmask.v2p0.v2i64(<2 x ptr> %gep, <2 x i64> <i64 281474976710655, i64 281474976710655>)
  ret <2 x ptr> %r
}

;--- invalid.ll

target datalayout = "e-p:64:64"

declare <2 x ptr> @llvm.ptrmask.v2p0.v2i64(<2 x ptr>, <2 x i64>)

define <2 x ptr> @invalid_nonnull_vector_ptrmask(<2 x ptr> %ptrs) {
; VERIFY-ERROR: Attribute 'nonnull' applied to incompatible type!
; VERIFY-ERROR-NEXT: %r = call nonnull align 4 <2 x ptr> @llvm.ptrmask.v2p0.v2i64
  %r = call nonnull align 4 <2 x ptr> @llvm.ptrmask.v2p0.v2i64(<2 x ptr> %ptrs, <2 x i64> <i64 281474976710652, i64 281474976710652>)
  ret <2 x ptr> %r
}
