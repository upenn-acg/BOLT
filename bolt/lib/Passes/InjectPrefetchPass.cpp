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


  // get the Instruction and Basic Block that contains 
  // the TOP LLC miss instruction. 
  BinaryBasicBlock* TopLLCMissBB;
  MCInst* TopLLCMissInstr;
  MCInst* DemandLoadInstr;

  for (auto BBI = BF.begin(); BBI != BF.end(); BBI ++){
    BinaryBasicBlock &BB = *BBI;
    for (auto It = BB.begin(); It != BB.end(); It++){
      MCInst &Instr = *It;
      if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
        uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");        
        if (AbsoluteAddr == 0x401520){
          llvm::outs()<<"[InjectPrefetchPass] find instruction that causes the TOP LLC miss\n";
          if (BC.MIB->isLoad(Instr)){
            llvm::outs()<<"[InjectPrefetchPass] TOP LLC miss instruction is a load\n";           
          }
          else if (BC.MIB->isStore(Instr)){
            llvm::outs()<<"[InjectPrefetchPass] TOP LLC miss instruction is a store\n";
          }
          else {
            return false;
          }  
          TopLLCMissBB = &BB;
          TopLLCMissInstr = &Instr;
        }
      }
    }
  }


  // get loop info of this function
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
  if (LoopDepth < 2) return false;

  BinaryLoop* OuterLoop = LoopsContainTopLLCMissBB[LoopDepth-2];
  BinaryLoop* InnerLoop = LoopsContainTopLLCMissBB[LoopDepth-1];


  std::vector<MCInst* > Loads;
  for (BinaryBasicBlock *BB : OuterLoop->getBlocks()){
    for (auto It = BB->begin(); It != BB->end(); It++){
      MCInst &Instr = *It;
      if (BC.MIB->isLoad(Instr)){
        Loads.push_back(&Instr);
      }
    }
  }  

  // based on TopLLCMissInstr, decide the DemandLoadInstr
  unsigned DemandLoadDstRegNum = TopLLCMissInstr->getOperand(1).getReg();
  std::vector<MCInst* >DemandLoads;
  for (unsigned i=0; i<Loads.size(); i++){
    if (Loads[i]->getOperand(0).getReg()==DemandLoadDstRegNum){
      DemandLoads.push_back(Loads[i]);
      uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(*Loads[i], "AbsoluteAddr"); 
      llvm::outs()<<"[InjectPrefetchPass] addr: "<<utohexstr(AbsoluteAddr)<<"\n";
    } 
  }
  if (DemandLoads.size()!=1){
    llvm::outs()<<"[InjectPrefetchPass][err] the number of potential Demand Load is "<<DemandLoads.size()<<"\n";
    return false;
  }
  DemandLoadInstr = DemandLoads[0];

  // get the loop header and check if the header is in
  // the loop we want
  // we are going to insert prefetch to the header basic block
  BinaryBasicBlock *HeaderBB = OuterLoop->getHeader();
  for (auto I = HeaderBB->begin(); I != HeaderBB->end(); I++) {
    const MCInst &Instr = *I;
    if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
      uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");        
      llvm::outs()<<"[InjectPrefetchPass] header: "<<utohexstr(AbsoluteAddr)<<"\n";
    } 
  } 

  // Now we have the loop header
  // In the loop header, we need to inject prefetch instruction 
  // first Push %rax 
  auto Loc = HeaderBB->begin();
  MCInst PushInst; 
  BC.MIB->createPushRegister(PushInst, BC.MIB->getX86RAX(), 8);
  Loc = HeaderBB->insertRealInstruction(Loc, PushInst);
  Loc++;  

  // then load prefetch target's address
  int numOperands = DemandLoadInstr->getNumOperands();
  MCInst LoadPrefetchAddrInstr;
  LoadPrefetchAddrInstr.setOpcode(DemandLoadInstr->getOpcode());
  for (int i=0; i<numOperands; i++){
     if (i==0){
       // the first operand is the dest reg
       LoadPrefetchAddrInstr.addOperand(MCOperand::createReg(BC.MIB->getX86RAX())); 
     }
     else if (i==4){
       // the 5th operand is the offset
       LoadPrefetchAddrInstr.addOperand(MCOperand::createImm(512));
     }
     else{
       LoadPrefetchAddrInstr.addOperand(DemandLoadInstr->getOperand(i));
     }
  }
  Loc = HeaderBB->insertRealInstruction(Loc, LoadPrefetchAddrInstr);
  Loc++;  

/*
  MCInst PrefetchInst;
  BC.MIB->createPrefetchT0(PrefetchInst, BC.MIB->getX86RAX());
  Loc = HeaderBB->insertRealInstruction(Loc, PrefetchInst);
  Loc++;  
*/

  // finally pop %rax
  MCInst PopInst; 
  BC.MIB->createPopRegister(PopInst, BC.MIB->getX86RAX(), 8);
  HeaderBB->insertRealInstruction(Loc, PopInst);

  // top llc miss insn
/*
  numOperands = TopLLCMissInstr->getNumOperands();
  std::vector<MCOperand> Operands;
  llvm::outs()<<"[InjectPrefetchPass] number of operands is: "<<numOperands<<"\n";
  for (int i = 0; i < numOperands; i++){
    MCOperand oprd = TopLLCMissInstr->getOperand(i);
    if (oprd.isReg()){
      unsigned regNum = oprd.getReg();
      llvm::outs()<<"@@@ "<<regNum<<"\n";
    }
    else if (oprd.isImm()){
      unsigned immNum = oprd.getImm();
      llvm::outs()<<"### "<<immNum<<"\n";
    }
  }
*/

  /*------------------no use code-----------------*/
  // if the loop doesn't contain latch, return false
  /*

  SmallVector<BinaryBasicBlock *, 1> Latches;
  OuterLoop->getLoopLatches(Latches);
  llvm::outs()<<"[InjectPrefetchPass] number of latches in the outer loop: "<< Latches.size()<<"\n";

  if (Latches.size()==0) return false;

  for (auto I = Latches[0]->begin(); I != Latches[0]->end(); I++) {
    const MCInst &Instr = *I;
    if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
      uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");        
      llvm::outs()<<"[InjectPrefetchPass] latch: "<<utohexstr(AbsoluteAddr)<<"\n";
    } 
  } 




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
}

} // end namespace bolt
} // end namespace llvm
