// PTSanLoopInfoGather.cpp
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

#include "PTSanUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/Transforms/Instrumentation/PTSanLoopInfoGather.h"

#define DEBUG_TYPE "ptsan-lig-dbg"
#define DBG(X) DEBUG_WITH_TYPE(DEBUG_TYPE, X)

using namespace llvm;
using std::optional;
using std::nullopt;

namespace llvm::ptsan {

// Defined in PointerTagSanitizer
extern cl::opt<bool> ClPTSanPreanalyzeLoops;
extern cl::opt<bool> ClNoPTSanOpts;


namespace {

struct CheckInfo {
  Value *BasePtr;
  Use *Sink;
  AccessRangePtr Range;
};

struct PTSanLoopInfoGather {
  LLVMContext &Ctx;
  const DataLayout &DL;
  Function &F;
  DominatorTree DT;
  PostDominatorTree PDT;
  LoopInfo LI;
  AssumptionCache AC;
  TargetLibraryInfo TLI;
  ScalarEvolution SE;
  PTSanUtils PTU;

  PTSanLoopInfoGather(Function &F, TargetLibraryInfo &TLI):
    Ctx(F.getContext()), DL(F.getDataLayout()), F(F), DT(F), PDT(F), LI(DT), AC(F), TLI(TLI), SE(F, TLI, AC, DT, LI),
    PTU(F, Ctx, DT, PDT, LI, SE) {};

