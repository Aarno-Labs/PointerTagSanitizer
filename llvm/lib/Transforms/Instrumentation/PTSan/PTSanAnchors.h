//===-- PTSanAnchors.h - Private PTSan anchor views ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "PTSanInternal.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"

namespace llvm::ptsan {

class Anchor {
public:
  PointerTagSanitizer &DS;
  CallInst *Call;

  Anchor(PointerTagSanitizer &DS, CallInst *Call) : DS(DS), Call(Call) {}
  Anchor(const Anchor &) = default;
  Anchor(Anchor &&) = default;

  Anchor &operator=(const Anchor &Other) {
    Call = Other.Call;
    return *this;
  }

  static Anchor create(PointerTagSanitizer &DS, FunctionCallee Callee,
                       Instruction *IP, ArrayRef<Value *> Args = {}) {
    PTSanIRB IRB(DS, IP);
    CallInst *CI = IRB.CreateCall(Callee, Args);
    CI->setDebugLoc(IP->getDebugLoc());
    return Anchor(DS, CI);
  }

  // Create a polymorphic compiler-only anchor call. Derive its parameter
  // types from the actual operands while retaining AnchorKind's canonical
  // callee operand, which is how PTSan recognizes the anchor later.
  static Anchor create(PointerTagSanitizer &DS, FunctionCallee AnchorKind,
                       Type *ReturnTy, Instruction *IP,
                       ArrayRef<Value *> Args = {}) {
    SmallVector<Type *> ParamTys;
    for (Value *Arg : Args) {
      ParamTys.push_back(Arg->getType());
    }

    FunctionType *CallTy =
        FunctionType::get(ReturnTy, ParamTys, /*isVarArg=*/false);
    return create(DS, FunctionCallee(CallTy, AnchorKind.getCallee()), IP, Args);
  }

  bool isCallTo(FunctionCallee Expected) const {
    return Call->getCalledOperand() == Expected.getCallee();
  }

  void erase() const {
    DS.unmarkAsInstr(Call);
    Call->eraseFromParent();
  }
};

// Loop preanalysis associates three pieces of IR using one numeric ID per
// original loop:
//
//   !bounds = !{!"ptsan.loop.bounds", i1 <complete>, !base0, !base1, ...}
//
//   %access = load ..., !ptsan.loop.ids !{i64 <loop-id>, ...}
//
//   call void @llvm.sideeffect()
//       [ "ptsan.loop.preheader"(i64 <loop-id>),
//         "deopt"(<capture0>, <capture1>, ...) ],
//       !ptsan.loop.bounds !bounds
//
// LoopPreheaderAnchor is the typed view over that relationship. Loop
// transformations clone the marker and remap its captured SSA operands before
// PTSan consumes the surviving calls as anchors. Raw bounds-node construction
// and range serialization remain in SerializeBoundsInfo/DeserializeBoundsInfo.
class LoopPreheaderAnchor : public Anchor {
  static constexpr StringLiteral BundleName = "ptsan.loop.preheader";

public:
  LoopPreheaderAnchor(Anchor A) : Anchor(A) {}

  static bool isMarker(const Instruction &I) {
    auto *Intrinsic = dyn_cast<IntrinsicInst>(&I);
    return Intrinsic && Intrinsic->getIntrinsicID() == Intrinsic::sideeffect;
  }

  static bool classof(const Anchor *Anchor) {
    return isMarker(*Anchor->Call);
  }

  // Loop-info gathering runs before PointerTagSanitizer exists, so creation
  // emits the durable marker directly. PTSan wraps surviving copies in this
  // anchor view after loop optimization.
  static CallInst *create(Loop &L, uint64_t ID, MDNode &SerializedBounds,
                          ArrayRef<Value *> Captures) {
    assert(L.getLoopPreheader() && "preanalyzed loop requires a preheader");
    Module *M = L.getHeader()->getModule();
    Instruction *InsertBefore = L.getLoopPreheader()->getTerminator();
    Function *SideEffect = Intrinsic::getDeclaration(M, Intrinsic::sideeffect);
    Value *IDValue = ConstantInt::get(Type::getInt64Ty(M->getContext()), ID);
    SmallVector<Value *, 1> MarkerInputs{IDValue};
    OperandBundleDef MarkerBundle(BundleName.str(), MarkerInputs);
    // Keep captured SSA values in a deopt bundle so LLVM treats pointer
    // captures as nonescaping and remaps every capture when cloning the marker.
    // PTSan never deoptimizes through this call.
    OperandBundleDef CaptureBundle("deopt", Captures);
    CallInst *Marker = CallInst::Create(
        SideEffect, {}, {MarkerBundle, CaptureBundle}, "", InsertBefore);
    Marker->setMetadata(LLVMContext::MD_ptsan_loop_bounds, &SerializedBounds);
    return Marker;
  }

