//===-- PTSanPreparation.cpp - PTSan preparation phase ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"

namespace llvm::ptsan {


bool PTSanPreparationPass::run(PTSanAnalysisState &PTA) {
  for (Loop *L : PTA.LI) {
    formLCSSARecursively(*L, PTA.DT, &PTA.LI, &PTA.SE);
  }

  for (Loop *L : PTA.LI.getLoopsInPreorder()) {
    InsertPreheaderForLoop(L, &PTA.DT, &PTA.LI, nullptr,
                           /*PreserveLCSSA=*/true);
  }

  for (Loop *L : PTA.LI.getLoopsInPreorder()) {
    simplifyLoop(L, &PTA.DT, &PTA.LI, &PTA.SE, nullptr, nullptr,
                 /*PreserveLCSSA=*/true);
  }

  PTA.PDT.recalculate(PTA.F);

  return true;
}

} // namespace llvm::ptsan
