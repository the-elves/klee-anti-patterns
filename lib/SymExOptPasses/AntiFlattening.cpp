#include "klee/Support/ErrorHandling.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace {
  struct AntiFlatteningPass : public FunctionPass {
    static char ID;
    AntiFlatteningPass() : FunctionPass(ID) {}

    bool runOnFunction(Function &F) override {
      bool changed = false;
      std::vector<SelectInst*> Selects;

      for (auto &BB : F) {
        for (auto &I : BB) {
          if (auto *SI = dyn_cast<SelectInst>(&I)) {
            Selects.push_back(SI);
          }
        }
      }

      if (!Selects.empty()) {
        klee::klee_message("Anti-flattening: Converting %zu select instructions in function %s",
                           Selects.size(), F.getName().str().c_str());
      }

      for (auto *SI : Selects) {
        // Convert select to branch
        // Before:
        //   res = select cond, valTrue, valFalse
        // After:
        //   br cond, labelTrue, labelFalse
        // labelTrue:
        //   ...
        // labelFalse:
        //   ...
        // labelEnd:
        //   res = phi [valTrue, labelTrue], [valFalse, labelFalse]

        BasicBlock *BB = SI->getParent();
        Instruction *SplitPt = SI;
        BasicBlock *EndBB = BB->splitBasicBlock(SplitPt, "select.end");
        
        // Remove the unconditional branch created by splitBasicBlock
        BB->getTerminator()->eraseFromParent();

        BasicBlock *TrueBB = BasicBlock::Create(F.getContext(), "select.true", &F, EndBB);
        BasicBlock *FalseBB = BasicBlock::Create(F.getContext(), "select.false", &F, EndBB);

        IRBuilder<> Builder(BB);
        Builder.CreateCondBr(SI->getCondition(), TrueBB, FalseBB);

        Builder.SetInsertPoint(TrueBB);
        Builder.CreateBr(EndBB);

        Builder.SetInsertPoint(FalseBB);
        Builder.CreateBr(EndBB);

        PHINode *PN = PHINode::Create(SI->getType(), 2, "select.phi", &EndBB->front());
        PN->addIncoming(SI->getTrueValue(), TrueBB);
        PN->addIncoming(SI->getFalseValue(), FalseBB);

        SI->replaceAllUsesWith(PN);
        SI->eraseFromParent();

        changed = true;
      }

      return changed;
    }
  };
}

char AntiFlatteningPass::ID = 0;

FunctionPass *createAntiFlatteningPass() {
  return new AntiFlatteningPass();
}
