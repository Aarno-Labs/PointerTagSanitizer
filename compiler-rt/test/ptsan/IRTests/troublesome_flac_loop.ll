; RUN: rm -rf %t && mkdir -p %t && cd %t
; RUN: %opt_ptsan %s -S > out.ll
; Just making sure this compiles

; =======================  autoc.ll  =======================
; A self-contained “.ll” wrapper around the autocorrelation routine.
; You can assemble it with `llvm-as autoc.ll`, run `opt` passes, etc.
; -----------------------------------------------------------

; ModuleID = 'autoc'
source_filename = "autoc"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple   = "x86_64-unknown-linux-gnu"

; --- function ----------------------------------------------------------
; Just paste the real body where the comment is.
define hidden void @FLAC__lpc_compute_autocorrelation(
        ptr nocapture noundef readonly %data,
        i32 noundef                               %data_len,
        i32 noundef                               %lag,
        ptr nocapture noundef                     %autoc
) local_unnamed_addr #0 {
entry:
  %cmp = icmp ult i32 %data_len, 32
  %cmp1 = icmp ugt i32 %lag, 16
  %or.cond = or i1 %cmp, %cmp1
  br i1 %or.cond, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  %sub = sub i32 %data_len, %lag  %cmp2365.not = icmp eq i32 %lag, 0
  br i1 %cmp2365.not, label %for.cond3.preheader, label %for.body.preheader

for.body.preheader:                               ; preds = %if.then
  %0 = zext i32 %lag to i64
  %1 = shl nuw nsw i64 %0, 3
  tail call void @llvm.memset.p0.i64(ptr align 8 %autoc, i8 0, i64 %1, i1 false)
  br label %for.cond3.preheader

for.cond3.preheader:                              ; preds = %for.body.preheader, %if.then
  %2 = zext i32 %sub to i64
  %wide.trip.count463 = zext i32 %lag to i64
  %3 = add nsw i64 %wide.trip.count463, -1
  %min.iters.check828 = icmp ult i32 %lag, 4
  %4 = trunc i64 %3 to i32
  %5 = icmp ugt i64 %3, 4294967295
  %n.vec831 = and i64 %wide.trip.count463, 4294967292
  %cmp.n842 = icmp eq i64 %n.vec831, %wide.trip.count463
  %xtraiter953 = and i64 %wide.trip.count463, 1
  %lcmp.mod954.not = icmp eq i64 %xtraiter953, 0
  %6 = add nsw i64 %wide.trip.count463, -1
  br label %for.body5

for.cond23.preheader:                             ; preds = %for.inc20
  %7 = trunc nuw i64 %indvars.iv.next466 to i32
  %cmp24372 = icmp ult i32 %7, %data_len
  br i1 %cmp24372, label %for.body26.preheader, label %if.end229

for.body26.preheader:                             ; preds = %for.cond23.preheader
  %8 = zext i32 %data_len to i64
  %9 = add i32 %lag, -1
  br label %for.body26

for.body5:                                        ; preds = %for.cond3.preheader, %for.inc20
  %indvars.iv471 = phi i64 [ 1, %for.cond3.preheader ], [ %indvars.iv.next472, %for.inc20 ]
  %indvars.iv465 = phi i64 [ 0, %for.cond3.preheader ], [ %indvars.iv.next466, %for.inc20 ]
  %10 = trunc i64 %indvars.iv465 to i32
  %arrayidx7 = getelementptr inbounds float, ptr %data, i64 %indvars.iv465
  %11 = load float, ptr %arrayidx7, align 4
  %conv = fpext float %11 to double
  br i1 %cmp2365.not, label %for.inc20, label %for.body11.preheader

for.body11.preheader:                             ; preds = %for.body5
  br i1 %min.iters.check828, label %for.body11.preheader863, label %vector.scevcheck

vector.scevcheck:                                 ; preds = %for.body11.preheader
  %12 = xor i32 %10, -1
  %13 = icmp ult i32 %12, %4
  %14 = or i1 %13, %5
  br i1 %14, label %for.body11.preheader863, label %vector.ph829

vector.ph829:                                     ; preds = %vector.scevcheck
  %broadcast.splatinsert839 = insertelement <2 x double> poison, double %conv, i64 0
  %broadcast.splat840 = shufflevector <2 x double> %broadcast.splatinsert839, <2 x double> poison, <2 x i32> zeroinitializer
  br label %vector.body833

vector.body833:                                   ; preds = %vector.body833, %vector.ph829
  %index834 = phi i64 [ 0, %vector.ph829 ], [ %index.next841, %vector.body833 ]
  %15 = add nuw i64 %index834, %indvars.iv465
  %16 = and i64 %15, 4294967295
  %17 = getelementptr inbounds float, ptr %data, i64 %16
  %18 = getelementptr inbounds i8, ptr %17, i64 8
  %wide.load835 = load <2 x float>, ptr %17, align 4
  %wide.load836 = load <2 x float>, ptr %18, align 4
  %19 = fpext <2 x float> %wide.load835 to <2 x double>
  %20 = fpext <2 x float> %wide.load836 to <2 x double>
  %21 = getelementptr inbounds double, ptr %autoc, i64 %index834
  %22 = getelementptr inbounds i8, ptr %21, i64 16
  %wide.load837 = load <2 x double>, ptr %21, align 8
  %wide.load838 = load <2 x double>, ptr %22, align 8
  %23 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %broadcast.splat840, <2 x double> %19, <2 x double> %wide.load837)
  %24 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %broadcast.splat840, <2 x double> %20, <2 x double> %wide.load838)
  store <2 x double> %23, ptr %21, align 8
  store <2 x double> %24, ptr %22, align 8
  %index.next841 = add nuw i64 %index834, 4
  %25 = icmp eq i64 %index.next841, %n.vec831
  br i1 %25, label %middle.block826, label %vector.body833

middle.block826:                                  ; preds = %vector.body833
  br i1 %cmp.n842, label %for.inc20, label %for.body11.preheader863

for.body11.preheader863:                          ; preds = %middle.block826, %vector.scevcheck, %for.body11.preheader
  %indvars.iv460.ph = phi i64 [ 0, %vector.scevcheck ], [ 0, %for.body11.preheader ], [ %n.vec831, %middle.block826 ]
  br i1 %lcmp.mod954.not, label %for.body11.prol.loopexit, label %for.body11.prol

for.body11.prol:                                  ; preds = %for.body11.preheader863
  %add.prol = add nuw i64 %indvars.iv460.ph, %indvars.iv465
  %idxprom12.prol = and i64 %add.prol, 4294967295
  %arrayidx13.prol = getelementptr inbounds float, ptr %data, i64 %idxprom12.prol
  %26 = load float, ptr %arrayidx13.prol, align 4
  %conv14.prol = fpext float %26 to double
  %arrayidx16.prol = getelementptr inbounds double, ptr %autoc, i64 %indvars.iv460.ph
  %27 = load double, ptr %arrayidx16.prol, align 8
  %28 = tail call reassoc nsz arcp double @llvm.fmuladd.f64(double %conv, double %conv14.prol, double %27)
  store double %28, ptr %arrayidx16.prol, align 8
  %indvars.iv.next461.prol = or disjoint i64 %indvars.iv460.ph, 1
  br label %for.body11.prol.loopexit

for.body11.prol.loopexit:                         ; preds = %for.body11.prol, %for.body11.preheader863
  %indvars.iv460.unr = phi i64 [ %indvars.iv460.ph, %for.body11.preheader863 ], [ %indvars.iv.next461.prol, %for.body11.prol ]
  %29 = icmp eq i64 %indvars.iv460.ph, %6
  br i1 %29, label %for.inc20, label %for.body11

for.body11:                                       ; preds = %for.body11.prol.loopexit, %for.body11
  %indvars.iv460 = phi i64 [ %indvars.iv.next461.1, %for.body11 ], [ %indvars.iv460.unr, %for.body11.prol.loopexit ]
  %add = add nuw i64 %indvars.iv460, %indvars.iv465
  %idxprom12 = and i64 %add, 4294967295
  %arrayidx13 = getelementptr inbounds float, ptr %data, i64 %idxprom12
  %30 = load float, ptr %arrayidx13, align 4
  %conv14 = fpext float %30 to double
  %arrayidx16 = getelementptr inbounds double, ptr %autoc, i64 %indvars.iv460
  %31 = load double, ptr %arrayidx16, align 8
  %32 = tail call reassoc nsz arcp double @llvm.fmuladd.f64(double %conv, double %conv14, double %31)
  store double %32, ptr %arrayidx16, align 8
  %indvars.iv.next461 = add nuw nsw i64 %indvars.iv460, 1
  %add.1 = add nuw i64 %indvars.iv.next461, %indvars.iv465
  %idxprom12.1 = and i64 %add.1, 4294967295
  %arrayidx13.1 = getelementptr inbounds float, ptr %data, i64 %idxprom12.1
  %33 = load float, ptr %arrayidx13.1, align 4
  %conv14.1 = fpext float %33 to double
  %arrayidx16.1 = getelementptr inbounds double, ptr %autoc, i64 %indvars.iv.next461
  %34 = load double, ptr %arrayidx16.1, align 8
  %35 = tail call reassoc nsz arcp double @llvm.fmuladd.f64(double %conv, double %conv14.1, double %34)
  store double %35, ptr %arrayidx16.1, align 8
  %indvars.iv.next461.1 = add nuw nsw i64 %indvars.iv460, 2
  %exitcond464.not.1 = icmp eq i64 %indvars.iv.next461.1, %wide.trip.count463
  br i1 %exitcond464.not.1, label %for.inc20, label %for.body11

for.inc20:                                        ; preds = %for.body11.prol.loopexit, %for.body11, %middle.block826, %for.body5
  %indvars.iv.next466 = add nuw nsw i64 %indvars.iv465, 1
  %cmp4.not.not = icmp ult i64 %indvars.iv465, %2
  %indvars.iv.next472 = add nuw nsw i64 %indvars.iv471, 1
  br i1 %cmp4.not.not, label %for.body5, label %for.cond23.preheader

for.body26:                                       ; preds = %for.body26.preheader, %for.inc44
  %indvar = phi i32 [ 0, %for.body26.preheader ], [ %indvar.next, %for.inc44 ]
  %indvars.iv473 = phi i64 [ %indvars.iv471, %for.body26.preheader ], [ %indvars.iv.next474, %for.inc44 ]
  %36 = sub i32 %9, %indvar
  %37 = tail call i32 @llvm.umax.i32(i32 %36, i32 1)
  %umax844 = zext i32 %37 to i64
  %38 = sub i32 %9, %indvar
  %39 = tail call i32 @llvm.umax.i32(i32 %38, i32 1)
  %umax = zext i32 %39 to i64
  %40 = add nsw i64 %umax, -1
  %41 = add i32 %indvar, %7
  %arrayidx28 = getelementptr inbounds float, ptr %data, i64 %indvars.iv473
  %42 = load float, ptr %arrayidx28, align 4
  %conv29 = fpext float %42 to double
  %cmp32370.not = icmp eq i64 %indvars.iv473, %8
  br i1 %cmp32370.not, label %for.inc44, label %for.body34.preheader

