//===- bolt/Passes/LoopInversionPass.h --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_INJECTPREFETCH_H
#define BOLT_PASSES_INJECTPREFETCH_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class InjectPrefetchPass : public BinaryFunctionPass {
private:
  std::unordered_map<std::string, std::unordered_map<uint64_t,long>> TopLLCMissLocations;
  typedef struct TopLLCMissInfo{
    BinaryBasicBlock* TopLLCMissBB;
    MCInst* TopLLCMissInstr;
    BinaryLoop* OuterLoop;
    std::vector<MCInst*> predLoadInstrs;
    MCInst DemandLoadInstr;  // the second element of predLoadInstrs
    BinaryBasicBlock* BoundsCheckBB;
    BinaryBasicBlock* PrefetchBB;
    long prefetchDist;
  } TopLLCMissInfo;

public:
  explicit InjectPrefetchPass() : BinaryFunctionPass(false) {}

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
  BinaryLoop* getOuterLoopForBB( BinaryFunction&, 
                                 BinaryBasicBlock*);
  BinaryBasicBlock* createBoundsCheckBB0(BinaryFunction&, BinaryBasicBlock*,
                                        MCInst*, MCInst*, int prefetchDist,
                                        MCPhysReg);
  BinaryBasicBlock* createBoundsCheckBB(BinaryFunction&, BinaryBasicBlock*,
                                        BinaryBasicBlock*, MCInst*, MCInst*, 
                                        int prefetchDist, MCPhysReg);
  BinaryBasicBlock* createPrefetchBB(BinaryFunction&, BinaryBasicBlock*,
                                     std::vector<MCInst*>, int prefetchDist, 
                                     MCPhysReg);
  BinaryBasicBlock* createPrefetchBB1(BinaryFunction&, BinaryBasicBlock*,
                                      BinaryBasicBlock*, std::vector<MCInst*>,
                                      int prefetchDist, 
                                      MCPhysReg);

};

} // namespace bolt
} // namespace llvm

#endif