  uint64_t getLoopID() {
    auto Bundle = Call->getOperandBundle(BundleName);
    assert(Bundle && Bundle->Inputs.size() == 1 &&
           "PTSan loop preheader marker requires a loop ID");
    return cast<ConstantInt>(Bundle->Inputs[0].get())->getZExtValue();
  }

  MDNode &getBoundsMetadata() {
    MDNode *Bounds = Call->getMetadata(LLVMContext::MD_ptsan_loop_bounds);
    return *Bounds;
  }

  SmallVector<Value *, 8> getCaptures() {
    auto Bundle = Call->getOperandBundle(LLVMContext::OB_deopt);
    assert(Bundle && "PTSan loop preheader marker requires a capture bundle");
    SmallVector<Value *, 8> Captures;
    for (const Use &Input : Bundle->Inputs) {
      Captures.push_back(Input.get());
    }
    return Captures;
  }
};

// __ptsan_source Identifies an allocation whose bounds PTSan can supply. The result is the
// raw pointer, not its tagged representation; GetIDAnchor requests its ID.
class SourceAnchor : public Anchor {
public:
  SourceAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanSource);
  }

  static SourceAnchor create(PointerTagSanitizer &DS, PTSanUtils &PTU,
                             Value *Source) {
    Instruction *IP = PTU.getFirstIPAfter(Source).getIP();
    SourceAnchor Result(Anchor::create(DS, DS.PTSanSource, Source->getType(),
                                       IP, {Source}));
    Source->replaceUsesWithIf(Result.Call, [&](Use &U) {
      return U.getUser() != Result.Call;
    });
    return Result;
  }

  Value *getSource() const { return Call->getArgOperand(0); }

  // An ID request for these sources allocates an ID owned by this frame.
  // TLS sources instead reuse an existing thread-owned ID.
  bool needsFrameID() const {
    return isa<AllocaInst, Argument>(getSource());
  }
};

// __ptsan_get_id(base) materializes the PTSan ID for a base pointer. For a
// SourceAnchor this is the allocation's assigned ID; otherwise it is extracted
// from the tagged pointer.
class GetIDAnchor : public Anchor {
public:
  GetIDAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanGetID);
  }

  static GetIDAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                            Value *Base) {
    assert(Base->getType()->isPtrOrPtrVectorTy() &&
           "get-id anchor expects pointer or pointer vector");
    Type *IDTy = Base->getType()->getWithNewType(DS.U64Ty);
    return GetIDAnchor(Anchor::create(DS, DS.PTSanGetID, IDTy, IP, {Base}));
  }

  Value *getBase() const { return Call->getArgOperand(0); }
};

// Bound anchors return bounds with the same scalar or vector shape as the ID.
class BoundAnchor : public Anchor {
public:
  BoundAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanLoadBase) ||
           Anchor->isCallTo(Anchor->DS.PTSanLoadSpan) ||
           Anchor->isCallTo(Anchor->DS.PTSanStaticBase) ||
           Anchor->isCallTo(Anchor->DS.PTSanStaticSpan);
  }

  bool isBase() const {
    return isCallTo(DS.PTSanLoadBase) || isCallTo(DS.PTSanStaticBase);
  }
  bool isSpan() const {
    return isCallTo(DS.PTSanLoadSpan) || isCallTo(DS.PTSanStaticSpan);
  }
  HoistTarget getTarget() const { return {Call, AFTER}; }
};

