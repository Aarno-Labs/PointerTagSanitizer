; Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; RUN: %opt_ptsan -ptsan-stop-after=prepare -verify-each %s | \
; RUN:   FileCheck %s --implicit-check-not='call {{.*}}@llvm.modf'
; RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-stop-after=prepare -verify-each %s | \
; RUN:   FileCheck %s --implicit-check-not='call {{.*}}@llvm.modf'

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare { float, float } @llvm.modf.f32(float)
declare { double, double } @llvm.modf.f64(double)
declare { x86_fp80, x86_fp80 } @llvm.modf.f80(x86_fp80)
declare { <4 x float>, <4 x float> } @llvm.modf.v4f32(<4 x float>)
declare { <vscale x 2 x double>, <vscale x 2 x double> }
    @llvm.modf.nxv2f64(<vscale x 2 x double>)

define { float, float } @scalar_float(float %value) {
; CHECK-LABEL: define { float, float } @scalar_float(
; CHECK-NEXT:    [[INT:%.*]] = call float @llvm.trunc.f32(float %value)
; CHECK-NEXT:    [[FRAC:%.*]] = fsub float %value, [[INT]]
; CHECK-NEXT:    [[ABS:%.*]] = call float @llvm.fabs.f32(float %value)
; CHECK-NEXT:    [[ISINF:%.*]] = fcmp oeq float [[ABS]], +inf
; CHECK-NEXT:    [[SELECT:%.*]] = select i1 [[ISINF]], float 0.000000e+00, float [[FRAC]]
; CHECK-NEXT:    [[SIGNED:%.*]] = call float @llvm.copysign.f32(float [[SELECT]], float %value)
; CHECK-NEXT:    [[PAIR0:%.*]] = insertvalue { float, float } poison, float [[SIGNED]], 0
; CHECK-NEXT:    [[PAIR1:%.*]] = insertvalue { float, float } [[PAIR0]], float [[INT]], 1
; CHECK-NEXT:    ret { float, float } [[PAIR1]]
  %result = call { float, float } @llvm.modf.f32(float %value)
  ret { float, float } %result
}

define { double, double } @scalar_double_ninf(double %value) {
; CHECK-LABEL: define { double, double } @scalar_double_ninf(
; CHECK-NEXT:    [[INT:%.*]] = call ninf double @llvm.trunc.f64(double %value)
; CHECK-NEXT:    [[FRAC:%.*]] = fsub ninf double %value, [[INT]]
; CHECK-NOT:     @llvm.fabs.f64
; CHECK-NEXT:    [[SIGNED:%.*]] = call ninf double @llvm.copysign.f64(double [[FRAC]], double %value)
; CHECK-NEXT:    [[PAIR0:%.*]] = insertvalue { double, double } poison, double [[SIGNED]], 0
; CHECK-NEXT:    [[PAIR1:%.*]] = insertvalue { double, double } [[PAIR0]], double [[INT]], 1
; CHECK-NEXT:    ret { double, double } [[PAIR1]]
  %result = call ninf { double, double } @llvm.modf.f64(double %value)
  ret { double, double } %result
}

define { x86_fp80, x86_fp80 } @scalar_long_double(x86_fp80 %value) {
; CHECK-LABEL: define { x86_fp80, x86_fp80 } @scalar_long_double(
; CHECK-NEXT:    [[INT:%.*]] = call x86_fp80 @llvm.trunc.f80(x86_fp80 %value)
; CHECK-NEXT:    [[FRAC:%.*]] = fsub x86_fp80 %value, [[INT]]
; CHECK-NEXT:    [[ABS:%.*]] = call x86_fp80 @llvm.fabs.f80(x86_fp80 %value)
; CHECK-NEXT:    [[ISINF:%.*]] = fcmp oeq x86_fp80 [[ABS]], +inf
; CHECK-NEXT:    [[SELECT:%.*]] = select i1 [[ISINF]], x86_fp80 0.000000e+00, x86_fp80 [[FRAC]]
; CHECK-NEXT:    [[SIGNED:%.*]] = call x86_fp80 @llvm.copysign.f80(x86_fp80 [[SELECT]], x86_fp80 %value)
; CHECK-NEXT:    [[PAIR0:%.*]] = insertvalue { x86_fp80, x86_fp80 } poison, x86_fp80 [[SIGNED]], 0
; CHECK-NEXT:    [[PAIR1:%.*]] = insertvalue { x86_fp80, x86_fp80 } [[PAIR0]], x86_fp80 [[INT]], 1
; CHECK-NEXT:    ret { x86_fp80, x86_fp80 } [[PAIR1]]
  %result = call { x86_fp80, x86_fp80 } @llvm.modf.f80(x86_fp80 %value)
  ret { x86_fp80, x86_fp80 } %result
}

define { <4 x float>, <4 x float> } @fixed_vector(<4 x float> %value) {
; CHECK-LABEL: define { <4 x float>, <4 x float> } @fixed_vector(
; CHECK-NEXT:    [[INT:%.*]] = call <4 x float> @llvm.trunc.v4f32(<4 x float> %value)
; CHECK-NEXT:    [[FRAC:%.*]] = fsub <4 x float> %value, [[INT]]
; CHECK-NEXT:    [[ABS:%.*]] = call <4 x float> @llvm.fabs.v4f32(<4 x float> %value)
; CHECK-NEXT:    [[ISINF:%.*]] = fcmp oeq <4 x float> [[ABS]], splat (float +inf)
; CHECK-NEXT:    [[SELECT:%.*]] = select <4 x i1> [[ISINF]], <4 x float> zeroinitializer, <4 x float> [[FRAC]]
; CHECK-NEXT:    [[SIGNED:%.*]] = call <4 x float> @llvm.copysign.v4f32(<4 x float> [[SELECT]], <4 x float> %value)
; CHECK-NEXT:    [[PAIR0:%.*]] = insertvalue { <4 x float>, <4 x float> } poison, <4 x float> [[SIGNED]], 0
; CHECK-NEXT:    [[PAIR1:%.*]] = insertvalue { <4 x float>, <4 x float> } [[PAIR0]], <4 x float> [[INT]], 1
; CHECK-NEXT:    ret { <4 x float>, <4 x float> } [[PAIR1]]
  %result = call { <4 x float>, <4 x float> }
      @llvm.modf.v4f32(<4 x float> %value)
  ret { <4 x float>, <4 x float> } %result
}

define { <vscale x 2 x double>, <vscale x 2 x double> }
    @scalable_vector(<vscale x 2 x double> %value) {
; CHECK-LABEL: define { <vscale x 2 x double>, <vscale x 2 x double> } @scalable_vector(
; CHECK-NEXT:    [[INT:%.*]] = call <vscale x 2 x double> @llvm.trunc.nxv2f64(<vscale x 2 x double> %value)
; CHECK-NEXT:    [[FRAC:%.*]] = fsub <vscale x 2 x double> %value, [[INT]]
; CHECK-NEXT:    [[ABS:%.*]] = call <vscale x 2 x double> @llvm.fabs.nxv2f64(<vscale x 2 x double> %value)
; CHECK-NEXT:    [[ISINF:%.*]] = fcmp oeq <vscale x 2 x double> [[ABS]], splat (double +inf)
; CHECK-NEXT:    [[SELECT:%.*]] = select <vscale x 2 x i1> [[ISINF]], <vscale x 2 x double> zeroinitializer, <vscale x 2 x double> [[FRAC]]
; CHECK-NEXT:    [[SIGNED:%.*]] = call <vscale x 2 x double> @llvm.copysign.nxv2f64(<vscale x 2 x double> [[SELECT]], <vscale x 2 x double> %value)
; CHECK-NEXT:    [[PAIR0:%.*]] = insertvalue { <vscale x 2 x double>, <vscale x 2 x double> } poison, <vscale x 2 x double> [[SIGNED]], 0
; CHECK-NEXT:    [[PAIR1:%.*]] = insertvalue { <vscale x 2 x double>, <vscale x 2 x double> } [[PAIR0]], <vscale x 2 x double> [[INT]], 1
; CHECK-NEXT:    ret { <vscale x 2 x double>, <vscale x 2 x double> } [[PAIR1]]
  %result = call { <vscale x 2 x double>, <vscale x 2 x double> }
      @llvm.modf.nxv2f64(<vscale x 2 x double> %value)
  ret { <vscale x 2 x double>, <vscale x 2 x double> } %result
}
