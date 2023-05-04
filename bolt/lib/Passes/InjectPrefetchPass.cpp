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

  llvm::outs()<<"[InjectPrefetchPass] The starting address of "<<demangledFuncName<<" is: 0x"
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


  // In the loop header, we need to inject prefetch instruction 
  // first Push %rax 
  auto Loc = HeaderBB->begin();

  // inject pop %rax
  MCInst PopInst; 
  BC.MIB->createPopRegister(PopInst, BC.MIB->getX86RAX(), 8);
  HeaderBB->insertRealInstruction(Loc, PopInst);


  // the next part is to inject the boundary check
  // first step is to detect loop induction variable
  // and the gaurd of the outer loop
  SmallVector<BinaryBasicBlock *, 1> Latches;
  OuterLoop->getLoopLatches(Latches);
  llvm::outs()<<"[InjectPrefetchPass] number of latches in the outer loop: "<< Latches.size()<<"\n";

  if (Latches.size()==0) return false;
  
  MCInst* LoopInductionInstr = NULL;
  MCInst* LoopGuardCMPInstr = NULL;
  for (unsigned i=0; i<Latches.size(); i++){
    for (auto I = Latches[i]->begin(); I != Latches[i]->end(); I++){
       MCInst &Inst = *I;
       if (BC.MIB->isADD(Inst)){
          int immValue = Inst.getOperand(2).getImm();
          if (immValue != 1) continue;
          if (!(DemandLoadInstr->getOperand(3).getReg()==Inst.getOperand(0).getReg())) continue;
          LoopInductionInstr = &Inst;
       }
       else if (BC.MIB->isCMP(Inst)){
          if (DemandLoadInstr->getOperand(3).getReg()==Inst.getOperand(0).getReg()){
            LoopGuardCMPInstr = & Inst;
          }
       }
    }
  }



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
  MCInst PushInst2; 
  BC.MIB->createPushRegister(PushInst2, BC.MIB->getX86RAX(), 8);
  BoundCheckBBs.back()->addInstruction(PushInst2);

  // create mov %rdx, %rax
  MCInst MovInstr;
  BC.MIB->createMOV64rr(MovInstr, BC.MIB->getX86RDX(), BC.MIB->getX86RAX());
  BoundCheckBBs.back()->addInstruction(MovInstr);

  // create add %rax 0x20
  int prefetchDist = 64;
  MCInst AddInstr;
  BC.MIB->createADD64ri32(AddInstr, BC.MIB->getX86RAX(), BC.MIB->getX86RAX(), prefetchDist);
  BoundCheckBBs.back()->addInstruction(AddInstr);

  // create comparison instruction
  // cmp 0x18(%rsp), %rax
  int NumOperandsCMP = LoopGuardCMPInstr->getNumOperands();
  MCInst CMPInstr;
  CMPInstr.setOpcode(LoopGuardCMPInstr->getOpcode());
  for (int i=0; i<NumOperandsCMP; i++){
    if (i==0){
      CMPInstr.addOperand(MCOperand::createReg(BC.MIB->getX86RAX()));
    }
    else if (i==4){
      int64_t newDisp = LoopGuardCMPInstr->getOperand(i).getImm() + 8;
      CMPInstr.addOperand(MCOperand::createImm(newDisp));
    }
    else{
      CMPInstr.addOperand(LoopGuardCMPInstr->getOperand(i));
    }
  }
  BoundCheckBBs.back()->addInstruction(CMPInstr);
  // create unconditional branch at the end of this basic block
  //BoundCheckBBs.back()->addBranchInstruction(HeaderBB);

  // insert this Basic Block to binary function
  BF.insertBasicBlocks(PredsOfHeaderBB[1], std::move(BoundCheckBBs));

  BinaryBasicBlock* BoundsCheckBB = BF.getBasicBlockForLabel(BoundsCheckLabel);    

  for (unsigned i=0; i<PredsOfHeaderBB.size(); i++){
    PredsOfHeaderBB[i]->addSuccessor(BoundsCheckBB);
  }



  // create prefetchBB
  // in prefetchBB it contains
  // mov 0x200(%r9,%rdx,8),%rax 
  // prefetcht0 (%rax) 
  MCSymbol *PrefetchBBLabel = BC.Ctx->createNamedTempSymbol("PrefetchBB");
  std::vector<std::unique_ptr<BinaryBasicBlock>> PrefetchBBs;
  PrefetchBBs.emplace_back(BF.createBasicBlock(BinaryBasicBlock::INVALID_OFFSET, PrefetchBBLabel));
  PrefetchBBs.back()->addSuccessor(HeaderBB, 0,0);
  PrefetchBBs.back()->addPredecessor(BoundsCheckBB);

  // add the load instructiona that load the target address
  // for prefetch
  // then load prefetch target's address
  // mov 0x200(%r9,%rdx,8),%rax 
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
       LoadPrefetchAddrInstr.addOperand(MCOperand::createImm(prefetchDist*8));
     }
     else{
       LoadPrefetchAddrInstr.addOperand(DemandLoadInstr->getOperand(i));
     }
  }
  PrefetchBBs.back()->addInstruction(LoadPrefetchAddrInstr);

  // prefetcht0 (%rax) 
  MCInst PrefetchInst;
  BC.MIB->createPrefetchT0(PrefetchInst, BC.MIB->getX86RAX(), 0, BC.MIB->getNoRegister(), 0, BC.MIB->getNoRegister(), LoadPrefetchAddrInstr);
  PrefetchBBs.back()->addInstruction(PrefetchInst);
 
  // create unconditional branch at the end of 
  // prefetchBB
  PrefetchBBs.back()->addBranchInstruction(HeaderBB);  
  BF.insertBasicBlocks(BoundsCheckBB, std::move(PrefetchBBs));

  // add PrefetchBB to be the successor of the BoundsCheckBB
  BinaryBasicBlock* PrefetchBB = BF.getBasicBlockForLabel(PrefetchBBLabel);    
  BoundsCheckBB->addSuccessor(PrefetchBB);

  // add Predecessors to HeaderBB
  HeaderBB->addPredecessor(BoundsCheckBB); 
  HeaderBB->addPredecessor(PrefetchBB);



  // create Branch Instruction at the end of 
  // BoundsCheckBB
  MCInst BoundsCheckBranch;
  BC.MIB->createJZ(BoundsCheckBranch, HeaderBB->getLabel()  , BC.Ctx.get());
  BoundsCheckBB->addInstruction(BoundsCheckBranch);

  // change the pr
  for (int i=0; i<PredsOfHeaderBB.size(); i++){
    MCInst* LastBranch = PredsOfHeaderBB[i]->getLastNonPseudoInstr();
    const MCExpr* LastBranchTargetExpr = LastBranch->getOperand(0).getExpr();
    const MCSymbol* LastBranchTargetSymbol = BC.MIB->getTargetSymbol(LastBranchTargetExpr);
    if (LastBranchTargetSymbol==HeaderBB->getLabel()){
      BC.MIB->replaceBranchTarget(*LastBranch, BoundsCheckBB->getLabel(), BC.Ctx.get());
    }
  }

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
