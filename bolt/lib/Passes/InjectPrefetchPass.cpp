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

} // namespace opts

namespace llvm {
namespace bolt {

bool InjectPrefetchPass::runOnFunction(BinaryFunction &BF) {

 // if (BF.getOneName() != "_Z7do_workPv") return false;

  BinaryContext& BC = BF.getBinaryContext();
  uint64_t startingAddr = BF.getAddress();
  std::string demangledFuncName = removeSuffix(BF.getDemangledName());
  uint64_t TopLLCMissAddr = TopLLCMissLocations[demangledFuncName];

  llvm::outs()<<"[InjectPrefetchPass] The starting address of do_work is: 0x"
              <<utohexstr(startingAddr)<<"\n";
  llvm::outs()<<"[InjectPrefetchPass] The top llc miss addr is: 0x"
              <<utohexstr(TopLLCMissAddr)<<"\n";


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
      //llvm::outs()<<"[InjectPrefetchPass] addr: "<<utohexstr(AbsoluteAddr)<<"\n";
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
      //llvm::outs()<<"[InjectPrefetchPass] header: "<<utohexstr(AbsoluteAddr)<<"\n";
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


  MCInst PrefetchInst;
  BC.MIB->createPrefetchT0(PrefetchInst, BC.MIB->getX86RAX(), 0, BC.MIB->getNoRegister(), 0, BC.MIB->getNoRegister(), LoadPrefetchAddrInstr);
  Loc = HeaderBB->insertRealInstruction(Loc, PrefetchInst);
  Loc++;  


  // finally pop %rax
  MCInst PopInst; 
  BC.MIB->createPopRegister(PopInst, BC.MIB->getX86RAX(), 8);
  HeaderBB->insertRealInstruction(Loc, PopInst);

  /*------------------no use code-----------------*/
  // if the loop doesn't contain latch, return false
  /*
  SmallVector<BinaryBasicBlock *, 1> Latches;
  OuterLoop->getLoopLatches(Latches);
  llvm::outs()<<"[InjectPrefetchPass] number of latches in the outer loop: "<< Latches.size()<<"\n";

  if (Latches.size()==0) return false;

  for (BinaryBasicBlock *BB : BF.layout()) {
    if (BB->succ_size() != 1 || BB->pred_size() != 1)
      continue;

    BinaryBasicBlock *SuccBB = *BB->succ_begin();
    BinaryBasicBlock *PredBB = *BB->pred_begin();
    const unsigned BBIndex = BB->getLayoutIndex();
    const unsigned SuccBBIndex = SuccBB->getLayoutIndex();
    const uint64_t BBCount = SuccBB->getBranchInfo(*BB).Count;

    BB->setLayoutIndex(SuccBBIndex);
    SuccBB->setLayoutIndex(BBIndex);
  }

  */
   return true;
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
