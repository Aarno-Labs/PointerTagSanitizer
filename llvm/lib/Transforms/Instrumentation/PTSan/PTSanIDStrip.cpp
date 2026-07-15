//===-- PTSanIDStrip.cpp - Strip out PTSan Ids without doing anything else-*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Regex.h"
#include "llvm/Transforms/Instrumentation/PTSanIDStrip.h"

#include <stdlib.h>

using namespace llvm;
#define DEBUG_TYPE "ptsan"

namespace llvm::ptsan {

static cl::opt<bool> ClClearCmps(
  "ptsan-strip-ids-on-cmp",
  cl::desc("Clear ids on cmps as well as loads"),
  cl::init(false)
);


namespace {
class PTSanIDStrip;

class PTSanIDStripVisitor : public InstVisitor<PTSanIDStripVisitor> {
public:
  Function &F;
  PTSanIDStrip &DS;
  const TargetLibraryInfo &TLI;
  PTSanIDStripVisitor(Function &F, PTSanIDStrip &DS,
               const TargetLibraryInfo &TLI): F(F), DS(DS), TLI(TLI) {};
  bool runOnFunction();
  void visitCallBase(CallBase &CB);
  void visitLoadInst(LoadInst &I);
  void visitStoreInst(StoreInst &I);
  void visitAtomicRMWInst(AtomicRMWInst &I);
  void visitAtomicCmpXchg(AtomicCmpXchgInst &I);
  void visitCmpInst(CmpInst &CI);
  void visitMemIntrinsic(MemIntrinsic &I);
  // Utils
  Value *stripID(Value *Ptr);
  void stripOperandID(Instruction &I, int idx);
  Instruction *getFirstIPAfter(Value *V);

  DenseMap<Value *, Value *> StrippedCache;
};

class PTSanIDStrip {
private:
  friend class PTSanIDStripVisitor;
  LLVMContext &Ctx;
  const DataLayout &DL;
  IntegerType *U32Ty;
  IntegerType *U64Ty;
  IntegerType *U8Ty;
  IntegerType *UptrTy;
  Type *PtrTy;

  Value *AddrMask;

public:
  PTSanIDStrip(Module &M);
  bool sanitizeFunction(Function &F, TargetLibraryInfo &TLI);
};

PTSanIDStrip::PTSanIDStrip(Module &M) : Ctx(M.getContext()),
                                        DL(M.getDataLayout()) {
  PtrTy = PointerType::getUnqual(Ctx);
  UptrTy = DL.getIntPtrType(Ctx);
  U32Ty = IntegerType::get(Ctx, 32);
  U64Ty = IntegerType::get(Ctx, 64);
  U8Ty = IntegerType::get(Ctx, 8);

  AddrMask = ConstantInt::get(U64Ty, 0x0000FFFFFFFFFFFFULL);
}
} // end namespace

bool PTSanIDStrip::sanitizeFunction(Function &F, TargetLibraryInfo &TLI) {
  if (F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation)) {
    return false;
  }

  PTSanIDStripVisitor V(F, *this, TLI);
  return V.runOnFunction();
}

Instruction *PTSanIDStripVisitor::getFirstIPAfter(Value *V) {
  if (PHINode *PHI = dyn_cast<PHINode>(V)) {
    return &*PHI->getParent()->getFirstInsertionPt();
  } else if (InvokeInst *II = dyn_cast<InvokeInst>(V)) {
    return &*II->getNormalDest()->getFirstInsertionPt();
  } else if (Instruction *I = dyn_cast<Instruction>(V)) {
    assert(!I->isTerminator());
    return I->getNextNode();
  } else {
    return &*F.getEntryBlock().getFirstInsertionPt();
  }
}

Value *PTSanIDStripVisitor::stripID(Value *Ptr) {
  if (StrippedCache.contains(Ptr)) {
    return StrippedCache.at(Ptr);
  }

  if(!Ptr->getType()->isPointerTy()) {
    errs() << "Can't strip ID of non-ptr: " << Ptr << "\n";
    exit(EXIT_FAILURE);
  }

  if (isa<Constant>(Ptr)) {
    // Don't strip the tops of constants. -1 is a ptr constant that does exist
    return Ptr;
  }

  Function *PMFn =
      Intrinsic::getDeclaration(F.getParent(), Intrinsic::ptrmask, {DS.PtrTy, DS.U64Ty});

  IRBuilder<> IRB(getFirstIPAfter(Ptr));
  Value *True = IRB.CreateCall(PMFn, {Ptr, DS.AddrMask}, Ptr->getName() + ".true");
  StrippedCache[Ptr] = True;
  return True;
}

void PTSanIDStripVisitor::stripOperandID(Instruction &I, int idx) {
  I.setOperand(idx, stripID(I.getOperand(idx)));
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

  if (auto *VTy = dyn_cast<VectorType>(Ty)) {
    if(VTy->getElementType()->isPointerTy()) {
    errs() << "Handle ptr vector cmps like this one " << CI << "\n";
    exit(EXIT_FAILURE);
    }
  }

  if (Ty->isPointerTy()) {
    errs() << "Clearing ids on cmp " << CI << "\n";
    stripOperandID(CI, 0);
    stripOperandID(CI, 1);
  }
}

void PTSanIDStripVisitor::visitAtomicRMWInst(AtomicRMWInst &I) {
  stripOperandID(I, 0);
}

void PTSanIDStripVisitor::visitAtomicCmpXchg(AtomicCmpXchgInst &I) {
  stripOperandID(I, 0);
}

void PTSanIDStripVisitor::visitStoreInst(StoreInst &I) {
  stripOperandID(I, 1);
}

void PTSanIDStripVisitor::visitLoadInst(LoadInst &I) {
  stripOperandID(I, 0);
}


bool PTSanIDStripVisitor::runOnFunction() {
  // Iterate all BBs in depth-first order and mark instructions for visiting.
  // This is much easier than trying to modify the program while we visit it
  SmallVector<Instruction *, 16> Instructions;
  for (BasicBlock *BB : depth_first(&F.getEntryBlock())) {
    for (Instruction &I : *BB) {
      Instructions.push_back(&I);
    }
  }

  if (Instructions.size() == 0) { return false; }

  for (Instruction *I : Instructions) {
    this->visit(I);
  }

  // XXX: Re-optimize function? Might save a
  // bit of perf?
  return true;
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
  auto *Callee = CB.getCalledFunction();
  if (!Callee || !Callee->isIntrinsic()) { return; }
  if(callOnPtsanInternals(CB)) { return; }

  // Gotta strip IDs from intrinsics' ptr args
  switch(Callee->getIntrinsicID()){
    // XXX: maybe these should point back to the original object?
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
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
    case Intrinsic::memmove:
      stripOperandID(I, 0);
      stripOperandID(I, 1);
      break;
    case Intrinsic::memset:
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

PreservedAnalyses PTSanIDStripPass::run(Module &M,
                                          ModuleAnalysisManager &AM) {
  using namespace llvm::ptsan;
  errs() << "Running ptsan strip!\n";

  bool Modified = false;
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (Function &F : M) {
    if (F.empty())
      continue;

    PTSanIDStrip Stripper(*F.getParent());
    Modified |=
        Stripper.sanitizeFunction(F, FAM.getResult<TargetLibraryAnalysis>(F));

  }

  if(!Modified) {
    return PreservedAnalyses::all();
  }

  // XXX: Probably most analyses are preserved, but I don't want to mess with this
  PreservedAnalyses PA = PreservedAnalyses::none();
  return PA;
}