class LoadBoundAnchor : public BoundAnchor {
  static LoadBoundAnchor create(PointerTagSanitizer &DS, FunctionCallee Callee,
                                Instruction *IP, GetIDAnchor ID, Value *Mask) {
    assert(ID.Call->getType()->isVectorTy() == (Mask != nullptr) &&
           "vector bounds loads require a mask");
    SmallVector<Value *> Args = {ID.Call};
    if (Mask) {
      Args.push_back(Mask);
    }
    return LoadBoundAnchor(
        Anchor::create(DS, Callee, ID.Call->getType(), IP, Args));
  }

public:
  LoadBoundAnchor(Anchor A) : BoundAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanLoadBase) ||
           Anchor->isCallTo(Anchor->DS.PTSanLoadSpan);
  }

  static LoadBoundAnchor createBase(PointerTagSanitizer &DS, Instruction *IP,
                                    GetIDAnchor ID, Value *Mask = nullptr) {
    return create(DS, DS.PTSanLoadBase, IP, ID, Mask);
  }

  static LoadBoundAnchor createSpan(PointerTagSanitizer &DS, Instruction *IP,
                                    GetIDAnchor ID, Value *Mask = nullptr) {
    return create(DS, DS.PTSanLoadSpan, IP, ID, Mask);
  }

  // Load-bound IDs begin as GetIDAnchors, but late optimization may replace
  // them with materialized SSA values such as PHIs or selects.
  Value *getID() const { return Call->getArgOperand(0); }
  bool isVector() const { return Call->arg_size() == 2; }
  Value *getMask() const {
    return isVector() ? Call->getArgOperand(1) : nullptr;
  }
};

class StaticBoundAnchor : public BoundAnchor {
public:
  StaticBoundAnchor(Anchor A) : BoundAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanStaticBase) ||
           Anchor->isCallTo(Anchor->DS.PTSanStaticSpan);
  }

  static StaticBoundAnchor createBase(PointerTagSanitizer &DS, Instruction *IP,
                                      Value *Base) {
    return StaticBoundAnchor(Anchor::create(DS, DS.PTSanStaticBase, IP,
                                            {Base}));
  }

  static StaticBoundAnchor createSpan(PointerTagSanitizer &DS, Instruction *IP,
                                      Value *Span) {
    return StaticBoundAnchor(Anchor::create(DS, DS.PTSanStaticSpan, IP,
                                            {Span}));
  }

  Value *getStaticValue() const {
    return Call->getArgOperand(0);
  }
};

// Check anchors are either real memory access checks or loop-preanalysis checks.
class CheckAnchor : public Anchor {
public:
  static constexpr unsigned IDIndex = 0;
  static constexpr unsigned BaseIndex = 1;
  static constexpr unsigned SpanIndex = 2;
  static constexpr unsigned RangeMinIndex = 3;
  static constexpr unsigned RangeSizeIndex = 4;

  CheckAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanAccess) ||
           Anchor->isCallTo(Anchor->DS.PTSanLoopPreanalysis);
  }

  // Check IDs begin as GetIDAnchors, but late optimization may replace them
  // with materialized scalar SSA values such as PHIs or selects.
  Value *getID() const { return Call->getArgOperand(IDIndex); }
  BoundAnchor getBaseBound() const {
    return *DS.getAsAnchor<BoundAnchor>(Call->getArgOperand(BaseIndex));
  }
  BoundAnchor getSpanBound() const {
    return *DS.getAsAnchor<BoundAnchor>(Call->getArgOperand(SpanIndex));
  }

  bool hasMaterializedRange() const {
    return Call->getArgOperand(RangeMinIndex) != DS.PTSanUnmaterializedRange;
  }

  Value *getRangeMinPtr() const {
    assert(hasMaterializedRange() && "check range metadata not materialized");
    return Call->getArgOperand(RangeMinIndex);
  }

  Value *getRangeSize() const {
    assert(hasMaterializedRange() && "check range metadata not materialized");
    return Call->getArgOperand(RangeSizeIndex);
  }

  void setMaterializedRange(Value *RangeMin, Value *RangeSize) const {
    assert(!hasMaterializedRange() && "check range metadata already set");
    Call->setArgOperand(RangeMinIndex, RangeMin);
    Call->setArgOperand(RangeSizeIndex, RangeSize);
  }

  HoistTarget getBoundsTarget(PTSanUtils &PTU) const {
    HoistTarget BaseTarget = getBaseBound().getTarget();
    HoistTarget SpanTarget = getSpanBound().getTarget();
    bool BaseDominatesSpan =
        PTU.hoistTargetDominates(BaseTarget, SpanTarget.getIP());
    bool SpanDominatesBase =
        PTU.hoistTargetDominates(SpanTarget, BaseTarget.getIP());
    assert((BaseDominatesSpan || SpanDominatesBase) &&
           "check bounds must have comparable insertion targets");
    return BaseDominatesSpan ? SpanTarget : BaseTarget;
  }
};

