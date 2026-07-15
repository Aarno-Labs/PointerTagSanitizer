//===-- PTSanLoopBounds.h - Preserve PTSan loop bounds metadata -*- C++ -*-===//
//===----------------------------------------------------------------------===//

#pragma once

namespace llvm {

class Loop;
class BasicBlock;
class MDNode;

namespace ptsan {

void copyLoopBounds(Loop *Dst, const Loop *Src);
void copyLoopBounds(Loop *Dst, const MDNode *SrcLoopID);
void copyLoopBounds(BasicBlock *Header, BasicBlock *Preheader,
                    const MDNode *SrcLoopID);

} // namespace ptsan
} // namespace llvm