for.body34.preheader:                             ; preds = %for.body26
  %43 = trunc nuw i64 %indvars.iv473 to i32
  %sub31 = sub i32 %data_len, %43
  %44 = zext i32 %sub31 to i64
  %min.iters.check847 = icmp ult i32 %36, 4
  br i1 %min.iters.check847, label %for.body34.preheader862, label %vector.scevcheck843

vector.scevcheck843:                              ; preds = %for.body34.preheader
  %45 = trunc i64 %40 to i32
  %46 = xor i32 %41, -1
  %47 = icmp ult i32 %46, %45
  %48 = icmp ugt i64 %40, 4294967295
  %49 = or i1 %47, %48
  br i1 %49, label %for.body34.preheader862, label %vector.ph848

vector.ph848:                                     ; preds = %vector.scevcheck843
  %n.vec850 = and i64 %umax844, 4294967292
  %broadcast.splatinsert858 = insertelement <2 x double> poison, double %conv29, i64 0
  %broadcast.splat859 = shufflevector <2 x double> %broadcast.splatinsert858, <2 x double> poison, <2 x i32> zeroinitializer
  br label %vector.body852

vector.body852:                                   ; preds = %vector.body852, %vector.ph848
  %index853 = phi i64 [ 0, %vector.ph848 ], [ %index.next860, %vector.body852 ]
  %50 = add nuw i64 %index853, %indvars.iv473
  %51 = and i64 %50, 4294967295
  %52 = getelementptr inbounds float, ptr %data, i64 %51
  %53 = getelementptr inbounds i8, ptr %52, i64 8
  %wide.load854 = load <2 x float>, ptr %52, align 4
  %wide.load855 = load <2 x float>, ptr %53, align 4
  %54 = fpext <2 x float> %wide.load854 to <2 x double>
  %55 = fpext <2 x float> %wide.load855 to <2 x double>
  %56 = getelementptr inbounds double, ptr %autoc, i64 %index853
  %57 = getelementptr inbounds i8, ptr %56, i64 16
  %wide.load856 = load <2 x double>, ptr %56, align 8
  %wide.load857 = load <2 x double>, ptr %57, align 8
  %58 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %broadcast.splat859, <2 x double> %54, <2 x double> %wide.load856)
  %59 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %broadcast.splat859, <2 x double> %55, <2 x double> %wide.load857)
  store <2 x double> %58, ptr %56, align 8
  store <2 x double> %59, ptr %57, align 8
  %index.next860 = add nuw i64 %index853, 4
  %60 = icmp eq i64 %index.next860, %n.vec850
  br i1 %60, label %middle.block845, label %vector.body852

middle.block845:                                  ; preds = %vector.body852
  %cmp.n861 = icmp eq i64 %n.vec850, %umax844
  br i1 %cmp.n861, label %for.inc44, label %for.body34.preheader862

for.body34.preheader862:                          ; preds = %middle.block845, %vector.scevcheck843, %for.body34.preheader
  %indvars.iv468.ph = phi i64 [ 0, %vector.scevcheck843 ], [ 0, %for.body34.preheader ], [ %n.vec850, %middle.block845 ]
  br label %for.body34

for.body34:                                       ; preds = %for.body34.preheader862, %for.body34
  %indvars.iv468 = phi i64 [ %indvars.iv.next469, %for.body34 ], [ %indvars.iv468.ph, %for.body34.preheader862 ]
  %add35 = add nuw i64 %indvars.iv468, %indvars.iv473
  %idxprom36 = and i64 %add35, 4294967295
  %arrayidx37 = getelementptr inbounds float, ptr %data, i64 %idxprom36
  %61 = load float, ptr %arrayidx37, align 4
  %conv38 = fpext float %61 to double
  %arrayidx40 = getelementptr inbounds double, ptr %autoc, i64 %indvars.iv468
  %62 = load double, ptr %arrayidx40, align 8
  %63 = tail call reassoc nsz arcp double @llvm.fmuladd.f64(double %conv29, double %conv38, double %62)
  store double %63, ptr %arrayidx40, align 8
  %indvars.iv.next469 = add nuw nsw i64 %indvars.iv468, 1
  %cmp32 = icmp ult i64 %indvars.iv.next469, %44
  br i1 %cmp32, label %for.body34, label %for.inc44

for.inc44:                                        ; preds = %for.body34, %middle.block845, %for.body26
  %indvars.iv.next474 = add nuw nsw i64 %indvars.iv473, 1
  %cmp24 = icmp ult i64 %indvars.iv.next474, %8
  %indvar.next = add i32 %indvar, 1
  br i1 %cmp24, label %for.body26, label %if.end229

if.else:                                          ; preds = %entry
  %cmp47 = icmp ult i32 %lag, 9
  br i1 %cmp47, label %for.body53.preheader, label %if.else105

for.body53.preheader:                             ; preds = %if.else
  %64 = getelementptr inbounds i8, ptr %autoc, i64 56
  store i64 0, ptr %64, align 8
  %arrayidx68.2 = getelementptr inbounds i8, ptr %data, i64 8
  %arrayidx75.2.2 = getelementptr inbounds i8, ptr %autoc, i64 16
  %arrayidx68.4 = getelementptr inbounds i8, ptr %data, i64 16
  %arrayidx75.4.4 = getelementptr inbounds i8, ptr %autoc, i64 32
  %arrayidx68.6 = getelementptr inbounds i8, ptr %data, i64 24
  %arrayidx75.6.6 = getelementptr inbounds i8, ptr %autoc, i64 48
  %65 = load <2 x float>, ptr %arrayidx68.2, align 4
  %66 = fpext <2 x float> %65 to <2 x double>
  %67 = shufflevector <2 x double> %66, <2 x double> poison, <2 x i32> zeroinitializer
  %68 = shufflevector <2 x double> %66, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %69 = shufflevector <2 x double> %66, <2 x double> poison, <2 x i32> <i32 1, i32 1>
  %70 = extractelement <2 x double> %66, i64 1
  %71 = load <2 x float>, ptr %arrayidx68.4, align 4
  %72 = fpext <2 x float> %71 to <2 x double>
  %73 = shufflevector <2 x double> %72, <2 x double> poison, <2 x i32> zeroinitializer
  %74 = shufflevector <2 x double> %69, <2 x double> %72, <2 x i32> <i32 0, i32 2>
  %75 = shufflevector <2 x double> %72, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %76 = shufflevector <2 x double> %72, <2 x double> poison, <2 x i32> <i32 1, i32 1>
  %77 = extractelement <2 x double> %72, i64 1
  %78 = load <2 x float>, ptr %arrayidx68.6, align 4
  %79 = fpext <2 x float> %78 to <2 x double>
  %80 = shufflevector <2 x double> %79, <2 x double> poison, <2 x i32> zeroinitializer
  %81 = shufflevector <2 x double> %76, <2 x double> %79, <2 x i32> <i32 0, i32 2>
  %82 = shufflevector <2 x double> %79, <2 x double> poison, <2 x i32> <i32 1, i32 1>
  %83 = shufflevector <2 x double> %72, <2 x double> %66, <2 x i32> <i32 0, i32 3>
  %arrayidx75.7.7 = getelementptr inbounds i8, ptr %autoc, i64 56
  %84 = load double, ptr %arrayidx75.7.7, align 8
  %85 = load <2 x float>, ptr %data, align 4
  %86 = fpext <2 x float> %85 to <2 x double>
  %87 = extractelement <2 x double> %86, i64 0
  %88 = fmul reassoc nsz arcp <2 x double> %86, %86
  %89 = extractelement <2 x double> %88, i64 0
  %90 = extractelement <2 x double> %86, i64 1
  %91 = tail call reassoc nsz arcp double @llvm.fmuladd.f64(double %90, double %90, double %89)
  %92 = fmul reassoc nsz arcp double %90, %87
  %93 = shufflevector <2 x double> %67, <2 x double> %86, <2 x i32> <i32 3, i32 1>
  %94 = insertelement <2 x double> poison, double %92, i64 0
  %95 = insertelement <2 x double> %94, double %91, i64 1
  %96 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %67, <2 x double> %93, <2 x double> %95)
  %97 = fmul reassoc nsz arcp <2 x double> %66, %86
  %98 = extractelement <2 x double> %97, i64 0
  %99 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %69, <2 x double> %66, <2 x double> %96)
  %100 = tail call reassoc nsz arcp double @llvm.fmuladd.f64(double %70, double %90, double %98)
  %101 = fmul reassoc nsz arcp double %70, %87
  %102 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %73, <2 x double> %74, <2 x double> %99)
  %103 = fmul reassoc nsz arcp <2 x double> %72, %86
  %104 = extractelement <2 x double> %103, i64 0
  %105 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %76, <2 x double> %72, <2 x double> %102)
  %106 = tail call reassoc nsz arcp double @llvm.fmuladd.f64(double %77, double %90, double %104)
  %107 = fmul reassoc nsz arcp double %77, %87
  %108 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %80, <2 x double> %81, <2 x double> %105)
  %109 = fmul reassoc nsz arcp <2 x double> %79, %86
  %110 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %82, <2 x double> %79, <2 x double> %108)
  %111 = shufflevector <2 x double> %110, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  store <2 x double> %111, ptr %autoc, align 8
  %112 = shufflevector <2 x double> %66, <2 x double> %86, <2 x i32> <i32 0, i32 3>
  %113 = insertelement <2 x double> poison, double %100, i64 0
  %114 = insertelement <2 x double> %113, double %101, i64 1
  %115 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %73, <2 x double> %112, <2 x double> %114)
  %116 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %76, <2 x double> %68, <2 x double> %115)
  %117 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %80, <2 x double> %83, <2 x double> %116)
  %118 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %82, <2 x double> %75, <2 x double> %117)
  store <2 x double> %118, ptr %arrayidx75.2.2, align 8
  %119 = insertelement <2 x double> poison, double %106, i64 0
  %120 = insertelement <2 x double> %119, double %107, i64 1
  %121 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %80, <2 x double> %112, <2 x double> %120)
  %122 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %82, <2 x double> %68, <2 x double> %121)
  store <2 x double> %122, ptr %arrayidx75.4.4, align 8
  %123 = insertelement <2 x double> poison, double %84, i64 0
  %124 = shufflevector <2 x double> %123, <2 x double> %109, <2 x i32> <i32 0, i32 2>
  %125 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %82, <2 x double> %86, <2 x double> %124)
  %126 = shufflevector <2 x double> %125, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  store <2 x double> %126, ptr %arrayidx75.6.6, align 8
  %cmp83363 = icmp sgt i32 %data_len, 8
  br i1 %cmp83363, label %for.cond86.preheader.preheader, label %if.end229

