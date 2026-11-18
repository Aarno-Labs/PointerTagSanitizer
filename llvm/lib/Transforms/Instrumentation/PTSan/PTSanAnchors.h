//===-- PTSanAnchors.h - Private PTSan anchor views ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "PTSanInternal.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/ValueHandle.h"

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
    Function *SideEffect =
        Intrinsic::getOrInsertDeclaration(M, Intrinsic::sideeffect);
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

  static SourceAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                             Value *Source) {
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

// __ptsan_get_id(base) requests the allocation ID without constraining the
// pointer's representation. Optimization resolves raw-pointer requests through
// their allocation identity. Lowering handles SourceAnchor IDs and extraction
// from tagged pointers or their pointer-sized integer representations.
class GetIDAnchor : public Anchor {
public:
  GetIDAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanGetID);
  }

  static GetIDAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                            Value *Base) {
    assert((Base->getType()->isPtrOrPtrVectorTy() ||
            Base->getType() == DS.U64Ty) &&
           "get-id anchor expects a pointer or pointer-sized integer");
    Type *IDTy = Base->getType()->getWithNewType(DS.U64Ty);
    return GetIDAnchor(Anchor::create(DS, DS.PTSanGetID, IDTy, IP, {Base}));
  }

  Value *getBase() const { return Call->getArgOperand(0); }
};

// Bound anchors carry the metadata needed to interpret an object's bounds. In
// ID32 mode, base anchors return the decode bias D = B - O, and origin anchors
// return O. All three match the scalar or vector shape of the ID.
class BoundAnchor : public Anchor {
public:
  BoundAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanLoadBase) ||
           Anchor->isCallTo(Anchor->DS.PTSanLoadSpan) ||
           Anchor->isCallTo(Anchor->DS.PTSanLoadOrigin) ||
           Anchor->isCallTo(Anchor->DS.PTSanStaticBase) ||
           Anchor->isCallTo(Anchor->DS.PTSanStaticSpan);
  }

  bool isBase() const {
    return isCallTo(DS.PTSanLoadBase) || isCallTo(DS.PTSanStaticBase);
  }
  bool isSpan() const {
    return isCallTo(DS.PTSanLoadSpan) || isCallTo(DS.PTSanStaticSpan);
  }
  bool isOrigin() const { return isCallTo(DS.PTSanLoadOrigin); }
  HoistTarget getTarget() const { return {Call, AFTER}; }
};

// Scalar IDs take no mask; an omitted mask for vector IDs means all lanes.
// Callers with masked accesses must pass their actual active-lane mask.
class LoadBoundAnchor : public BoundAnchor {
  static LoadBoundAnchor create(PointerTagSanitizer &DS, FunctionCallee Callee,
                                Type *ScalarTy, Instruction *IP, Value *ID,
                                Value *Mask) {
    assert(ID->getType()->getScalarType() == DS.U64Ty &&
           "load-bound anchor expects an i64 ID");
    if (ID->getType()->isVectorTy() && !Mask) {
      Mask = Constant::getAllOnesValue(ID->getType()->getWithNewType(DS.BoolTy));
    }
    assert(ID->getType()->isVectorTy() == (Mask != nullptr) &&
           "only vector bounds loads have a mask");
    SmallVector<Value *> Args = {ID};
    if (Mask) {
      Args.push_back(Mask);
    }
    Type *ReturnTy = ID->getType()->getWithNewType(ScalarTy);
    return LoadBoundAnchor(Anchor::create(DS, Callee, ReturnTy, IP, Args));
  }

public:
  LoadBoundAnchor(Anchor A) : BoundAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanLoadBase) ||
           Anchor->isCallTo(Anchor->DS.PTSanLoadSpan) ||
           Anchor->isCallTo(Anchor->DS.PTSanLoadOrigin);
  }

  static LoadBoundAnchor createBase(PointerTagSanitizer &DS, Instruction *IP,
                                    Value *ID, Value *Mask = nullptr) {
    return create(DS, DS.PTSanLoadBase, DS.U64Ty, IP, ID, Mask);
  }

  static LoadBoundAnchor createSpan(PointerTagSanitizer &DS, Instruction *IP,
                                    Value *ID, Value *Mask = nullptr) {
    return create(DS, DS.PTSanLoadSpan, DS.U64Ty, IP, ID, Mask);
  }

  static LoadBoundAnchor createOrigin(PointerTagSanitizer &DS, Instruction *IP,
                                      Value *ID, Value *Mask = nullptr) {
    assert(Cl32BitIDs && "origin anchors are only used by ID32");
    return create(DS, DS.PTSanLoadOrigin, DS.U32Ty, IP, ID, Mask);
  }

  // Load-bound IDs begin as GetIDAnchors, but late optimization may replace
  // them with materialized SSA values such as PHIs or selects.
  Value *getID() const { return Call->getArgOperand(0); }
  bool isVector() const { return getID()->getType()->isVectorTy(); }
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

  static StaticBoundAnchor createBase(PointerTagSanitizer &DS, Instruction *IP,
                                      Value *RawBase, Value *Origin) {
    assert(Cl32BitIDs && Origin->getType() == DS.U32Ty &&
           "ID32 static base requires a scalar origin");
    return StaticBoundAnchor(Anchor::create(
        DS, DS.PTSanStaticBase, DS.U64Ty, IP, {RawBase, Origin}));
  }

  static StaticBoundAnchor createSpan(PointerTagSanitizer &DS, Instruction *IP,
                                      Value *Span) {
    return StaticBoundAnchor(Anchor::create(DS, DS.PTSanStaticSpan, IP,
                                            {Span}));
  }

  Value *getStaticValue() const {
    return Call->getArgOperand(0);
  }

  Value *getOrigin() const { return Call->getArgOperand(1); }
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
  LoadBoundAnchor getOrigin() const {
    assert(Cl32BitIDs && "only ID32 checks carry an origin");
    auto Origin = DS.getAsAnchor<LoadBoundAnchor>(
        Call->getArgOperand(Call->arg_size() - 1));
    assert(Origin && Origin->isOrigin() &&
           "ID32 check must carry an origin anchor");
    return *Origin;
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
    HoistTarget Target = PTU.getFirstCommonInsertionPoint(
        {getBaseBound().getTarget(), getSpanBound().getTarget()});
    if (Cl32BitIDs) {
      Target = PTU.getFirstCommonInsertionPoint({Target, getOrigin().getTarget()});
    }
    return Target;
  }
};