// __ptsan_access(id, base, span, range_min, range_size, size, type, index,
//                [mask]) describes a pointer use. Mask is present only for a
// masked vector access.
class AccessAnchor : public CheckAnchor {
public:
  AccessAnchor(Anchor A) : CheckAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanAccess);
  }

  static AccessAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                             const PtrOp &Op, GetIDAnchor ID,
                             BoundAnchor BaseBound, BoundAnchor SpanBound) {
    assert(Op.CheckTy != NO_CHECK && !isKnownZeroAccessSize(Op.Size) &&
           "access anchors should only represent real checks");
    Value *RangeMin = DS.PTSanUnmaterializedRange;
    Value *RangeSize = ConstantInt::get(DS.U64Ty, 0);
    Value *OperandIndex = ConstantInt::get(DS.U32Ty, Op.U->getOperandNo());
    Value *CheckTy = ConstantInt::get(DS.U32Ty, Op.CheckTy);
    SmallVector<Value *> Args = {ID.Call,   BaseBound.Call, SpanBound.Call,
                                 RangeMin, RangeSize,       Op.Size,
                                 CheckTy,  OperandIndex};
    if (Op.Mask) {
      Args.push_back(Op.Mask);
    }
    return AccessAnchor(Anchor::create(DS, DS.PTSanAccess, IP, Args));
  }

  Value *getSize() const { return Call->getArgOperand(5); }

  CHK_TY getCheckType() const {
    return static_cast<CHK_TY>(
        cast<ConstantInt>(Call->getArgOperand(6))->getZExtValue());
  }

  unsigned getOperandIndex() const {
    return cast<ConstantInt>(Call->getArgOperand(7))->getZExtValue();
  }

  bool isVector() const { return Call->arg_size() == 9; }

  Value *getMask() const {
    return isVector() ? Call->getArgOperand(8) : nullptr;
  }

  // Return the exact use in the following real instruction described by index.
  Use *getAccessedUse() const {
    Instruction *User = DS.getNextRealInstruction(*this);
    return &User->getOperandUse(getOperandIndex());
  }
};

// __ptsan_loop_preanalysis(id, base, span, range_min, range_size) marks a
// placed preanalysis check.
class LoopPreanalysisAnchor : public CheckAnchor {
public:
  LoopPreanalysisAnchor(Anchor A) : CheckAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanLoopPreanalysis);
  }

  static LoopPreanalysisAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                                      GetIDAnchor ID, BoundAnchor BaseBound,
                                      BoundAnchor SpanBound) {
    Value *RangeMin = DS.PTSanUnmaterializedRange;
    Value *RangeSize = ConstantInt::get(DS.U64Ty, 0);
    return LoopPreanalysisAnchor(
        Anchor::create(DS, DS.PTSanLoopPreanalysis, IP,
                       {ID.Call, BaseBound.Call, SpanBound.Call, RangeMin,
                        RangeSize}));
  }
};

// __ptsan_strip variants return a stripped replacement for operand 0.
class StripAnchor : public Anchor {
public:
  StripAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanStrip) ||
           Anchor->isCallTo(Anchor->DS.PTSanStripI64);
  }

  static StripAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                            Value *Ptr) {
    bool IsPointer = Ptr->getType()->isPtrOrPtrVectorTy();
    if (!IsPointer && Ptr->getType() != DS.U64Ty) {
      errs() << "Unsupported strip anchor type: " << *Ptr->getType() << "\n";
      die();
    }

    StripAnchor Strip(
        IsPointer
            ? Anchor::create(DS, DS.PTSanStrip, Ptr->getType(), IP, {Ptr})
            : Anchor::create(DS, DS.PTSanStripI64, IP, {Ptr}));
    setPTSanPolarityMetadata(*Strip.Call, Polarity::Stripped);
    return Strip;
  }

  Value *getValue() const { return Call->getArgOperand(0); }
  bool isPointerStrip() const { return isCallTo(DS.PTSanStrip); }
  bool isIntegerStrip() const { return isCallTo(DS.PTSanStripI64); }
};

