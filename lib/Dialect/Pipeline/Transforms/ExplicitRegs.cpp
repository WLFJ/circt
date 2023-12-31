//===- ExplicitRegs.cpp - Explicit regs pass --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Contains the definitions of the explicit regs pass.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Support/BackedgeBuilder.h"
#include "llvm/Support/Debug.h"

using namespace mlir;
using namespace circt;
using namespace pipeline;

namespace {

struct NamedValue {
  Value v;
  StringAttr name;
  operator Value() const { return v; }
  operator Attribute() const { return name; }
  operator llvm::StringRef() const { return name.getValue(); }
};

class ExplicitRegsPass : public ExplicitRegsBase<ExplicitRegsPass> {
public:
  void runOnOperation() override;

private:
  void runOnPipeline(ScheduledPipelineOp p);

  // Recursively routes value v backwards through the pipeline, adding new
  // registers to 'stage' if the value was not already registered in the stage.
  // Returns the registered version of 'v' through 'stage'.
  NamedValue routeThroughStage(Value v, Block *stage);

  // Returns the distance between two stages in the pipeline. The distance is
  // defined wrt. the ordered stages of the pipeline.
  int64_t stageDistance(Block *from, Block *to);

  // Returns a suitable name for a value.
  StringAttr genValueName(Value v);

  struct RoutedValue {
    Backedge v;
    // If true, this value is routed through a stage as a register, else
    // it is routed through a stage as a pass-through.e
    bool isReg;
    StringAttr name;

    operator NamedValue() { return NamedValue{v, name}; }
  };
  // A mapping storing whether a given stage register constains a registerred
  // version of a given value. The registered version will be a backedge during
  // pipeline body analysis. Once the entire body has been analyzed, the
  // pipeline.stage operations will be replaced with pipeline.ss.reg
  // operations containing the requested regs, and the backedge will be
  // replaced. MapVector ensures deterministic iteration order, which in turn
  // ensures determinism during stage op IR emission.
  DenseMap<Block *, llvm::MapVector<Value, RoutedValue>> stageRegOrPassMap;

  // A mapping between stages and their index in the pipeline.
  llvm::DenseMap<Block *, unsigned> stageMap;

  std::shared_ptr<BackedgeBuilder> bb;
  ScheduledPipelineOp parent;
};

} // end anonymous namespace

int64_t ExplicitRegsPass::stageDistance(Block *from, Block *to) {
  int64_t fromStage = stageMap[from];
  int64_t toStage = stageMap[to];
  return toStage - fromStage;
}

StringAttr ExplicitRegsPass::genValueName(Value v) {
  Operation *definingOp = v.getDefiningOp();
  StringAttr valueName;
  auto setNameFn = [&](Value other, StringRef name) {
    if (other == v)
      valueName = StringAttr::get(other.getContext(), name);
  };
  if (definingOp) {
    if (OpAsmOpInterface asmInterface = dyn_cast<OpAsmOpInterface>(definingOp))
      asmInterface.getAsmResultNames(setNameFn);
    // Somewhat comb specific - look for an sv.namehint attribute.
    else if (auto nameHint =
                 definingOp->getAttrOfType<StringAttr>("sv.namehint"))
      valueName = nameHint;
    else
      valueName = StringAttr::get(v.getContext(), "");
  } else {
    // It's a block argument - leverage the OpAsmOpInterface of the pipeline
    // op.
    auto asmInterface = cast<OpAsmOpInterface>(parent.getOperation());
    asmInterface.getAsmBlockArgumentNames(parent.getBody(), setNameFn);
  }

  assert(valueName && "Wasn't able to generate a name for a value");
  return valueName;
}

// NOLINTNEXTLINE(misc-no-recursion)
NamedValue ExplicitRegsPass::routeThroughStage(Value v, Block *stage) {
  Value retVal = v;
  Block *definingStage = retVal.getParentBlock();

  // Is the value defined in the current stage?
  if (definingStage == stage) {
    // Value is defined in the current stage - no routing needed, but we need to
    // define a name.
    return NamedValue{retVal, genValueName(retVal)};
  }

  auto regIt = stageRegOrPassMap[stage].find(retVal);
  if (regIt != stageRegOrPassMap[stage].end()) {
    // 'v' is already routed through 'stage' - return the registered/passed
    // version.
    return regIt->second;
  }

  // Is the value a constant? If so, we allow it; constants are special cases
  // which are allowed to be used in any stage.
  auto *definingOp = retVal.getDefiningOp();
  if (definingOp && definingOp->hasTrait<OpTrait::ConstantLike>())
    return NamedValue{retVal, StringAttr::get(retVal.getContext(), "")};

  // Value is defined somewhere before the provided stage - route it through the
  // stage, and recurse to the predecessor stage.
  int64_t valueLatency = 0;
  if (auto latencyOp = dyn_cast_or_null<LatencyOp>(definingOp))
    valueLatency = latencyOp.getLatency();

  // A value should be registered in this stage if the latency of the value
  // is less than the distance between the current stage and the defining stage.
  bool isReg = valueLatency < stageDistance(definingStage, stage);
  auto valueBackedge = bb->get(retVal.getType());
  auto stageRegBE = stageRegOrPassMap[stage].insert(
      {retVal, RoutedValue{valueBackedge, isReg,
                           StringAttr::get(retVal.getContext(), "")}});
  retVal = valueBackedge;

  // Recurse - recursion will only create a new backedge if necessary.
  Block *stagePred = stage->getSinglePredecessor();
  assert(stagePred && "Expected stage to have a single predecessor");
  auto namedV = routeThroughStage(v, stagePred);
  // And name the backedge using the routed value result name.
  stageRegBE.first->second.name = namedV.name;

  return NamedValue{retVal, namedV.name};
}

