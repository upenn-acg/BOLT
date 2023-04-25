//===- bolt/Passes/InjectPrefetch.cpp ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the InjectPrefetchPass class.
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/InjectPrefetchPass.h"
#include "bolt/Core/ParallelUtilities.h"

using namespace llvm;

namespace opts {
/*
extern cl::OptionCategory BoltCategory;

extern cl::opt<bolt::ReorderBasicBlocks::LayoutType> ReorderBlocks;


static cl::opt<bool> LoopReorder(
    "loop-inversion-opt",
    cl::desc("reorder unconditional jump instructions in loops optimization"),
    cl::init(true), cl::cat(BoltCategory), cl::ReallyHidden);
*/
} // namespace opts

namespace llvm {
namespace bolt {

bool InjectPrefetchPass::runOnFunction(BinaryFunction &BF) {

  if (BF.getOneName() != "_Z7do_workPv") return false;

  // get the Basic Block that contains the TOP LLC miss
  // instruction. 
  BinaryContext& BC = BF.getBinaryContext();
  uint64_t startingAddr = BF.getAddress();
  llvm::outs()<<"### The starting address of do_work is: 0x"<<utohexstr(startingAddr)<<"\n";
  for (auto BBI = BF.begin(); BBI != BF.end(); BBI ++){
    BinaryBasicBlock &BB = *BBI;
    for (auto It = BB.begin(); It != BB.end(); It++){
      const MCInst &Inst = *It;
      if (BC.MIB->hasAnnotation(Inst, "Offset")){
        llvm::outs()<<"kkkkkkk\n";
//        auto addr = BC.MIB->getAnnotationAs<uint64_t>(Inst, "AbsoluteAddr");
//        llvm::outs()<<utohexstr(addr)<<"\n";

      }
      
    }
  }




  BF.updateLayoutIndices();

  BinaryDominatorTree DomTree;
  DomTree.recalculate(BF);
  BF.BLI.reset(new BinaryLoopInfo());
  BF.BLI->analyze(DomTree);

  std::vector<BinaryLoop *> OuterLoops;
  std::vector<BinaryLoop *> InnerLoops;
  for (auto I = BF.BLI->begin(), E = BF.BLI->end(); I != E; ++I) {
    OuterLoops.push_back(*I);
    ++BF.BLI->OuterLoops;
  }

  llvm::outs()<<"@@@@@ number of outer loops: "<<BF.BLI->OuterLoops<<"\n";

  while (!OuterLoops.empty()) {
    BinaryLoop *L = OuterLoops.back();
    OuterLoops.pop_back();
    InnerLoops.clear();
    ++BF.BLI->TotalLoops;
    BF.BLI->MaximumDepth = std::max(L->getLoopDepth(), BF.BLI->MaximumDepth);

    // get nested loops.
    for (BinaryLoop::iterator I = L->begin(), E = L->end(); I != E; ++I)
      InnerLoops.push_back(*I);

    // Compute back edge count.
    SmallVector<BinaryBasicBlock *, 1> Latches;
    L->getLoopLatches(Latches);
    llvm::outs()<<"@@@@@ number of inner loops: "<< InnerLoops.size()<<"\n";
    for(BinaryBasicBlock *BB : L->getBlocks()){
      printf("xxxxxx\n");
    }    

  }
/*
  for (BinaryBasicBlock *BB : BF.layout()) {
    if (BB->succ_size() != 1 || BB->pred_size() != 1)
      continue;

    BinaryBasicBlock *SuccBB = *BB->succ_begin();
    BinaryBasicBlock *PredBB = *BB->pred_begin();
    const unsigned BBIndex = BB->getLayoutIndex();
    const unsigned SuccBBIndex = SuccBB->getLayoutIndex();
    if (SuccBB == PredBB && BB != SuccBB && BBIndex != 0 && SuccBBIndex != 0 &&
        SuccBB->succ_size() == 2 && BB->isCold() == SuccBB->isCold()) {
      // Get the second successor (after loop BB)
      BinaryBasicBlock *SecondSucc = nullptr;
      for (BinaryBasicBlock *Succ : SuccBB->successors()) {
        if (Succ != &*BB) {
          SecondSucc = Succ;
          break;
        }
      }

      assert(SecondSucc != nullptr && "Unable to find second BB successor");
      const uint64_t BBCount = SuccBB->getBranchInfo(*BB).Count;
      const uint64_t OtherCount = SuccBB->getBranchInfo(*SecondSucc).Count;
      if ((BBCount < OtherCount) && (BBIndex > SuccBBIndex))
        continue;

      IsChanged = true;
      BB->setLayoutIndex(SuccBBIndex);
      SuccBB->setLayoutIndex(BBIndex);
    }
  }

  if (IsChanged) {
    BinaryFunction::BasicBlockOrderType NewOrder = BF.getLayout();
    std::sort(NewOrder.begin(), NewOrder.end(),
              [&](BinaryBasicBlock *BB1, BinaryBasicBlock *BB2) {
                return BB1->getLayoutIndex() < BB2->getLayoutIndex();
              });
    BF.updateBasicBlockLayout(NewOrder);
  }

  return IsChanged;
   */
   return true;
}

void InjectPrefetchPass::runOnFunctions(BinaryContext &BC) {
   for (auto &it: BC.getBinaryFunctions()){
      runOnFunction(it.second);
   }

/*
  std::atomic<uint64_t> ModifiedFuncCount{0};
//  if (opts::ReorderBlocks == ReorderBasicBlocks::LT_NONE ||
//      opts::LoopReorder == false)
//    return;

  llvm::errs()<<"hhhhhhhhhhhhhhh\n";

  ParallelUtilities::WorkFuncTy WorkFun = [&](BinaryFunction &BF) {
    llvm::errs()<<"iiiiiiiiiiiiiiii\n";
    if (runOnFunction(BF))
      ++ModifiedFuncCount;
  };

  ParallelUtilities::PredicateTy SkipFunc = [&](const BinaryFunction &BF) {
//    return !shouldOptimize(BF);
    return true;
  };

  ParallelUtilities::runOnEachFunction(
      BC, ParallelUtilities::SchedulingPolicy::SP_TRIVIAL, WorkFun, SkipFunc,
      "InjectPrefetchPass");

  outs() << "BOLT-INFO: " << ModifiedFuncCount
         << " Functions were reordered by InjectPrefetchPass\n";
*/
}

} // end namespace bolt
} // end namespace llvm
