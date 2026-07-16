//===-- PTSanLoopBounds.h - Preserve PTSan loop bounds metadata -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
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
