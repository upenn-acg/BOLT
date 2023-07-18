//===- bolt/Passes/Prefetchable.cpp ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Prefetchable class.
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/Prefetchable.h"
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
extern cl::opt<bool> TestPrefetchable;
extern cl::opt<std::string> PrefetchLocationFile;
extern cl::opt<std::string> PrefetchableLocationFile;
//extern cl::opt<unsigned> PrefetchDistance;
} // namespace opts

namespace llvm {
namespace bolt {

bool Prefetchable::runOnFunction(BinaryFunction &BF) {
  if (!opts::TestPrefetchable) return false;

  BinaryContext& BC = BF.getBinaryContext();
  uint64_t startingAddr = BF.getAddress();
  //int prefetchDist = opts::PrefetchDistance;
  std::string demangledFuncName = removeSuffix(BF.getDemangledName());
  //std::unordered_set<uint64_t> TopLLCMissAddrs = TopLLCMissLocations[demangledFuncName];
  std::unordered_map<uint64_t, long> TopLLCMissAddrAndPrefDist = TopLLCMissLocations[demangledFuncName];
  //uint64_t TopLLCMissAddr = *(TopLLCMissAddrs.begin());

  llvm::outs()<<"[Prefetchable] The starting address of "<<demangledFuncName<<" is: 0x"
              <<utohexstr(startingAddr)<<"\n";


  std::vector<BinaryBasicBlock*> TopLLCMissBBs;
  std::vector<MCInst*> TopLLCMissInstrs;
  std::vector<TopLLCMissInfo> TopLLCMissInfos;

  for (auto BBI = BF.begin(); BBI != BF.end(); BBI ++){
    BinaryBasicBlock &BB = *BBI;
    for (auto It = BB.begin(); It != BB.end(); It++){
      MCInst &Instr = *It;
      if (BC.MIB->hasAnnotation(Instr, "AbsoluteAddr")){
        uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(Instr, "AbsoluteAddr");        
        if ( TopLLCMissAddrAndPrefDist.find(AbsoluteAddr) != TopLLCMissAddrAndPrefDist.end() ){
          llvm::outs()<<"[Prefetchable] find instruction that causes the TOP LLC miss\n";
          if (BC.MIB->isLoad(Instr)){
            llvm::outs()<<"[Prefetchable] TOP LLC miss instruction is a load\n";           
            TopLLCMissBBs.push_back(&BB);
            TopLLCMissInstrs.push_back(&Instr); 
            TopLLCMissInfo newInfo;
            newInfo.TopLLCMissBB = &BB;
            newInfo.TopLLCMissInstr = &Instr;
            newInfo.prefetchDist = TopLLCMissAddrAndPrefDist[AbsoluteAddr];
            newInfo.prefetchable = true;
            TopLLCMissInfos.push_back(newInfo); 
          }
          else if (BC.MIB->isStore(Instr)){
            llvm::outs()<<"[Prefetchable] TOP LLC miss instruction is a store\n";
          }
          else {
            llvm::outs()<<"[Prefetchable] This pass only inject prefetch for load or store instruction\n";
            return false;
          }
        }
      }
    }
  }

 for (unsigned i=0; i!=TopLLCMissInfos.size(); i++){
    // get the Instruction and Basic Block that contains 
    // the TOP LLC miss instruction. 
    BinaryBasicBlock* TopLLCMissBB = TopLLCMissInfos[i].TopLLCMissBB;
    MCInst* TopLLCMissInstr = TopLLCMissInfos[i].TopLLCMissInstr;
 
    // get the loop (OuterLoop) that contains the Top LLC miss BB
    // later on we need to utilize the Header Basic Block of this 
    // loop
    TopLLCMissInfos[i].OuterLoop = getOuterLoopForBB(BF, TopLLCMissBB);
    TopLLCMissInfos[i].InnerLoop = getInnerLoopForBB(BF, TopLLCMissBB);

    MCInst LoopInductionInstr; 
    MCInst LoopGuardCMPInstr;
    if (TopLLCMissInfos[i].OuterLoop==NULL){
       TopLLCMissInfos[i].prefetchable = false;
       continue;
    }
    if (!getLoopInductionInstrs(BF, TopLLCMissInfos[i].OuterLoop, LoopInductionInstr, LoopGuardCMPInstr)){
       TopLLCMissInfos[i].prefetchable = false;
       continue;
    }
    TopLLCMissInfos[i].OuterLoopInductionReg = LoopInductionInstr.getOperand(0).getReg();

    getLoopInductionInstrs(BF, TopLLCMissInfos[i].InnerLoop, LoopInductionInstr, LoopGuardCMPInstr);
    TopLLCMissInfos[i].InnerLoopInductionReg = LoopInductionInstr.getOperand(0).getReg();

    bool hasInnerLoopInduction = false;
    for (int j=0; j<TopLLCMissInstr->getNumOperands(); j++){
      if (TopLLCMissInstr->getOperand(j).isReg()){
        if (TopLLCMissInstr->getOperand(j).getReg() == TopLLCMissInfos[i].InnerLoopInductionReg){
          hasInnerLoopInduction = true;
          break;
        }
      }
    }
    if (!hasInnerLoopInduction){
       TopLLCMissInfos[i].prefetchable = false;
       continue;
    }
    if (TopLLCMissInfos[i].OuterLoop == NULL){
       TopLLCMissInfos[i].prefetchable = false;
       llvm::outs()<<"[Prefetchable] The outer loop that contains top LLC miss BB doesn't exist\n";
       continue;
    }

    // get all destination register of load instructions in the 
    // for loop. and stored them into an set (TODO:I hope this is an 
    // unordered_set) 
    std::set<MCRegister> DstRegsInOuterLoop;
    for (BinaryBasicBlock *BB : TopLLCMissInfos[i].OuterLoop->getBlocks()){
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
    if (!TopLLCMissInstr->getOperand(1).isReg()) continue;
    MCPhysReg newReg = predLoad->getOperand(1).getReg();
    while ((predLoad->getOperand(1).isReg())&&
           (DstRegsInOuterLoop.find(predLoad->getOperand(1).getReg()) != DstRegsInOuterLoop.end())) {
      predLoadInstrs.push_back(predLoad);
      auto newDemandLoadPkg = findDemandLoad(BF, TopLLCMissInfos[i].OuterLoop, predLoad, predLoadBB);
      predLoad = newDemandLoadPkg.first;
      if (predLoad == NULL) break;

      predLoadBB = newDemandLoadPkg.second;
    }
    if (predLoad !=NULL) predLoadInstrs.push_back(predLoad);
    
    MCInst DemandLoadInstr = *(predLoadInstrs[1]);

    TopLLCMissInfos[i].predLoadInstrs = predLoadInstrs;
    TopLLCMissInfos[i].DemandLoadInstr = DemandLoadInstr;
  }

  BinaryLoop* OuterLoopForAll = NULL;
  BinaryLoop* InnerLoopForAll = NULL;
  for (unsigned i=0; i<TopLLCMissInfos.size(); i++){
    if (TopLLCMissInfos[i].OuterLoop == NULL) continue;
    if (!TopLLCMissInfos[i].prefetchable) continue;
    if ((TopLLCMissInfos[i].TopLLCMissInstr->getOperand(3).getReg()==TopLLCMissInfos[i].InnerLoopInductionReg) &&
        (TopLLCMissInfos[i].DemandLoadInstr.getOperand(3).getReg()==TopLLCMissInfos[i].OuterLoopInductionReg))
    {
      OuterLoopForAll = TopLLCMissInfos[i].OuterLoop;
      InnerLoopForAll = TopLLCMissInfos[i].InnerLoop;
      break; 
    }
  }

  std::vector<TopLLCMissInfo> PrefetchableInfos;
  for (unsigned i=0; i<TopLLCMissInfos.size(); i++){
    if (TopLLCMissInfos[i].OuterLoop == NULL) continue;
    if (!TopLLCMissInfos[i].prefetchable) continue;
    if (TopLLCMissInfos[i].InnerLoop != InnerLoopForAll) continue;
    if (TopLLCMissInfos[i].OuterLoop != OuterLoopForAll) continue;
    if ((TopLLCMissInfos[i].TopLLCMissInstr->getOperand(3).getReg()!=TopLLCMissInfos[i].InnerLoopInductionReg) ||
        (TopLLCMissInfos[i].DemandLoadInstr.getOperand(3).getReg()!=TopLLCMissInfos[i].OuterLoopInductionReg))
    {
      continue;
    }
    PrefetchableInfos.push_back(TopLLCMissInfos[i]); 
  }

  writeTopLLCMissLocationToFile(BF, PrefetchableInfos, demangledFuncName);

  llvm::outs()<<"############# "<<PrefetchableInfos.size()<<"\n";


  return true;
}






BinaryLoop* Prefetchable::getInnerLoopForBB( BinaryFunction& BF,
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
  if (LoopDepth < 2) {
    llvm::outs()<<"[Prefetchable] The outer loop that contains top LLC miss BB doesn't exist\n";
    return NULL;
  }

  InnerLoop = LoopsContainTopLLCMissBB[LoopDepth-1];

  return InnerLoop;
}






BinaryLoop* Prefetchable::getOuterLoopForBB( BinaryFunction& BF,
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
    llvm::outs()<<"[Prefetchable] The outer loop that contains top LLC miss BB doesn't exist\n";
    return NULL;
  }

  OuterLoop = LoopsContainTopLLCMissBB[LoopDepth-2];

  return OuterLoop;
}




bool Prefetchable::getLoopInductionInstrs(BinaryFunction& BF, 
                                          BinaryLoop* Loop,
                                          MCInst& LoopInductionInstr,
                                          MCInst& LoopGuardCMPInstr){

  BinaryContext& BC = BF.getBinaryContext();
  SmallVector<BinaryBasicBlock *, 1> Latches;
  Loop->getLoopLatches(Latches);
  llvm::outs()<<"[Prefetchable] number of latches in the outer loop: "<< Latches.size()<<"\n";

  if (Latches.size()==0) return false;
  
  // Here we assume that LoopInductionInstr is always 
  // LoopGuradCMPInstr. This is a reasonable assumption 
  // because after the loopGuardCMPInstr, the Latch will 
  // be ended with a branch instr.
  for (unsigned i=0; i<Latches.size(); i++){
    bool findLoopInduction = false;
    for (auto I = Latches[i]->begin(); I != Latches[i]->end(); I++){
      MCInst &Inst = *I;
      if (BC.MIB->isADD(Inst)){
        if ((Inst.getOperand(2).isImm()) &&
            (Inst.getOperand(0).getReg()==Inst.getOperand(1).getReg())){
           LoopInductionInstr = (*I);
           findLoopInduction = true;
        }
      }
      else if (BC.MIB->isCMP(Inst)){
        if (findLoopInduction){
          for (unsigned i=0; i<Inst.getNumOperands(); i++){
            if (Inst.getOperand(i).isReg()){
              // the third operand of DemandLoadInstr is the index register
              // namely the loop induction variable
              if (LoopInductionInstr.getOperand(0).getReg()==Inst.getOperand(i).getReg()){
                LoopGuardCMPInstr = (*I);
                return true;
              }
              else if ((BC.MIB->is64bitReg(LoopInductionInstr.getOperand(0).getReg()))
                      &&(BC.MIB->isLower32bitReg(LoopInductionInstr.getOperand(0).getReg(), Inst.getOperand(i).getReg()))){
                LoopGuardCMPInstr =(*I);
                return true;
              }
            }
          }
        }
      }
    }
  }
  return false;
}





MCPhysReg Prefetchable::getLoopInductionReg(BinaryFunction& BF,
                                            MCInst& LoopInductionInstr,
                                            MCInst& LoopGuardCMPInstr){
  BinaryContext& BC = BF.getBinaryContext();
  MCPhysReg LoopInductionReg;
  for (int i=0; i<LoopGuardCMPInstr.getNumOperands(); i++){
    if (LoopGuardCMPInstr.getOperand(i).isReg()){
      if (LoopGuardCMPInstr.getOperand(i).getReg()==LoopInductionInstr.getOperand(0).getReg()){
        LoopInductionReg = LoopGuardCMPInstr.getOperand(i).getReg();
        break; 
      }
    }
  }
  return LoopInductionReg;
}






std::pair<MCInst*, BinaryBasicBlock*> Prefetchable::findDemandLoad(BinaryFunction& BF,
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







std::vector<std::string> Prefetchable::splitLine(std::string str){
   std::vector<std::string> words;
   std::stringstream ss(str);
   std::string tmp;
   while (ss >> tmp){
      words.push_back(tmp);
      tmp.clear();
   }
   return words;
}





std::unordered_map<std::string, std::unordered_map<uint64_t, long>> Prefetchable::getTopLLCMissLocationFromFile(){
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
            llvm::outs()<<"[Prefetchable] Error: The format of the prefetch-loc-file is wrong\n";
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





void Prefetchable::writeTopLLCMissLocationToFile(BinaryFunction& BF,
                                                 std::vector<TopLLCMissInfo> prefetchableInfos, 
                                                 std::string FuncName){
   BinaryContext& BC = BF.getBinaryContext();
   std::string FileName = opts::PrefetchLocationFile; 
   std::fstream f;
   f.open(FileName, std::ios::out); 
 
   if (f.is_open()) { 
      f << FuncName << " ";
      for (unsigned i=0; i<prefetchableInfos.size(); i++){
         if (BC.MIB->hasAnnotation(*(prefetchableInfos[i].TopLLCMissInstr), "AbsoluteAddr")){
            uint64_t AbsoluteAddr = (uint64_t)BC.MIB->getAnnotationAs<uint64_t>(*(prefetchableInfos[i].TopLLCMissInstr), "AbsoluteAddr");
            f<<utohexstr(AbsoluteAddr)<<" 512 "; 
         }
      }
      // Close the file object.
      f.close(); 
   }
}







std::string Prefetchable::removeSuffix(std::string FuncName){
   return FuncName.substr(0, FuncName.find("("));
}





void Prefetchable::runOnFunctions(BinaryContext &BC) {
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