void ExplicitRegsPass::runOnPipeline(ScheduledPipelineOp pipeline) {
  OpBuilder b(pipeline.getContext());
  parent = pipeline;
  bb = std::make_shared<BackedgeBuilder>(b, pipeline.getLoc());

  // Cache external-like inputs in a set for fast lookup. This also includes
  // clock, reset, and stall.
  llvm::DenseSet<Value> extLikeInputs;
  for (auto extInput : pipeline.getExtInputs())
    extLikeInputs.insert(extInput);
  extLikeInputs.insert(pipeline.getInnerClock());
  extLikeInputs.insert(pipeline.getInnerReset());
  if (pipeline.hasStall())
    extLikeInputs.insert(pipeline.getInnerStall());

  // Iterate over the pipeline body in-order (!).
  stageMap = pipeline.getStageMap();
  for (Block *stage : pipeline.getOrderedStages()) {
    // Walk the stage body - we do this since register materialization needs
    // to consider all levels of nesting within the stage.
    stage->walk([&](Operation *op) {
      // Check the operands of this operation to see if any of them cross a
      // stage boundary.
      for (OpOperand &operand : op->getOpOperands()) {
        if (extLikeInputs.contains(operand.get())) {
          // Never route external inputs through a stage.
          continue;
        }
        if (getParentStageInPipeline(pipeline, operand.get()) == stage) {
          // The operand is defined by some operation or block which ultimately
          // resides within the current pipeline stage. No routing needed.
          continue;
        }
        Value reroutedValue = routeThroughStage(operand.get(), stage);
        if (reroutedValue != operand.get())
          op->setOperand(operand.getOperandNumber(), reroutedValue);
      }
    });
  }

  auto *ctx = &getContext();

  // All values have been recorded through the stages. Now, add registers to the
  // stage blocks.
  for (auto &[stage, regMap] : stageRegOrPassMap) {
    // Gather register inputs to this stage, either from a predecessor stage
    // or from the original op.
    llvm::SmallVector<Value> regIns, passIns;
    llvm::SmallVector<Attribute> regNames, passNames;
    Block *predecessorStage = stage->getSinglePredecessor();
    auto predStageRegOrPassMap = stageRegOrPassMap.find(predecessorStage);
    assert(predecessorStage && "Stage should always have a single predecessor");
    for (auto &[value, backedge] : regMap) {
      if (backedge.isReg)
        regNames.push_back(backedge.name);
      else
        passNames.push_back(backedge.name);

      if (predStageRegOrPassMap != stageRegOrPassMap.end()) {
        // Grab the value if passed through the predecessor stage, else,
        // use the raw value.
        auto predRegIt = predStageRegOrPassMap->second.find(value);
        if (predRegIt != predStageRegOrPassMap->second.end()) {
          if (backedge.isReg) {
            regIns.push_back(predRegIt->second.v);
          } else {
            passIns.push_back(predRegIt->second.v);
          }
          continue;
        }
      }

      // Not passed through the stage - must be the original value.
      if (backedge.isReg)
        regIns.push_back(value);
      else
        passIns.push_back(value);
    }

    // Replace the predecessor stage terminator, which feeds this stage, with
    // a new terminator that has materialized arguments.
    StageOp terminator = cast<StageOp>(predecessorStage->getTerminator());
    b.setInsertionPoint(terminator);
    llvm::SmallVector<llvm::SmallVector<Value>> clockGates;
    b.create<StageOp>(terminator.getLoc(), terminator.getNextStage(), regIns,
                      passIns, clockGates, b.getArrayAttr(regNames),
                      b.getArrayAttr(passNames));
    terminator.erase();

    // ... add arguments to the next stage. Registers first, then passthroughs.
    llvm::SmallVector<Type> regAndPassTypes;
    llvm::append_range(regAndPassTypes, ValueRange(regIns).getTypes());
    llvm::append_range(regAndPassTypes, ValueRange(passIns).getTypes());
    for (auto [i, type] : llvm::enumerate(regAndPassTypes))
      stage->insertArgument(i, type, UnknownLoc::get(ctx));

    // Replace backedges for the next stage with the new arguments.
    for (auto it : llvm::enumerate(regMap)) {
      auto index = it.index();
      auto &[value, backedge] = it.value();
      backedge.v.setValue(stage->getArgument(index));
    }
  }

  // Clear internal state. See https://github.com/llvm/circt/issues/3235
  stageRegOrPassMap.clear();
}

void ExplicitRegsPass::runOnOperation() {
  getOperation()->walk(
      [&](ScheduledPipelineOp pipeline) { runOnPipeline(pipeline); });
}

std::unique_ptr<mlir::Pass> circt::pipeline::createExplicitRegsPass() {
  return std::make_unique<ExplicitRegsPass>();
}