for.cond86.preheader.preheader:                   ; preds = %for.body53.preheader
  %wide.trip.count455 = zext nneg i32 %data_len to i64
  %invariant.gep575 = getelementptr i8, ptr %data, i64 -4
  %invariant.gep577 = getelementptr i8, ptr %data, i64 -8
  %invariant.gep579 = getelementptr i8, ptr %data, i64 -12
  %invariant.gep581 = getelementptr i8, ptr %data, i64 -16
  %invariant.gep583 = getelementptr i8, ptr %data, i64 -20
  %invariant.gep585 = getelementptr i8, ptr %data, i64 -24
  %invariant.gep587 = getelementptr i8, ptr %data, i64 -28
  %127 = add nsw i64 %wide.trip.count455, -8
  %min.iters.check791 = icmp ult i64 %127, 2
  br i1 %min.iters.check791, label %for.cond86.preheader.preheader864, label %vector.ph792

vector.ph792:                                     ; preds = %for.cond86.preheader.preheader
  %n.vec794 = and i64 %127, -2
  %ind.end795 = add nsw i64 %n.vec794, 8
  %128 = insertelement <2 x double> %125, double 0.000000e+00, i64 1
  %129 = insertelement <2 x double> %126, double 0.000000e+00, i64 1
  %130 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %122, <2 x i32> <i32 3, i32 1>
  %131 = insertelement <2 x double> %122, double 0.000000e+00, i64 1
  %132 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %118, <2 x i32> <i32 3, i32 1>
  %133 = insertelement <2 x double> %118, double 0.000000e+00, i64 1
  %134 = insertelement <2 x double> %110, double 0.000000e+00, i64 1
  %135 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %110, <2 x i32> <i32 3, i32 1>
  br label %vector.body797

vector.body797:                                   ; preds = %vector.body797, %vector.ph792
  %index798 = phi i64 [ 0, %vector.ph792 ], [ %index.next816, %vector.body797 ]
  %vec.phi799 = phi <2 x double> [ %128, %vector.ph792 ], [ %159, %vector.body797 ]
  %vec.phi800 = phi <2 x double> [ %129, %vector.ph792 ], [ %156, %vector.body797 ]
  %vec.phi801 = phi <2 x double> [ %130, %vector.ph792 ], [ %153, %vector.body797 ]
  %vec.phi802 = phi <2 x double> [ %131, %vector.ph792 ], [ %150, %vector.body797 ]
  %vec.phi803 = phi <2 x double> [ %132, %vector.ph792 ], [ %147, %vector.body797 ]
  %vec.phi804 = phi <2 x double> [ %133, %vector.ph792 ], [ %144, %vector.body797 ]
  %vec.phi805 = phi <2 x double> [ %134, %vector.ph792 ], [ %141, %vector.body797 ]
  %vec.phi806 = phi <2 x double> [ %135, %vector.ph792 ], [ %138, %vector.body797 ]
  %offset.idx807 = add i64 %index798, 8
  %136 = getelementptr inbounds float, ptr %data, i64 %offset.idx807
  %wide.load808 = load <2 x float>, ptr %136, align 4
  %137 = fpext <2 x float> %wide.load808 to <2 x double>
  %138 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %137, <2 x double> %137, <2 x double> %vec.phi806)
  %139 = getelementptr float, ptr %invariant.gep575, i64 %offset.idx807
  %wide.load809 = load <2 x float>, ptr %139, align 4
  %140 = fpext <2 x float> %wide.load809 to <2 x double>
  %141 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %137, <2 x double> %140, <2 x double> %vec.phi805)
  %142 = getelementptr float, ptr %invariant.gep577, i64 %offset.idx807
  %wide.load810 = load <2 x float>, ptr %142, align 4
  %143 = fpext <2 x float> %wide.load810 to <2 x double>
  %144 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %137, <2 x double> %143, <2 x double> %vec.phi804)
  %145 = getelementptr float, ptr %invariant.gep579, i64 %offset.idx807
  %wide.load811 = load <2 x float>, ptr %145, align 4
  %146 = fpext <2 x float> %wide.load811 to <2 x double>
  %147 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %137, <2 x double> %146, <2 x double> %vec.phi803)
  %148 = getelementptr float, ptr %invariant.gep581, i64 %offset.idx807
  %wide.load812 = load <2 x float>, ptr %148, align 4
  %149 = fpext <2 x float> %wide.load812 to <2 x double>
  %150 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %137, <2 x double> %149, <2 x double> %vec.phi802)
  %151 = getelementptr float, ptr %invariant.gep583, i64 %offset.idx807
  %wide.load813 = load <2 x float>, ptr %151, align 4
  %152 = fpext <2 x float> %wide.load813 to <2 x double>
  %153 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %137, <2 x double> %152, <2 x double> %vec.phi801)
  %154 = getelementptr float, ptr %invariant.gep585, i64 %offset.idx807
  %wide.load814 = load <2 x float>, ptr %154, align 4
  %155 = fpext <2 x float> %wide.load814 to <2 x double>
  %156 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %137, <2 x double> %155, <2 x double> %vec.phi800)
  %157 = getelementptr float, ptr %invariant.gep587, i64 %offset.idx807
  %wide.load815 = load <2 x float>, ptr %157, align 4
  %158 = fpext <2 x float> %wide.load815 to <2 x double>
  %159 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %137, <2 x double> %158, <2 x double> %vec.phi799)
  %index.next816 = add nuw i64 %index798, 2
  %160 = icmp eq i64 %index.next816, %n.vec794
  br i1 %160, label %middle.block789, label %vector.body797

middle.block789:                                  ; preds = %vector.body797
  %161 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %159)
  %162 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %156)
  %163 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %153)
  %164 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %150)
  %165 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %147)
  %166 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %144)
  %167 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %141)
  %168 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %138)
  %cmp.n817 = icmp eq i64 %127, %n.vec794
  %169 = insertelement <2 x double> poison, double %167, i64 0
  %170 = insertelement <2 x double> %169, double %168, i64 1
  %171 = insertelement <2 x double> poison, double %166, i64 0
  %172 = insertelement <2 x double> %171, double %165, i64 1
  %173 = insertelement <2 x double> poison, double %164, i64 0
  %174 = insertelement <2 x double> %173, double %163, i64 1
  %175 = insertelement <2 x double> poison, double %162, i64 0
  %176 = insertelement <2 x double> %175, double %161, i64 1
  br i1 %cmp.n817, label %if.end229.loopexit495, label %for.cond86.preheader.preheader864

for.cond86.preheader.preheader864:                ; preds = %middle.block789, %for.cond86.preheader.preheader
  %indvars.iv452.ph = phi i64 [ 8, %for.cond86.preheader.preheader ], [ %ind.end795, %middle.block789 ]
  %.ph = phi <2 x double> [ %110, %for.cond86.preheader.preheader ], [ %170, %middle.block789 ]
  %.ph865 = phi <2 x double> [ %118, %for.cond86.preheader.preheader ], [ %172, %middle.block789 ]
  %.ph866 = phi <2 x double> [ %122, %for.cond86.preheader.preheader ], [ %174, %middle.block789 ]
  %.ph867 = phi <2 x double> [ %126, %for.cond86.preheader.preheader ], [ %176, %middle.block789 ]
  br label %for.cond86.preheader

for.cond86.preheader:                             ; preds = %for.cond86.preheader.preheader864, %for.cond86.preheader
  %indvars.iv452 = phi i64 [ %indvars.iv.next453, %for.cond86.preheader ], [ %indvars.iv452.ph, %for.cond86.preheader.preheader864 ]
  %177 = phi <2 x double> [ %184, %for.cond86.preheader ], [ %.ph, %for.cond86.preheader.preheader864 ]
  %178 = phi <2 x double> [ %188, %for.cond86.preheader ], [ %.ph865, %for.cond86.preheader.preheader864 ]
  %179 = phi <2 x double> [ %192, %for.cond86.preheader ], [ %.ph866, %for.cond86.preheader.preheader864 ]
  %180 = phi <2 x double> [ %196, %for.cond86.preheader ], [ %.ph867, %for.cond86.preheader.preheader864 ]
  %gep576 = getelementptr float, ptr %invariant.gep575, i64 %indvars.iv452
  %181 = load <2 x float>, ptr %gep576, align 4
  %182 = fpext <2 x float> %181 to <2 x double>
  %183 = shufflevector <2 x double> %182, <2 x double> poison, <2 x i32> <i32 1, i32 1>
  %184 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %183, <2 x double> %182, <2 x double> %177)
  %gep580 = getelementptr float, ptr %invariant.gep579, i64 %indvars.iv452
  %185 = load <2 x float>, ptr %gep580, align 4
  %186 = fpext <2 x float> %185 to <2 x double>
  %187 = shufflevector <2 x double> %186, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %188 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %183, <2 x double> %187, <2 x double> %178)
  %gep584 = getelementptr float, ptr %invariant.gep583, i64 %indvars.iv452
  %189 = load <2 x float>, ptr %gep584, align 4
  %190 = fpext <2 x float> %189 to <2 x double>
  %191 = shufflevector <2 x double> %190, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %192 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %183, <2 x double> %191, <2 x double> %179)
  %gep588 = getelementptr float, ptr %invariant.gep587, i64 %indvars.iv452
  %193 = load <2 x float>, ptr %gep588, align 4
  %194 = fpext <2 x float> %193 to <2 x double>
  %195 = shufflevector <2 x double> %194, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %196 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %183, <2 x double> %195, <2 x double> %180)
  %indvars.iv.next453 = add nuw nsw i64 %indvars.iv452, 1
  %exitcond456.not = icmp eq i64 %indvars.iv.next453, %wide.trip.count455
  br i1 %exitcond456.not, label %if.end229.loopexit495, label %for.cond86.preheader

if.else105:                                       ; preds = %if.else
  %cmp106 = icmp ult i32 %lag, 13
  br i1 %cmp106, label %for.body114.preheader, label %for.body175.preheader

for.body175.preheader:                            ; preds = %if.else105
  tail call void @llvm.memset.p0.i64(ptr noundef nonnull align 8 dereferenceable(128) %autoc, i8 0, i64 128, i1 false)
  br label %for.cond185.preheader

for.body114.preheader:                            ; preds = %if.else105
  tail call void @llvm.memset.p0.i64(ptr noundef nonnull align 8 dereferenceable(96) %autoc, i8 0, i64 96, i1 false)
  br label %for.cond124.preheader

