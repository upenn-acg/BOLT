//===- bolt/Passes/InjectPrefetch.cpp ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the InjectPrefetchLitePass class.
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/InjectPrefetchLitePass.h"
#include "bolt/Core/ParallelUtilities.h"
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>

using namespace llvm;

namespace opts {

extern cl::OptionCategory BoltCategory;

extern cl::opt<bool> InjectPrefetch;
extern cl::opt<std::string> PrefetchLocationFile;
extern cl::opt<unsigned> PrefetchDistance;
} // namespace opts

namespace llvm {
namespace bolt {

bool InjectPrefetchLitePass::runOnFunction(BinaryFunction &BF) {

  BinaryContext& BC = BF.getBinaryContext();
  uint64_t startingAddr = BF.getAddress();
  int prefetchDist = opts::PrefetchDistance;
  std::string demangledFuncName = removeSuffix(BF.getDemangledName());
  std::vector<uint64_t> TopLLCMissAddrs = TopLLCMissLocations[demangledFuncName];
  uint64_t TopLLCMissAddr = TopLLCMissAddrs[0];
 
  llvm::outs()<<"[InjectPrefetchLitePass] The starting address of "<<demangledFuncName<<" is: 0x"
              <<utohexstr(startingAddr)<<"\n";
  llvm::outs()<<"[InjectPrefetchLitePass] The top llc miss addr is: 0x"
              <<utohexstr(TopLLCMissAddr)<<"\n";

  std::unordered_set<uint64_t> TopLLCMissAddrs_set;
  for (unsigned i=0; i<TopLLCMissAddrs.size(); i++){
    TopLLCMissAddrs_set.insert(TopLLCMissAddrs[i]);
  }

  // get the Instruction and Basic Block that contains 
  // the TOP LLC miss instruction. 
  std::unordered_map<BinaryBasicBlock*, std::vector<MCInst*> > TopLLCMissInstrs;

  for (auto BBI = BF.begin(); BBI != BF.end(); BBI ++){
    BinaryBasicBlock &BB = *BBI;
    for (auto It = BB.begin(); It != BB.end(); It++){
      MCInst &Instr = *It;
      if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
        uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");        
        if (TopLLCMissAddrs_set.find(AbsoluteAddr) != TopLLCMissAddrs_set.end()){
          llvm::outs()<<"[InjectPrefetchLitePass] find instruction that causes the TOP LLC miss\n";
          if (BC.MIB->isLoad(Instr)){
            llvm::outs()<<"[InjectPrefetchLitePass] TOP LLC miss instruction is a load\n";           
          }
          else if (BC.MIB->isStore(Instr)){
            llvm::outs()<<"[InjectPrefetchLitePass] TOP LLC miss instruction is a store\n";
          }
          else {
            llvm::outs()<<"[InjectPrefetchLitePass] This pass only work for injecting prefetch for load/store instruction\n"; 
            return false;
          }
          if (TopLLCMissInstrs.find(&(*BBI)) != TopLLCMissInstrs.end()){
            TopLLCMissInstrs[&(*BBI)].push_back(&(*It));
          }
          else {
            std::vector<MCInst*> instrs;
            instrs.push_back(&(*It));
            TopLLCMissInstrs.insert(std::make_pair(&(*BBI), instrs));
          } 
        }
      }
    }
  }

