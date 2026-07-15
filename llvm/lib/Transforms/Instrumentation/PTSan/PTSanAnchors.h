//===-- PTSanAnchors.h - Private PTSan anchor views ------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#pragma once

#include "PTSanInternal.h"

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

  bool isCallTo(FunctionCallee Expected) const {
    return Call->getCalledOperand() == Expected.getCallee();
  }

  void erase() const {
    DS.unmarkAsInstr(Call);
    Call->eraseFromParent();
  }
};

// __ptsan_source carries the pointer source to tag as operand 0.
class SourceAnchor : public Anchor {
public:
  SourceAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanSource);
  }

  static SourceAnchor create(PointerTagSanitizer &DS, PTSanUtils &PTU,
                             Value *Source) {
    Instruction *IP = PTU.getFirstIPAfter(Source).getIP();
    return SourceAnchor(Anchor::create(DS, DS.PTSanSource, IP, {Source}));
  }

  Value *getSource() const { return Call->getArgOperand(0); }
};

// __ptsan_get_id(base) materializes the PTSan ID for a base pointer.
class GetIDAnchor : public Anchor {
public:
  GetIDAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanGetID);
  }

  static GetIDAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                            Value *Base) {
    return GetIDAnchor(Anchor::create(DS, DS.PTSanGetID, IP, {Base}));
  }

  Value *getBase() const { return Call->getArgOperand(0); }
};

// Bound anchors return one scalar bound value consumed by check anchors.
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
public:
  LoadBoundAnchor(Anchor A) : BoundAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanLoadBase) ||
           Anchor->isCallTo(Anchor->DS.PTSanLoadSpan);
  }

  static LoadBoundAnchor createBase(PointerTagSanitizer &DS, Instruction *IP,
                                    GetIDAnchor ID) {
    return LoadBoundAnchor(Anchor::create(DS, DS.PTSanLoadBase, IP,
                                          {ID.Call}));
  }

  static LoadBoundAnchor createSpan(PointerTagSanitizer &DS, Instruction *IP,
                                    GetIDAnchor ID) {
    return LoadBoundAnchor(Anchor::create(DS, DS.PTSanLoadSpan, IP,
                                          {ID.Call}));
  }

  // Load-bound IDs begin as GetIDAnchors, but late optimization may replace
  // them with materialized scalar SSA values such as PHIs or selects.
  Value *getID() const { return Call->getArgOperand(0); }
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

// __ptsan_access(id, base, span, range_min, range_size, size, type, index)
// describes a pointer use.
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
    SmallVector<Value*> Args = {ID.Call, BaseBound.Call, SpanBound.Call,
                                    RangeMin, RangeSize, Op.Size, CheckTy,
                                    OperandIndex};
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
    FunctionCallee Callee;
    if (Ptr->getType()->isPointerTy()) {
      Callee = DS.PTSanStrip;
    } else if (Ptr->getType() == DS.U64Ty) {
      Callee = DS.PTSanStripI64;
    } else {
      errs() << "Unsupported strip anchor type: " << *Ptr->getType() << "\n";
      die();
    }

    return StripAnchor(Anchor::create(DS, Callee, IP, {Ptr}));
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
    assert(Ptr->getType()->isPointerTy() &&
           "tagged anchor expects pointer value");
    assert(!DS.getAsAnchor<TaggedAnchor>(Ptr) &&
           "tagged anchor should not wrap another tagged anchor");
    assert(!DS.getAsAnchor<StripAnchor>(Ptr) &&
           "tagged anchor should not wrap a stripped pointer");
    return TaggedAnchor(Anchor::create(DS, DS.PTSanTagged, IP, {Ptr}));
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
    assert(Ptr->getType()->isPointerTy() && "tag anchor expects pointer value");
    assert(ID->getType() == DS.U64Ty && "tag anchor expects i64 ID");
    return TagAnchor(Anchor::create(DS, DS.PTSanTag, IP, {Ptr, ID}));
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

  Instruction *getCheckIP(DominatorTree &DT) const {
    Instruction *CheckIP = nullptr;
    for (Instruction *NewIP : CheckIPs) {
      if (!CheckIP || DT.dominates(NewIP, CheckIP)) {
        CheckIP = NewIP;
      }
    }
    assert(CheckIP && "optimized PTSan check must have an insertion point");
    return CheckIP;
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