for.cond124.preheader:                            ; preds = %for.body114.preheader, %for.inc140
  %indvars.iv412 = phi i64 [ 0, %for.body114.preheader ], [ %indvars.iv.next413, %for.inc140 ]
  %indvars.iv410 = phi i64 [ 1, %for.body114.preheader ], [ %indvars.iv.next411, %for.inc140 ]
  %arrayidx129 = getelementptr inbounds float, ptr %data, i64 %indvars.iv412
  %197 = load float, ptr %arrayidx129, align 4
  %conv130 = fpext float %197 to double
  %xtraiter = and i64 %indvars.iv410, 1
  %198 = icmp eq i64 %indvars.iv412, 0
  br i1 %198, label %for.inc140.unr-lcssa, label %for.cond124.preheader.new

for.cond124.preheader.new:                        ; preds = %for.cond124.preheader
  %unroll_iter = and i64 %indvars.iv410, 9223372036854775806
  br label %for.body127

for.cond143.preheader:                            ; preds = %for.inc140
  %cmp144357 = icmp sgt i32 %data_len, 12
  br i1 %cmp144357, label %for.cond147.preheader.preheader, label %if.end229

for.cond147.preheader.preheader:                  ; preds = %for.cond143.preheader
  %wide.trip.count426 = zext nneg i32 %data_len to i64
  %199 = load <2 x double>, ptr %autoc, align 8
  %arrayidx159.2.phi.trans.insert = getelementptr inbounds i8, ptr %autoc, i64 16
  %200 = load <2 x double>, ptr %arrayidx159.2.phi.trans.insert, align 8
  %arrayidx159.4.phi.trans.insert = getelementptr inbounds i8, ptr %autoc, i64 32
  %201 = load <2 x double>, ptr %arrayidx159.4.phi.trans.insert, align 8
  %arrayidx159.6.phi.trans.insert = getelementptr inbounds i8, ptr %autoc, i64 48
  %202 = load <2 x double>, ptr %arrayidx159.6.phi.trans.insert, align 8
  %arrayidx159.8.phi.trans.insert = getelementptr inbounds i8, ptr %autoc, i64 64
  %203 = load <2 x double>, ptr %arrayidx159.8.phi.trans.insert, align 8
  %arrayidx159.10.phi.trans.insert = getelementptr inbounds i8, ptr %autoc, i64 80
  %204 = load <2 x double>, ptr %arrayidx159.10.phi.trans.insert, align 8
  %invariant.gep541 = getelementptr i8, ptr %data, i64 -4
  %invariant.gep543 = getelementptr i8, ptr %data, i64 -8
  %invariant.gep545 = getelementptr i8, ptr %data, i64 -12
  %invariant.gep547 = getelementptr i8, ptr %data, i64 -16
  %invariant.gep549 = getelementptr i8, ptr %data, i64 -20
  %invariant.gep551 = getelementptr i8, ptr %data, i64 -24
  %invariant.gep553 = getelementptr i8, ptr %data, i64 -28
  %invariant.gep555 = getelementptr i8, ptr %data, i64 -32
  %invariant.gep557 = getelementptr i8, ptr %data, i64 -36
  %invariant.gep559 = getelementptr i8, ptr %data, i64 -40
  %invariant.gep561 = getelementptr i8, ptr %data, i64 -44
  %205 = add nsw i64 %wide.trip.count426, -12
  %min.iters.check742 = icmp ult i64 %205, 2
  br i1 %min.iters.check742, label %for.cond147.preheader.preheader879, label %vector.ph743

for.cond147.preheader.preheader879:               ; preds = %middle.block740, %for.cond147.preheader.preheader
  %indvars.iv423.ph = phi i64 [ 12, %for.cond147.preheader.preheader ], [ %ind.end746, %middle.block740 ]
  %.ph880 = phi <2 x double> [ %199, %for.cond147.preheader.preheader ], [ %268, %middle.block740 ]
  %.ph881 = phi <2 x double> [ %200, %for.cond147.preheader.preheader ], [ %270, %middle.block740 ]
  %.ph882 = phi <2 x double> [ %201, %for.cond147.preheader.preheader ], [ %272, %middle.block740 ]
  %.ph883 = phi <2 x double> [ %202, %for.cond147.preheader.preheader ], [ %274, %middle.block740 ]
  %.ph884 = phi <2 x double> [ %203, %for.cond147.preheader.preheader ], [ %276, %middle.block740 ]
  %.ph885 = phi <2 x double> [ %204, %for.cond147.preheader.preheader ], [ %278, %middle.block740 ]
  br label %for.cond147.preheader

vector.ph743:                                     ; preds = %for.cond147.preheader.preheader
  %n.vec745 = and i64 %205, -2
  %ind.end746 = add nsw i64 %n.vec745, 12
  %206 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %204, <2 x i32> <i32 3, i32 1>
  %207 = insertelement <2 x double> %204, double 0.000000e+00, i64 1
  %208 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %203, <2 x i32> <i32 3, i32 1>
  %209 = insertelement <2 x double> %203, double 0.000000e+00, i64 1
  %210 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %202, <2 x i32> <i32 3, i32 1>
  %211 = insertelement <2 x double> %202, double 0.000000e+00, i64 1
  %212 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %201, <2 x i32> <i32 3, i32 1>
  %213 = insertelement <2 x double> %201, double 0.000000e+00, i64 1
  %214 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %200, <2 x i32> <i32 3, i32 1>
  %215 = insertelement <2 x double> %200, double 0.000000e+00, i64 1
  %216 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %199, <2 x i32> <i32 3, i32 1>
  %217 = insertelement <2 x double> %199, double 0.000000e+00, i64 1
  br label %vector.body748

vector.body748:                                   ; preds = %vector.body748, %vector.ph743
  %index749 = phi i64 [ 0, %vector.ph743 ], [ %index.next775, %vector.body748 ]
  %vec.phi750 = phi <2 x double> [ %206, %vector.ph743 ], [ %253, %vector.body748 ]
  %vec.phi751 = phi <2 x double> [ %207, %vector.ph743 ], [ %250, %vector.body748 ]
  %vec.phi752 = phi <2 x double> [ %208, %vector.ph743 ], [ %247, %vector.body748 ]
  %vec.phi753 = phi <2 x double> [ %209, %vector.ph743 ], [ %244, %vector.body748 ]
  %vec.phi754 = phi <2 x double> [ %210, %vector.ph743 ], [ %241, %vector.body748 ]
  %vec.phi755 = phi <2 x double> [ %211, %vector.ph743 ], [ %238, %vector.body748 ]
  %vec.phi756 = phi <2 x double> [ %212, %vector.ph743 ], [ %235, %vector.body748 ]
  %vec.phi757 = phi <2 x double> [ %213, %vector.ph743 ], [ %232, %vector.body748 ]
  %vec.phi758 = phi <2 x double> [ %214, %vector.ph743 ], [ %229, %vector.body748 ]
  %vec.phi759 = phi <2 x double> [ %215, %vector.ph743 ], [ %226, %vector.body748 ]
  %vec.phi760 = phi <2 x double> [ %216, %vector.ph743 ], [ %223, %vector.body748 ]
  %vec.phi761 = phi <2 x double> [ %217, %vector.ph743 ], [ %220, %vector.body748 ]
  %offset.idx762 = add i64 %index749, 12
  %218 = getelementptr inbounds float, ptr %data, i64 %offset.idx762
  %wide.load763 = load <2 x float>, ptr %218, align 4
  %219 = fpext <2 x float> %wide.load763 to <2 x double>
  %220 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %219, <2 x double> %vec.phi761)
  %221 = getelementptr float, ptr %invariant.gep541, i64 %offset.idx762
  %wide.load764 = load <2 x float>, ptr %221, align 4
  %222 = fpext <2 x float> %wide.load764 to <2 x double>
  %223 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %222, <2 x double> %vec.phi760)
  %224 = getelementptr float, ptr %invariant.gep543, i64 %offset.idx762
  %wide.load765 = load <2 x float>, ptr %224, align 4
  %225 = fpext <2 x float> %wide.load765 to <2 x double>
  %226 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %225, <2 x double> %vec.phi759)
  %227 = getelementptr float, ptr %invariant.gep545, i64 %offset.idx762
  %wide.load766 = load <2 x float>, ptr %227, align 4
  %228 = fpext <2 x float> %wide.load766 to <2 x double>
  %229 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %228, <2 x double> %vec.phi758)
  %230 = getelementptr float, ptr %invariant.gep547, i64 %offset.idx762
  %wide.load767 = load <2 x float>, ptr %230, align 4
  %231 = fpext <2 x float> %wide.load767 to <2 x double>
  %232 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %231, <2 x double> %vec.phi757)
  %233 = getelementptr float, ptr %invariant.gep549, i64 %offset.idx762
  %wide.load768 = load <2 x float>, ptr %233, align 4
  %234 = fpext <2 x float> %wide.load768 to <2 x double>
  %235 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %234, <2 x double> %vec.phi756)
  %236 = getelementptr float, ptr %invariant.gep551, i64 %offset.idx762
  %wide.load769 = load <2 x float>, ptr %236, align 4
  %237 = fpext <2 x float> %wide.load769 to <2 x double>
  %238 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %237, <2 x double> %vec.phi755)
  %239 = getelementptr float, ptr %invariant.gep553, i64 %offset.idx762
  %wide.load770 = load <2 x float>, ptr %239, align 4
  %240 = fpext <2 x float> %wide.load770 to <2 x double>
  %241 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %240, <2 x double> %vec.phi754)
  %242 = getelementptr float, ptr %invariant.gep555, i64 %offset.idx762
  %wide.load771 = load <2 x float>, ptr %242, align 4
  %243 = fpext <2 x float> %wide.load771 to <2 x double>
  %244 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %243, <2 x double> %vec.phi753)
  %245 = getelementptr float, ptr %invariant.gep557, i64 %offset.idx762
  %wide.load772 = load <2 x float>, ptr %245, align 4
  %246 = fpext <2 x float> %wide.load772 to <2 x double>
  %247 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %246, <2 x double> %vec.phi752)
  %248 = getelementptr float, ptr %invariant.gep559, i64 %offset.idx762
  %wide.load773 = load <2 x float>, ptr %248, align 4
  %249 = fpext <2 x float> %wide.load773 to <2 x double>
  %250 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %249, <2 x double> %vec.phi751)
  %251 = getelementptr float, ptr %invariant.gep561, i64 %offset.idx762
  %wide.load774 = load <2 x float>, ptr %251, align 4
  %252 = fpext <2 x float> %wide.load774 to <2 x double>
  %253 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %219, <2 x double> %252, <2 x double> %vec.phi750)
  %index.next775 = add nuw i64 %index749, 2
  %254 = icmp eq i64 %index.next775, %n.vec745
  br i1 %254, label %middle.block740, label %vector.body748

