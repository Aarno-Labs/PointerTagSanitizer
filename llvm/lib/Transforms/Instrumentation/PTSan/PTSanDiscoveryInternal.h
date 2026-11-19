//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===-- PTSanDiscoveryInternal.h - Private discovery helpers --*- C++ -*-===//
//===----------------------------------------------------------------------===//

#pragma once

#include "PTSanPhases.h"

namespace llvm::ptsan {

class PTSanDiscoverer : public InstVisitor<PTSanDiscoverer> {
private:
  PTSanAnalysisState &PTA;
  Function &F;
  PointerTagSanitizer &DS;
  DominatorTree &DT;
  PostDominatorTree &PDT;
  LoopInfo &LI;

  // Helpers
  void addPtrSink(const PtrOp &Op);
  void IRStripFn(CallBase &CB);
  void IRStripSpecialFn(CallBase &CB);
  void discoverFunctionExits();

public:
  PTSanDiscoverer(PTSanAnalysisState &PTA)
      : PTA(PTA), F(PTA.F), DS(PTA.DS), DT(PTA.DT), PDT(PTA.PDT), LI(PTA.LI) {}

  void run();
  // Shared with strip-only discovery, which also supplies the vararg ABI.
  static void discoverVarargCall(PointerTagSanitizer &DS, CallBase &CB);
  static StripAnchor createStripWithDependencies(PTSanAnalysisState &PTA,
                                                Instruction *IP, Value *Ptr,
                                                Value *Mask = nullptr);

  // Visitors
  void visitAllocaInst(AllocaInst &I);
  void visitCallBase(CallBase &CB);
  void visitGetElementPtrInst(GetElementPtrInst &I);
  void visitCmpInst(CmpInst &CI);
  void visitBinaryOperator(BinaryOperator &I);
  void visitPtrToIntInst(PtrToIntInst &I);

  void handleMainIfMain();
};

} // namespace llvm::ptsan