  for (auto map_it = TopLLCMissInstrs.begin(); map_it != TopLLCMissInstrs.end(); map_it++){
    BinaryBasicBlock* TopLLCMissBB = map_it->first;
    MCInst* TopLLCMissInstr = map_it->second[0];
    std::vector<MCInst*> TopLLCMissInstrsInThisBB = map_it->second;
    bool canInject = true;
    for (unsigned i=0; i < TopLLCMissInstrsInThisBB.size(); i++){
      MCInst* topllcmissinstr = TopLLCMissInstrsInThisBB[i];
      for (int j=1; j<topllcmissinstr->getNumOperands(); j++){
        if ((topllcmissinstr->getOperand(j).isReg()) && 
            (topllcmissinstr->getOperand(j).getReg() != TopLLCMissInstr->getOperand(j).getReg())){
          canInject = false;
          break;
        }
      } 
    }
    if (!canInject) continue;
    // get the working loop, also update the LoopInductionInstr
    // and LoopGuardCMPInstr
    MCInst* LoopInductionInstr = NULL;
    MCInst* LoopGuardCMPInstr = NULL;

    BinaryLoop* workingLoop = getWorkingLoop( BF,TopLLCMissBB,TopLLCMissInstr, 
                                            &LoopInductionInstr, &LoopGuardCMPInstr);

    if (workingLoop==NULL) {
      llvm::outs()<<"[InjectPrefetchLitePass] TopLLCMissInstr is not in a loop\n";
      return false;
    }
 
    BinaryBasicBlock* HeaderBB = workingLoop->getHeader(); 

    // create BoundsCheckBB and PrefetchBB
    SmallVector<BinaryBasicBlock*, 0> PredsOfHeaderBB = HeaderBB->getPredecessors();

 
    std::unordered_set<MCPhysReg> usedRegs;
    int numOperands = TopLLCMissInstr->getNumOperands();
    // the first operand of a load instruction is the dst register
    for (int i=1; i<numOperands; i++){
      if (TopLLCMissInstr->getOperand(i).isReg()){
        if (usedRegs.find(TopLLCMissInstr->getOperand(i).getReg())==usedRegs.end()){
          usedRegs.insert(TopLLCMissInstr->getOperand(i).getReg());   
        }
      }
    }

    for (unsigned i=0; i<LoopGuardCMPInstr->getNumOperands(); i++){
      if (LoopGuardCMPInstr->getOperand(i).isReg()){
        if (usedRegs.find(LoopGuardCMPInstr->getOperand(i).getReg()) == usedRegs.end()){
          usedRegs.insert(LoopGuardCMPInstr->getOperand(i).getReg());
        }
      }
    }

    MCPhysReg freeReg = BC.MIB->getUnusedReg(usedRegs);
    if (freeReg == BC.MIB->getNoRegister()){
      llvm::outs()<<"BOLT-ERROR: LoopGuardCMPInstr doesn't exist\n";
      return false;
    } 

    BinaryBasicBlock* BoundsCheckBB = createBoundsCheckBB(BF, HeaderBB, 
                                                          LoopGuardCMPInstr, 
                                                          LoopInductionInstr,
                                                          TopLLCMissInstr, 
                                                          prefetchDist,
                                                          freeReg);

    BinaryBasicBlock* PrefetchBB = createPrefetchBB(BF, HeaderBB, BoundsCheckBB, 
                                                    TopLLCMissInstrsInThisBB, LoopInductionInstr, prefetchDist, 
                                                    freeReg);

    // change the control-flow-graph 
    // first add set PrefetchBB to be the successor of
    // the BoundsCheckBB
    BoundsCheckBB->addSuccessor(PrefetchBB);

    // add Predecessors to HeaderBB
    HeaderBB->addPredecessor(BoundsCheckBB); 
    HeaderBB->addPredecessor(PrefetchBB);

    // create Branch Instruction at the end of 
    // BoundsCheckBB
    MCInst BoundsCheckBranch;
    BC.MIB->createJZ(BoundsCheckBranch, HeaderBB->getLabel()  , BC.Ctx.get());
    BoundsCheckBB->addInstruction(BoundsCheckBranch);

    // change HeaderBB's original predecessors' tail branch targets
    // to be BoundsCheckBB
    for (unsigned i=0; i<PredsOfHeaderBB.size(); i++){
      //MCInst* LastBranch = PredsOfTopLLCMissBB[i]->getLastNonPseudoInstr();
      MCInst* LastBranch = NULL;
      for (auto it = PredsOfHeaderBB[i]->begin(); it != PredsOfHeaderBB[i]->end(); it++){
        if (BC.MIB->isBranch(*it)){
           LastBranch = &(*it);
           break;
        } 
      }
      if ((LastBranch != NULL) && (BC.MIB->isBranch(*LastBranch))){
        if (BC.MIB->hasAnnotation(*LastBranch, "AbsoluteAddr")){
          uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(*LastBranch, "AbsoluteAddr");  
        //llvm::outs()<<"address of last branch is: "<<utohexstr(AbsoluteAddr)<<"\n";      
        }
        const MCExpr* LastBranchTargetExpr = LastBranch->getOperand(0).getExpr();
        const MCSymbol* LastBranchTargetSymbol = BC.MIB->getTargetSymbol(LastBranchTargetExpr);
        if (LastBranchTargetSymbol==HeaderBB->getLabel()){
          BC.MIB->replaceBranchTarget(*LastBranch, BoundsCheckBB->getLabel(), BC.Ctx.get());
        }
      }
      else{
        PredsOfHeaderBB[i]->addBranchInstruction(BoundsCheckBB);  
      }
    }

    // inject pop %rax to the Loop Header.
    // pop instruction should be the first instruction of 
    // the HeaderBB
    auto Loc = HeaderBB->begin();
    MCInst PopInst; 
    BC.MIB->createPopRegister(PopInst, freeReg, 8);
    HeaderBB->insertRealInstruction(Loc, PopInst);
  }
  return true;
}





BinaryLoop* InjectPrefetchLitePass::getWorkingLoop( BinaryFunction& BF,
                                                    BinaryBasicBlock* TopLLCMissBB,
                                                    MCInst* TopLLCMissInstr,
                                                    MCInst** LoopInductionInstr,
                                                    MCInst** LoopGuardCMPInstr){
  BinaryContext& BC = BF.getBinaryContext();
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
  }

