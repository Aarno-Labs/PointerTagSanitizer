//===-- PTSanLoopBounds.cpp - Preserve PTSan loop bounds metadata ---------===//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/PTSanLoopBounds.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Casting.h"

#include <cassert>

using namespace llvm;

static MDNode *getPTSanBounds(const MDNode *LoopID);

static MDNode *addLoopBoundsTag(LLVMContext &Ctx, MDNode *OldID, MDNode *Tag) {
  if (OldID != nullptr && getPTSanBounds(OldID) != nullptr) {
    return OldID;
  }

  SmallVector<Metadata *, 8> NewOps;

  if (OldID != nullptr) {
    for (unsigned I = 0; I < OldID->getNumOperands(); I++) {
      NewOps.push_back(OldID->getOperand(I));
    }
  } else {
    NewOps.push_back(nullptr);
  }

  NewOps.push_back(Tag);

  MDNode *NewID = MDNode::getDistinct(Ctx, NewOps);
  NewID->replaceOperandWith(0, NewID);
  return NewID;
}

static void addLoopBoundsTag(Loop *L, MDNode *Tag) {
  assert(Tag != nullptr);

  L->setLoopID(addLoopBoundsTag(L->getHeader()->getContext(), L->getLoopID(),
                                Tag));
}

static void addLoopBoundsTag(BasicBlock *Header, BasicBlock *Preheader,
                             MDNode *Tag) {
  assert(Header != nullptr);
  assert(Preheader != nullptr);
  assert(Tag != nullptr);

  for (BasicBlock *Pred : predecessors(Header)) {
    if (Pred == Preheader) {
      continue;
    }

    Instruction *Term = Pred->getTerminator();
    MDNode *OldID = Term->getMetadata(LLVMContext::MD_loop);
    Term->setMetadata(LLVMContext::MD_loop,
                      addLoopBoundsTag(Header->getContext(), OldID, Tag));
  }
}

static MDNode *getPTSanBounds(const MDNode *LoopID) {
  if (LoopID == nullptr) {
    return nullptr;
  }

  unsigned NumOps = LoopID->getNumOperands();
  if (NumOps <= 1) {
    return nullptr;
  }

  for (unsigned I = 1; I < NumOps; I++) {
    Metadata *Op = LoopID->getOperand(I);

    MDNode *Node = dyn_cast<MDNode>(Op);
    if (Node == nullptr || Node->getNumOperands() == 0) {
      continue;
    }

    MDString *Tag = dyn_cast<MDString>(Node->getOperand(0));
    if (Tag == nullptr) {
      continue;
    }

    if (Tag->getString() == "ptsan.loop.bounds") {
      return Node;
    }
  }

  return nullptr;
}

void ptsan::copyLoopBounds(Loop *Dst, const Loop *Src) {
  copyLoopBounds(Dst, Src->getLoopID());
}

void ptsan::copyLoopBounds(Loop *Dst, const MDNode *SrcLoopID) {
  MDNode *LoopBounds = getPTSanBounds(SrcLoopID);
  if (LoopBounds) {
    addLoopBoundsTag(Dst, LoopBounds);
  }
}

void ptsan::copyLoopBounds(BasicBlock *Header, BasicBlock *Preheader,
                           const MDNode *SrcLoopID) {
  MDNode *LoopBounds = getPTSanBounds(SrcLoopID);
  if (LoopBounds) {
    addLoopBoundsTag(Header, Preheader, LoopBounds);
  }
}