middle.block740:                                  ; preds = %vector.body748
  %255 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %253)
  %256 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %250)
  %257 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %247)
  %258 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %244)
  %259 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %241)
  %260 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %238)
  %261 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %235)
  %262 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %232)
  %263 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %229)
  %264 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %226)
  %265 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %223)
  %266 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %220)
  %cmp.n776 = icmp eq i64 %205, %n.vec745
  %267 = insertelement <2 x double> poison, double %266, i64 0
  %268 = insertelement <2 x double> %267, double %265, i64 1
  %269 = insertelement <2 x double> poison, double %264, i64 0
  %270 = insertelement <2 x double> %269, double %263, i64 1
  %271 = insertelement <2 x double> poison, double %262, i64 0
  %272 = insertelement <2 x double> %271, double %261, i64 1
  %273 = insertelement <2 x double> poison, double %260, i64 0
  %274 = insertelement <2 x double> %273, double %259, i64 1
  %275 = insertelement <2 x double> poison, double %258, i64 0
  %276 = insertelement <2 x double> %275, double %257, i64 1
  %277 = insertelement <2 x double> poison, double %256, i64 0
  %278 = insertelement <2 x double> %277, double %255, i64 1
  br i1 %cmp.n776, label %if.end229.loopexit496, label %for.cond147.preheader.preheader879

for.body127:                                      ; preds = %for.body127, %for.cond124.preheader.new
  %indvars.iv402 = phi i64 [ 0, %for.cond124.preheader.new ], [ %indvars.iv.next403.1, %for.body127 ]
  %niter = phi i64 [ 0, %for.cond124.preheader.new ], [ %niter.next.1, %for.body127 ]
  %279 = sub nsw i64 %indvars.iv412, %indvars.iv402
  %arrayidx133 = getelementptr inbounds float, ptr %data, i64 %279
  %280 = load float, ptr %arrayidx133, align 4
  %conv134 = fpext float %280 to double
  %arrayidx136 = getelementptr inbounds double, ptr %autoc, i64 %indvars.iv402
  %281 = load double, ptr %arrayidx136, align 8
  %282 = tail call reassoc nsz arcp double @llvm.fmuladd.f64(double %conv130, double %conv134, double %281)
  store double %282, ptr %arrayidx136, align 8
  %indvars.iv.next403 = or disjoint i64 %indvars.iv402, 1
  %283 = sub nsw i64 %indvars.iv412, %indvars.iv.next403
  %arrayidx133.1 = getelementptr inbounds float, ptr %data, i64 %283
  %284 = load float, ptr %arrayidx133.1, align 4
  %conv134.1 = fpext float %284 to double
  %arrayidx136.1 = getelementptr inbounds double, ptr %autoc, i64 %indvars.iv.next403
  %285 = load double, ptr %arrayidx136.1, align 8
  %286 = tail call reassoc nsz arcp double @llvm.fmuladd.f64(double %conv130, double %conv134.1, double %285)
  store double %286, ptr %arrayidx136.1, align 8
  %indvars.iv.next403.1 = add nuw nsw i64 %indvars.iv402, 2
  %niter.next.1 = add i64 %niter, 2
  %niter.ncmp.1 = icmp eq i64 %niter.next.1, %unroll_iter
  br i1 %niter.ncmp.1, label %for.inc140.unr-lcssa, label %for.body127

for.inc140.unr-lcssa:                             ; preds = %for.body127, %for.cond124.preheader
  %indvars.iv402.unr = phi i64 [ 0, %for.cond124.preheader ], [ %indvars.iv.next403.1, %for.body127 ]
  %lcmp.mod.not = icmp eq i64 %xtraiter, 0
  br i1 %lcmp.mod.not, label %for.inc140, label %for.body127.epil

for.body127.epil:                                 ; preds = %for.inc140.unr-lcssa
  %287 = sub nsw i64 %indvars.iv412, %indvars.iv402.unr
  %arrayidx133.epil = getelementptr inbounds float, ptr %data, i64 %287
  %288 = load float, ptr %arrayidx133.epil, align 4
  %conv134.epil = fpext float %288 to double
  %arrayidx136.epil = getelementptr inbounds double, ptr %autoc, i64 %indvars.iv402.unr
  %289 = load double, ptr %arrayidx136.epil, align 8
  %290 = tail call reassoc nsz arcp double @llvm.fmuladd.f64(double %conv130, double %conv134.epil, double %289)
  store double %290, ptr %arrayidx136.epil, align 8
  br label %for.inc140

for.inc140:                                       ; preds = %for.inc140.unr-lcssa, %for.body127.epil
  %indvars.iv.next413 = add nuw nsw i64 %indvars.iv412, 1
  %indvars.iv.next411 = add nuw nsw i64 %indvars.iv410, 1
  %exitcond417.not = icmp eq i64 %indvars.iv.next413, 12
  br i1 %exitcond417.not, label %for.cond143.preheader, label %for.cond124.preheader

for.cond147.preheader:                            ; preds = %for.cond147.preheader.preheader879, %for.cond147.preheader
  %indvars.iv423 = phi i64 [ %indvars.iv.next424, %for.cond147.preheader ], [ %indvars.iv423.ph, %for.cond147.preheader.preheader879 ]
  %291 = phi <2 x double> [ %301, %for.cond147.preheader ], [ %.ph880, %for.cond147.preheader.preheader879 ]
  %292 = phi <2 x double> [ %305, %for.cond147.preheader ], [ %.ph881, %for.cond147.preheader.preheader879 ]
  %293 = phi <2 x double> [ %309, %for.cond147.preheader ], [ %.ph882, %for.cond147.preheader.preheader879 ]
  %294 = phi <2 x double> [ %313, %for.cond147.preheader ], [ %.ph883, %for.cond147.preheader.preheader879 ]
  %295 = phi <2 x double> [ %317, %for.cond147.preheader ], [ %.ph884, %for.cond147.preheader.preheader879 ]
  %296 = phi <2 x double> [ %321, %for.cond147.preheader ], [ %.ph885, %for.cond147.preheader.preheader879 ]
  %gep542 = getelementptr float, ptr %invariant.gep541, i64 %indvars.iv423
  %297 = load <2 x float>, ptr %gep542, align 4
  %298 = fpext <2 x float> %297 to <2 x double>
  %299 = shufflevector <2 x double> %298, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %300 = shufflevector <2 x double> %298, <2 x double> poison, <2 x i32> <i32 1, i32 1>
  %301 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %300, <2 x double> %299, <2 x double> %291)
  %gep546 = getelementptr float, ptr %invariant.gep545, i64 %indvars.iv423
  %302 = load <2 x float>, ptr %gep546, align 4
  %303 = fpext <2 x float> %302 to <2 x double>
  %304 = shufflevector <2 x double> %303, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %305 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %300, <2 x double> %304, <2 x double> %292)
  %gep550 = getelementptr float, ptr %invariant.gep549, i64 %indvars.iv423
  %306 = load <2 x float>, ptr %gep550, align 4
  %307 = fpext <2 x float> %306 to <2 x double>
  %308 = shufflevector <2 x double> %307, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %309 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %300, <2 x double> %308, <2 x double> %293)
  %gep554 = getelementptr float, ptr %invariant.gep553, i64 %indvars.iv423
  %310 = load <2 x float>, ptr %gep554, align 4
  %311 = fpext <2 x float> %310 to <2 x double>
  %312 = shufflevector <2 x double> %311, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %313 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %300, <2 x double> %312, <2 x double> %294)
  %gep558 = getelementptr float, ptr %invariant.gep557, i64 %indvars.iv423
  %314 = load <2 x float>, ptr %gep558, align 4
  %315 = fpext <2 x float> %314 to <2 x double>
  %316 = shufflevector <2 x double> %315, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %317 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %300, <2 x double> %316, <2 x double> %295)
  %gep562 = getelementptr float, ptr %invariant.gep561, i64 %indvars.iv423
  %318 = load <2 x float>, ptr %gep562, align 4
  %319 = fpext <2 x float> %318 to <2 x double>
  %320 = shufflevector <2 x double> %319, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %321 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %300, <2 x double> %320, <2 x double> %296)
  %indvars.iv.next424 = add nuw nsw i64 %indvars.iv423, 1
  %exitcond427.not = icmp eq i64 %indvars.iv.next424, %wide.trip.count426
  br i1 %exitcond427.not, label %if.end229.loopexit496, label %for.cond147.preheader

for.cond185.preheader:                            ; preds = %for.body175.preheader, %for.inc201
  %indvars.iv384 = phi i64 [ 0, %for.body175.preheader ], [ %indvars.iv.next385, %for.inc201 ]
  %indvars.iv382 = phi i64 [ 1, %for.body175.preheader ], [ %indvars.iv.next383, %for.inc201 ]
  %arrayidx190 = getelementptr inbounds float, ptr %data, i64 %indvars.iv384
  %322 = load float, ptr %arrayidx190, align 4
  %conv191 = fpext float %322 to double
  %min.iters.check = icmp ult i64 %indvars.iv382, 4
  br i1 %min.iters.check, label %for.body188.preheader, label %vector.ph

for.body188.preheader:                            ; preds = %middle.block, %for.cond185.preheader
  %indvars.iv.ph = phi i64 [ 0, %for.cond185.preheader ], [ %n.vec, %middle.block ]
  br label %for.body188

vector.ph:                                        ; preds = %for.cond185.preheader
  %n.vec = and i64 %indvars.iv382, 9223372036854775804
  %broadcast.splatinsert = insertelement <2 x double> poison, double %conv191, i64 0
  %broadcast.splat = shufflevector <2 x double> %broadcast.splatinsert, <2 x double> poison, <2 x i32> zeroinitializer
  br label %vector.body

vector.body:                                      ; preds = %vector.body, %vector.ph
  %index = phi i64 [ 0, %vector.ph ], [ %index.next, %vector.body ]
  %323 = sub nsw i64 %indvars.iv384, %index
  %324 = getelementptr inbounds float, ptr %data, i64 %323
  %325 = getelementptr inbounds i8, ptr %324, i64 -4
  %326 = getelementptr inbounds i8, ptr %324, i64 -12
  %wide.load = load <2 x float>, ptr %325, align 4
  %reverse = shufflevector <2 x float> %wide.load, <2 x float> poison, <2 x i32> <i32 1, i32 0>
  %wide.load632 = load <2 x float>, ptr %326, align 4
  %reverse633 = shufflevector <2 x float> %wide.load632, <2 x float> poison, <2 x i32> <i32 1, i32 0>
  %327 = fpext <2 x float> %reverse to <2 x double>
  %328 = fpext <2 x float> %reverse633 to <2 x double>
  %329 = getelementptr inbounds double, ptr %autoc, i64 %index
  %330 = getelementptr inbounds i8, ptr %329, i64 16
  %wide.load634 = load <2 x double>, ptr %329, align 8
  %wide.load635 = load <2 x double>, ptr %330, align 8
  %331 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %broadcast.splat, <2 x double> %327, <2 x double> %wide.load634)
  %332 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %broadcast.splat, <2 x double> %328, <2 x double> %wide.load635)
  store <2 x double> %331, ptr %329, align 8
  store <2 x double> %332, ptr %330, align 8
  %index.next = add nuw i64 %index, 4
  %333 = icmp eq i64 %index.next, %n.vec
  br i1 %333, label %middle.block, label %vector.body