  // get all loops that contains the TopLLCMissBB
  // we need the inner most 2 loops
  std::vector<BinaryLoop*> LoopsContainTopLLCMissBB;
  while (!OuterLoops.empty()) {
    BinaryLoop *L = OuterLoops.back();
    OuterLoops.pop_back();

    bool containTopLLCMissBB = false;
    if (L->contains(TopLLCMissBB)){
      containTopLLCMissBB = true;
      LoopsContainTopLLCMissBB.push_back(L);
      OuterLoops.clear();
    }

    if (containTopLLCMissBB){
      // iterate through all inner loop of the current outer loop
      for (BinaryLoop::iterator I = L->begin(), E = L->end(); I != E; ++I){
        OuterLoops.push_back(*I);
      }
    } 
  }

  int LoopDepth = (int)LoopsContainTopLLCMissBB.size();

  // if the top LLC miss instruction doesn't exist in 
  // a nested loop, we are not going to inject prefetch
  if (LoopDepth == 0) {
    llvm::outs()<<"[InjectPrefetchLitePass] The loop that contains top LLC miss BB doesn't exist\n";
    return NULL;
  }

  for (int i=LoopDepth-1; i>=0; i--){
    SmallVector<BinaryBasicBlock *, 1> Latches;
    if (LoopsContainTopLLCMissBB[i]->getBlocks().size()==1){
      BinaryBasicBlock* Latch = LoopsContainTopLLCMissBB[i]->getBlocks()[0];
      Latches.clear();
      Latches.push_back(Latch);
    }
    else {
      LoopsContainTopLLCMissBB[i]->getLoopLatches(Latches);
    }
    for (auto &Latch: Latches){
      *LoopInductionInstr = NULL;
      *LoopGuardCMPInstr = NULL;
      // get all potential loop induction instructions
      for (auto I = Latch->begin(); I != Latch->end(); I++){
        MCInst instr = *I;
        if ((BC.MIB->isADD(instr)) && (instr.getOperand(2).isImm())) {
          if ((TopLLCMissInstr->getOperand(3).getReg()==instr.getOperand(0).getReg())){
            *LoopInductionInstr = &(*I); 
            break;
          }
          else if ((TopLLCMissInstr->getOperand(1).getReg()==instr.getOperand(0).getReg())){
            *LoopInductionInstr = &(*I); 
            break;
          }
        }
      }
      
      if (*LoopInductionInstr){
        for (auto I = Latch->begin(); I != Latch->end(); I++){
          MCInst instr = *I;
          if (BC.MIB->isCMP(instr)){
            for (unsigned j=0; j<instr.getNumOperands(); j++){
              if (instr.getOperand(j).isReg()){
                // the third operand of DemandLoadInstr is the index register
                // namely the loop induction variable
                if ((*LoopInductionInstr)->getOperand(0).getReg()==instr.getOperand(j).getReg()){
                  *LoopGuardCMPInstr = &(*I);
                  break;
                }
                else if (BC.MIB->isLower32bitReg((*LoopInductionInstr)->getOperand(0).getReg(), instr.getOperand(j).getReg())){
                  *LoopGuardCMPInstr = &(*I);
                  break;
                }
              }
            }
          }
        }
      }

      if (*LoopGuardCMPInstr){
        if (BC.MIB->hasAnnotation(**LoopGuardCMPInstr, "AbsoluteAddr")){
          uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(**LoopGuardCMPInstr, "AbsoluteAddr");    
          llvm::outs()<<"@@@ "<<utohexstr(AbsoluteAddr)<<"\n";
        }
        return LoopsContainTopLLCMissBB[i];
      }
    }
  }

