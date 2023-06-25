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

#include "bolt/Passes/InjectPrefetchInnerLoop.h"
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
//extern cl::opt<unsigned> PrefetchDistance;
} // namespace opts

namespace llvm {
namespace bolt {

bool InjectPrefetchInnerLoop::runOnFunction(BinaryFunction &BF) {

  BinaryContext& BC = BF.getBinaryContext();
  uint64_t startingAddr = BF.getAddress();
//  int prefetchDist = opts::PrefetchDistance;
  std::string demangledFuncName = removeSuffix(BF.getDemangledName());
  std::unordered_map<uint64_t, long> TopLLCMissAddrs = TopLLCMissLocations[demangledFuncName];
  uint64_t TopLLCMissAddr = TopLLCMissAddrs.begin()->second;

  llvm::outs()<<"[InjectPrefetchInnerLoop] The starting address of "<<demangledFuncName<<" is: 0x"
              <<utohexstr(startingAddr)<<"\n";
  llvm::outs()<<"[InjectPrefetchInnerLoop] The top llc miss addr is: 0x"
              <<utohexstr(TopLLCMissAddr)<<"\n";

  std::vector<BinaryBasicBlock*> TopLLCMissBBs;
  std::vector<MCInst*> TopLLCMissInstrs;
  std::vector<TopLLCMissInfo> TopLLCMissInfos;

  for (auto BBI = BF.begin(); BBI != BF.end(); BBI ++){
    BinaryBasicBlock &BB = *BBI;
    for (auto It = BB.begin(); It != BB.end(); It++){
      MCInst &Instr = *It;
      if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
        uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");        
        if ( TopLLCMissAddrs.find(AbsoluteAddr) != TopLLCMissAddrs.end() ){
          llvm::outs()<<"[InjectPrefetchInnerLoop] find instruction that causes the TOP LLC miss\n";
          if (BC.MIB->isLoad(Instr)){
            llvm::outs()<<"[InjectPrefetchInnerLoop] TOP LLC miss instruction is a load\n";         
          }
          else if (BC.MIB->isStore(Instr)){
            llvm::outs()<<"[InjectPrefetchInnerLoop] TOP LLC miss instruction is a store\n";
          }
          else {
            llvm::outs()<<"[InjectPrefetchInnerLoop] This pass only inject prefetch for load or store instruction\n";
            return false;
          }
          TopLLCMissBBs.push_back(&BB);
          TopLLCMissInstrs.push_back(&Instr); 
          TopLLCMissInfo newInfo;
          newInfo.TopLLCMissBB = &BB;
          newInfo.TopLLCMissInstr = &Instr;
          newInfo.PrefetchDist = TopLLCMissAddrs[AbsoluteAddr];
          TopLLCMissInfos.push_back(newInfo); 
          // TopLLCMissBB = &BB;
          // TopLLCMissInstr = &Instr;
        }
      }
    }
  }

  for (unsigned i=0; i!=TopLLCMissInfos.size(); i++){
    // get the Instruction and Basic Block that contains 
    // the TOP LLC miss instruction. 

    if (i!=0){
      TopLLCMissInfos.clear();
      for (auto BBI = BF.begin(); BBI != BF.end(); BBI ++){
        BinaryBasicBlock &BB = *BBI;
        for (auto It = BB.begin(); It != BB.end(); It++){
          MCInst &Instr = *It;
          if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
            uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");        
            if ( TopLLCMissAddrs.find(AbsoluteAddr) != TopLLCMissAddrs.end() ){
              llvm::outs()<<"[InjectPrefetchInnerLoop] find instruction that causes the TOP LLC miss\n";
              if (BC.MIB->isLoad(Instr)){
                llvm::outs()<<"[InjectPrefetchInnerLoop] TOP LLC miss instruction is a load\n";         
              }
              else if (BC.MIB->isStore(Instr)){
                llvm::outs()<<"[InjectPrefetchInnerLoop] TOP LLC miss instruction is a store\n";
              }
              else {
                llvm::outs()<<"[InjectPrefetchInnerLoop] This pass only inject prefetch for load or store instruction\n";
                return false;
              }
              TopLLCMissBBs.push_back(&BB);
              TopLLCMissInstrs.push_back(&Instr); 
              TopLLCMissInfo newInfo;
              newInfo.TopLLCMissBB = &BB;
              newInfo.TopLLCMissInstr = &Instr;
              TopLLCMissInfos.push_back(newInfo); 
              // TopLLCMissBB = &BB;
              // TopLLCMissInstr = &Instr;
            }
          }
        }
      }
    }