middle.block:                                     ; preds = %vector.body
  %cmp.n = icmp eq i64 %indvars.iv382, %n.vec
  br i1 %cmp.n, label %for.inc201, label %for.body188.preheader

for.cond204.preheader:                            ; preds = %for.inc201
  %cmp205351 = icmp sgt i32 %data_len, 16
  br i1 %cmp205351, label %for.cond208.preheader.preheader, label %if.end229

for.cond208.preheader.preheader:                  ; preds = %for.cond204.preheader
  %wide.trip.count = zext nneg i32 %data_len to i64
  %arrayidx220.4.phi.trans.insert = getelementptr inbounds i8, ptr %autoc, i64 32
  %334 = load <2 x double>, ptr %arrayidx220.4.phi.trans.insert, align 8
  %arrayidx220.6.phi.trans.insert = getelementptr inbounds i8, ptr %autoc, i64 48
  %335 = load <2 x double>, ptr %arrayidx220.6.phi.trans.insert, align 8
  %arrayidx220.8.phi.trans.insert = getelementptr inbounds i8, ptr %autoc, i64 64
  %336 = load <2 x double>, ptr %arrayidx220.8.phi.trans.insert, align 8
  %arrayidx220.10.phi.trans.insert = getelementptr inbounds i8, ptr %autoc, i64 80
  %337 = load <2 x double>, ptr %arrayidx220.10.phi.trans.insert, align 8
  %invariant.gep = getelementptr i8, ptr %data, i64 -4
  %invariant.gep498 = getelementptr i8, ptr %data, i64 -8
  %arrayidx220.2 = getelementptr inbounds i8, ptr %autoc, i64 16
  %invariant.gep500 = getelementptr i8, ptr %data, i64 -12
  %invariant.gep502 = getelementptr i8, ptr %data, i64 -16
  %invariant.gep504 = getelementptr i8, ptr %data, i64 -20
  %invariant.gep506 = getelementptr i8, ptr %data, i64 -24
  %invariant.gep508 = getelementptr i8, ptr %data, i64 -28
  %invariant.gep510 = getelementptr i8, ptr %data, i64 -32
  %invariant.gep512 = getelementptr i8, ptr %data, i64 -36
  %invariant.gep514 = getelementptr i8, ptr %data, i64 -40
  %invariant.gep516 = getelementptr i8, ptr %data, i64 -44
  %invariant.gep518 = getelementptr i8, ptr %data, i64 -48
  %arrayidx220.12 = getelementptr inbounds i8, ptr %autoc, i64 96
  %invariant.gep520 = getelementptr i8, ptr %data, i64 -52
  %invariant.gep522 = getelementptr i8, ptr %data, i64 -56
  %arrayidx220.14 = getelementptr inbounds i8, ptr %autoc, i64 112
  %invariant.gep524 = getelementptr i8, ptr %data, i64 -60
  %338 = load <2 x double>, ptr %autoc, align 8
  %339 = load <2 x double>, ptr %arrayidx220.2, align 8
  %340 = load <2 x double>, ptr %arrayidx220.12, align 8
  %341 = load <2 x double>, ptr %arrayidx220.14, align 8
  %342 = add nsw i64 %wide.trip.count, -16
  %min.iters.check638 = icmp ult i64 %342, 4
  br i1 %min.iters.check638, label %for.cond208.preheader.preheader904, label %vector.ph639

for.cond208.preheader.preheader904:               ; preds = %middle.block636, %for.cond208.preheader.preheader
  %indvars.iv395.ph = phi i64 [ 16, %for.cond208.preheader.preheader ], [ %ind.end, %middle.block636 ]
  %.ph905 = phi <2 x double> [ %338, %for.cond208.preheader.preheader ], [ %473, %middle.block636 ]
  %.ph906 = phi <2 x double> [ %339, %for.cond208.preheader.preheader ], [ %475, %middle.block636 ]
  %.ph907 = phi <2 x double> [ %334, %for.cond208.preheader.preheader ], [ %477, %middle.block636 ]
  %.ph908 = phi <2 x double> [ %335, %for.cond208.preheader.preheader ], [ %479, %middle.block636 ]
  %.ph909 = phi <2 x double> [ %336, %for.cond208.preheader.preheader ], [ %481, %middle.block636 ]
  %.ph910 = phi <2 x double> [ %337, %for.cond208.preheader.preheader ], [ %483, %middle.block636 ]
  %.ph911 = phi <2 x double> [ %340, %for.cond208.preheader.preheader ], [ %485, %middle.block636 ]
  %.ph912 = phi <2 x double> [ %341, %for.cond208.preheader.preheader ], [ %487, %middle.block636 ]
  br label %for.cond208.preheader

vector.ph639:                                     ; preds = %for.cond208.preheader.preheader
  %n.vec641 = and i64 %342, -4
  %ind.end = add nsw i64 %n.vec641, 16
  %343 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %341, <2 x i32> <i32 3, i32 1>
  %344 = insertelement <2 x double> %341, double 0.000000e+00, i64 1
  %345 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %340, <2 x i32> <i32 3, i32 1>
  %346 = insertelement <2 x double> %340, double 0.000000e+00, i64 1
  %347 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %339, <2 x i32> <i32 3, i32 1>
  %348 = insertelement <2 x double> %339, double 0.000000e+00, i64 1
  %349 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %338, <2 x i32> <i32 3, i32 1>
  %350 = insertelement <2 x double> %338, double 0.000000e+00, i64 1
  %351 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %337, <2 x i32> <i32 3, i32 1>
  %352 = insertelement <2 x double> %337, double 0.000000e+00, i64 1
  %353 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %336, <2 x i32> <i32 3, i32 1>
  %354 = insertelement <2 x double> %336, double 0.000000e+00, i64 1
  %355 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %335, <2 x i32> <i32 3, i32 1>
  %356 = insertelement <2 x double> %335, double 0.000000e+00, i64 1
  %357 = shufflevector <2 x double> <double poison, double 0.000000e+00>, <2 x double> %334, <2 x i32> <i32 3, i32 1>
  %358 = insertelement <2 x double> %334, double 0.000000e+00, i64 1
  br label %vector.body643