  return NULL;
}






std::pair<MCInst*, BinaryBasicBlock*> InjectPrefetchLitePass::findDemandLoad(BinaryFunction& BF,
                                                                         BinaryLoop* OuterLoop, 
                                                                         MCInst* DstLoad, 
                                                                         BinaryBasicBlock* DstLoadBB){

  BinaryContext& BC = BF.getBinaryContext();
  uint64_t DstLoadAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(*DstLoad, "AbsoluteAddr");
  MCInst* DemandLoadInstr = NULL;
  BinaryBasicBlock* DemandLoadBB = NULL;
  // based on TopLLCMissInstr, decide the DemandLoadInstr
  unsigned DemandLoadDstRegNum = DstLoad->getOperand(1).getReg();

  // check if the demandLoad is in the same BB
  std::vector<MCInst*> potentialDemandLoadsBefore;
  std::vector<MCInst*> potentialDemandLoadsAfter;
  std::vector<BinaryBasicBlock*> potentialDemandLoadBBs;
  for (auto It = DstLoadBB->begin(); It != DstLoadBB->end(); It++){
     MCInst &Instr = *It;
     if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
       uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");
       if (AbsoluteAddr < DstLoadAddr){
         if ((BC.MIB->isLoad(Instr)) && (Instr.getOperand(0).getReg()==DemandLoadDstRegNum)){
            potentialDemandLoadsBefore.push_back(&Instr);
         }
       }
       else if (AbsoluteAddr > DstLoadAddr){
         if ((BC.MIB->isLoad(Instr)) && (Instr.getOperand(0).getReg()==DemandLoadDstRegNum)){
            potentialDemandLoadsAfter.push_back(&Instr);
         }
       }
     }
  }
  if (potentialDemandLoadsBefore.size()!=0){
    DemandLoadInstr = potentialDemandLoadsBefore.back();
    DemandLoadBB = DstLoadBB;
  }
  else {
    // if demandLoad is not in the same BB as the TopLLCMissInstr
    // find the demendLoad in the predcessors of the TopLLCMissBB
    std::unordered_set<BinaryBasicBlock* > currentBBs;
    std::unordered_set<BinaryBasicBlock*> visitedBBs;
    visitedBBs.insert(DstLoadBB);
    for (auto BBI = DstLoadBB->pred_begin(); BBI != DstLoadBB->pred_end(); BBI++ ){
      BinaryBasicBlock* BB = *BBI;
      if (OuterLoop->contains(BB)){
        if (currentBBs.find(*BBI)==currentBBs.end()){
          currentBBs.insert(*BBI);
        }
      }
    }
    while (true){
      for (auto it = currentBBs.begin(); it != currentBBs.end(); it++){
        BinaryBasicBlock* currentBB = *it;
        visitedBBs.insert(currentBB);
        
        MCInst* DemandLoadInThisBB = NULL;
        for (auto It = currentBB->begin(); It != currentBB->end(); It++){
          MCInst &Instr = *It;
          if ((BC.MIB->isLoad(Instr)) && (Instr.getOperand(0).getReg()==DemandLoadDstRegNum)){
             DemandLoadInThisBB = &Instr;
          }
        }
        if (DemandLoadInThisBB != NULL){
          potentialDemandLoadsBefore.push_back(DemandLoadInThisBB); 
          potentialDemandLoadBBs.push_back(currentBB);
        }
      }
      if (potentialDemandLoadsBefore.size()==0){
        std::unordered_set<BinaryBasicBlock* > predBBs;
        for (auto It = currentBBs.begin(); It != currentBBs.end(); It++){
          BinaryBasicBlock* currentBB = *It;
          for (auto BBI = currentBB->pred_begin(); BBI != currentBB->pred_end(); BBI++ ){
            if ((OuterLoop->contains(*BBI)) && 
                (visitedBBs.find(*BBI)==visitedBBs.end()) &&
                (predBBs.find(*BBI)==predBBs.end())){
               predBBs.insert(*BBI);
            }
          }
        }
        currentBBs = predBBs;
        if (currentBBs.empty()){
          if (potentialDemandLoadsBefore.size()!=0){
            DemandLoadInstr = potentialDemandLoadsAfter.back();
            DemandLoadBB = DstLoadBB;
          }
          break;
        }
      }
      else if (potentialDemandLoadsBefore.size()>1){
         llvm::outs()<<"BOLT-ERROR: contains more than 1 demand load!\n";
         break;
      }
      else {
        DemandLoadInstr = potentialDemandLoadsBefore[0];
        DemandLoadBB = potentialDemandLoadBBs[0];
        break;
      }
    }
  }

  return std::make_pair(DemandLoadInstr, DemandLoadBB);
}






