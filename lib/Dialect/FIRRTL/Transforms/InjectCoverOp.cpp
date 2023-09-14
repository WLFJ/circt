//===- InjectCoverOp.cpp - Inject CoverOp into branches ---------------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the InjectCoverOp.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotationHelper.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"

using namespace circt;
using namespace firrtl;

namespace {
struct Injecter {
  Injecter(CircuitOp circuit)
      : circuit(circuit), symbolTable(circuit),
        builder(OpBuilder::atBlockBegin(circuit->getBlock())) {}

  CircuitOp circuit;
  SymbolTable symbolTable;
  hw::InnerSymbolNamespaceCollection namespaces;
  OpBuilder builder;

  LogicalResult inject(WhenOp whenOp) {
    auto loc = whenOp.getLoc();
    ImplicitLocOpBuilder b(loc, whenOp);
    // auto *context = b.getContext();
    // get global module clock
    
    whenOp->getParentOp();
    Value clk;
    // Here we may have to recursively get it's parent Op.
    if(llvm::isa<FModuleOp>(whenOp->getParentOp())) {
      FModuleOp module = dyn_cast<FModuleOp>(whenOp->getParentOp());
      clk = module.getArgument(0);
    } else {
      llvm::errs() << "Is whenOp not nested in FModuleOp?\n";
      whenOp->getParentOp()->dump();
      return failure();
    }
    const APInt cone(1, 1);

    llvm::errs() << "Matched WhenOp---->\n";
    whenOp.dump();
    llvm::errs() << "<-----\n\n";

    // Here we just add CoverOp into it's all alternavite brach.
    auto &thenBlock = whenOp.getThenRegion();
    // inser CCoverOp at the head of then block
    ImplicitLocOpBuilder thenBuilder(thenBlock.getLoc(), thenBlock);
    auto constTrueOp = thenBuilder.create<ConstantOp>(loc, UIntType::get(thenBuilder.getContext(), 1, true), cone); 

    thenBuilder.create<CCoverOp>(clk, constTrueOp);

    auto &elseBlock = whenOp.getElseRegion();
    ImplicitLocOpBuilder elseBuilder(elseBlock.getLoc(), elseBlock);
    constTrueOp = elseBuilder.create<ConstantOp>(loc, UIntType::get(elseBuilder.getContext(), 1, true), cone);
    
    elseBuilder.create<CCoverOp>(clk, constTrueOp);

    return success();
  }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

namespace {
struct InjectCoverOpPass : public InjectCoverOpBase<InjectCoverOpPass> {
  void runOnOperation() override;
};
} // end anonymous namespace

void InjectCoverOpPass::runOnOperation() {
  llvm::errs() << "Injecting ConverOp ...\n";
  auto circuit = getOperation();
  Injecter injecter(circuit);
  auto result = circuit.walk([&](WhenOp whenOp) {
    if (failed(injecter.inject(whenOp))) {
      signalPassFailure();
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  if (result.wasInterrupted())
    signalPassFailure();
  markAnalysesPreserved<InstanceGraph>();
}

std::unique_ptr<mlir::Pass> circt::firrtl::createInjectCoverOpPass() {
  return std::make_unique<InjectCoverOpPass>();
}
