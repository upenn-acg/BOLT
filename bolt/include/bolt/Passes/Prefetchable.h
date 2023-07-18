//===- bolt/Passes/LoopInversionPass.h --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_PREFETCHABLE_H
#define BOLT_PASSES_PREFETCHABLE_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class Prefetchable : public BinaryFunctionPass {
private:
  std::unordered_map<std::string, std::unordered_map<uint64_t,long>> TopLLCMissLocations;
  typedef struct TopLLCMissInfo{
    BinaryBasicBlock* TopLLCMissBB;
    MCInst* TopLLCMissInstr;
    BinaryLoop* OuterLoop;
    BinaryLoop* InnerLoop;
    std::vector<MCInst*> predLoadInstrs;
    MCInst DemandLoadInstr;  // the second element of predLoadInstrs
    long prefetchDist;
    MCPhysReg OuterLoopInductionReg;
    MCPhysReg InnerLoopInductionReg;
    bool prefetchable;
  } TopLLCMissInfo;

public:
  explicit Prefetchable() : BinaryFunctionPass(false) {}

  const char *getName() const override { return "inject-prefetch"; }
  /// Helper functions
  //std::unordered_map<std::string, uint64_t> getTopLLCMissLocationFromFile();
  std::vector<std::string> splitLine(std::string);
  std::string removeSuffix(std::string);
  std::unordered_map<std::string, std::unordered_map<uint64_t, long>> getTopLLCMissLocationFromFile();
  void writeTopLLCMissLocationToFile(BinaryFunction& BF,
                                     std::vector<TopLLCMissInfo> prefetchableInfos,
                                     std::string FuncName);
  /// real functions
  void runOnFunctions(BinaryContext &BC) override;
  bool runOnFunction(BinaryFunction &Function);
  std::pair<MCInst*, BinaryBasicBlock*> findDemandLoad( BinaryFunction&, BinaryLoop*, 
                                                        MCInst*, BinaryBasicBlock*);
  BinaryLoop* getOuterLoopForBB( BinaryFunction&, BinaryBasicBlock*);
  BinaryLoop* getInnerLoopForBB( BinaryFunction&, BinaryBasicBlock*);
  bool getLoopInductionInstrs(BinaryFunction& BF, BinaryLoop* Loop,
                              MCInst&, MCInst&);
  MCPhysReg getLoopInductionReg(BinaryFunction& BF,
                                MCInst& LoopInductionInstr,
                                MCInst& LoopGuardCMPInstr);
};

} // namespace bolt
} // namespace llvm

#endif
