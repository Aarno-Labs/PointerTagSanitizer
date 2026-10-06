//===-- PTSanDiscovery.cpp - PTSan discovery phase -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Transforms/Utils/EscapeEnumerator.h"

namespace llvm::ptsan {


static bool shouldClearIDForUse(const PtrOp &Op);

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
  // Visitors
  void visitAllocaInst(AllocaInst &I);
  void visitCallBase(CallBase &CB);
  void visitGetElementPtrInst(GetElementPtrInst &I);
  void visitCmpInst(CmpInst &CI);
  void visitBinaryOperator(BinaryOperator &I);

  void handleMainIfMain();
};

bool PTSanDiscoveryPass::run(PTSanAnalysisState &PTA) {
  DBG(errs() << "\n\n-----------------------------------------\n");
  INFO(errs() << "Discovering PTSan ops in " << PTA.F.getName() << "\n");
  DBG(errs() << "-----------------------------------------\n");

  PTSanDiscoverer Discoverer(PTA);
  Discoverer.run();
  return true;
}

static bool isProgramMain(Function &F) {
  if (F.isDeclaration()) {
    return false;
  }

  if (F.getName() != "main") {
    return false;
  }

  if (!F.getReturnType()->isIntegerTy()) {
    return false;
  }

  unsigned N = F.arg_size();
  if (N != 0 && N != 2 && N != 3) {
    return false;
  }

  return true;
}

void PTSanDiscoverer::addPtrSink(const PtrOp &Op) {
  Instruction *UserI = cast<Instruction>(Op.U->getUser());
  Value *Ptr = Op.U->get();

  if (shouldClearIDForUse(Op) && !DS.getAsAnchor<StripAnchor>(Ptr)) {
    StripAnchor Strip = StripAnchor::create(DS, UserI, Ptr);
    Op.U->set(Strip.Call);
  }

  if (Op.CheckTy == NO_CHECK || Op.Size == nullptr ||
      isKnownZeroAccessSize(Op.Size)) {
    return;
  }

  Ptr = BoundedPtrCheck::getUnstrippedPtrFromUse(Op.U, DS);
  Value *Base = findPTSanBasePointer(Ptr, PTA.PTU);
  auto ID = GetIDAnchor::create(DS, UserI, Base);
  Value *BoundsMask = ID.Call->getType()->isVectorTy() ? Op.Mask : nullptr;
  auto BaseBound = LoadBoundAnchor::createBase(DS, UserI, ID, BoundsMask);
  auto SpanBound = LoadBoundAnchor::createSpan(DS, UserI, ID, BoundsMask);
  AccessAnchor::create(DS, UserI, Op, ID, BaseBound, SpanBound);
}

void PTSanDiscoverer::handleMainIfMain() {
  if (!isProgramMain(F)) {
    return;
  }

  if (F.arg_size() == 0) {
    return;
  }

  Value *ArgC = F.getArg(0);
  Value *ArgV = F.getArg(1);

  assert(ArgC->getType() == DS.U32Ty);
  assert(ArgV->getType() == DS.PtrTy);

  SourceAnchor::create(DS, PTA.PTU, ArgV);
}

void PTSanDiscoverer::run() {
  assert(PTA.PlaceablePtrChecks.empty());

  handleMainIfMain();

  // A byval parameter points at an ABI-created copy, not the caller's object.
  // Treat that copy like a stack allocation so it receives fresh bounds and a
  // fresh ID for this invocation.
  for (Argument &Arg : F.args()) {
    if (Arg.hasByValAttr())
      SourceAnchor::create(DS, PTA.PTU, &Arg);
  }

  SmallVector<PtrOp, 4> MemOps;
  FindMemAccesses FMA(DS.DL, MemOps);
  for (BasicBlock *BB : depth_first(&F.getEntryBlock())) {
    for (Instruction &I : *BB) {
      if (DS.isAnchor(&I)) {
        continue;
      }

      this->visit(I);
      MemOps.clear();
      FMA.visit(I);
      for (const PtrOp &Op : MemOps) {
        addPtrSink(Op);
      }
    }
  }

  discoverFunctionExits();

  DBG(
    assert(DT.verify());
    assert(PDT.verify());
    LI.verify(DT);
  );

}

static bool shouldClearIDForUse(const PtrOp &Op) {
  if (!ClLAM)
    return true;

  Instruction *I = cast<Instruction>(Op.U->getUser());
  if (isa<LoadInst>(I) && Op.U->getOperandNo() == 0)
    return false;
  if (isa<StoreInst>(I) && Op.U->getOperandNo() == 1)
    return false;
  if (isa<AtomicRMWInst>(I) && Op.U->getOperandNo() == 0)
    return false;
  if (isa<AtomicCmpXchgInst>(I) && Op.U->getOperandNo() == 0)
    return false;

  return true;
}