// __ptsan_access(id, base, span, range_min, range_size, size, type, index,
//                [mask], [origin]) describes a pointer use. Mask is present
// only for a masked vector access; origin is the final operand in ID32 mode.
class AccessAnchor : public CheckAnchor {
public:
  AccessAnchor(Anchor A) : CheckAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanAccess);
  }

  static AccessAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                             const PtrOp &Op, GetIDAnchor ID,
                             BoundAnchor BaseBound, BoundAnchor SpanBound,
                             optional<LoadBoundAnchor> Origin = nullopt) {
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
    assert(Origin.has_value() == Cl32BitIDs &&
           "only ID32 access anchors carry an origin");
    if (Origin) {
      assert(Origin->isOrigin() && "access requires an origin anchor");
      Args.push_back(Origin->Call);
    }
    return AccessAnchor(Anchor::create(DS, DS.PTSanAccess, IP, Args));
  }

  Value *getSize() const { return Call->getArgOperand(5); }

  CHK_TY getCheckType() const {
    return (CHK_TY)cast<ConstantInt>(Call->getArgOperand(6))->getZExtValue();
  }

  unsigned getOperandIndex() const {
    return cast<ConstantInt>(Call->getArgOperand(7))->getZExtValue();
  }

  bool isVector() const { return Call->arg_size() == (Cl32BitIDs ? 10 : 9); }

  Value *getMask() const {
    return isVector() ? Call->getArgOperand(8) : nullptr;
  }

  // Return the exact use in the following real instruction described by index.
  Use *getAccessedUse() const {
    Instruction *User = DS.getNextRealInstruction(*this);
    return &User->getOperandUse(getOperandIndex());
  }
};

// __ptsan_loop_preanalysis(id, base, span, range_min, range_size, [origin])
// marks a placed preanalysis check. Origin is present only in ID32 mode.
class LoopPreanalysisAnchor : public CheckAnchor {
public:
  LoopPreanalysisAnchor(Anchor A) : CheckAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanLoopPreanalysis);
  }

  static LoopPreanalysisAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                                      GetIDAnchor ID, BoundAnchor BaseBound,
                                      BoundAnchor SpanBound,
                                      optional<LoadBoundAnchor> Origin =
                                          nullopt) {
    Value *RangeMin = DS.PTSanUnmaterializedRange;
    Value *RangeSize = ConstantInt::get(DS.U64Ty, 0);
    SmallVector<Value *> Args = {ID.Call, BaseBound.Call, SpanBound.Call,
                                 RangeMin, RangeSize};
    assert(Origin.has_value() == Cl32BitIDs &&
           "only ID32 preanalysis anchors carry an origin");
    if (Origin) {
      assert(Origin->isOrigin() && "preanalysis requires an origin anchor");
      Args.push_back(Origin->Call);
    }
    return LoopPreanalysisAnchor(
        Anchor::create(DS, DS.PTSanLoopPreanalysis, IP, Args));
  }
};