// __ptsan_tagged(value) returns value, marking that this use needs tags.
class TaggedAnchor : public Anchor {
public:
  TaggedAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanTagged);
  }

  static TaggedAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                             Value *Ptr) {
    assert(Ptr->getType()->isPtrOrPtrVectorTy() &&
           "tagged anchor expects pointer or pointer vector");
    assert(!DS.getAsAnchor<TaggedAnchor>(Ptr) &&
           "tagged anchor should not wrap another tagged anchor");
    assert(!DS.getAsAnchor<StripAnchor>(Ptr) &&
           "tagged anchor should not wrap a stripped pointer");
    TaggedAnchor Tagged(
        Anchor::create(DS, DS.PTSanTagged, Ptr->getType(), IP, {Ptr}));
    setPTSanPolarityMetadata(*Tagged.Call, Polarity::Tagged);
    return Tagged;
  }

  Value *getValue() const { return Call->getArgOperand(0); }
};

// __ptsan_tag(value, id) returns value with id installed in its tag bits.
class TagAnchor : public Anchor {
public:
  TagAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanTag);
  }

  static TagAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                          Value *Ptr, Value *ID) {
    assert(Ptr->getType()->isPtrOrPtrVectorTy() &&
           "tag anchor expects pointer or pointer vector");
    Type *PointerIDTy = Ptr->getType()->getWithNewType(DS.U64Ty);
    assert((ID->getType() == PointerIDTy || ID->getType() == DS.U64Ty) &&
           "tag anchor expects a per-lane or scalar-common ID");
    TagAnchor Tag(
        Anchor::create(DS, DS.PTSanTag, Ptr->getType(), IP, {Ptr, ID}));
    setPTSanPolarityMetadata(*Tag.Call, Polarity::Tagged);
    return Tag;
  }

  Value *getValue() const { return Call->getArgOperand(0); }
  Value *getID() const { return Call->getArgOperand(1); }
};

// __ptsan_validate_arith(lhs, rhs) validates pointer arithmetic operands.
class ValidateArithAnchor : public Anchor {
public:
  ValidateArithAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanValidateArith);
  }

  static ValidateArithAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                                    Value *Op0, Value *Op1) {
    return ValidateArithAnchor(
        Anchor::create(DS, DS.PTSanValidateArith, IP, {Op0, Op1}));
  }

  Value *getLHS() const { return Call->getArgOperand(0); }
  Value *getRHS() const { return Call->getArgOperand(1); }
};

// No-operand positioned anchors refer to the following real instruction.
class PositionedAnchor : public Anchor {
public:
  PositionedAnchor(Anchor A) : Anchor(A) {}

  Instruction *getPositionedInstruction() const {
    return DS.getNextRealInstruction(*this);
  }
};

class FunctionExitAnchor : public PositionedAnchor {
public:
  FunctionExitAnchor(Anchor A) : PositionedAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanFunctionExit);
  }

  static FunctionExitAnchor create(PointerTagSanitizer &DS, Instruction *IP) {
    return FunctionExitAnchor(Anchor::create(DS, DS.PTSanFunctionExit, IP));
  }
};

class SetjmpAnchor : public PositionedAnchor {
public:
  SetjmpAnchor(Anchor A) : PositionedAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanSetjmp);
  }

  static SetjmpAnchor create(PointerTagSanitizer &DS, CallBase *CB) {
    return SetjmpAnchor(Anchor::create(DS, DS.PTSanSetjmp, CB));
  }
};

class LibcCallAnchor : public PositionedAnchor {
public:
  LibcCallAnchor(Anchor A) : PositionedAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanLibcCall);
  }

  static LibcCallAnchor create(PointerTagSanitizer &DS, Instruction *IP) {
    return LibcCallAnchor(Anchor::create(DS, DS.PTSanLibcCall, IP));
  }
};

class SpecialStripAnchor : public PositionedAnchor {
public:
  SpecialStripAnchor(Anchor A) : PositionedAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanSpecialStrip);
  }

  static SpecialStripAnchor create(PointerTagSanitizer &DS, CallBase *CB) {
    return SpecialStripAnchor(Anchor::create(DS, DS.PTSanSpecialStrip, CB));
  }
};

