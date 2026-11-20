//===-- PTSanIDStrip.cpp - Strip out PTSan ids and nothing else ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanDiscoveryInternal.h"
#include "PTSanUtils.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/Transforms/Instrumentation/PTSanIDStrip.h"

using namespace llvm;
#undef DEBUG_TYPE
#define DEBUG_TYPE "ptsan"

namespace llvm::ptsan {

static cl::opt<bool> ClClearCmps(
  "ptsan-strip-ids-on-cmp",
  cl::desc("Clear ids on cmps as well as loads"),
  cl::init(false)
);


namespace {
class PTSanIDStripVisitor : public InstVisitor<PTSanIDStripVisitor> {
  PTSanAnalysisState &PTA;
  Function &F;
  PointerTagSanitizer &DS;

public:
  PTSanIDStripVisitor(PTSanAnalysisState &PTA)
      : PTA(PTA), F(PTA.F), DS(PTA.DS) {}
  void runOnFunction();
  void visitCallBase(CallBase &CB);
  void visitLoadInst(LoadInst &I);
  void visitStoreInst(StoreInst &I);
  void visitAtomicRMWInst(AtomicRMWInst &I);
  void visitAtomicCmpXchgInst(AtomicCmpXchgInst &I);
  void visitCmpInst(CmpInst &CI);
  void visitMemIntrinsic(MemIntrinsic &I);
  void stripOperandID(Instruction &I, unsigned Index);
};
} // end namespace

void PTSanIDStripVisitor::stripOperandID(Instruction &I, unsigned Index) {
  StripAnchor Strip =
      PTSanDiscoverer::createStripWithDependencies(PTA, &I, I.getOperand(Index));
  I.setOperand(Index, Strip.Call);
}

// Unlike 'real' ptsan, we can't garantee that pointers
// will have the same ID -- one could be tagged and one
// could be untagged
void PTSanIDStripVisitor::visitCmpInst(CmpInst &CI) {
  if (!ClClearCmps) {
    return;
  }

  Type *Ty = CI.getOperand(0)->getType();
  assert(Ty == CI.getOperand(1)->getType());

  if (Ty->isPtrOrPtrVectorTy()) {
    stripOperandID(CI, 0);
    stripOperandID(CI, 1);
  }
}

void PTSanIDStripVisitor::visitAtomicRMWInst(AtomicRMWInst &I) {
  stripOperandID(I, 0);
}

void PTSanIDStripVisitor::visitAtomicCmpXchgInst(AtomicCmpXchgInst &I) {
  stripOperandID(I, 0);
}

void PTSanIDStripVisitor::visitStoreInst(StoreInst &I) {
  stripOperandID(I, 1);
}

void PTSanIDStripVisitor::visitLoadInst(LoadInst &I) {
  stripOperandID(I, 0);
}


void PTSanIDStripVisitor::runOnFunction() {
  // Iterate all BBs in depth-first order and mark instructions for visiting.
  // This is much easier than trying to modify the program while we visit it
  SmallVector<Instruction *, 16> Instructions;
  for (BasicBlock *BB : depth_first(&F.getEntryBlock())) {
    for (Instruction &I : *BB) {
      Instructions.push_back(&I);
    }
  }

  for (Instruction *I : Instructions) {
    this->visit(I);
  }
}

static bool isPTSanVar(Value *V) {
  return V->getName().starts_with("ptsan");
}

static bool callOnPtsanInternals(CallBase &CB) {
  for (unsigned i = 0; i < CB.arg_size(); ++i) {
    llvm::Value *Arg = CB.getArgOperand(i);
    // We assume ptsan stuff is handled
    if (isPTSanVar(Arg)) {
      return true;
    }
  }
  // No arguments were ptsan
  return false;
}

static bool shouldStripCallArg(CallBase &CB) {
  for (unsigned i = 0; i < CB.arg_size(); ++i) {
    llvm::Value *Arg = CB.getArgOperand(i);
    if (Arg->getType()->isPointerTy()) {
      return true;
    }
  }
  // No arguments were pointers
  return false;
}

void PTSanIDStripVisitor::visitCallBase(CallBase &CB) {
  PTSanDiscoverer::discoverVarargCall(DS, CB);
  auto *Callee = CB.getCalledFunction();
  if (!Callee || !Callee->isIntrinsic()) { return; }
  if(callOnPtsanInternals(CB)) { return; }

  // Gotta strip IDs from intrinsics' ptr args
  switch(Callee->getIntrinsicID()){
    // XXX: maybe these should point back to the original object?
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::invariant_start:
    case Intrinsic::invariant_end:
    // These two are paired, so as long as save doesn't tag, restore
    // doesn't have to untag
    case Intrinsic::stacksave:
    case Intrinsic::stackrestore:
      break;
    // va_list isn't id'd, so we don't do anything for these
    case Intrinsic::vastart:
    case Intrinsic::vaend:
    case Intrinsic::vacopy:
      break;
    // Unary ops we want to strip:
    case Intrinsic::threadlocal_address:
      // XXX: When we implement ids for globals, we'll need to
      // handle this carefully.
      break;
    case Intrinsic::x86_sse3_ldu_dq:
    case Intrinsic::x86_avx_ldu_dq_256:
    case Intrinsic::x86_sse2_clflush:
      stripOperandID(CB, 0);
      break;
    case Intrinsic::ptrmask:
      // If we run into an instruction which is masking any of the top 16
      // bits, we're in trooooooble
      // XXX: check non-const version maybe?
      if (auto *ConstMask = dyn_cast<ConstantInt>(CB.getOperand(1))) {
        uint64_t Mask = ConstMask->getZExtValue();
        constexpr uint64_t TOP_16_MASK = 0xFFFF000000000000ULL;
        assert((Mask & TOP_16_MASK) == TOP_16_MASK &&
               "ptrmask touches top 16 bits!");
      }
      break;
    default:
      if (shouldStripCallArg(CB)) {
        errs() << "Unhandled pointer-taking intrinsic: " << CB << " in " << F.getName() << "\n";
        //exit(EXIT_FAILURE);
      }
  }
}

void PTSanIDStripVisitor::visitMemIntrinsic(MemIntrinsic &I) {
  //errs() << "Got memintrinsic" << I << "\n";
  if (callOnPtsanInternals(I)) { return; }

  switch(I.getIntrinsicID()) {
    case Intrinsic::memcpy:
    case Intrinsic::memcpy_inline:
    case Intrinsic::memmove:
      stripOperandID(I, 0);
      stripOperandID(I, 1);
      break;
    case Intrinsic::memset:
    case Intrinsic::memset_inline:
      stripOperandID(I, 0);
      break;
    default:
      errs() << "Unhandled memintrinsic: " << I << " in " << F.getName() << "\n";
  }
  // Memcpy intrinsics don't return, so no need
  // to mess with return value
  assert(I.getType()->isVoidTy());
}


} // namespace llvm::ptsan

