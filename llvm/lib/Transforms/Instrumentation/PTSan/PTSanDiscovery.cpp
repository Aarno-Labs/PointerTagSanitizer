//===-- PTSanDiscovery.cpp - PTSan discovery phase -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanDiscoveryInternal.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Transforms/Utils/EscapeEnumerator.h"

namespace llvm::ptsan {


static bool shouldClearIDForUse(const PtrOp &Op);

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

void PTSanDiscoverer::discoverVarargCall(PointerTagSanitizer &DS, CallBase &CB) {
  // Variadic intrinsics (e.g. stackmap) do not use the C varargs ABI.
  if (!CB.getFunctionType()->isVarArg() ||
      CB.getIntrinsicID() != Intrinsic::not_intrinsic) {
    return;
  }
  // The sentinel uses C ABI stack placement and cannot be appended to
  // musttail's forwarded argument list.
  if (CB.getCallingConv() != CallingConv::C || CB.isMustTailCall()) {
    report_fatal_error(
        "PTSan varargs require the C calling convention without musttail");
  }
  VarargCallAnchor::create(DS, &CB);
}

StripAnchor PTSanDiscoverer::createStripWithDependencies(
    PTSanAnalysisState &PTA, Instruction *IP, Value *Ptr, Value *Mask) {
  PointerTagSanitizer &DS = PTA.DS;
  // ID16 strips are just masks; ID32 strips also need the allocation's decode base.
  if (Cl32BitIDs) {
    Value *Base = Ptr;
    if (Ptr->getType()->isPtrOrPtrVectorTy()) {
      Base = findPTSanBasePointer(Ptr, PTA.PTU);
    }
    Value *ID = GetIDAnchor::create(DS, IP, Base).Call;
    Value *BoundsMask = ID->getType()->isVectorTy() ? Mask : nullptr;
    auto Bias = LoadBoundAnchor::createBase(DS, IP, ID, BoundsMask);
    return ID32StripAnchor::create(DS, IP, Ptr, ID, Bias);
  } else {
    return ID16StripAnchor::create(DS, IP, Ptr);
  }
}

void PTSanDiscoverer::addPtrSink(const PtrOp &Op) {
  Instruction *UserI = cast<Instruction>(Op.U->getUser());
  Value *Ptr = Op.U->get();

  if (shouldClearIDForUse(Op) && !DS.getAsAnchor<StripAnchor>(Ptr)) {
    Value *Mask = Cl32BitIDs && Ptr->getType()->isVectorTy() ? Op.Mask : nullptr;
    StripAnchor Strip = createStripWithDependencies(PTA, UserI, Ptr, Mask);
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
  auto BaseBound = LoadBoundAnchor::createBase(DS, UserI, ID.Call, BoundsMask);
  auto SpanBound = LoadBoundAnchor::createSpan(DS, UserI, ID.Call, BoundsMask);
  optional<LoadBoundAnchor> Origin;
  if (Cl32BitIDs) {
    Origin = LoadBoundAnchor::createOrigin(DS, UserI, ID.Call, BoundsMask);
  }
  AccessAnchor::create(DS, UserI, Op, ID, BaseBound, SpanBound, Origin);
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

  SourceAnchor::create(DS, &*F.getEntryBlock().getFirstInsertionPt(), ArgV);
}

void PTSanDiscoverer::run() {
  assert(PTA.PlaceablePtrChecks.empty());

  handleMainIfMain();

  // A byval parameter points at an ABI-created copy, not the caller's object.
  // Treat that copy like a stack allocation so it receives fresh bounds and a
  // fresh ID for this invocation.
  for (Argument &Arg : F.args()) {
    if (Arg.hasByValAttr()) {
      SourceAnchor::create(DS, &*F.getEntryBlock().getFirstInsertionPt(), &Arg);
    }
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

  SourceAnchor::create(DS, I.getNextNode(), &I);
}

void PTSanDiscoverer::discoverFunctionExits() {
  // TLS sources do not allocate frame-owned IDs. Other sources may need
  // cleanup, although optimization can later remove their ID requests.
  bool HasStackSources = llvm::any_of(
      DS.collectAnchors<SourceAnchor>(F), [](SourceAnchor Source) {
        return Source.needsFrameID();
      });

  bool HasVAStart = !DS.collectAnchors<VAStartAnchor>(F).empty();

  // Finish visiting calls before replacing them with invokes. The conversion
  // preserves target metadata, so anchors still identify their instructions.
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

void PTSanDiscoverer::visitCmpInst(CmpInst &CI) {
  Type *Ty = CI.getOperand(0)->getType();
  assert(Ty == CI.getOperand(1)->getType());

  Value *Op0 = CI.getOperand(0);
  Value *Op1 = CI.getOperand(1);
  // Strip before numeric casts lose bits or change the interpretation of an ID.
  auto findPtrToInt = [](Value *V) -> CastInst * {
    while (auto *Cast = dyn_cast<CastInst>(V)) {
      if (CastInst *PtrToInt = getPtrToInt(Cast)) {
        return PtrToInt;
      }
      V = Cast->getOperand(0);
    }
    return nullptr;
  };
  // Stripping ids off of cmps is a bit sus, since sometimes (mmap failure
  // which returns -1), for example, we do actually want the orignal value.
  // What seems to work *pretty* reliably is to only strip pointer ids
  // if both of them aren't provably constant.
  if (isUntaggedConstantPtr(Op0) || isUntaggedConstantPtr(Op1)) {
    // LLVM also folds integer sign tests into pointer comparisons against -1.
    // Keep those tests consistent with the pointer bits used by integer code.
    return;
  }

  if (Ty->isPtrOrPtrVectorTy()) {
    addPtrSink(PtrOp(&CI, 0));
    addPtrSink(PtrOp(&CI, 1));
  }

  CastInst *PtrToInt0 = findPtrToInt(Op0);
  CastInst *PtrToInt1 = findPtrToInt(Op1);
  if (!(Ty->isPtrOrPtrVectorTy() || (PtrToInt0 && PtrToInt1))) {
    return;
  }

  // Validate the pointers, not potentially narrowed or floating-point casts.
  // Capture them before inserting strips for the comparison.
  if (ClValidateArith && ClValidate && !Ty->isVectorTy()) {
    ValidateArithAnchor::create(
        DS, &CI, PtrToInt0 ? PtrToInt0->getOperand(0) : Op0,
        PtrToInt1 ? PtrToInt1->getOperand(0) : Op1);
  }

  if (PTA.ClearIDsOnCmp) {
    if (Ty->isPtrOrPtrVectorTy()) {
      addPtrSink(PtrOp(&CI, 0));
      addPtrSink(PtrOp(&CI, 1));
    } else {
      addPtrSink(PtrOp(PtrToInt0, 0));
      addPtrSink(PtrOp(PtrToInt1, 0));
    }
  }

}

static bool shouldStripPointerBitTwiddle(unsigned Opcode,
                                         const ConstantInt &Mask) {
  if (Opcode == Instruction::Xor && Mask.isMinusOne()) {
    // LLVM represents X - Y - 1 as X + ~Y. Keep Y tagged so matching pointer
    // IDs still cancel in the surrounding arithmetic.
    return false;
  }

  APInt TouchedBits = Mask.getValue();
  if (Opcode == Instruction::And) {
    TouchedBits.flipAllBits();
  }

  // Keep page-offset bits available for alignment and pointer flags. Masks
  // that reach beyond them are observations of the numeric address.
  unsigned PointerFlagBits = ClStripPointerBitTwiddles;
  return TouchedBits.getActiveBits() > PointerFlagBits;
}

void PTSanDiscoverer::visitBinaryOperator(BinaryOperator &I) {
  if (!I.getType()->isIntegerTy()) {
    return;
  }

  Value *Op0 = I.getOperand(0);
  Value *Op1 = I.getOperand(1);

  // If we're dealing with c++, we can't garantee that pointers
  // will have the same ID -- one could be tagged and one
  // could be untagged
  bool IsPointerSubtraction = I.getOpcode() == Instruction::Sub &&
                              getPtrToInt(Op0) && getPtrToInt(Op1);

  if (IsPointerSubtraction && ClValidateArith && ClValidate) {
    ValidateArithAnchor::create(DS, &I, Op0, Op1);
  }

  if (IsPointerSubtraction) {
    // Ptrtoint stripping already makes both operands raw addresses.
    if (PTA.ClearIDsOnArith && !PTA.StripPtrToInt) {
      DBG(errs() << "Stripping ids for pointer subtraction " << I << "\n");
      addPtrSink(PtrOp(&I, 0));
      addPtrSink(PtrOp(&I, 1));
    }
    return;
  }

  if (ClStripPointerBitTwiddles < 0) {
    return;
  }

  if (I.getOpcode() == Instruction::AShr ||
      I.getOpcode() == Instruction::LShr) {
    if (getPtrToInt(Op0)) {
      StripAnchor Strip = createStripWithDependencies(PTA, &I, Op0);
      I.setOperand(0, Strip.Call);
    }
  } else if (I.isBitwiseLogicOp()) {
    // Some programs (gcc!) use the high bits of pointers as
    // hash keys. We should preserve their operation
    if (getPtrToInt(Op0)) {
      auto *Mask = dyn_cast<ConstantInt>(Op1);
      if (Mask && shouldStripPointerBitTwiddle(I.getOpcode(), *Mask)) {
        StripAnchor Strip = createStripWithDependencies(PTA, &I, Op0);
        I.setOperand(0, Strip.Call);
      }
    } else if (getPtrToInt(Op1)) {
      auto *Mask = dyn_cast<ConstantInt>(Op0);
      if (Mask && shouldStripPointerBitTwiddle(I.getOpcode(), *Mask)) {
        StripAnchor Strip = createStripWithDependencies(PTA, &I, Op1);
        I.setOperand(1, Strip.Call);
      }
    }
  }
}

void PTSanDiscoverer::visitPtrToIntInst(PtrToIntInst &I) {
  if (PTA.StripPtrToInt) {
    addPtrSink(PtrOp(&I, 0));
  }
}

void PTSanDiscoverer::visitPtrToAddrInst(PtrToAddrInst &I) {
  if (PTA.StripPtrToInt) {
    addPtrSink(PtrOp(&I, 0));
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
        getPtrToInt(CB.getOperand(i))) {
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

  discoverVarargCall(DS, CB);

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
    auto *Asm = cast<InlineAsm>(CB.getCalledOperand());
    auto Constraints = Asm->ParseConstraints();
    unsigned ArgIdx = 0;
    for (const auto &Constraint : Constraints) {
      if (!Constraint.hasArg()) {
        continue;
      }
      unsigned Operand = ArgIdx++;
      // Empty asm does not consume direct operands as addresses. Indirect
      // memory operands still need raw addresses, even with an empty template.
      if (Asm->getAsmString().empty() && !Constraint.isIndirect) {
        continue;
      }
      llvm::Value *Arg = CB.getArgOperand(Operand);
      if (Arg->getType()->isPtrOrPtrVectorTy() || getPtrToInt(Arg)) {
        DBG(errs() << "\tStripping ptr from arg " << Operand << "\n");
        addPtrSink(PtrOp(&CB, Operand));
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
      VAStartAnchor::create(DS, II);
      [[fallthrough]];
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
        SourceAnchor::create(DS, CB.getNextNode(), &CB);
      }
      return;
    // Handled in FindMemAccess
    case Intrinsic::masked_load:
    case Intrinsic::masked_store:
    case Intrinsic::masked_gather:
    case Intrinsic::masked_scatter:
    case Intrinsic::x86_sse3_ldu_dq:
    case Intrinsic::x86_avx_ldu_dq_256:
    case Intrinsic::x86_sse_ldmxcsr:
    case Intrinsic::x86_sse_stmxcsr:
    case Intrinsic::x86_sse2_clflush:
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
