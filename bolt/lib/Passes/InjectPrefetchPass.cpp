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

bool InjectPrefetchPass::runOnFunction(BinaryFunction &BF) {

  BinaryContext& BC = BF.getBinaryContext();
  uint64_t startingAddr = BF.getAddress();
  int prefetchDist = opts::PrefetchDistance;
  std::string demangledFuncName = removeSuffix(BF.getDemangledName());
  uint64_t TopLLCMissAddr = TopLLCMissLocations[demangledFuncName];

  llvm::outs()<<"[InjectPrefetchPass] The starting address of "<<demangledFuncName<<" is: 0x"
              <<utohexstr(startingAddr)<<"\n";
  llvm::outs()<<"[InjectPrefetchPass] The top llc miss addr is: 0x"
              <<utohexstr(TopLLCMissAddr)<<"\n";


  // get the Instruction and Basic Block that contains 
  // the TOP LLC miss instruction. 
  BinaryBasicBlock* TopLLCMissBB;
  MCInst* TopLLCMissInstr;

  for (auto BBI = BF.begin(); BBI != BF.end(); BBI ++){
    BinaryBasicBlock &BB = *BBI;
    for (auto It = BB.begin(); It != BB.end(); It++){
      MCInst &Instr = *It;
      if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
        uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");        
        if (AbsoluteAddr == TopLLCMissAddr){
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

  // get the loop (OuterLoop) that contains the Top LLC miss BB
  // later on we need to utilize the Header Basic Block of this 
  // loop
  BinaryLoop* OuterLoop = getOuterLoopForBB (BF, TopLLCMissBB);
  if (OuterLoop == NULL){
    llvm::outs()<<"[InjectPrefetchPass] The outer loop that contains top LLC miss BB doesn't exist\n";
    return false;
  }

  // get all destination register of load instructions in the 
  // for loop. and stored them into an set (TODO:I hope this is an 
  // unordered_set) 
  std::set<MCRegister> DstRegsInOuterLoop;
  for (BinaryBasicBlock *BB : OuterLoop->getBlocks()){
    for (auto It = BB->begin(); It != BB->end(); It++){
      MCInst &Instr = *It;
      if ((BC.MIB->isLoad(Instr))&&(TopLLCMissInstr!= (&Instr))){
        MCRegister DstReg = Instr.getOperand(0).getReg();
        if (DstRegsInOuterLoop.find(DstReg)==DstRegsInOuterLoop.end()){
          DstRegsInOuterLoop.insert(DstReg);
        }
      }
    }
  }  

  // get all the predcessor Loads of the Top LLC miss instruction
  // now we only assume one load instruction depends on another load 
  // instruction.
  std::vector<MCInst*> predLoadInstrs;
  MCInst* predLoad = TopLLCMissInstr;
  BinaryBasicBlock* predLoadBB = TopLLCMissBB;
  while (DstRegsInOuterLoop.find(predLoad->getOperand(1).getReg()) != DstRegsInOuterLoop.end()){
    predLoadInstrs.push_back(predLoad);
    auto newDemandLoadPkg = findDemandLoad(BF, OuterLoop, predLoad, predLoadBB);
    predLoad = newDemandLoadPkg.first;
    predLoadBB = newDemandLoadPkg.second;
  }
  predLoadInstrs.push_back(predLoad);
 
  MCInst DemandLoadInstr = *(predLoadInstrs[1]);



  // get the loop header and check if the header is in
  // the loop we want
  // we are going to insert prefetch to the header basic block
  BinaryBasicBlock *HeaderBB = OuterLoop->getHeader();

  // the next part is to inject the boundary check
  // first step is to detect loop induction variable
  // and the gaurd of the outer loop
  SmallVector<BinaryBasicBlock *, 1> Latches;
  OuterLoop->getLoopLatches(Latches);
  llvm::outs()<<"[InjectPrefetchPass] number of latches in the outer loop: "<< Latches.size()<<"\n";

  if (Latches.size()==0) return false;
  
  // Here we assume that LoopInductionInstr is always 
  // LoopGuradCMPInstr. This is a reasonable assumption 
  // because after the loopGuardCMPInstr, the Latch will 
  // be ended with a branch instr.
  MCInst* LoopInductionInstr = NULL;
  MCInst* LoopGuardCMPInstr = NULL;
  for (unsigned i=0; i<Latches.size(); i++){
    for (auto I = Latches[i]->begin(); I != Latches[i]->end(); I++){
       MCInst &Inst = *I;
       if (BC.MIB->isADD(Inst)){
          int immValue = Inst.getOperand(2).getImm();
          if (immValue != 1) continue;
          // the third operand of DemandLoadInstr is the index register
          // namely the loop induction variable
          if (!(DemandLoadInstr.getOperand(3).getReg()==Inst.getOperand(0).getReg())) continue;
          LoopInductionInstr = &Inst;
       }
       else if (BC.MIB->isCMP(Inst)){
          for (unsigned i=0; i<Inst.getNumOperands(); i++){
            if (Inst.getOperand(i).isReg()){
              // the third operand of DemandLoadInstr is the index register
              // namely the loop induction variable
              if (DemandLoadInstr.getOperand(3).getReg()==Inst.getOperand(i).getReg()){
                LoopGuardCMPInstr = & Inst;
              }
              else if (BC.MIB->isLower32bitReg(DemandLoadInstr.getOperand(3).getReg(), Inst.getOperand(i).getReg())){
                LoopGuardCMPInstr = & Inst;
              }
            }
          }
       }
    }
  }

  if (LoopGuardCMPInstr==NULL) {
    llvm::outs()<<"BOLT-ERROR: LoopGuardCMPInstr doesn't exist\n";
    return false;
  }

  std::unordered_set<MCPhysReg> usedRegs;
  for (auto &instr: predLoadInstrs){
    int numOperands = instr->getNumOperands();
    // the first operand of a load instruction is the dst register
    for (int i=1; i<numOperands; i++){
      if (instr->getOperand(i).isReg()){
        if (usedRegs.find(instr->getOperand(i).getReg())==usedRegs.end()){
          usedRegs.insert(instr->getOperand(i).getReg());   
        }
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


  // inject pop %rax to the Loop Header.
  // pop instruction should be the first instruction of 
  // the HeaderBB
  auto Loc = HeaderBB->begin();
  MCInst PopInst; 
  BC.MIB->createPopRegister(PopInst, freeReg, 8);
  HeaderBB->insertRealInstruction(Loc, PopInst);



  // create BoundsCheckBB and PrefetchBB
  SmallVector<BinaryBasicBlock*, 0> PredsOfHeaderBB = HeaderBB->getPredecessors();

  BinaryBasicBlock* BoundsCheckBB = createBoundsCheckBB(BF, HeaderBB, 
                                                        LoopGuardCMPInstr, 
                                                        DemandLoadInstr, 
                                                        prefetchDist,
                                                        freeReg);

  BinaryBasicBlock* PrefetchBB = createPrefetchBB(BF, HeaderBB, BoundsCheckBB, 
                                                  predLoadInstrs, prefetchDist, 
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
    MCInst* LastBranch = PredsOfHeaderBB[i]->getLastNonPseudoInstr();
    const MCExpr* LastBranchTargetExpr = LastBranch->getOperand(0).getExpr();
    const MCSymbol* LastBranchTargetSymbol = BC.MIB->getTargetSymbol(LastBranchTargetExpr);
    if (LastBranchTargetSymbol==HeaderBB->getLabel()){
      BC.MIB->replaceBranchTarget(*LastBranch, BoundsCheckBB->getLabel(), BC.Ctx.get());
    }
  }

  return true;
}





BinaryLoop* InjectPrefetchPass::getOuterLoopForBB( BinaryFunction& BF,
                                                   BinaryBasicBlock* TopLLCMissBB){
  // get loop info of this function
  BF.updateLayoutIndices();

  BinaryDominatorTree DomTree;
  DomTree.recalculate(BF);
  BF.BLI.reset(new BinaryLoopInfo());
  BF.BLI->analyze(DomTree);

  std::vector<BinaryLoop *> OuterLoops;
  BinaryLoop* OuterLoop = NULL;

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
  if (LoopDepth < 2) {
    llvm::outs()<<"[InjectPrefetchPass] The outer loop that contains top LLC miss BB doesn't exist\n";
    return NULL;
  }

  OuterLoop = LoopsContainTopLLCMissBB[LoopDepth-2];

  return OuterLoop;
}






std::pair<MCInst*, BinaryBasicBlock*> InjectPrefetchPass::findDemandLoad(BinaryFunction& BF,
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






BinaryBasicBlock* InjectPrefetchPass::createBoundsCheckBB(BinaryFunction& BF,
                                      BinaryBasicBlock* HeaderBB,
                                      MCInst* LoopGuardCMPInstr,
                                      MCInst DemandLoadInstr,
                                      int prefetchDist,
                                      MCPhysReg freeReg){
  BinaryContext& BC = BF.getBinaryContext();

  // before we change the CFG of this function to add the 
  // BoundCHeckBB and PrefetchBB, we need to save all the 
  // predecessors of the HeaderBB   
  SmallVector<BinaryBasicBlock*, 0> PredsOfHeaderBB = HeaderBB->getPredecessors();
  for (unsigned i=0; i<PredsOfHeaderBB.size(); i++){
    PredsOfHeaderBB[i]->removeSuccessor(HeaderBB);
  }
  HeaderBB->removeAllPredecessors();

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
  for (unsigned i=0; i<PredsOfHeaderBB.size(); i++){
    BinaryBasicBlock* PredOfHeaderBB = PredsOfHeaderBB[i];
    BoundCheckBBs.back()->addPredecessor(PredOfHeaderBB);
  }
  // add successor
  BoundCheckBBs.back()->addSuccessor(HeaderBB, 0,0);

  // add instructions
  // create push %rax 
  MCInst PushInst; 
  BC.MIB->createPushRegister(PushInst, freeReg, 8);
  BoundCheckBBs.back()->addInstruction(PushInst);

  // create mov %rdx, %rax
  MCInst MovInstr;
  BC.MIB->createMOV64rr(MovInstr, BC.MIB->getX86RDX(), freeReg);
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
  bool hasRSP = false;
  for (int i=0; i<NumOperandsCMP; i++){
    if (LoopGuardCMPInstr->getOperand(i).isReg()){
      if (LoopGuardCMPInstr->getOperand(i).getReg()==BC.MIB->getStackPointer()){
        hasRSP = true;
      }
    }
  }

  for (int i=0; i<NumOperandsCMP; i++){
    if (LoopGuardCMPInstr->getOperand(i).isReg()){
      if (DemandLoadInstr.getOperand(3).getReg()==LoopGuardCMPInstr->getOperand(i).getReg()){
          CMPInstr.addOperand(MCOperand::createReg(freeReg));
      }
      else if (BC.MIB->isLower32bitReg(DemandLoadInstr.getOperand(3).getReg(), LoopGuardCMPInstr->getOperand(i).getReg())){
          CMPInstr.addOperand(MCOperand::createReg(freeReg));

      }
      else {
        CMPInstr.addOperand(LoopGuardCMPInstr->getOperand(i));
      }
    }
    else {
      if (LoopGuardCMPInstr->getOperand(i).isImm()){
        int64_t newDisp = LoopGuardCMPInstr->getOperand(i).getImm() + 8;
        CMPInstr.addOperand(MCOperand::createImm(newDisp));
      }
      else{
        CMPInstr.addOperand(LoopGuardCMPInstr->getOperand(i));
      }
    }
  }
  BoundCheckBBs.back()->addInstruction(CMPInstr);

  // insert this Basic Block to binary function
  BF.insertBasicBlocks(PredsOfHeaderBB[1], std::move(BoundCheckBBs));

  BinaryBasicBlock* BoundsCheckBB = BF.getBasicBlockForLabel(BoundsCheckLabel);    

  for (unsigned i=0; i<PredsOfHeaderBB.size(); i++){
    PredsOfHeaderBB[i]->addSuccessor(BoundsCheckBB);
  }

  return BoundsCheckBB;

}





BinaryBasicBlock* InjectPrefetchPass::createPrefetchBB(BinaryFunction& BF,
                                      BinaryBasicBlock* HeaderBB,
                                      BinaryBasicBlock* BoundsCheckBB,
                                      std::vector<MCInst*> predLoadInstrs,
                                      int prefetchDist,
                                      MCPhysReg freeReg){

  BinaryContext& BC = BF.getBinaryContext();
  MCInst DemandLoadInstr = *(predLoadInstrs[1]); 
  // create prefetchBB
  // in prefetchBB it contains
  // mov 0x200(%r9,%rdx,8),%rax 
  // prefetcht0 (%rax) 
  MCSymbol *PrefetchBBLabel = BC.Ctx->createNamedTempSymbol("PrefetchBB");
  std::vector<std::unique_ptr<BinaryBasicBlock>> PrefetchBBs;
  PrefetchBBs.emplace_back(BF.createBasicBlock(BinaryBasicBlock::INVALID_OFFSET, PrefetchBBLabel));
  PrefetchBBs.back()->addSuccessor(HeaderBB, 0,0);
  PrefetchBBs.back()->addPredecessor(BoundsCheckBB);

  // add the load instructiona that compute the target address
  // for prefetch. 
  // Note: here might be a dependency chain. 
  for (unsigned idx = predLoadInstrs.size()-1 ; idx > 1 ; idx --){
    int numOperands = predLoadInstrs[idx]->getNumOperands();
    MCInst predLoad;
    predLoad.setOpcode(predLoadInstrs[idx]->getOpcode());
    for (int i=0; i<numOperands; i++){
      if (i==4){
        if (predLoadInstrs[idx]->getOperand(1).getReg()==BC.MIB->getStackPointer()){
           predLoad.addOperand(MCOperand::createImm(predLoadInstrs[idx]->getOperand(4).getImm()+8));
        }
        else {
          predLoad.addOperand(predLoadInstrs[idx]->getOperand(i));
        }
      }
      else{
        predLoad.addOperand(predLoadInstrs[idx]->getOperand(i));
      }
    }
    PrefetchBBs.back()->addInstruction(predLoad);  
  }

  // create the last load and also change its prefetch distance
  // mov 0x200(%r9,%rdx,8),%rax 
  int numOperands = DemandLoadInstr.getNumOperands();
  MCInst LoadPrefetchAddrInstr;
  LoadPrefetchAddrInstr.setOpcode(DemandLoadInstr.getOpcode());
  for (int i=0; i<numOperands; i++){
     if (i==0){
       // the first operand is the dest reg
       LoadPrefetchAddrInstr.addOperand(MCOperand::createReg(freeReg)); 
     }
     else if (i==4){
       // the 5th operand is the offset
       LoadPrefetchAddrInstr.addOperand(MCOperand::createImm(prefetchDist*8));
     }
     else{
       LoadPrefetchAddrInstr.addOperand(DemandLoadInstr.getOperand(i));
     }
  }
  PrefetchBBs.back()->addInstruction(LoadPrefetchAddrInstr);

  // add prefetch instruction
  // prefetcht0 (%rax) 
  MCInst PrefetchInst;
  BC.MIB->createPrefetchT0(PrefetchInst, freeReg, 0, BC.MIB->getNoRegister(), 0, BC.MIB->getNoRegister(), LoadPrefetchAddrInstr);

  PrefetchBBs.back()->addInstruction(PrefetchInst);
 
  // create unconditional branch at the end of 
  // prefetchBB
  PrefetchBBs.back()->addBranchInstruction(HeaderBB);  
  BF.insertBasicBlocks(BoundsCheckBB, std::move(PrefetchBBs));

  // add PrefetchBB to be the successor of the BoundsCheckBB
  BinaryBasicBlock* PrefetchBB = BF.getBasicBlockForLabel(PrefetchBBLabel);    

  return PrefetchBB;
}






std::vector<std::string> InjectPrefetchPass::splitLine(std::string str){
   std::vector<std::string> words;
   std::stringstream ss(str);
   std::string tmp;
   while (ss >> tmp){
      words.push_back(tmp);
      tmp.clear();
   }
   return words;
}





std::unordered_map<std::string, uint64_t> InjectPrefetchPass::getTopLLCMissLocationFromFile(){
   std::unordered_map<std::string, uint64_t> locations;

   std::string FileName = opts::PrefetchLocationFile; 
   std::fstream f;
   f.open(FileName, std::ios::in); 
    
   if (f.is_open()) { 
      std::string line;
      while (getline(f, line)) { 
         std::vector<std::string> words = splitLine(line); 
         if (words.size()==2){
            uint64_t addr = stoi(words[1], 0, 16);
            locations.insert(make_pair(words[0], addr));
         }
         llvm::outs() << line << "\n"; 
      }
        
      // Close the file object.
      f.close(); 
   }
   return locations;
}





std::string InjectPrefetchPass::removeSuffix(std::string FuncName){
   return FuncName.substr(0, FuncName.find("("));
}





void InjectPrefetchPass::runOnFunctions(BinaryContext &BC) {
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