    BinaryBasicBlock* TopLLCMissBB = TopLLCMissInfos[i].TopLLCMissBB;
    MCInst* TopLLCMissInstrP = TopLLCMissInfos[i].TopLLCMissInstr;

    SmallVector<BinaryBasicBlock*, 0> PredsOfTopLLCMissBB = TopLLCMissBB->getPredecessors();
    bool BBcontainsLoop = false; 
    for (unsigned j=0; j<PredsOfTopLLCMissBB.size(); j++){
      if (PredsOfTopLLCMissBB[j] == TopLLCMissBB){
        BBcontainsLoop = true;
      }
    }
    if (!BBcontainsLoop){
      llvm::outs()<<"[InjectPrefetchInnerLoop] TopLLCMissBB is not a loop\n";
      return false;
    }
  
    // get the loop (OuterLoop) that contains the Top LLC miss BB
    // later on we need to utilize the Header Basic Block of this 
    // loop
    TopLLCMissInfos[i].InnerLoop = getInnerLoopForBB (BF, TopLLCMissBB);

    if (TopLLCMissInfos[i].InnerLoop == NULL){
      llvm::outs()<<"[InjectPrefetchInnerLoop] The inner loop that contains top LLC miss BB doesn't exist\n";
      return false;
    }

    BinaryBasicBlock* HeaderBB = TopLLCMissInfos[i].InnerLoop->getHeader();
    SmallVector<BinaryBasicBlock*, 0> PredsOfHeaderBB = HeaderBB->getPredecessors();

    MCInst* LoopInductionInstr = NULL;
    MCInst* LoopGuardCMPInstr = NULL;
    for (auto I = TopLLCMissBB->begin(); I != TopLLCMissBB->end(); I++){
      MCInst &Inst = *I;
      if (BC.MIB->isADD(Inst)){
        if (!Inst.getOperand(2).isImm())
          continue;
        if (Inst.getOperand(0).getReg()!=Inst.getOperand(1).getReg())
          continue;
        // the third operand of DemandLoadInstr is the index register
        // namely the loop induction variable
        LoopInductionInstr = &Inst;
      }
      else if (BC.MIB->isCMP(Inst)){
        if (LoopInductionInstr){
          for (unsigned i=0; i<Inst.getNumOperands(); i++){
            if (Inst.getOperand(i).isReg()){
              // the third operand of DemandLoadInstr is the index register
              // namely the loop induction variable
              if (LoopInductionInstr->getOperand(0).getReg()==Inst.getOperand(i).getReg()){
                LoopGuardCMPInstr = & Inst;
              }
            }
          }
        }
      }
    }

    std::vector<MCInst*>predInstrs = getPredInstrs ( BF, TopLLCMissBB, LoopInductionInstr, TopLLCMissInstrP); 
 
    std::unordered_map<MCPhysReg, MCPhysReg> dstRegMapTable = getDstRegMapTable(BF,predInstrs, TopLLCMissInstrP);

    llvm::outs()<<"@@@@@@@ predInstrs.size()="<<predInstrs.size()<<"\n";
    llvm::outs()<<"@@@@@@@ dstRegMapTable.size()="<<dstRegMapTable.size()<<"\n";

    std::vector<MCInst> predInstrsForPrefetch = getPredInstrsForPrefetch ( BF, LoopInductionInstr, TopLLCMissInstrP,
                                                                           predInstrs, dstRegMapTable,
                                                                           TopLLCMissInfos[i].PrefetchDist );


