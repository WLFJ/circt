//===- ResolvePaths.cpp - Resolve path operations ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the ResolvePathsPass.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotationHelper.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"

using namespace circt;
using namespace firrtl;

namespace {
struct InjectCoverOp {
  InjectCoverOp(CircuitOp circuit, InstanceGraph &instanceGraph)
      : circuit(circuit), symbolTable(circuit), instanceGraph(instanceGraph),
        builder(OpBuilder::atBlockBegin(circuit->getBlock())) {}

  CircuitOp circuit;
  SymbolTable symbolTable;
  CircuitTargetCache targetCache;
  InstanceGraph &instanceGraph;
  hw::InnerSymbolNamespaceCollection namespaces;
  OpBuilder builder;
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
  llvm::errs() << "Hello world!\n";
}

std::unique_ptr<mlir::Pass> circt::firrtl::createInjectCoverOpPass() {
  return std::make_unique<InjectCoverOpPass>();
}