BinaryBasicBlock* InjectPrefetchLitePass::createBoundsCheckBB(BinaryFunction& BF,
                                      BinaryBasicBlock* TargetBB,
                                      MCInst* LoopGuardCMPInstr,
                                      MCInst* LoopInductionInstr,
                                      MCInst* TopLLCMissInstrs,
                                      int prefetchDist,
                                      MCPhysReg freeReg){
  BinaryContext& BC = BF.getBinaryContext();

  // before we change the CFG of this function to add the 
  // BoundCHeckBB and PrefetchBB, we need to save all the 
  // predecessors of the HeaderBB   
  SmallVector<BinaryBasicBlock*, 0> PredsOfTargetBB = TargetBB->getPredecessors();
  for (unsigned i=0; i<PredsOfTargetBB.size(); i++){
    PredsOfTargetBB[i]->removeSuccessor(TargetBB);
  }
  TargetBB->removeAllPredecessors();

  // create the boundary check basic block
  // in this BB, it contains the following 4 instructions
  // push %rax
  // mov %rdx, %rax
  // add 0x20, %rax
  // cmp 0x18(%rsp), rax
  // jz 0xa1(%rip)
  MCSymbol *BoundsCheckLabel = BC.Ctx->createNamedTempSymbol("BoundaryCheckBB");

  std::vector<std::unique_ptr<BinaryBasicBlock>> BoundCheckBBs;
  BoundCheckBBs.emplace_back(BF.createBasicBlock(BinaryBasicBlock::INVALID_OFFSET, BoundsCheckLabel));
 
  // add predecessor  
  for (unsigned i=0; i<PredsOfTargetBB.size(); i++){
    BinaryBasicBlock* PredOfTargetBB = PredsOfTargetBB[i];
    BoundCheckBBs.back()->addPredecessor(PredOfTargetBB);
  }
  // add successor
  BoundCheckBBs.back()->addSuccessor(TargetBB, 0,0);

  // add instructions
  // create push %rax 
  MCInst PushInst; 
  BC.MIB->createPushRegister(PushInst, freeReg, 8);
  BoundCheckBBs.back()->addInstruction(PushInst);

  // create mov %rdx, %rax
  MCInst MovInstr;
  BC.MIB->createMOV64rr(MovInstr, LoopInductionInstr->getOperand(1).getReg(), freeReg);
  BoundCheckBBs.back()->addInstruction(MovInstr);

  // create add %rax prefetchDist
  MCInst AddInstr;
  BC.MIB->createADD64ri32(AddInstr, freeReg, freeReg, prefetchDist);
  BoundCheckBBs.back()->addInstruction(AddInstr);


  // create comparison instruction
  // cmp 0x18(%rsp), %rax -> cmp 0x20(%rsp), %rax
  int NumOperandsCMP = LoopGuardCMPInstr->getNumOperands();
  MCInst CMPInstr;
  CMPInstr.setOpcode(LoopGuardCMPInstr->getOpcode());

  // check if the CMP instr contains %rsp
  // if it tries to load a value from 
  bool hasRSP = false;
  for (int i=0; i<NumOperandsCMP; i++){
    if (LoopGuardCMPInstr->getOperand(i).isReg()){
      if (LoopGuardCMPInstr->getOperand(i).getReg()==BC.MIB->getStackPointer()){
        hasRSP = true;
      }
    }
  }

  for (int i=0; i<NumOperandsCMP; i++) {
    if (LoopGuardCMPInstr->getOperand(i).isReg()){
      if (LoopInductionInstr->getOperand(1).getReg()==LoopGuardCMPInstr->getOperand(i).getReg()){
        CMPInstr.addOperand(MCOperand::createReg(freeReg));
      }
      else if (BC.MIB->isLower32bitReg(LoopInductionInstr->getOperand(1).getReg(), LoopGuardCMPInstr->getOperand(i).getReg())){
        CMPInstr.addOperand(MCOperand::createReg(freeReg));
      }
      else {
        CMPInstr.addOperand(LoopGuardCMPInstr->getOperand(i));
      }
    }
    else{
/*    else {
      // LoopGuardCMPInstr is a CMP instruction
      // In CMP instruction, there is only 1 immediate operand
      // which is the displacement
      if ((LoopGuardCMPInstr->getOperand(i).isImm())&&(hasRSP)){
        int64_t newDisp = LoopGuardCMPInstr->getOperand(i).getImm() + 8;
        CMPInstr.addOperand(MCOperand::createImm(newDisp));
      }
      else{
        CMPInstr.addOperand(LoopGuardCMPInstr->getOperand(i));
      }
    }
*/
      CMPInstr.addOperand(LoopGuardCMPInstr->getOperand(i));
    }
  }
  BoundCheckBBs.back()->addInstruction(CMPInstr);

  // insert this Basic Block to binary function
  for (unsigned i=0; i<PredsOfTargetBB.size(); i++){
    if (PredsOfTargetBB[i]==TargetBB) continue;
    else{
      BF.insertBasicBlocks(PredsOfTargetBB[i], std::move(BoundCheckBBs));
      break;
    }
  }

  BinaryBasicBlock* BoundsCheckBB = BF.getBasicBlockForLabel(BoundsCheckLabel);    

  for (unsigned i=0; i<PredsOfTargetBB.size(); i++){
    PredsOfTargetBB[i]->addSuccessor(BoundsCheckBB);
  }

  return BoundsCheckBB;

}