void PTSanDiscoverer::visitAllocaInst(AllocaInst &I) {
  //DBG(errs() << "alloca: " << I << "\n");
  auto Size = I.getAllocationSize(DS.DL);
  // Size > 2^32 causes problems with packing sizes later. Variable-sized
  // allocas initialize their bounds separately, without packing the size.
  assert((!Size || *Size < 0xFFFFFFFFULL) &&
         "PTSan source alloca is too large");

  SourceAnchor::create(DS, PTA.PTU, &I);
}

void PTSanDiscoverer::discoverFunctionExits() {
  // TLS sources do not allocate frame-owned IDs. Other sources may need
  // cleanup, although optimization can later remove their ID requests.
  bool HasStackSources = llvm::any_of(
      DS.collectAnchors<SourceAnchor>(F), [](SourceAnchor Source) {
        return Source.needsFrameID();
      });

  bool HasVAStart = llvm::any_of(instructions(F), [](Instruction &I) {
    auto *II = dyn_cast<IntrinsicInst>(&I);
    return II && II->getIntrinsicID() == Intrinsic::vastart;
  });

  // Finish visiting calls before replacing them with invokes. Anchors remain
  // before their instructions; later phases see the final call/exit topology.
  // Always enumerate existing returns/resumes; create unwind paths for
  // throwing calls only when frame-owned IDs may need cleanup.
  EscapeEnumerator EE(F, "ptsan.cleanup",
                      /*HandleExceptions=*/HasStackSources || HasVAStart,
                      &PTA.DTU);
  while (IRBuilder<> *AtExit = EE.Next()) {
    FunctionExitAnchor::create(DS, &*AtExit->GetInsertPoint());
  }

  // EscapeEnumerator updates dominance but not LoopInfo.
  LI.releaseMemory();
  LI.analyze(DT);
}

void PTSanDiscoverer::visitGetElementPtrInst(GetElementPtrInst &I) {
  //DBG(errs() << "GEP " << I << "\n");
  // *None* of our pointers are inbounds
  // Might want to do this in the post-analysis pass, but I
  // don't think this will mess with our analyses, so it's probably fine
  // I.setIsInBounds(false);
}

// If we're dealing with c++, we can't garantee that pointers
// will have the same ID -- one could be tagged and one
// could be untagged
void PTSanDiscoverer::visitCmpInst(CmpInst &CI) {
  Type *Ty = CI.getOperand(0)->getType();
  assert(Ty == CI.getOperand(1)->getType());

  Value *Op0 = CI.getOperand(0);
  Value *Op1 = CI.getOperand(1);
  // Stripping ids off of cmps is a bit sus, since sometimes (mmap failure
  // which returns -1), for example, we do actually want the orignal value.
  // What seems to work *pretty* reliably is to only strip pointer ids
  // if both of them aren't provably constant.
  if(isUntaggedConstantPtr(Op0) ||
     isUntaggedConstantPtr(Op1)) {
    return;
  }

  if (Ty->isPtrOrPtrVectorTy()) {
    addPtrSink(PtrOp(&CI, 0));
    addPtrSink(PtrOp(&CI, 1));
  }

  if (!(Ty->isPtrOrPtrVectorTy() ||
        (isa<PtrToIntInst>(Op0) && isa<PtrToIntInst>(Op1)))) {
    return;
  }

  if (ClClearIdsOnCmp) {
    addPtrSink(PtrOp(&CI, 0));
    addPtrSink(PtrOp(&CI, 1));
  }

  // The runtime arithmetic validator accepts scalar pointer values. Vector
  // comparisons are still stripped above, but cannot use that scalar-only
  // diagnostic interface.
  if (ClValidateArith && ClValidate && !Ty->isVectorTy()) {
    ValidateArithAnchor::create(DS, &CI, Op0, Op1);
  }
}

// If we're dealing with c++, we can't garantee that pointers
// will have the same ID -- one could be tagged and one
// could be untagged
void PTSanDiscoverer::visitBinaryOperator(BinaryOperator &I) {
  if (I.getOpcode() != Instruction::Sub ||
      !I.getType()->isIntegerTy()) {
    return;
  }

  Value *Op0 = I.getOperand(0);
  Value *Op1 = I.getOperand(1);
  // Ptr-ptr subtraction is represented in IR as int-int
  assert(!(Op0->getType()->isPointerTy() &&
           Op0->getType()->isPointerTy()));

  if (!isa<PtrToIntInst>(Op0) || !isa<PtrToIntInst>(Op1)) {
    return;
  }

  // Strip ids before subtracting pointers
  if (ClClearIdsOnArith) {
    DBG(errs() << "Stripping ids for pointer subtraction " << I << "\n");
    addPtrSink(PtrOp(&I, 0));
    addPtrSink(PtrOp(&I, 1));
  }

  if (ClValidateArith && ClValidate) {
    ValidateArithAnchor::create(DS, &I, Op0, Op1);
  }
}

