#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/MathExtras.h"
#include "klee/Support/ErrorHandling.h"

using namespace llvm;

namespace {
struct LowerBitWidthsPass : public FunctionPass {
  static char ID;
  LowerBitWidthsPass() : FunctionPass(ID) {
    PassRegistry &Registry = *PassRegistry::getPassRegistry();
    initializeDemandedBitsWrapperPassPass(Registry);
    initializeTargetLibraryInfoWrapperPassPass(Registry);
  }

  StringRef getPassName() const override { return "Lower BitWidths Pass"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DemandedBitsWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.setPreservesCFG();
  }

  bool runOnFunction(Function &F) override {
    auto &DB = getAnalysis<DemandedBitsWrapperPass>().getDemandedBits();
    bool Changed = false;

    for (auto &BB : F) {
      for (auto I = BB.begin(); I != BB.end();) {
        Instruction &Inst = *I++;
        if (!Inst.getType()->isIntegerTy())
          continue;

        unsigned CurrentBits = Inst.getType()->getIntegerBitWidth();
        if (CurrentBits <= 8)
          continue;

        APInt Demanded = DB.getDemandedBits(&Inst);
        if (Demanded == 0)
          continue;

        unsigned ActiveBits = Demanded.getActiveBits();
        if (ActiveBits == 0)
          continue;

        unsigned TargetBits = llvm::PowerOf2Ceil(ActiveBits);
        if (TargetBits < 8)
          TargetBits = 8;

        if (TargetBits >= CurrentBits)
          continue;

        if (lowerInstruction(&Inst, TargetBits)) {
          Changed = true;
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
      klee::klee_message("Lowering %s from %u to %u bits", I->getOpcodeName(), OldTy->getIntegerBitWidth(), TargetBits);
      Value *Res = Builder.CreateZExt(NewInst, OldTy);
      I->replaceAllUsesWith(Res);
      I->eraseFromParent();
      return true;
    }

    return false;
  }
};
} // namespace

char LowerBitWidthsPass::ID = 0;

llvm::FunctionPass *createLowerBitWidthsPass() { return new LowerBitWidthsPass(); }