BinaryBasicBlock* InjectPrefetchLitePass::createPrefetchBB(BinaryFunction& BF,
                                      BinaryBasicBlock* TargetBB,
                                      BinaryBasicBlock* BoundsCheckBB,
                                      std::vector<MCInst*> TopLLCMissInstrs,
                                      MCInst* LoopInductionInstr,
                                      int prefetchDist,
                                      MCPhysReg freeReg){

  BinaryContext& BC = BF.getBinaryContext();
  // create prefetchBB
  // in prefetchBB it contains
  // mov 0x200(%r9,%rdx,8),%rax 
  // prefetcht0 (%rax) 
  MCSymbol *PrefetchBBLabel = BC.Ctx->createNamedTempSymbol("PrefetchBB");
  std::vector<std::unique_ptr<BinaryBasicBlock>> PrefetchBBs;
  PrefetchBBs.emplace_back(BF.createBasicBlock(BinaryBasicBlock::INVALID_OFFSET, PrefetchBBLabel));
  PrefetchBBs.back()->addSuccessor(TargetBB, 0,0);
  PrefetchBBs.back()->addPredecessor(BoundsCheckBB);

  MCInst* TopLLCMissInstr = TopLLCMissInstrs[0];

  MCInst Lea64rInstr;
  if (TopLLCMissInstr->getOperand(1).getReg()==LoopInductionInstr->getOperand(1).getReg()){
    BC.MIB->createLEA64r(Lea64rInstr, 
                         TopLLCMissInstr->getOperand(1).getReg(), 
                         1,  
                         BC.MIB->getNoRegister(), 
                         prefetchDist, 
                         BC.MIB->getNoRegister(), 
                         freeReg );
  }
  else if  (TopLLCMissInstr->getOperand(3).getReg()==LoopInductionInstr->getOperand(1).getReg()){
    BC.MIB->createLEA64r(Lea64rInstr, 
                         TopLLCMissInstr->getOperand(3).getReg(), 
                         1,  
                         BC.MIB->getNoRegister(), 
                         prefetchDist, 
                         BC.MIB->getNoRegister(), 
                         freeReg );
  }
  PrefetchBBs.back()->addInstruction(Lea64rInstr);



  if (TopLLCMissInstr->getOperand(4).isExpr()){
    // add prefetch instruction
    // prefetcht0 (%rax) 
    for (unsigned i=0; i<TopLLCMissInstrs.size(); i++){
      MCInst PrefetchInst;
      MCInst tmp;
      if (TopLLCMissInstrs[i]->getOperand(1).getReg()==LoopInductionInstr->getOperand(1).getReg()){
        BC.MIB->createPrefetchT0Expr(PrefetchInst, 
                                     freeReg, 
                                     TopLLCMissInstrs[i]->getOperand(4).getExpr(), 
                                     BC.MIB->getNoRegister(), 
                                     TopLLCMissInstrs[i]->getOperand(2).getImm(), 
                                     BC.MIB->getNoRegister(), tmp);
      }
      else if (TopLLCMissInstrs[i]->getOperand(3).getReg()==LoopInductionInstr->getOperand(1).getReg()){
        BC.MIB->createPrefetchT0Expr(PrefetchInst, 
                                     TopLLCMissInstrs[i]->getOperand(1).getReg(), 
                                     TopLLCMissInstrs[i]->getOperand(4).getExpr(), 
                                     freeReg, 
                                     TopLLCMissInstrs[i]->getOperand(2).getImm(), 
                                     BC.MIB->getNoRegister(), tmp);
      }
      else{
        llvm::outs()<<"[InjectPrefetchLitePass] TopLLCMiss instr must contain loop induction var\n";
        exit(1);
      }

      PrefetchBBs.back()->addInstruction(PrefetchInst);
    }
  }
  else if (TopLLCMissInstr->getOperand(4).isImm()){
    for (unsigned i=0; i<TopLLCMissInstrs.size(); i++){
      MCInst PrefetchInst;
      MCInst tmp;
      if (TopLLCMissInstrs[i]->getOperand(1).getReg()==LoopInductionInstr->getOperand(1).getReg()){
        BC.MIB->createPrefetchT0(PrefetchInst, 
                                 freeReg, 
                                 TopLLCMissInstrs[i]->getOperand(4).getImm(), 
                                 BC.MIB->getNoRegister(), 
                                 TopLLCMissInstrs[i]->getOperand(2).getImm(), 
                                 BC.MIB->getNoRegister(), tmp);
      }
      else if (TopLLCMissInstrs[i]->getOperand(3).getReg()==LoopInductionInstr->getOperand(1).getReg()){
        BC.MIB->createPrefetchT0(PrefetchInst, 
                                 TopLLCMissInstrs[i]->getOperand(1).getReg(), 
                                 TopLLCMissInstrs[i]->getOperand(4).getImm(), 
                                 freeReg, 
                                 TopLLCMissInstrs[i]->getOperand(2).getImm(), 
                                 BC.MIB->getNoRegister(), tmp);
      }
      else{
        llvm::outs()<<"[InjectPrefetchLitePass] TopLLCMiss instr must contain loop induction var\n";
        exit(1);
      }

      PrefetchBBs.back()->addInstruction(PrefetchInst);
    }
  }

  // create unconditional branch at the end of 
  // prefetchBB
  PrefetchBBs.back()->addBranchInstruction(TargetBB);  
  BF.insertBasicBlocks(BoundsCheckBB, std::move(PrefetchBBs));

  // add PrefetchBB to be the successor of the BoundsCheckBB
  BinaryBasicBlock* PrefetchBB = BF.getBasicBlockForLabel(PrefetchBBLabel);    

  return PrefetchBB;
}






