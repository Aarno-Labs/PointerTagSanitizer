//===-- PTSanMetadataLowering.cpp - PTSan metadata lowering ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"

namespace llvm::ptsan {

class PTSanMetadataLowerer {
private:
  PTSanAnalysisState &PTA;
  PointerTagSanitizer &DS;
  Function &F;
  DominatorTree &DT;
  PostDominatorTree &PDT;
  LoopInfo &LI;
  ScalarEvolution &SE;
  PTSanUtils &PTU;

  void materializeCheckMetadata(CheckAnchor Anchor);

public:
  PTSanMetadataLowerer(PTSanAnalysisState &PTA)
      : PTA(PTA), DS(PTA.DS), F(PTA.F), DT(PTA.DT), PDT(PTA.PDT),
        LI(PTA.LI), SE(PTA.SE), PTU(PTA.PTU) {}

  void run();
};

bool PTSanMetadataLoweringPass::run(PTSanAnalysisState &PTA) {
  PTSanMetadataLowerer Lowerer(PTA);
  Lowerer.run();
  return true;
}

void PTSanMetadataLowerer::materializeCheckMetadata(CheckAnchor Anchor) {
  assert(!Anchor.hasMaterializedRange() &&
         "check range metadata already materialized");

  PlaceablePtrCheck &Check = PTA.PlaceablePtrChecks.at(Anchor.Call);
  assert(Check.Anchor.Call == Anchor.Call &&
         "check plan should reference its live anchor");

  DBG(
    SE.verify();
    assert(DT.verify());
    assert(PDT.verify());
    LI.verify(DT);
  );

  Instruction *CheckIP = Check.getCheckIP(DT);
  DBG(errs() << "Materializing check metadata at " << *CheckIP << "\n");
  DBG(Check.Range->dump(2));

  SCEVExpander Expander(SE, DS.DL, "bounds-check");
  auto [RangeMin, RangeSize] =
      Check.Range->materializeMinSize(&PTU, Expander, CheckIP);

  Anchor.Call->moveBefore(CheckIP);
  Anchor.setMaterializedRange(RangeMin, RangeSize);
}

void PTSanMetadataLowerer::run() {
  for (CheckAnchor Anchor : DS.collectAnchors<CheckAnchor>(F)) {
    materializeCheckMetadata(Anchor);
  }
}

} // namespace llvm::ptsan