static bool callHasPointerArg(llvm::CallBase &CB) {
  for (unsigned i = 0; i < CB.arg_size(); ++i) {
    Value *Arg = CB.getArgOperand(i);
    if (Arg->getType()->isPtrOrPtrVectorTy()) {
      return true;
    }
  }
  // No arguments were pointers
  return false;
}

static bool callReturnsPtr(llvm::CallBase &CB) {
  return CB.getType()->isPointerTy();
}

void PTSanDiscoverer::IRStripFn(CallBase &CB) {
  static const SmallSet<StringRef, 16> StripList = []() {
    SmallSet<StringRef, 16> S;
    S.insert("__cxa_guard_abort");
    S.insert("dlsym");
    S.insert("setcontext");
    S.insert("getcontext");
    S.insert("swapcontext");
    S.insert("__unw_getcontext");
    S.insert("unw_getcontext");
    S.insert("__libunwind_Registers_x86_64_jumpto");
    S.insert("__libunwind_Registers_arm64_jumpto");
    S.insert("__kmp_invoke_microtask");
    return S;
  }();

  Function *Callee = CB.getCalledFunction();

  if (!Callee || !StripList.contains(Callee->getName())) {
    return;
  }

  DBG(errs() << "Found " << Callee->getName() << ". Stripping ID\n");
  for (unsigned i = 0; i < CB.arg_size(); i++) {
    if (CB.getOperand(i)->getType()->isPtrOrPtrVectorTy() ||
        isa<PtrToIntInst>(CB.getOperand(i))) {
      addPtrSink(PtrOp(&CB, i));
    }
  }
}

void PTSanDiscoverer::IRStripSpecialFn(CallBase &CB) {
  // These are handled in ptsan_ir_interceptor_helpers with a _before and _after
  // method
  static const SmallSet<StringRef, 8> SpecialStripList = []() {
    SmallSet<StringRef, 8> S;
    S.insert("makecontext");
    return S;
  }();

  Function *Callee = CB.getCalledFunction();

  if (!Callee || !SpecialStripList.contains(Callee->getName())) {
    return;

  }

  DBG(errs() << "Found " << Callee->getName()
              << ". Handling special strip\n");

  assert(!CB.isTerminator() && "Need to support invokes here :(");
  SpecialStripAnchor::create(DS, &CB);

  // The helpers strip internals, but we have to actually strip
  // base arguments as well
  for (unsigned i = 0; i < CB.arg_size(); i++) {
    if (CB.getOperand(i)->getType()->isPointerTy()) {
      addPtrSink(PtrOp(&CB, i));
    }
  }
}


