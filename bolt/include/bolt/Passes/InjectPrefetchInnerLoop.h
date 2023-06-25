//===- bolt/Passes/LoopInversionPass.h --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_INJECTPREFETCHINNERLOOP_H
#define BOLT_PASSES_INJECTPREFETCHINNERLOOP_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class InjectPrefetchInnerLoop : public BinaryFunctionPass {
private:
  std::unordered_map<std::string, std::unordered_map<uint64_t,long>> TopLLCMissLocations;
  typedef struct TopLLCMissInfo{
    BinaryBasicBlock* TopLLCMissBB;
    MCInst* TopLLCMissInstr;
    BinaryLoop* OuterLoop;
    BinaryLoop* InnerLoop;
    std::vector<MCInst*> predLoadInstrs;
    MCInst DemandLoadInstr;  // the second element of predLoadInstrs
    MCPhysReg freeReg;
    long PrefetchDist;
  } TopLLCMissInfo;

public:
  explicit InjectPrefetchInnerLoop() : BinaryFunctionPass(false) {}

  const char *getName() const override { return "inject-prefetch"; }
  /// Helper functions
  //std::unordered_map<std::string, uint64_t> getTopLLCMissLocationFromFile();
  std::vector<std::string> splitLine(std::string);
  std::string removeSuffix(std::string);
  std::unordered_map<std::string, std::unordered_map<uint64_t, long>> getTopLLCMissLocationFromFile();
  /// real functions
  void runOnFunctions(BinaryContext &BC) override;
  bool runOnFunction(BinaryFunction &Function);
  std::pair<MCInst*, BinaryBasicBlock*> findDemandLoad( BinaryFunction&, BinaryLoop*, 
                                                        MCInst*, BinaryBasicBlock*);
  std::vector<MCInst*> getPredInstrs ( BinaryFunction& BF,
                                       BinaryBasicBlock* TopLLCMissBB,
                                       MCInst* LoopInductionInstr,
                                       MCInst* TopLLCMissP);
  BinaryLoop* getOuterLoopForBB( BinaryFunction&, 
                                 BinaryBasicBlock*);
  BinaryLoop* getInnerLoopForBB( BinaryFunction&, 
                                 BinaryBasicBlock*);
  BinaryBasicBlock* createBoundsCheckBB(BinaryFunction&, BinaryBasicBlock*,
                                        MCInst*, MCInst*, std::vector<MCInst*> predInstrs, 
                                        std::unordered_map<MCPhysReg, MCPhysReg>, int prefetchDist);                                        
  BinaryBasicBlock* createPrefetchBB(BinaryFunction&, BinaryBasicBlock*, BinaryBasicBlock*,
                                     MCInst*, MCInst*, std::unordered_map<MCPhysReg, MCPhysReg>,
                                     int prefetchDist, std::vector<MCInst>);
  std::unordered_map<MCPhysReg, MCPhysReg> getDstRegMapTable ( BinaryFunction& BF,
                                                               std::vector<MCInst*> predInstrs,
                                                               MCInst*);
  std::vector<MCInst> getPredInstrsForPrefetch ( BinaryFunction& BF,
                                                 MCInst* , MCInst*,
                                                 std::vector<MCInst*> predInstrs,
                                                 std::unordered_map<MCPhysReg, MCPhysReg>,
                                                 int);

};

} // namespace bolt
} // namespace llvm

#endif