    BinaryBasicBlock* BoundsCheckBB = createBoundsCheckBB(BF, HeaderBB, LoopGuardCMPInstr, LoopInductionInstr,
                                                          predInstrs, dstRegMapTable, TopLLCMissInfos[i].PrefetchDist);


    BinaryBasicBlock* PrefetchBB = createPrefetchBB( BF, HeaderBB, BoundsCheckBB,
                                                     TopLLCMissInstrP, LoopInductionInstr,
                                                     dstRegMapTable, TopLLCMissInfos[i].PrefetchDist, 
                                                     predInstrsForPrefetch);


 //   BoundsCheckBB->addSuccessor(PrefetchBB);
//    BoundsCheckBB->addSuccessor(HeaderBB);
//    PrefetchBB->addSuccessor(HeaderBB);
//    PrefetchBB->addBranchInstruction(HeaderBB);
 
    for (unsigned i=0; i<PredsOfHeaderBB.size(); i++){
      //MCInst* LastBranch = PredsOfHeaderBB[i]->getLastNonPseudoInstr();
      
      MCInst* LastBranch = NULL;
      for (auto it = PredsOfHeaderBB[i]->begin(); it != PredsOfHeaderBB[i]->end(); it++){
        if (BC.MIB->isBranch(*it)){
           LastBranch = &(*it);
           break;
        } 
      }
      
      if ((LastBranch) && (BC.MIB->isBranch(*LastBranch))){
        const MCExpr* LastBranchTargetExpr = LastBranch->getOperand(0).getExpr();
        const MCSymbol* LastBranchTargetSymbol = BC.MIB->getTargetSymbol(LastBranchTargetExpr);
        if (LastBranchTargetSymbol==HeaderBB->getLabel()){
          BC.MIB->replaceBranchTarget(*LastBranch, BoundsCheckBB->getLabel(), BC.Ctx.get());
        }
        else{
      //    PredsOfHeaderBB[i]->addBranchInstruction(BoundsCheckBB);
        }
      }
    } 

    // add pop instruction in Header Basic Blocks
    std::vector<MCPhysReg> regsToBePoped; 
    for (auto it = dstRegMapTable.begin(); it != dstRegMapTable.end(); it++){
      regsToBePoped.push_back(it->second);
    }
    reverse(regsToBePoped.begin(), regsToBePoped.end());

    MCInst BoundsCheckBranch;
    BC.MIB->createJZ(BoundsCheckBranch, HeaderBB->getLabel(), BC.Ctx.get());
    BoundsCheckBB->addInstruction(BoundsCheckBranch);

    auto loc = HeaderBB->begin();
    for (auto freeReg: regsToBePoped){
      MCInst PopInst;
      BC.MIB->createPopRegister(PopInst, freeReg, 8);
      loc = HeaderBB->insertRealInstruction(loc, PopInst);
      loc++; 
    }

    break; 
  }


  return true;
}





BinaryLoop* InjectPrefetchInnerLoop::getInnerLoopForBB( BinaryFunction& BF,
                                                            BinaryBasicBlock* TopLLCMissBB){

  // get loop info of this function
  BF.updateLayoutIndices();

  BinaryDominatorTree DomTree;
  DomTree.recalculate(BF);
  BF.BLI.reset(new BinaryLoopInfo());
  BF.BLI->analyze(DomTree);

  std::vector<BinaryLoop *> OuterLoops;
  BinaryLoop* InnerLoop = NULL;

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
  if (LoopDepth < 1) {
    llvm::outs()<<"[InjectPrefetchInnerLoop] The inner loop that contains top LLC miss BB doesn't exist\n";
    return NULL;
  }

  InnerLoop = LoopsContainTopLLCMissBB[LoopDepth-1];

  return InnerLoop;
}








BinaryLoop* InjectPrefetchInnerLoop::getOuterLoopForBB( BinaryFunction& BF,
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
    llvm::outs()<<"[InjectPrefetchInnerLoop] The outer loop that contains top LLC miss BB doesn't exist\n";
    return NULL;
  }

  OuterLoop = LoopsContainTopLLCMissBB[LoopDepth-2];

  return OuterLoop;
}






