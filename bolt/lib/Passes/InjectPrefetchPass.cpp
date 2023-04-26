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
#include <unordered_map>

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

  BinaryContext& BC = BF.getBinaryContext();
  uint64_t startingAddr = BF.getAddress();

  llvm::outs()<<"[InjectPrefetchPass] The starting address of do_work is: 0x"
              <<utohexstr(startingAddr)<<"\n";


  // get the Basic Block that contains the TOP LLC miss
  // instruction. 
  BinaryBasicBlock* TopLLCMissBB;

  for (auto BBI = BF.begin(); BBI != BF.end(); BBI ++){
    BinaryBasicBlock &BB = *BBI;
    for (auto It = BB.begin(); It != BB.end(); It++){
      const MCInst &Instr = *It;
      if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
        uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");        
        if (AbsoluteAddr == 0x401520){
          llvm::outs()<<"[InjectPrefetchPass] find instruction that causes the TOP LLC miss\n";
          TopLLCMissBB = &BB;
        }
      }
    }
  }

  BF.updateLayoutIndices();

  BinaryDominatorTree DomTree;
  DomTree.recalculate(BF);
  BF.BLI.reset(new BinaryLoopInfo());
  BF.BLI->analyze(DomTree);

  std::vector<BinaryLoop *> OuterLoops;

  // get outer most loops in the function
  for (auto I = BF.BLI->begin(), E = BF.BLI->end(); I != E; ++I) {
    OuterLoops.push_back(*I);
    ++BF.BLI->OuterLoops;
  }

  std::vector<BinaryLoop*> LoopsContainTopLLCMissBB;
  while (!OuterLoops.empty()) {
    BinaryLoop *L = OuterLoops.back();
    OuterLoops.pop_back();

    ++BF.BLI->TotalLoops;
    BF.BLI->MaximumDepth = std::max(L->getLoopDepth(), BF.BLI->MaximumDepth);

    bool containTopLLCMissBB = false;
    for (BinaryBasicBlock *BB : L->getBlocks()){
      if (BB==TopLLCMissBB){
        containTopLLCMissBB = true;
        LoopsContainTopLLCMissBB.push_back(L);
        OuterLoops.clear();
        break;    
      }
    }  

    // get inner loops of the current outer loop.
    // set these inner loops to be the new round 
    // of outer loops
    if (containTopLLCMissBB){
      for (BinaryLoop::iterator I = L->begin(), E = L->end(); I != E; ++I){
        OuterLoops.push_back(*I);
      }
    } 
  }

  int LoopDepth = (int)LoopsContainTopLLCMissBB.size();
  llvm::outs()<<"[InjectPrefetchPass] the depth of nested Loop that contains TopLLCMissBB is "<<LoopDepth<<"\n";

  // if the top LLC miss instruction doesn't exist in 
  // a nested loop, we are not going to inject prefetch
  if (LoopDepth<2) return false;

  BinaryLoop* OuterLoop = LoopsContainTopLLCMissBB[LoopDepth-2];
  BinaryLoop* InnerLoop = LoopsContainTopLLCMissBB[LoopDepth-1];

  SmallVector<BinaryBasicBlock *, 1> Latches;
  OuterLoop->getLoopLatches(Latches);
  llvm::outs()<<"[InjectPrefetchPass] number of latches in the outer loop: "<< Latches.size()<<"\n";

  // if the loop doesn't contain latch, return false
  if (Latches.size()==0) return false;

  BinaryBasicBlock *HeaderBB = OuterLoop->getHeader();
  for (auto I = HeaderBB->begin(); I != HeaderBB->end(); I++) {
    const MCInst &Instr = *I;
    if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
      uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");        
      llvm::outs()<<"[InjectPrefetchPass] header: "<<utohexstr(AbsoluteAddr)<<"\n";
    } 
  } 

  auto Loc = HeaderBB->begin();
  MCInst PushInst; 
  //BC.MIB->createPrefetchT0(NewInst, BC.MIB->getX86RAX());
  BC.MIB->createPushRegister(PushInst, BC.MIB->getX86RAX(), 8);
  Loc = HeaderBB->insertRealInstruction(Loc, PushInst);
  Loc++;  

  MCInst PopInst; 
  BC.MIB->createPopRegister(PopInst, BC.MIB->getX86RAX(), 8);
  HeaderBB->insertRealInstruction(Loc, PopInst);



  for (auto I = Latches[0]->begin(); I != Latches[0]->end(); I++) {
    const MCInst &Instr = *I;
    if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
      uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");        
      llvm::outs()<<"[InjectPrefetchPass] latch: "<<utohexstr(AbsoluteAddr)<<"\n";
    } 
  } 

  // Now we have both the loop header and the latch
  // In the loop header, we need to inject prefetch instruction 
 

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