void PTSanDiscoverer::visitCallBase(CallBase &CB) {
  SmallVector<PtrOp, 2> PtrOps;

  if (DS.isAnchor(&CB)) {
    return;
  }

  // We need to strip ID on any pass-by-value pointer, since
  // it devolves to a memcpy. I (think) we can assume the bounds
  // are right
  // XXX: Ideally we'd re-assign an ID in the caller/do checking, but
  // that's a *later* problem
  bool FoundPassByValue = false;
  for (unsigned ArgIdx = 0; ArgIdx < CB.arg_size(); ++ArgIdx) {
    if (CB.paramHasAttr(ArgIdx, llvm::Attribute::ByVal)) {

      llvm::Value *Arg = CB.getArgOperand(ArgIdx);
      DBG(errs() << "Argument #" << ArgIdx
                 << " is passed by value (" << *Arg << ")\n");

      assert(Arg->getType()->isPointerTy());
      addPtrSink(PtrOp(&CB, ArgIdx));
      FoundPassByValue = true;
    }
  }

  auto *Callee = CB.getCalledFunction();

  if (FoundPassByValue) {
    // I don't think there are any pass-by-value intrinsics
    assert(!Callee || !Callee->isIntrinsic());
    return;
  }

  assert(PtrOps.empty());
  if (CB.isInlineAsm()) {
    DBG(errs() << "Found inline ASM " << CB << "\n");
    for (unsigned ArgIdx = 0; ArgIdx < CB.arg_size(); ++ArgIdx) {
      llvm::Value *Arg = CB.getArgOperand(ArgIdx);
      if (Arg->getType()->isPtrOrPtrVectorTy() || isa<PtrToIntInst>(Arg)) {
        DBG(errs() << "\tStripping ptr from arg " << ArgIdx << "\n");
        addPtrSink(PtrOp(&CB, ArgIdx));
      }
    }

    return;
  }

  if (!Callee) {
    return;
  }

  StringRef Name = Callee->getName();
  if (Name == "setjmp" || Name == "_setjmp" || Name == "__setjmp" ||
      Name == "sigsetjmp" || Name == "__sigsetjmp") {
    addPtrSink(PtrOp(&CB, 0));
    SetjmpAnchor::create(DS, &CB);
    return;
  }

  IRStripFn(CB);
  IRStripSpecialFn(CB);

  if (Callee->getName() == "dlsym") {
    DBG(errs() << "Found dlsym. Marking as ptsan_in_libc\n");
    LibcCallAnchor::create(DS, &CB);
  }


  if (!Callee->isIntrinsic()) {
    return;
  }

  IntrinsicInst *II = cast<IntrinsicInst>(&CB);

  // XXX: Should I modify these to point back to the original?
  if (II->isAssumeLikeIntrinsic() ||
      isDbgInfoIntrinsic(II->getIntrinsicID())) {
    return;
  }

  // Gotta strip IDs from intrinsics' ptr args
  switch(Callee->getIntrinsicID()){
    case Intrinsic::stacksave:
    case Intrinsic::stackrestore:
    case Intrinsic::prefetch:
    case Intrinsic::returnaddress:
    case Intrinsic::addressofreturnaddress:
    case Intrinsic::sponentry:
    case Intrinsic::frameaddress:
    case Intrinsic::thread_pointer:
      return;
    case Intrinsic::eh_typeid_for:
      // Code generation needs the type-info symbol, not a tagged proxy load.
      // Record raw demand here; stripping the global folds back to the symbol.
      addPtrSink(PtrOp(&CB, 0));
      return;
    case Intrinsic::vastart:
    case Intrinsic::vaend:
    case Intrinsic::vacopy:
      for (unsigned ArgIdx = 0; ArgIdx < CB.arg_size(); ++ArgIdx) {
        if (CB.getArgOperand(ArgIdx)->getType()->isPtrOrPtrVectorTy()) {
          addPtrSink(PtrOp(&CB, ArgIdx));
        }
      }
      return;
    // Ptrmask is just arithmetic, so we don't have to do anything to it,
    // If we run into an instruction which is masking any of the top 16
    // bits, we're in trooooooble
    // XXX: check non-const version maybe?
    case Intrinsic::ptrmask:
      if (auto *ConstMask = dyn_cast<ConstantInt>(CB.getOperand(1))) {
        uint64_t Mask = ConstMask->getZExtValue();
        constexpr uint64_t TOP_16_MASK = 0xFFFF000000000000ULL;
        if((Mask & TOP_16_MASK) != TOP_16_MASK) {
          errs() << "ptrmask touches top 16 bits: " << CB << "\n";

          exit(EXIT_FAILURE);
        }
      }
      return;
    // Unary ops we want to strip:
    case Intrinsic::threadlocal_address:
      // LLVM requires the operand to remain a literal GlobalValue. It is
      // therefore inherently raw rather than an ordinary tagged pointer use, which
      // also means we can't use a StripAnchor here; we have to special-case it in
      // PTSanLateOptimization
      // Preserve the raw per-thread address. A tagged use requests its TLS ID.
      if (DS.TLSIDs.contains(cast<GlobalValue>(CB.getArgOperand(0)))) {
        SourceAnchor::create(DS, PTA.PTU, &CB);
      }
      return;
    // Handled in FindMemAccess
    case Intrinsic::masked_load:
    case Intrinsic::masked_store:
    case Intrinsic::masked_gather:
    case Intrinsic::masked_scatter:
    case Intrinsic::x86_sse_ldmxcsr:
    case Intrinsic::x86_sse_stmxcsr:
    case Intrinsic::x86_umonitor:
      return;
    default:
      // XXX: Remove -mno-avx2 from spec and support vector gathers
      if (callHasPointerArg(CB) ||
          callReturnsPtr(CB)) {

        if (Callee->getName().starts_with("llvm.eh")) {
          return;
        }

        if (isa<MemIntrinsic>(CB)) {
          return; // Handled in FindMemAccess
        }
        errs() << "Unhandled pointer-manipulating intrinsic: " << CB << "\n";
        errs() << "Intrinsic ID is " << CB.getIntrinsicID() << "\n";
        die();
      }
  }
}

} // namespace llvm::ptsan