std::pair<MCInst*, BinaryBasicBlock*> InjectPrefetchInnerLoop::findDemandLoad(BinaryFunction& BF,
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
          if ((BC.MIB->isLoad(Instr)) &&(!BC.MIB->isCMP(Instr))&& (Instr.getOperand(0).getReg()==DemandLoadDstRegNum)){
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




std::vector<MCInst*> InjectPrefetchInnerLoop::getPredInstrs ( BinaryFunction& BF,
                                                                  BinaryBasicBlock* TopLLCMissBB,
                                                                  MCInst* LoopInductionInstr,
                                                                  MCInst* TopLLCMissInstrP){ 
  BinaryContext& BC = BF.getBinaryContext();

  auto loc_prefetch = TopLLCMissBB->begin();
  auto loc_predStart = TopLLCMissBB->begin();

  for (auto I = TopLLCMissBB->begin(); I != TopLLCMissBB->end(); I++){
    if (&(*I) == TopLLCMissInstrP) {
      loc_prefetch = I;
      break;
    }
  }    

  MCPhysReg LoopInductionReg = LoopInductionInstr->getOperand(0).getReg();  

  for (auto I = loc_prefetch; ; I--){
    if (BC.MIB->isStore(*I)) continue;
    MCInst instr = *I;
    int numOperands = instr.getNumOperands();
    bool findLocPredStart = false;
    for (int i=1; i<numOperands; i++){
      if ((instr.getOperand(i).isReg()) && (instr.getOperand(i).getReg()==LoopInductionReg)){
        loc_predStart = I;
        findLocPredStart = true;
        break;
      }
    }
    if (findLocPredStart) break;
    if (I==TopLLCMissBB->begin()) break;
  }

  std::vector<MCInst*> predInstrs;
  for (auto I = loc_predStart; I != loc_prefetch; I++) {
    if (BC.MIB->isStore(*I)) continue;
    predInstrs.push_back(&(*I));
  }

  return predInstrs;
}


std::unordered_map<MCPhysReg, MCPhysReg> InjectPrefetchInnerLoop::getDstRegMapTable ( BinaryFunction& BF,
                                                                                          std::vector<MCInst*> predInstrs,
                                                                                          MCInst* TopLLCMissInstrP){
  BinaryContext& BC = BF.getBinaryContext();

  std::unordered_set<MCPhysReg> usedRegs;
  for (auto instr: predInstrs){
    int numOperands = instr->getNumOperands();
    for (int j=0; j<numOperands; j++){
      if (instr->getOperand(j).isReg()){
        MCPhysReg reg = instr->getOperand(j).getReg();
        if (BC.MIB->is32bitReg(reg)) reg = BC.MIB->get64bitReg(reg);
        if (usedRegs.find(reg) == usedRegs.end()) usedRegs.insert(reg);
      }
    }
  }

  for (int i=0; i<TopLLCMissInstrP->getNumOperands(); i++){
    if (TopLLCMissInstrP->getOperand(i).isReg()){
      MCPhysReg reg = TopLLCMissInstrP->getOperand(i).getReg();
      if (usedRegs.find(reg) == usedRegs.end()) usedRegs.insert(reg);
    }
  }

  llvm::outs()<<"number of used regs: "<<usedRegs.size()<<"\n";

  std::unordered_map<MCPhysReg, MCPhysReg> dstRegMapTable;
  for (auto instr: predInstrs){
    if (!BC.MIB->isStore(*instr)){
      if (instr->getOperand(0).isReg()){
        MCPhysReg dstReg = instr->getOperand(0).getReg();
        if (BC.MIB->is32bitReg(dstReg)) dstReg = BC.MIB->get64bitReg(dstReg);
        if ((dstRegMapTable.find(dstReg))==(dstRegMapTable.end())){
          MCPhysReg newReg = BC.MIB->getUnusedReg(usedRegs);
          usedRegs.insert(newReg);
          dstRegMapTable.insert(std::make_pair(dstReg, newReg));
        }
      } 
    }
  }

  if (predInstrs.size()==0){
    if (TopLLCMissInstrP->getOperand(0).isReg()){
      MCPhysReg dstReg = TopLLCMissInstrP->getOperand(0).getReg();
      if (BC.MIB->is32bitReg(dstReg)) dstReg = BC.MIB->get64bitReg(dstReg);
      if ((dstRegMapTable.find(dstReg))==(dstRegMapTable.end())){
        MCPhysReg newReg = BC.MIB->getUnusedReg(usedRegs);
        usedRegs.insert(newReg);
        dstRegMapTable.insert(std::make_pair(dstReg, newReg));
      }
    } 
  }

  llvm::outs()<<"number of predInstrs: "<<predInstrs.size()<<"\n";
  llvm::outs()<<"number of dstRegs: "<<dstRegMapTable.size()<<"\n";
  return dstRegMapTable;
}





std::vector<MCInst> InjectPrefetchInnerLoop::getPredInstrsForPrefetch ( BinaryFunction& BF,
                                                                            MCInst* LoopInductionInstr,
                                                                            MCInst* TopLLCMissInstrP,
                                                                            std::vector<MCInst*> predInstrs,
                                                                            std::unordered_map<MCPhysReg, MCPhysReg> dstRegMapTable,
                                                                            int prefetchDist  ){
  BinaryContext& BC = BF.getBinaryContext();

  std::vector<MCInst> predInstrsForPrefetch;


  if (predInstrs.size()!=0){
    for (auto instr: predInstrs){
      MCInst newInstr;
      if (BC.MIB->isStore(*instr)) continue;
      newInstr.setOpcode(instr->getOpcode());
      for (int j=0; j<instr->getNumOperands(); j++){
        if (instr->getOperand(j).isReg()){
          MCPhysReg reg = instr->getOperand(j).getReg();
          if (BC.MIB->is32bitReg(reg)) reg = BC.MIB->get64bitReg(reg);
          if (dstRegMapTable.find(reg) != dstRegMapTable.end()) {
            newInstr.addOperand(MCOperand::createReg(dstRegMapTable[reg]));
          }
          else{
            newInstr.addOperand(instr->getOperand(j));
          }
        }
        else {
          newInstr.addOperand(instr->getOperand(j));
        }
      }
      predInstrsForPrefetch.push_back(newInstr);
    }

    MCInst newPredInstr;
    newPredInstr.setOpcode(predInstrsForPrefetch[0].getOpcode());
    for (int j=0; j<predInstrsForPrefetch[0].getNumOperands(); j++){
      if (j!=4){
        newPredInstr.addOperand(predInstrsForPrefetch[0].getOperand(j));
      }
      else{
        newPredInstr.addOperand(MCOperand::createImm(prefetchDist));
      }
    }
    predInstrsForPrefetch[0] = newPredInstr;
  }
  return predInstrsForPrefetch;
} 






BinaryBasicBlock* InjectPrefetchInnerLoop::createBoundsCheckBB(BinaryFunction& BF,
                                                                   BinaryBasicBlock* HeaderBB,
                                                                   MCInst* LoopGuardCMPInstr,
                                                                   MCInst* LoopInductionInstr,
                                                                   std::vector<MCInst*> predInstrs,
                                                                   std::unordered_map<MCPhysReg, MCPhysReg> dstRegMapTable,
                                                                   int prefetchDist){
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
  BoundCheckBBs.back()->addSuccessor(HeaderBB, 0, 0);

  // add instructions
  // create push %rax
  for (auto it = dstRegMapTable.begin(); it != dstRegMapTable.end(); it++){ 
    MCInst PushInst; 
    BC.MIB->createPushRegister(PushInst, it->second, 8);
    BoundCheckBBs.back()->addInstruction(PushInst);
  }

  MCPhysReg freeReg = dstRegMapTable.begin()->second;
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

  for (int i=0; i<NumOperandsCMP; i++){
    if (LoopGuardCMPInstr->getOperand(i).isReg()){
      if (LoopInductionInstr->getOperand(0).getReg()==LoopGuardCMPInstr->getOperand(i).getReg()){
          CMPInstr.addOperand(MCOperand::createReg(freeReg));
      }
      else if (BC.MIB->isLower32bitReg(LoopInductionInstr->getOperand(0).getReg(), LoopGuardCMPInstr->getOperand(i).getReg())){
          CMPInstr.addOperand(MCOperand::createReg(freeReg));

      }
      else {
        CMPInstr.addOperand(LoopGuardCMPInstr->getOperand(i));
      }
    }
    else {
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
  }
  BoundCheckBBs.back()->addInstruction(CMPInstr);

  // insert this Basic Block to binary function
  // TODO: change the way to decide the BB for injecting
  BF.insertBasicBlocks(PredsOfHeaderBB[1], std::move(BoundCheckBBs));

  BinaryBasicBlock* BoundsCheckBB = BF.getBasicBlockForLabel(BoundsCheckLabel);    

  for (unsigned i=0; i<PredsOfHeaderBB.size(); i++){
    PredsOfHeaderBB[i]->addSuccessor(BoundsCheckBB);
  }

  return BoundsCheckBB;
}





BinaryBasicBlock* InjectPrefetchInnerLoop::createPrefetchBB(BinaryFunction& BF,
                                                                BinaryBasicBlock* HeaderBB,
                                                                BinaryBasicBlock* BoundsCheckBB,
                                                                MCInst* TopLLCMissInstrP,
                                                                MCInst* LoopInductionInstr,
                                                                std::unordered_map<MCPhysReg, MCPhysReg> dstRegMapTable,
                                                                int prefetchDist,
                                                                std::vector<MCInst> predInstrsForPrefetch){

  BinaryContext& BC = BF.getBinaryContext();
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

  if (predInstrsForPrefetch.size()!=0){
    for (unsigned j=0; j<predInstrsForPrefetch.size(); j++){
      PrefetchBBs.back()->addInstruction(predInstrsForPrefetch[j]);
    }

    MCInst TopLLCMissInstr = *TopLLCMissInstrP; 

    MCInst TopLLCMissInstrNew;
    TopLLCMissInstrNew.setOpcode(TopLLCMissInstr.getOpcode());
    for (int j=0; j<TopLLCMissInstr.getNumOperands(); j++){
      if (TopLLCMissInstr.getOperand(j).isReg()){
        MCPhysReg reg = TopLLCMissInstr.getOperand(j).getReg();
        if (BC.MIB->is32bitReg(reg)) reg = BC.MIB->get64bitReg(reg);
        if (dstRegMapTable.find(reg) != dstRegMapTable.end()) {
          TopLLCMissInstrNew.addOperand(MCOperand::createReg(dstRegMapTable[reg]));
        }
        else{
          TopLLCMissInstrNew.addOperand(TopLLCMissInstr.getOperand(j));
        }
      }
      else {
        TopLLCMissInstrNew.addOperand(TopLLCMissInstr.getOperand(j));
      }
    }

    MCInst PrefetchInst;
    MCInst LoadPrefetchAddrInstr;
    BC.MIB->createPrefetchT0(PrefetchInst, TopLLCMissInstrNew.getOperand(0).getReg(), 0, TopLLCMissInstrNew.getOperand(2).getReg(), TopLLCMissInstrNew.getOperand(1).getImm(), BC.MIB->getNoRegister(), LoadPrefetchAddrInstr); 
    PrefetchBBs.back()->addInstruction(PrefetchInst);
  }
  else{
    MCPhysReg freeReg = dstRegMapTable.begin()->second;
    MCInst Lea64rInstr;
    if (TopLLCMissInstrP->getOperand(1).getReg()==LoopInductionInstr->getOperand(1).getReg()){
      BC.MIB->createLEA64r(Lea64rInstr, TopLLCMissInstrP->getOperand(1).getReg(), 1, BC.MIB->getNoRegister(), prefetchDist, BC.MIB->getNoRegister(), freeReg );
    }
    else if  (TopLLCMissInstrP->getOperand(3).getReg()==LoopInductionInstr->getOperand(1).getReg()){
      BC.MIB->createLEA64r(Lea64rInstr, TopLLCMissInstrP->getOperand(3).getReg(), 1, BC.MIB->getNoRegister(), prefetchDist, BC.MIB->getNoRegister(), freeReg );
    }
    PrefetchBBs.back()->addInstruction(Lea64rInstr);

    MCInst PrefetchInst;
    MCInst tmp;
    if (TopLLCMissInstrP->getOperand(1).getReg()==LoopInductionInstr->getOperand(1).getReg()){
      BC.MIB->createPrefetchT0Expr(PrefetchInst, freeReg, TopLLCMissInstrP->getOperand(4).getExpr(), BC.MIB->getNoRegister(), TopLLCMissInstrP->getOperand(2).getImm(), BC.MIB->getNoRegister(), tmp);
    }
    else if (TopLLCMissInstrP->getOperand(3).getReg()==LoopInductionInstr->getOperand(1).getReg()){
      BC.MIB->createPrefetchT0Expr(PrefetchInst, TopLLCMissInstrP->getOperand(1).getReg(), TopLLCMissInstrP->getOperand(4).getExpr(), freeReg, TopLLCMissInstrP->getOperand(2).getImm(), BC.MIB->getNoRegister(), tmp);
    }
    else{
      llvm::outs()<<"[InjectPrefetchLite] TopLLCMiss instr must contain loop induction var\n";
      exit(1);
    }

    PrefetchBBs.back()->addInstruction(PrefetchInst);

  }
  // create unconditional branch at the end of 
  // prefetchBB
  PrefetchBBs.back()->addBranchInstruction(HeaderBB);  

  // add PrefetchBB to be the successor of the BoundsCheckBB
  BF.insertBasicBlocks(BoundsCheckBB, std::move(PrefetchBBs));
  BinaryBasicBlock* PrefetchBB = BF.getBasicBlockForLabel(PrefetchBBLabel);    

  return PrefetchBB;
}






std::vector<std::string> InjectPrefetchInnerLoop::splitLine(std::string str){
   std::vector<std::string> words;
   std::stringstream ss(str);
   std::string tmp;
   while (ss >> tmp){
      words.push_back(tmp);
      tmp.clear();
   }
   return words;
}





std::unordered_map<std::string, std::unordered_map<uint64_t, long>> InjectPrefetchInnerLoop::getTopLLCMissLocationFromFile(){
   std::unordered_map<std::string, std::unordered_map<uint64_t, long>> locations;

   std::string FileName = opts::PrefetchLocationFile; 
   std::fstream f;
   f.open(FileName, std::ios::in); 
    
   if (f.is_open()) { 
      std::string line;
      while (getline(f, line)) { 
         std::vector<std::string> words = splitLine(line);
         std::unordered_map<uint64_t, long> addrAndPrefDist;
         if (words.size()%2==0){
            llvm::outs()<<"[InjectPrefetchPass] Error: The format of the prefetch-loc-file is wrong\n";
            exit(1);
         }
         for (unsigned i=1; i<words.size(); i=i+2){ 
            uint64_t addr = stoi(words[i], 0, 16);
            long pref_dist = stoi(words[i+1],0, 10);
            addrAndPrefDist.insert(std::make_pair(addr, pref_dist));
         }
         locations.insert(std::make_pair(words[0], addrAndPrefDist));
         llvm::outs() << line << "\n"; 
      }
        
      // Close the file object.
      f.close(); 
   }
   return locations;
}






std::string InjectPrefetchInnerLoop::removeSuffix(std::string FuncName){
   return FuncName.substr(0, FuncName.find("("));
}





void InjectPrefetchInnerLoop::runOnFunctions(BinaryContext &BC) {
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
