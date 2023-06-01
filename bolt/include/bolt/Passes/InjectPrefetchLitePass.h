//===- bolt/Passes/LoopInversionPass.h --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_INJECTPREFETCHLITE_H
#define BOLT_PASSES_INJECTPREFETCHLITE_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class InjectPrefetchLitePass : public BinaryFunctionPass {
public:
  explicit InjectPrefetchLitePass() : BinaryFunctionPass(false) {}

  const char *getName() const override { return "inject-prefetch-lite"; }

  /// Helper functions
  std::unordered_map<std::string, std::vector<uint64_t>> getTopLLCMissLocationFromFile();
  std::vector<std::string> splitLine(std::string);
  std::string removeSuffix(std::string);
  /// real functions
  void runOnFunctions(BinaryContext &BC) override;
  bool runOnFunction(BinaryFunction &Function);
  std::pair<MCInst*, BinaryBasicBlock*> findDemandLoad( BinaryFunction&, BinaryLoop*, 
                                                        MCInst*, BinaryBasicBlock*);
  BinaryLoop* getWorkingLoop( BinaryFunction&, 
                              BinaryBasicBlock*, MCInst*,
                              MCInst**, MCInst**);
  BinaryBasicBlock* createBoundsCheckBB(BinaryFunction&, BinaryBasicBlock*,
                                        MCInst*, MCInst*, MCInst*,
                                        int prefetchDist, MCPhysReg);
  BinaryBasicBlock* createPrefetchBB(BinaryFunction&, BinaryBasicBlock*,
                                     BinaryBasicBlock*, std::vector<MCInst*>, MCInst*, 
                                     int prefetchDist, MCPhysReg);
  void getLoopGuardInstrs( BinaryFunction& BF,
                           BinaryLoop* workingLoop,
                           MCInst* TopLLCMissInstr,
                           MCInst** LoopInductionInstr,
                           MCInst** LoopGuardCMPInstr); 

private:
  std::unordered_map<std::string, std::vector<uint64_t>> TopLLCMissLocations;
};

} // namespace bolt
} // namespace llvm

#endif
