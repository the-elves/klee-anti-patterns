#include "llvm/IR/ConstantRange.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "klee/Support/ErrorHandling.h"
#include <fstream>

using namespace llvm;

namespace {
struct ValueRangeLoweringPass : public FunctionPass {
  static char ID;
  unsigned LoweringCount = 0;

  ValueRangeLoweringPass() : FunctionPass(ID) {
    PassRegistry &Registry = *PassRegistry::getPassRegistry();
    initializeLazyValueInfoWrapperPassPass(Registry);
    initializeTargetLibraryInfoWrapperPassPass(Registry);
  }

  StringRef getPassName() const override { return "Value Range Lowering Pass"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LazyValueInfoWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.setPreservesCFG();
  }

  bool runOnFunction(Function &F) override {
    auto &LVI = getAnalysis<LazyValueInfoWrapperPass>().getLVI();
    bool Changed = false;

    for (auto &BB : F) {
      for (auto I = BB.begin(); I != BB.end();) {
        Instruction &Inst = *I++;
        if (!Inst.getType()->isIntegerTy())
          continue;

        unsigned CurrentBits = Inst.getType()->getIntegerBitWidth();
        if (CurrentBits <= 3)
          continue;

        // Get the range of the value produced by this instruction
        ConstantRange Range = LVI.getConstantRange(&Inst, &Inst);
        if (Range.isFullSet() || Range.isEmptySet())
          continue;

        // Check if the value is non-negative and fits in fewer bits
        if (Range.getLower().isNegative())
          continue;

        APInt MaxValue = Range.getUpper() - 1;
        unsigned ActiveBits = MaxValue.getActiveBits();
        if (ActiveBits == 0)
          ActiveBits = 1;

        unsigned TargetBits = ActiveBits;

        // Minimal target bits as per user's suggestion of <2^3, 2^4, 2^5
        // We'll allow any reduction.
        if (TargetBits >= CurrentBits)
          continue;

        if (lowerInstruction(&Inst, TargetBits)) {
          Changed = true;
          LoweringCount++;
        }
      }
    }
    return Changed;
  }

  bool lowerInstruction(Instruction *I, unsigned TargetBits) {
    unsigned Opcode = I->getOpcode();
    switch (Opcode) {
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::Mul:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
      break;
    default:
      return false;
    }

    IRBuilder<> Builder(I);
    Type *OldTy = I->getType();
    Type *NewTy = Builder.getIntNTy(TargetBits);

    Value *Op0 = I->getOperand(0);
    Value *Op1 = I->getOperand(1);

    if (Opcode == Instruction::Shl || Opcode == Instruction::LShr ||
        Opcode == Instruction::AShr) {
      if (auto *CI = dyn_cast<ConstantInt>(Op1)) {
        if (CI->getZExtValue() >= TargetBits) {
          return false;
        }
      } else {
        return false;
      }
    }

    Value *NewOp0 = Builder.CreateTrunc(Op0, NewTy);
    Value *NewOp1 = Builder.CreateTrunc(Op1, NewTy);
    Value *NewInst = nullptr;

    switch (Opcode) {
    case Instruction::Add:  NewInst = Builder.CreateAdd(NewOp0, NewOp1); break;
    case Instruction::Sub:  NewInst = Builder.CreateSub(NewOp0, NewOp1); break;
    case Instruction::Mul:  NewInst = Builder.CreateMul(NewOp0, NewOp1); break;
    case Instruction::And:  NewInst = Builder.CreateAnd(NewOp0, NewOp1); break;
    case Instruction::Or:   NewInst = Builder.CreateOr(NewOp0, NewOp1); break;
    case Instruction::Xor:  NewInst = Builder.CreateXor(NewOp0, NewOp1); break;
    case Instruction::Shl:  NewInst = Builder.CreateShl(NewOp0, NewOp1); break;
    case Instruction::LShr: NewInst = Builder.CreateLShr(NewOp0, NewOp1); break;
    case Instruction::AShr: NewInst = Builder.CreateAShr(NewOp0, NewOp1); break;
    }

    if (NewInst) {
      Value *Res = Builder.CreateZExt(NewInst, OldTy);
      I->replaceAllUsesWith(Res);
      I->eraseFromParent();
      return true;
    }

    return false;
  }

  bool doFinalization(Module &M) override {
    std::ofstream Out("range_lowering_stats.txt");
    if (Out.is_open()) {
      Out << "Module: " << M.getName().str() << " Lowerings: " << LoweringCount << "\n";
      Out.close();
    }
    return false;
  }
};
} // namespace

char ValueRangeLoweringPass::ID = 0;

FunctionPass *createValueRangeLoweringPass() { return new ValueRangeLoweringPass(); }