vector.body643:                                   ; preds = %vector.body643, %vector.ph639
  %index644 = phi i64 [ 0, %vector.ph639 ], [ %index.next708, %vector.body643 ]
  %vec.phi = phi <2 x double> [ %343, %vector.ph639 ], [ %453, %vector.body643 ]
  %vec.phi645 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %454, %vector.body643 ]
  %vec.phi646 = phi <2 x double> [ %344, %vector.ph639 ], [ %447, %vector.body643 ]
  %vec.phi647 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %448, %vector.body643 ]
  %vec.phi648 = phi <2 x double> [ %345, %vector.ph639 ], [ %441, %vector.body643 ]
  %vec.phi649 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %442, %vector.body643 ]
  %vec.phi650 = phi <2 x double> [ %346, %vector.ph639 ], [ %435, %vector.body643 ]
  %vec.phi651 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %436, %vector.body643 ]
  %vec.phi652 = phi <2 x double> [ %347, %vector.ph639 ], [ %381, %vector.body643 ]
  %vec.phi653 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %382, %vector.body643 ]
  %vec.phi654 = phi <2 x double> [ %348, %vector.ph639 ], [ %375, %vector.body643 ]
  %vec.phi655 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %376, %vector.body643 ]
  %vec.phi656 = phi <2 x double> [ %349, %vector.ph639 ], [ %369, %vector.body643 ]
  %vec.phi657 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %370, %vector.body643 ]
  %vec.phi658 = phi <2 x double> [ %350, %vector.ph639 ], [ %363, %vector.body643 ]
  %vec.phi659 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %364, %vector.body643 ]
  %vec.phi660 = phi <2 x double> [ %351, %vector.ph639 ], [ %429, %vector.body643 ]
  %vec.phi661 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %430, %vector.body643 ]
  %vec.phi662 = phi <2 x double> [ %352, %vector.ph639 ], [ %423, %vector.body643 ]
  %vec.phi663 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %424, %vector.body643 ]
  %vec.phi664 = phi <2 x double> [ %353, %vector.ph639 ], [ %417, %vector.body643 ]
  %vec.phi665 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %418, %vector.body643 ]
  %vec.phi666 = phi <2 x double> [ %354, %vector.ph639 ], [ %411, %vector.body643 ]
  %vec.phi667 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %412, %vector.body643 ]
  %vec.phi668 = phi <2 x double> [ %355, %vector.ph639 ], [ %405, %vector.body643 ]
  %vec.phi669 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %406, %vector.body643 ]
  %vec.phi670 = phi <2 x double> [ %356, %vector.ph639 ], [ %399, %vector.body643 ]
  %vec.phi671 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %400, %vector.body643 ]
  %vec.phi672 = phi <2 x double> [ %357, %vector.ph639 ], [ %393, %vector.body643 ]
  %vec.phi673 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %394, %vector.body643 ]
  %vec.phi674 = phi <2 x double> [ %358, %vector.ph639 ], [ %387, %vector.body643 ]
  %vec.phi675 = phi <2 x double> [ zeroinitializer, %vector.ph639 ], [ %388, %vector.body643 ]
  %offset.idx = add i64 %index644, 16
  %359 = getelementptr inbounds float, ptr %data, i64 %offset.idx
  %360 = getelementptr inbounds i8, ptr %359, i64 8
  %wide.load676 = load <2 x float>, ptr %359, align 4
  %wide.load677 = load <2 x float>, ptr %360, align 4
  %361 = fpext <2 x float> %wide.load676 to <2 x double>
  %362 = fpext <2 x float> %wide.load677 to <2 x double>
  %363 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %361, <2 x double> %vec.phi658)
  %364 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %362, <2 x double> %vec.phi659)
  %365 = getelementptr float, ptr %invariant.gep, i64 %offset.idx
  %366 = getelementptr i8, ptr %365, i64 8
  %wide.load678 = load <2 x float>, ptr %365, align 4
  %wide.load679 = load <2 x float>, ptr %366, align 4
  %367 = fpext <2 x float> %wide.load678 to <2 x double>
  %368 = fpext <2 x float> %wide.load679 to <2 x double>
  %369 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %367, <2 x double> %vec.phi656)
  %370 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %368, <2 x double> %vec.phi657)
  %371 = getelementptr float, ptr %invariant.gep498, i64 %offset.idx
  %372 = getelementptr i8, ptr %371, i64 8
  %wide.load680 = load <2 x float>, ptr %371, align 4
  %wide.load681 = load <2 x float>, ptr %372, align 4
  %373 = fpext <2 x float> %wide.load680 to <2 x double>
  %374 = fpext <2 x float> %wide.load681 to <2 x double>
  %375 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %373, <2 x double> %vec.phi654)
  %376 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %374, <2 x double> %vec.phi655)
  %377 = getelementptr float, ptr %invariant.gep500, i64 %offset.idx
  %378 = getelementptr i8, ptr %377, i64 8
  %wide.load682 = load <2 x float>, ptr %377, align 4
  %wide.load683 = load <2 x float>, ptr %378, align 4
  %379 = fpext <2 x float> %wide.load682 to <2 x double>
  %380 = fpext <2 x float> %wide.load683 to <2 x double>
  %381 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %379, <2 x double> %vec.phi652)
  %382 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %380, <2 x double> %vec.phi653)
  %383 = getelementptr float, ptr %invariant.gep502, i64 %offset.idx
  %384 = getelementptr i8, ptr %383, i64 8
  %wide.load684 = load <2 x float>, ptr %383, align 4
  %wide.load685 = load <2 x float>, ptr %384, align 4
  %385 = fpext <2 x float> %wide.load684 to <2 x double>
  %386 = fpext <2 x float> %wide.load685 to <2 x double>
  %387 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %385, <2 x double> %vec.phi674)
  %388 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %386, <2 x double> %vec.phi675)
  %389 = getelementptr float, ptr %invariant.gep504, i64 %offset.idx
  %390 = getelementptr i8, ptr %389, i64 8
  %wide.load686 = load <2 x float>, ptr %389, align 4
  %wide.load687 = load <2 x float>, ptr %390, align 4
  %391 = fpext <2 x float> %wide.load686 to <2 x double>
  %392 = fpext <2 x float> %wide.load687 to <2 x double>
  %393 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %391, <2 x double> %vec.phi672)
  %394 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %392, <2 x double> %vec.phi673)
  %395 = getelementptr float, ptr %invariant.gep506, i64 %offset.idx
  %396 = getelementptr i8, ptr %395, i64 8
  %wide.load688 = load <2 x float>, ptr %395, align 4
  %wide.load689 = load <2 x float>, ptr %396, align 4
  %397 = fpext <2 x float> %wide.load688 to <2 x double>
  %398 = fpext <2 x float> %wide.load689 to <2 x double>
  %399 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %397, <2 x double> %vec.phi670)
  %400 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %398, <2 x double> %vec.phi671)
  %401 = getelementptr float, ptr %invariant.gep508, i64 %offset.idx
  %402 = getelementptr i8, ptr %401, i64 8
  %wide.load690 = load <2 x float>, ptr %401, align 4
  %wide.load691 = load <2 x float>, ptr %402, align 4
  %403 = fpext <2 x float> %wide.load690 to <2 x double>
  %404 = fpext <2 x float> %wide.load691 to <2 x double>
  %405 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %403, <2 x double> %vec.phi668)
  %406 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %404, <2 x double> %vec.phi669)
  %407 = getelementptr float, ptr %invariant.gep510, i64 %offset.idx
  %408 = getelementptr i8, ptr %407, i64 8
  %wide.load692 = load <2 x float>, ptr %407, align 4
  %wide.load693 = load <2 x float>, ptr %408, align 4
  %409 = fpext <2 x float> %wide.load692 to <2 x double>
  %410 = fpext <2 x float> %wide.load693 to <2 x double>
  %411 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %409, <2 x double> %vec.phi666)
  %412 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %410, <2 x double> %vec.phi667)
  %413 = getelementptr float, ptr %invariant.gep512, i64 %offset.idx
  %414 = getelementptr i8, ptr %413, i64 8
  %wide.load694 = load <2 x float>, ptr %413, align 4
  %wide.load695 = load <2 x float>, ptr %414, align 4
  %415 = fpext <2 x float> %wide.load694 to <2 x double>
  %416 = fpext <2 x float> %wide.load695 to <2 x double>
  %417 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %415, <2 x double> %vec.phi664)
  %418 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %416, <2 x double> %vec.phi665)
  %419 = getelementptr float, ptr %invariant.gep514, i64 %offset.idx
  %420 = getelementptr i8, ptr %419, i64 8
  %wide.load696 = load <2 x float>, ptr %419, align 4
  %wide.load697 = load <2 x float>, ptr %420, align 4
  %421 = fpext <2 x float> %wide.load696 to <2 x double>
  %422 = fpext <2 x float> %wide.load697 to <2 x double>
  %423 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %421, <2 x double> %vec.phi662)
  %424 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %422, <2 x double> %vec.phi663)
  %425 = getelementptr float, ptr %invariant.gep516, i64 %offset.idx
  %426 = getelementptr i8, ptr %425, i64 8
  %wide.load698 = load <2 x float>, ptr %425, align 4
  %wide.load699 = load <2 x float>, ptr %426, align 4
  %427 = fpext <2 x float> %wide.load698 to <2 x double>
  %428 = fpext <2 x float> %wide.load699 to <2 x double>
  %429 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %427, <2 x double> %vec.phi660)
  %430 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %428, <2 x double> %vec.phi661)
  %431 = getelementptr float, ptr %invariant.gep518, i64 %offset.idx
  %432 = getelementptr i8, ptr %431, i64 8
  %wide.load700 = load <2 x float>, ptr %431, align 4
  %wide.load701 = load <2 x float>, ptr %432, align 4
  %433 = fpext <2 x float> %wide.load700 to <2 x double>
  %434 = fpext <2 x float> %wide.load701 to <2 x double>
  %435 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %433, <2 x double> %vec.phi650)
  %436 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %434, <2 x double> %vec.phi651)
  %437 = getelementptr float, ptr %invariant.gep520, i64 %offset.idx
  %438 = getelementptr i8, ptr %437, i64 8
  %wide.load702 = load <2 x float>, ptr %437, align 4
  %wide.load703 = load <2 x float>, ptr %438, align 4
  %439 = fpext <2 x float> %wide.load702 to <2 x double>
  %440 = fpext <2 x float> %wide.load703 to <2 x double>
  %441 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %439, <2 x double> %vec.phi648)
  %442 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %440, <2 x double> %vec.phi649)
  %443 = getelementptr float, ptr %invariant.gep522, i64 %offset.idx
  %444 = getelementptr i8, ptr %443, i64 8
  %wide.load704 = load <2 x float>, ptr %443, align 4
  %wide.load705 = load <2 x float>, ptr %444, align 4
  %445 = fpext <2 x float> %wide.load704 to <2 x double>
  %446 = fpext <2 x float> %wide.load705 to <2 x double>
  %447 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %445, <2 x double> %vec.phi646)
  %448 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %446, <2 x double> %vec.phi647)
  %449 = getelementptr float, ptr %invariant.gep524, i64 %offset.idx
  %450 = getelementptr i8, ptr %449, i64 8
  %wide.load706 = load <2 x float>, ptr %449, align 4
  %wide.load707 = load <2 x float>, ptr %450, align 4
  %451 = fpext <2 x float> %wide.load706 to <2 x double>
  %452 = fpext <2 x float> %wide.load707 to <2 x double>
  %453 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %361, <2 x double> %451, <2 x double> %vec.phi)
  %454 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %362, <2 x double> %452, <2 x double> %vec.phi645)
  %index.next708 = add nuw i64 %index644, 4
  %455 = icmp eq i64 %index.next708, %n.vec641
  br i1 %455, label %middle.block636, label %vector.body643

middle.block636:                                  ; preds = %vector.body643
  %bin.rdx = fadd reassoc nsz arcp <2 x double> %454, %453
  %456 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx)
  %bin.rdx709 = fadd reassoc nsz arcp <2 x double> %448, %447
  %457 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx709)
  %bin.rdx710 = fadd reassoc nsz arcp <2 x double> %442, %441
  %458 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx710)
  %bin.rdx711 = fadd reassoc nsz arcp <2 x double> %436, %435
  %459 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx711)
  %bin.rdx712 = fadd reassoc nsz arcp <2 x double> %382, %381
  %460 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx712)
  %bin.rdx713 = fadd reassoc nsz arcp <2 x double> %376, %375
  %461 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx713)
  %bin.rdx714 = fadd reassoc nsz arcp <2 x double> %370, %369
  %462 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx714)
  %bin.rdx715 = fadd reassoc nsz arcp <2 x double> %364, %363
  %463 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx715)
  %bin.rdx716 = fadd reassoc nsz arcp <2 x double> %430, %429
  %464 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx716)
  %bin.rdx717 = fadd reassoc nsz arcp <2 x double> %424, %423
  %465 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx717)
  %bin.rdx718 = fadd reassoc nsz arcp <2 x double> %418, %417
  %466 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx718)
  %bin.rdx719 = fadd reassoc nsz arcp <2 x double> %412, %411
  %467 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx719)
  %bin.rdx720 = fadd reassoc nsz arcp <2 x double> %406, %405
  %468 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx720)
  %bin.rdx721 = fadd reassoc nsz arcp <2 x double> %400, %399
  %469 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx721)
  %bin.rdx722 = fadd reassoc nsz arcp <2 x double> %394, %393
  %470 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx722)
  %bin.rdx723 = fadd reassoc nsz arcp <2 x double> %388, %387
  %471 = tail call reassoc nsz arcp double @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx723)
  %cmp.n724 = icmp eq i64 %342, %n.vec641
  %472 = insertelement <2 x double> poison, double %463, i64 0
  %473 = insertelement <2 x double> %472, double %462, i64 1
  %474 = insertelement <2 x double> poison, double %461, i64 0
  %475 = insertelement <2 x double> %474, double %460, i64 1
  %476 = insertelement <2 x double> poison, double %471, i64 0
  %477 = insertelement <2 x double> %476, double %470, i64 1
  %478 = insertelement <2 x double> poison, double %469, i64 0
  %479 = insertelement <2 x double> %478, double %468, i64 1
  %480 = insertelement <2 x double> poison, double %467, i64 0
  %481 = insertelement <2 x double> %480, double %466, i64 1
  %482 = insertelement <2 x double> poison, double %465, i64 0
  %483 = insertelement <2 x double> %482, double %464, i64 1
  %484 = insertelement <2 x double> poison, double %459, i64 0
  %485 = insertelement <2 x double> %484, double %458, i64 1
  %486 = insertelement <2 x double> poison, double %457, i64 0
  %487 = insertelement <2 x double> %486, double %456, i64 1
  br i1 %cmp.n724, label %if.end229.loopexit497, label %for.cond208.preheader.preheader904