  void analyzeFunction();
  void analyzeLoop(Loop *L,
                   SmallVector<CheckInfo> &SolvedChecks,
                   DenseMap<Value *, SmallPtrSet<Use *, 4>> &&BaseToUses);
  void analyzePtrOp(DenseMap<Loop *, SmallVector<CheckInfo>> &HoistableChecks, PtrOp &Op);
};


void PTSanLoopInfoGather::analyzePtrOp(DenseMap<Loop *, SmallVector<CheckInfo>> &HoistableChecks, PtrOp &Op) {
  Instruction *I = cast<Instruction>(Op.U->getUser());
  Value *Ptr = Op.U->get();

  Loop *L = LI.getLoopFor(I->getParent());

  if (!L) {
    DBG(errs() << "\nNot in a loop. Skipping: " << *I << " \n");
    return;
  }
  DBG(errs() << "\nAnalyzing " << *I << ": " << Op.U->getOperandNo() << " \n");

  Value *BasePtr = PTU.findBasePointer(Ptr);
  DBG(errs() << "Got base pointer " << *BasePtr << "\n");

  for (; L != nullptr; L = L->getParentLoop()) {
    DBG(errs() << "Check for Loop op in terms of\n\t" << *L << "\n");

    Instruction *BPI = dyn_cast<Instruction>(BasePtr);
    if (BPI && !DT.dominates(BPI, L->getLoopPreheader()->getTerminator())) {
      DBG(errs() << "Base Pointer doesn't dominate loop -- cannot hoist out of loop\n");
      continue;
    }

    if (!PTU.isValidCheckBlock(L->getLoopPreheader(), I)) {
      DBG(errs() << "Preheader is not a valid IP for instruction. Skipping!\n");
      DBG(errs() << "\tInstruction block is " << I->getParent()->getName() << "\n");
      DBG(errs() << "\tPreheader block is " << L->getLoopPreheader()->getName() << "\n");
      continue;
    }

    auto AccessExtent = PTU.getLoopAccessExtent(Ptr, Op.Size, L);

    if (!AccessExtent) {
      DBG(errs() << "\tFailed to find access extent\n");
      continue;
    }

    DBG(errs() << "\t\tAccessed range:\n");
    DBG(AccessExtent->dump(4));

    // Preanalysis serializes ranges that preparation may later rematerialize
    // in the loop preheader. Require the range to be safe there now; otherwise
    // a loop-body dependency, such as a load, could be re-created as a stale
    // preheader value.
    Instruction *PreheaderIP = L->getLoopPreheader()->getTerminator();
    SCEVExpander Expander(SE, DL, "ptsan-lig");
    if (!AccessExtent->isSafeToExpandAt(Expander, PreheaderIP)) {
      DBG(errs() << "\tAccess extent is not safe in the preheader\n");
      continue;
    }

    HoistableChecks[L].push_back(CheckInfo{BasePtr, Op.U, AccessExtent});
  }
}

void PTSanLoopInfoGather::analyzeFunction() {
  SmallVector<PtrOp, 32> PtrOps;
  FindMemAccesses FMA(F.getDataLayout(), PtrOps);
  for (BasicBlock *BB : depth_first(&F.getEntryBlock())) {
    for (Instruction &I : *BB) {
      FMA.visit(I);
    }
  }

  // First, figure out which checks can be hoisted
  DenseMap<Loop *, SmallVector<CheckInfo>> HoistableChecks;
  for (PtrOp &Op : PtrOps) {
    analyzePtrOp(HoistableChecks, Op);
  }

  // Now given that, figure out which a.) bases and b.) full loops we have solved
  DenseMap<Loop *, DenseMap<Value *, SmallPtrSet<Use *, 4>>> LoopBasesSinks;
  for (PtrOp &Op : PtrOps) {
    Instruction *I = cast<Instruction>(Op.U->getUser());
    Loop *L = LI.getLoopFor(I->getParent());
    if (!L) { continue; }

    for (; L != nullptr; L = L->getParentLoop()) {
      Value *Base = PTU.findBasePointer(Op.U->get());
      LoopBasesSinks[L][Base].insert(Op.U);
    }
  }

  for (auto [L, BasesToUses] : LoopBasesSinks) {
    assert(BasesToUses.size() > 0 && "Should have at least one entry if any");
    analyzeLoop(L, HoistableChecks[L], std::move(BasesToUses));
  }
}

// Analyze loop. We consume/delete BasesToUses
void PTSanLoopInfoGather::analyzeLoop(
    Loop *L,
    SmallVector<CheckInfo> &SolvedChecks,
    DenseMap<Value *, SmallPtrSet<Use *, 4>> &&BasesToUses) {
  // You could do this all at once, but I find this conceptually a lot easier

  // Now remove checks which are solved!
  for (CheckInfo &Chk : SolvedChecks) {
    if(!BasesToUses[Chk.BasePtr].erase(Chk.Sink)) {
      errs() << "Analyzed sink that wasn't in loop???\n";
      die();
    }
  }

  // We need a list of solved bases
  SmallPtrSet<Value *, 8> SolvedBases;
  // If there are entries left, it wasn't fully solved
  SmallPtrSet<Value *, 8> UnsolvedBases;
  for (auto [Base, Uses] : BasesToUses) {
    DBG(errs() << "Considering base " << *Base << "\n");
    if (Uses.size() > 0) {
      DBG(errs() << "\tFailed to solve all uses :(\n");
      UnsolvedBases.insert(Base);
      continue;
    }

    // XXX: Special-case non-dominating allocas??
    if (Instruction *BaseI = dyn_cast<Instruction>(Base)) {
      if(DT.dominates(BaseI, L->getLoopPreheader()->getTerminator())) {
        DBG(errs() << "\tBase dominates loop preaheader :D\n");
        SolvedBases.insert(Base);
      } else {
        DBG(errs() << "\tBase does not dominate loop preheader :(\n");
        UnsolvedBases.insert(Base);
      }
      continue;
    }

    DBG(errs() << "\tNon-instruction base should be good :D\n");
    SolvedBases.insert(Base);
  }

  assert(SolvedBases.size() + UnsolvedBases.size() == BasesToUses.size());
  bool Complete = SolvedBases.size() == BasesToUses.size();
  if (Complete) {
    DBG(errs() << "Solved all bases for loop!\n");
  } else {
    DBG(errs() << "Failed to solved all bases :(\n");
  }

  // Next pass: We need Bases -> List of ranges the loop accesses
  DenseMap<Value *, SmallVector<AccessRangePtr>> AccessedRanges;
  for (Value *Base : SolvedBases) {
    for (CheckInfo &Check : SolvedChecks) {
      if (Check.BasePtr != Base) {
        continue;
      }

      bool Merged = false;
      for (auto Range : AccessedRanges[Base]) {
        if (Range->cmp(&PTU, Check.Range.get()) != RCR_UNKNOWN) {
          Range->mergeIn(&PTU, Check.Range.get());
          Merged = true;
        }
      }

      if (!Merged) {
        AccessedRanges[Base].push_back(Check.Range);
      }
    }
  }

  // Now! We know which bases have been solved. We know the ranges for each base.
  // This is the information we need to pass on to our next pass.
  SerializeBoundsInfo SBI(Ctx);
  MDNode *Node = SBI.serializeSolvedRanges(Complete, AccessedRanges);
  SBI.addLoopTag(L, Node);
  //errs() << "\n\n################################################\n";
  //errs() << *F.getParent() << "\n";
  //errs() << "################################################\n";
}

} // end anonymous namespace


} // namespace llvm::ptsan

PreservedAnalyses PTSanLoopInfoGatherPass::run(Function &F, FunctionAnalysisManager &AM) {
  using namespace llvm::ptsan;
  if (!ClPTSanPreanalyzeLoops || ClNoPTSanOpts) {
    return PreservedAnalyses::all();
  }

  DBG(errs() << "\n\n###############################################\n";
  errs() << "Gathering loop info for function " << F.getName() << "\n";
  errs() << F << "\n");

  PTSanLoopInfoGather LIG(F, AM.getResult<TargetLibraryAnalysis>(F));

  LIG.analyzeFunction();
  DBG(errs() << "#############################################\n");

  // We only mutated metadata
  return PreservedAnalyses::all();
  }