struct BoundedPtrCheck : public PtrOp {
  CheckAnchor Anchor;
  Value *BasePtr;
  AccessRangePtr Range;

  BoundedPtrCheck() = delete;
  BoundedPtrCheck(AccessAnchor Anchor, const PtrOp &Op, Value *BasePtr,
                  AccessRangePtr Range)
      : BoundedPtrCheck(CheckAnchor(Anchor), Op, BasePtr, Range) {}
  BoundedPtrCheck(LoopPreanalysisAnchor Anchor, const PtrOp &Op,
                  Value *BasePtr, AccessRangePtr Range)
      : BoundedPtrCheck(CheckAnchor(Anchor), Op, BasePtr, Range) {}
  BoundedPtrCheck(CheckAnchor Anchor, const PtrOp &Op, Value *BasePtr,
                  AccessRangePtr Range)
      : PtrOp(Op), Anchor(Anchor), BasePtr(BasePtr), Range(Range) {}

  Value *getUnstrippedPtr(PointerTagSanitizer &DS) const;
  static Value *getUnstrippedPtrFromUse(Use *U, PointerTagSanitizer &DS);
};

struct PlaceablePtrCheck : public BoundedPtrCheck {
  SmallPtrSet<Instruction *, 32> CheckIPs;
  SmallPtrSet<Use *, 4> InstigatingUses;
  SmallPtrSet<Use *, 4> UsesCovering;
  PlaceablePtrCheck() = delete;
  PlaceablePtrCheck(const BoundedPtrCheck &Op,
                    SmallPtrSet<Instruction *, 32> CheckIPs,
                    AccessRangePtr Range):
    BoundedPtrCheck(Op),
    CheckIPs(CheckIPs) {
      this->Range = Range;
      InstigatingUses.insert(Op.U);
      UsesCovering.insert(Op.U);
    }

  static Instruction *
  getCheckIP(const SmallPtrSetImpl<Instruction *> &CheckIPs,
             DominatorTree &DT) {
    Instruction *CheckIP = nullptr;
    for (Instruction *NewIP : CheckIPs) {
      if (!CheckIP || DT.dominates(NewIP, CheckIP)) {
        CheckIP = NewIP;
      }
    }
    assert(CheckIP && "optimized PTSan check must have an insertion point");
    return CheckIP;
  }

  Instruction *getCheckIP(DominatorTree &DT) const {
    return getCheckIP(CheckIPs, DT);
  }
};

inline Value *BoundedPtrCheck::getUnstrippedPtr(PointerTagSanitizer &DS) const {
  return getUnstrippedPtrFromUse(U, DS);
}

inline Value *BoundedPtrCheck::getUnstrippedPtrFromUse(Use *U,
                                                       PointerTagSanitizer &DS) {
  Value *Ptr = U->get();
  optional<StripAnchor> Strip = DS.getAsAnchor<StripAnchor>(Ptr);
  if (!Strip) {
    return Ptr;
  }

  Value *Unstripped = Strip->getValue();
  assert(!DS.getAsAnchor<StripAnchor>(Unstripped) &&
         "PTSan should not create nested strip anchors");
  return Unstripped;
}

template <typename AnchorT>
optional<AnchorT>
PointerTagSanitizer::getAsAnchor(Value *V) {
  auto *CI = dyn_cast<CallInst>(V);
  if (!CI) {
    return nullopt;
  }

  Anchor Anchor(*this, CI);
  if (!dyn_cast<AnchorT>(&Anchor)) {
    return nullopt;
  }

  return AnchorT(Anchor);
}

template <typename AnchorT>
SmallVector<AnchorT, 16>
PointerTagSanitizer::collectAnchors(Function &F) {
  SmallVector<AnchorT, 16> Anchors;
  for (BasicBlock *BB : depth_first(&F.getEntryBlock())) {
    for (Instruction &I : *BB) {
      if (optional<AnchorT> Anchor = getAsAnchor<AnchorT>(&I)) {
        Anchors.push_back(*Anchor);
      }
    }
  }
  return Anchors;
}

} // namespace llvm::ptsan