for.body188:                                      ; preds = %for.body188.preheader, %for.body188
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.body188 ], [ %indvars.iv.ph, %for.body188.preheader ]
  %488 = sub nsw i64 %indvars.iv384, %indvars.iv
  %arrayidx194 = getelementptr inbounds float, ptr %data, i64 %488
  %489 = load float, ptr %arrayidx194, align 4
  %conv195 = fpext float %489 to double
  %arrayidx197 = getelementptr inbounds double, ptr %autoc, i64 %indvars.iv
  %490 = load double, ptr %arrayidx197, align 8
  %491 = tail call reassoc nsz arcp double @llvm.fmuladd.f64(double %conv191, double %conv195, double %490)
  store double %491, ptr %arrayidx197, align 8
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond.not = icmp eq i64 %indvars.iv.next, %indvars.iv382
  br i1 %exitcond.not, label %for.inc201, label %for.body188

for.inc201:                                       ; preds = %for.body188, %middle.block
  %indvars.iv.next385 = add nuw nsw i64 %indvars.iv384, 1
  %indvars.iv.next383 = add nuw nsw i64 %indvars.iv382, 1
  %exitcond389.not = icmp eq i64 %indvars.iv.next385, 16
  br i1 %exitcond389.not, label %for.cond204.preheader, label %for.cond185.preheader

for.cond208.preheader:                            ; preds = %for.cond208.preheader.preheader904, %for.cond208.preheader
  %indvars.iv395 = phi i64 [ %indvars.iv.next396, %for.cond208.preheader ], [ %indvars.iv395.ph, %for.cond208.preheader.preheader904 ]
  %492 = phi <2 x double> [ %504, %for.cond208.preheader ], [ %.ph905, %for.cond208.preheader.preheader904 ]
  %493 = phi <2 x double> [ %508, %for.cond208.preheader ], [ %.ph906, %for.cond208.preheader.preheader904 ]
  %494 = phi <2 x double> [ %512, %for.cond208.preheader ], [ %.ph907, %for.cond208.preheader.preheader904 ]
  %495 = phi <2 x double> [ %516, %for.cond208.preheader ], [ %.ph908, %for.cond208.preheader.preheader904 ]
  %496 = phi <2 x double> [ %520, %for.cond208.preheader ], [ %.ph909, %for.cond208.preheader.preheader904 ]
  %497 = phi <2 x double> [ %524, %for.cond208.preheader ], [ %.ph910, %for.cond208.preheader.preheader904 ]
  %498 = phi <2 x double> [ %528, %for.cond208.preheader ], [ %.ph911, %for.cond208.preheader.preheader904 ]
  %499 = phi <2 x double> [ %532, %for.cond208.preheader ], [ %.ph912, %for.cond208.preheader.preheader904 ]
  %gep = getelementptr float, ptr %invariant.gep, i64 %indvars.iv395
  %500 = load <2 x float>, ptr %gep, align 4
  %501 = fpext <2 x float> %500 to <2 x double>
  %502 = shufflevector <2 x double> %501, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %503 = shufflevector <2 x double> %501, <2 x double> poison, <2 x i32> <i32 1, i32 1>
  %504 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %503, <2 x double> %502, <2 x double> %492)
  %gep501 = getelementptr float, ptr %invariant.gep500, i64 %indvars.iv395
  %505 = load <2 x float>, ptr %gep501, align 4
  %506 = fpext <2 x float> %505 to <2 x double>
  %507 = shufflevector <2 x double> %506, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %508 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %503, <2 x double> %507, <2 x double> %493)
  %gep505 = getelementptr float, ptr %invariant.gep504, i64 %indvars.iv395
  %509 = load <2 x float>, ptr %gep505, align 4
  %510 = fpext <2 x float> %509 to <2 x double>
  %511 = shufflevector <2 x double> %510, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %512 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %503, <2 x double> %511, <2 x double> %494)
  %gep509 = getelementptr float, ptr %invariant.gep508, i64 %indvars.iv395
  %513 = load <2 x float>, ptr %gep509, align 4
  %514 = fpext <2 x float> %513 to <2 x double>
  %515 = shufflevector <2 x double> %514, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %516 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %503, <2 x double> %515, <2 x double> %495)
  %gep513 = getelementptr float, ptr %invariant.gep512, i64 %indvars.iv395
  %517 = load <2 x float>, ptr %gep513, align 4
  %518 = fpext <2 x float> %517 to <2 x double>
  %519 = shufflevector <2 x double> %518, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %520 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %503, <2 x double> %519, <2 x double> %496)
  %gep517 = getelementptr float, ptr %invariant.gep516, i64 %indvars.iv395
  %521 = load <2 x float>, ptr %gep517, align 4
  %522 = fpext <2 x float> %521 to <2 x double>
  %523 = shufflevector <2 x double> %522, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %524 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %503, <2 x double> %523, <2 x double> %497)
  %gep521 = getelementptr float, ptr %invariant.gep520, i64 %indvars.iv395
  %525 = load <2 x float>, ptr %gep521, align 4
  %526 = fpext <2 x float> %525 to <2 x double>
  %527 = shufflevector <2 x double> %526, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %528 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %503, <2 x double> %527, <2 x double> %498)
  %gep525 = getelementptr float, ptr %invariant.gep524, i64 %indvars.iv395
  %529 = load <2 x float>, ptr %gep525, align 4
  %530 = fpext <2 x float> %529 to <2 x double>
  %531 = shufflevector <2 x double> %530, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  %532 = tail call reassoc nsz arcp <2 x double> @llvm.fmuladd.v2f64(<2 x double> %503, <2 x double> %531, <2 x double> %499)
  %indvars.iv.next396 = add nuw nsw i64 %indvars.iv395, 1
  %exitcond398.not = icmp eq i64 %indvars.iv.next396, %wide.trip.count
  br i1 %exitcond398.not, label %if.end229.loopexit497, label %for.cond208.preheader

if.end229.loopexit495:                            ; preds = %for.cond86.preheader, %middle.block789
  %533 = phi <2 x double> [ %170, %middle.block789 ], [ %184, %for.cond86.preheader ]
  %534 = phi <2 x double> [ %172, %middle.block789 ], [ %188, %for.cond86.preheader ]
  %535 = phi <2 x double> [ %174, %middle.block789 ], [ %192, %for.cond86.preheader ]
  %536 = phi <2 x double> [ %176, %middle.block789 ], [ %196, %for.cond86.preheader ]
  %537 = shufflevector <2 x double> %533, <2 x double> poison, <2 x i32> <i32 1, i32 0>
  store <2 x double> %537, ptr %autoc, align 8
  store <2 x double> %534, ptr %arrayidx75.2.2, align 8
  store <2 x double> %535, ptr %arrayidx75.4.4, align 8
  store <2 x double> %536, ptr %arrayidx75.6.6, align 8
  br label %if.end229

if.end229.loopexit496:                            ; preds = %for.cond147.preheader, %middle.block740
  %538 = phi <2 x double> [ %268, %middle.block740 ], [ %301, %for.cond147.preheader ]
  %539 = phi <2 x double> [ %270, %middle.block740 ], [ %305, %for.cond147.preheader ]
  %540 = phi <2 x double> [ %272, %middle.block740 ], [ %309, %for.cond147.preheader ]
  %541 = phi <2 x double> [ %274, %middle.block740 ], [ %313, %for.cond147.preheader ]
  %542 = phi <2 x double> [ %276, %middle.block740 ], [ %317, %for.cond147.preheader ]
  %543 = phi <2 x double> [ %278, %middle.block740 ], [ %321, %for.cond147.preheader ]
  store <2 x double> %538, ptr %autoc, align 8
  store <2 x double> %539, ptr %arrayidx159.2.phi.trans.insert, align 8
  store <2 x double> %540, ptr %arrayidx159.4.phi.trans.insert, align 8
  store <2 x double> %541, ptr %arrayidx159.6.phi.trans.insert, align 8
  store <2 x double> %542, ptr %arrayidx159.8.phi.trans.insert, align 8
  store <2 x double> %543, ptr %arrayidx159.10.phi.trans.insert, align 8
  br label %if.end229

if.end229.loopexit497:                            ; preds = %for.cond208.preheader, %middle.block636
  %544 = phi <2 x double> [ %473, %middle.block636 ], [ %504, %for.cond208.preheader ]
  %545 = phi <2 x double> [ %475, %middle.block636 ], [ %508, %for.cond208.preheader ]
  %546 = phi <2 x double> [ %477, %middle.block636 ], [ %512, %for.cond208.preheader ]
  %547 = phi <2 x double> [ %479, %middle.block636 ], [ %516, %for.cond208.preheader ]
  %548 = phi <2 x double> [ %481, %middle.block636 ], [ %520, %for.cond208.preheader ]
  %549 = phi <2 x double> [ %483, %middle.block636 ], [ %524, %for.cond208.preheader ]
  %550 = phi <2 x double> [ %485, %middle.block636 ], [ %528, %for.cond208.preheader ]
  %551 = phi <2 x double> [ %487, %middle.block636 ], [ %532, %for.cond208.preheader ]
  store <2 x double> %544, ptr %autoc, align 8
  store <2 x double> %545, ptr %arrayidx220.2, align 8
  store <2 x double> %546, ptr %arrayidx220.4.phi.trans.insert, align 8
  store <2 x double> %547, ptr %arrayidx220.6.phi.trans.insert, align 8
  store <2 x double> %548, ptr %arrayidx220.8.phi.trans.insert, align 8
  store <2 x double> %549, ptr %arrayidx220.10.phi.trans.insert, align 8
  store <2 x double> %550, ptr %arrayidx220.12, align 8
  store <2 x double> %551, ptr %arrayidx220.14, align 8
  br label %if.end229

if.end229:                                        ; preds = %for.inc44, %if.end229.loopexit497, %if.end229.loopexit496, %if.end229.loopexit495, %for.cond204.preheader, %for.cond143.preheader, %for.body53.preheader, %for.cond23.preheader
  ret void
  ; ▲▲▲  ------------------------------------  ▲▲▲
}

; --- attributes --------------------------------------------------------
attributes #0 = { minsize nobuiltin nofree noinline
                  norecurse nosync nounwind uwtable }

; -----------------------------------------------------------
; ¹ If you do want the debug / TBAA metadata as well, just keep all the
;   `!dbg`, `!tbaa`, … attachments from your dump **and** add dummy
;   definitions for the metadata IDs at the end of the file, e.g.
;
;      !220 = !{}
;      !221 = !{}
;      …
;
;   That’s purely optional for `opt`; stripping the metadata is simpler.
; ===================  end of autoc.ll  =====================
