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
    BinaryLoop* InnerLoop;
    std::vector<MCInst*> predLoadInstrs;
    MCInst DemandLoadInstr;  // the second element of predLoadInstrs
    BinaryBasicBlock* BoundsCheckBB;
    BinaryBasicBlock* PrefetchBB;
    long prefetchDist;
    std::unordered_map<MCPhysReg, MCPhysReg> dstRegMapTable;
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
  BinaryLoop* getOuterLoopForBB( BinaryFunction&, BinaryBasicBlock*);
  BinaryLoop* getInnerLoopForBB( BinaryFunction&, BinaryBasicBlock*);
  BinaryBasicBlock* createBoundsCheckBB0(BinaryFunction&, BinaryBasicBlock*,
                                        MCInst*, MCInst*, int prefetchDist,
                                        std::unordered_map<MCPhysReg, MCPhysReg>);
  BinaryBasicBlock* createBoundsCheckBB(BinaryFunction&, BinaryBasicBlock*,
                                        BinaryBasicBlock*, MCInst*, MCInst*, 
                                        int prefetchDist, 
                                        std::unordered_map<MCPhysReg, MCPhysReg>,
                                        std::unordered_map<MCPhysReg, MCPhysReg>);
  BinaryBasicBlock* createPrefetchBB(BinaryFunction&, BinaryBasicBlock*,
                                     std::vector<MCInst*>, int prefetchDist, 
                                     std::unordered_map<MCPhysReg, MCPhysReg>);
  BinaryBasicBlock* createPrefetchBB1(BinaryFunction&, BinaryBasicBlock*,
                                      std::vector<MCInst*>,int prefetchDist,
                                      std::unordered_map<MCPhysReg, MCPhysReg>);
  BinaryBasicBlock* createPopRegBB(BinaryFunction&, BinaryBasicBlock*,
                                   BinaryBasicBlock*,MCInst*,MCInst*,
                                   std::unordered_map<MCPhysReg, MCPhysReg>);
  std::unordered_map<MCPhysReg, MCPhysReg> getDstRegMapTable (BinaryFunction& BF,
                                                              std::vector<MCInst*> predInstrs,
                                                              MCInst* TopLLCMissInstrP); 
  std::vector<MCInst> getPredInstrsForPrefetch ( BinaryFunction& BF,
                                                 MCInst* LoopInductionInstr,
                                                 MCInst* TopLLCMissInstrP,
                                                 std::vector<MCInst*> predInstrs,
                                                 std::unordered_map<MCPhysReg, MCPhysReg> dstRegMapTable,
                                                 int prefetchDist);
     
};

} // namespace bolt
} // namespace llvm

#endif