std::vector<std::string> InjectPrefetchLitePass::splitLine(std::string str){
   std::vector<std::string> words;
   std::stringstream ss(str);
   std::string tmp;
   while (ss >> tmp){
      words.push_back(tmp);
      tmp.clear();
   }
   return words;
}





std::unordered_map<std::string, std::vector<uint64_t>> InjectPrefetchLitePass::getTopLLCMissLocationFromFile(){
   std::unordered_map<std::string, std::vector<uint64_t>> locations;

   std::string FileName = opts::PrefetchLocationFile; 
   std::fstream f;
   f.open(FileName, std::ios::in); 
    
   if (f.is_open()) { 
      std::string line;
      while (getline(f, line)) { 
         std::vector<std::string> words = splitLine(line);
         std::vector<uint64_t> addrs;
         for (unsigned i=1; i<words.size(); i++){ 
            uint64_t addr = stoi(words[i], 0, 16);
            addrs.push_back(addr);
         }
         locations.insert(make_pair(words[0], addrs));
         llvm::outs() << line << "\n"; 
      }
        
      // Close the file object.
      f.close(); 
   }
   return locations;
}





std::string InjectPrefetchLitePass::removeSuffix(std::string FuncName){
   return FuncName.substr(0, FuncName.find("("));
}





void InjectPrefetchLitePass::runOnFunctions(BinaryContext &BC) {
   if (!opts::InjectPrefetch) return;
   if (opts::PrefetchLocationFile.empty()) return;
   TopLLCMissLocations 
      = getTopLLCMissLocationFromFile();


   for (auto &it: BC.getBinaryFunctions()){
      std::string FunctionFullName = it.second.getDemangledName();
      std::string FuncName = removeSuffix(FunctionFullName);
      if (TopLLCMissLocations.find(FuncName)!=TopLLCMissLocations.end())
         runOnFunction(it.second);
   }
}


} // end namespace bolt
} // end namespace llvm