// Strip anchors return a stripped replacement for operand 0.
class StripAnchor : public Anchor {
public:
  StripAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanID16Strip) ||
           Anchor->isCallTo(Anchor->DS.PTSanID16StripI64) ||
           Anchor->isCallTo(Anchor->DS.PTSanID32Strip) ||
           Anchor->isCallTo(Anchor->DS.PTSanID32StripI64);
  }

  Value *getValue() const { return Call->getArgOperand(0); }
  bool isPointerStrip() const {
    return getValue()->getType()->isPtrOrPtrVectorTy();
  }
  bool isIntegerStrip() const { return getValue()->getType()->isIntegerTy(64); }
};

class ID16StripAnchor : public StripAnchor {
public:
  ID16StripAnchor(Anchor A) : StripAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanID16Strip) ||
           Anchor->isCallTo(Anchor->DS.PTSanID16StripI64);
  }

  static ID16StripAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                               Value *Ptr) {
    bool IsPointer = Ptr->getType()->isPtrOrPtrVectorTy();
    if (!IsPointer && Ptr->getType() != DS.U64Ty) {
      errs() << "Unsupported strip anchor type: " << *Ptr->getType() << "\n";
      die();
    }
    ID16StripAnchor Strip(Anchor::create(
        DS, IsPointer ? DS.PTSanID16Strip : DS.PTSanID16StripI64,
        Ptr->getType(), IP, {Ptr}));
    setPTSanPolarityMetadata(*Strip.Call, Polarity::Stripped);
    return Strip;
  }
};

// ID32 decoding needs an ID and base. Any vector mask belongs to the base load;
// the strip's base dependency constrains placement.
class ID32StripAnchor : public StripAnchor {
public:
  ID32StripAnchor(Anchor A) : StripAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanID32Strip) ||
           Anchor->isCallTo(Anchor->DS.PTSanID32StripI64);
  }

  static ID32StripAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                               Value *Ptr, Value *ID, BoundAnchor Base) {
    bool IsPointer = Ptr->getType()->isPtrOrPtrVectorTy();
    if (!IsPointer && Ptr->getType() != DS.U64Ty) {
      errs() << "Unsupported strip anchor type: " << *Ptr->getType() << "\n";
      die();
    }

    assert((ID->getType() == Ptr->getType()->getWithNewType(DS.U64Ty) ||
            ID->getType() == DS.U64Ty) && "strip ID must match pointer shape");
    assert(Base.isBase() && Base.Call->getType() == ID->getType());
    ID32StripAnchor Strip(Anchor::create(
        DS, IsPointer ? DS.PTSanID32Strip : DS.PTSanID32StripI64,
        Ptr->getType(), IP, {Ptr, ID, Base.Call}));
    setPTSanPolarityMetadata(*Strip.Call, Polarity::Stripped);
    return Strip;
  }

  Value *getID() const { return Call->getArgOperand(1); }
  BoundAnchor getBaseBound() const {
    return *DS.getAsAnchor<BoundAnchor>(Call->getArgOperand(2));
  }
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
    // This identity marker preserves both provenance and the numeric value.
    // Let pointer peeling and ScalarEvolution see through it.
    Tagged.Call->addParamAttr(0, Attribute::Returned);
    setPTSanPolarityMetadata(*Tagged.Call, Polarity::Tagged);
    return Tagged;
  }

  Value *getValue() const { return Call->getArgOperand(0); }
};

// Tag anchors encode operand 0 with the ID in operand 1.
class TagAnchor : public Anchor {
public:
  TagAnchor(Anchor A) : Anchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanID16Tag) ||
           Anchor->isCallTo(Anchor->DS.PTSanID32Tag);
  }

  Value *getValue() const { return Call->getArgOperand(0); }
  Value *getID() const { return Call->getArgOperand(1); }
};

class ID16TagAnchor : public TagAnchor {
public:
  ID16TagAnchor(Anchor A) : TagAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanID16Tag);
  }

  static ID16TagAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                             Value *Ptr, Value *ID) {
    assert(Ptr->getType()->isPtrOrPtrVectorTy() &&
           "tag anchor expects pointer or pointer vector");
    Type *PointerIDTy = Ptr->getType()->getWithNewType(DS.U64Ty);
    assert((ID->getType() == PointerIDTy || ID->getType() == DS.U64Ty) &&
           "tag anchor expects a per-lane or scalar-common ID");
    ID16TagAnchor Tag(
        Anchor::create(DS, DS.PTSanID16Tag, Ptr->getType(), IP, {Ptr, ID}));
    setPTSanPolarityMetadata(*Tag.Call, Polarity::Tagged);
    return Tag;
  }
};