PreservedAnalyses PTSanIDStripPass::run(Module &M, ModuleAnalysisManager &AM) {
  using namespace llvm::ptsan;

  validatePTSanTarget(M);

  // Expose relative loads before visiting ordinary loads and stores.
  bool Modified = lowerLoadRelativeIntrinsics(M);
  if (Modified)
    AM.invalidate(M, PreservedAnalyses::none());
  Modified |= addNoBuiltinForPointerFns(M);
  SmallVector<Function *, 16> Functions;
  SmallPtrSet<GlobalValue *, 32> OriginalGlobals;
  for (Function &F : M) {
    OriginalGlobals.insert(&F);
    if (!F.empty() &&
        !F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation)) {
      Functions.push_back(&F);
    }
  }
  if (Functions.empty()) {
    return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
  for (GlobalVariable &GV : M.globals()) {
    OriginalGlobals.insert(&GV);
  }

  bool HadDebugCU = M.getNamedMetadata("llvm.dbg.cu") != nullptr;
  PointerTagSanitizer DS(M);
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (Function *F : Functions) {
    auto &TLI = FAM.getResult<TargetLibraryAnalysis>(*F);
    PTSanAnalysisState PTA(*F, DS, TLI);
    PTSanIDStripVisitor(PTA).runOnFunction();
    PTSanLateOptimizationPass().run(PTA);
    PTSanLoweringPass().run(PTA, FAM);
  }
  DS.finalize();
  if (!HadDebugCU && !ClCreateIRDbg) {
    if (NamedMDNode *CU = M.getNamedMetadata("llvm.dbg.cu")) {
      CU->eraseFromParent();
    }
  }

  // The shared context declares the full runtime and anchor vocabulary.
  // Strip-only modules retain only the declarations they actually use.
  for (Function &F : make_early_inc_range(M)) {
    if (!OriginalGlobals.contains(&F) && F.use_empty()) {
      F.eraseFromParent();
    }
  }
  for (GlobalVariable &GV : make_early_inc_range(M.globals())) {
    if (!OriginalGlobals.contains(&GV) && GV.use_empty()) {
      GV.eraseFromParent();
    }
  }
  return PreservedAnalyses::none();
}