class ID32TagAnchor : public TagAnchor {
public:
  ID32TagAnchor(Anchor A) : TagAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanID32Tag);
  }

  static ID32TagAnchor create(PointerTagSanitizer &DS, Instruction *IP,
                             Value *Ptr, Value *ID, BoundAnchor Base) {
    assert(Ptr->getType()->isPtrOrPtrVectorTy() &&
           "tag anchor expects pointer or pointer vector");
    Type *PointerIDTy = Ptr->getType()->getWithNewType(DS.U64Ty);
    assert((ID->getType() == PointerIDTy || ID->getType() == DS.U64Ty) &&
           "tag anchor expects a per-lane or scalar-common ID");
    assert(Base.isBase() && Base.Call->getType() == ID->getType());
    ID32TagAnchor Tag(Anchor::create(DS, DS.PTSanID32Tag, Ptr->getType(), IP,
                                   {Ptr, ID, Base.Call}));
    setPTSanPolarityMetadata(*Tag.Call, Polarity::Tagged);
    return Tag;
  }

  BoundAnchor getBaseBound() const {
    return *DS.getAsAnchor<BoundAnchor>(Call->getArgOperand(2));
  }
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

// A target and all its anchors share one distinct metadata node. Their
// association does not depend on adjacency or the target producing an SSA value.
class TargetedAnchor : public Anchor {
  mutable WeakTrackingVH CachedTarget;

public:
  static constexpr StringLiteral TargetMetadata = "ptsan.target";

  TargetedAnchor(Anchor A) : Anchor(A) {}

  static MDNode *getTargetID(const Instruction *I) {
    return I->getMetadata(TargetMetadata);
  }

  Instruction *getTarget() const {
    if (CachedTarget) {
      return cast<Instruction>(CachedTarget);
    }
    MDNode *ID = getTargetID(Call);
    assert(ID && "targeted anchor must have target metadata");
    for (Instruction &I : instructions(Call->getFunction())) {
      if (!DS.isAnchor(&I) && getTargetID(&I) == ID) {
        CachedTarget = &I;
        return &I;
      }
    }
    llvm_unreachable("PTSan anchor target not found");
  }

  static TargetedAnchor create(PointerTagSanitizer &DS, FunctionCallee Callee,
                              Instruction *Target) {
    MDNode *ID = getTargetID(Target);
    if (!ID) {
      ID = MDNode::getDistinct(DS.Ctx, {});
      Target->setMetadata(TargetMetadata, ID);
    }
    Anchor A = Anchor::create(DS, Callee, Target);
    A.Call->setMetadata(TargetMetadata, ID);
    return TargetedAnchor(A);
  }
};

class FunctionExitAnchor : public TargetedAnchor {
public:
  FunctionExitAnchor(Anchor A) : TargetedAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanFunctionExit);
  }

  static FunctionExitAnchor create(PointerTagSanitizer &DS, Instruction *IP) {
    return FunctionExitAnchor(TargetedAnchor::create(DS, DS.PTSanFunctionExit, IP));
  }
};

class SetjmpAnchor : public TargetedAnchor {
public:
  SetjmpAnchor(Anchor A) : TargetedAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanSetjmp);
  }

  static SetjmpAnchor create(PointerTagSanitizer &DS, CallBase *CB) {
    return SetjmpAnchor(TargetedAnchor::create(DS, DS.PTSanSetjmp, CB));
  }
};

// va_start initializes several frame-owned regions, not one pointer source.
class VAStartAnchor : public TargetedAnchor {
public:
  VAStartAnchor(Anchor A) : TargetedAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanVAStart);
  }

  static VAStartAnchor create(PointerTagSanitizer &DS, IntrinsicInst *Start) {
    return VAStartAnchor(TargetedAnchor::create(DS, DS.PTSanVAStart, Start));
  }
};

// Both fully instrumented and strip-only callers supply the vararg sentinel.
class VarargCallAnchor : public TargetedAnchor {
public:
  VarargCallAnchor(Anchor A) : TargetedAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanVarargCall);
  }

  static VarargCallAnchor create(PointerTagSanitizer &DS, CallBase *CB) {
    return VarargCallAnchor(TargetedAnchor::create(DS, DS.PTSanVarargCall, CB));
  }
};

class LibcCallAnchor : public TargetedAnchor {
public:
  LibcCallAnchor(Anchor A) : TargetedAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanLibcCall);
  }

  static LibcCallAnchor create(PointerTagSanitizer &DS, Instruction *IP) {
    return LibcCallAnchor(TargetedAnchor::create(DS, DS.PTSanLibcCall, IP));
  }
};

class SpecialStripAnchor : public TargetedAnchor {
public:
  SpecialStripAnchor(Anchor A) : TargetedAnchor(A) {}

  static bool classof(const Anchor *Anchor) {
    return Anchor->isCallTo(Anchor->DS.PTSanSpecialStrip);
  }

  static SpecialStripAnchor create(PointerTagSanitizer &DS, CallBase *CB) {
    return SpecialStripAnchor(TargetedAnchor::create(DS, DS.PTSanSpecialStrip, CB));
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
