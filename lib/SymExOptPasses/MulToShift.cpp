#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
  struct StrengthReductionPass : public FunctionPass {
    static char ID;
    StrengthReductionPass() : FunctionPass(ID) {}

    // Decomposes multiplication by constant into shifts and adds/subs
    Value *decomposeMul(Value *X, APInt C, IRBuilder<> &Builder, Type *Ty) {
      if (C == 0) return ConstantInt::get(Ty, 0);
      if (C == 1) return X;
      if (C.isAllOnes()) return Builder.CreateNeg(X);

      bool Neg = C.isNegative();
      APInt AbsC = C.abs();
      
      if (AbsC.isPowerOf2()) {
        Value *Res = Builder.CreateShl(X, ConstantInt::get(Ty, AbsC.logBase2()));
        return Neg ? Builder.CreateNeg(Res) : Res;
      }

      unsigned floor = AbsC.logBase2();
      unsigned ceil = floor + 1;
      
      // Use wider bitwidth for distance calculation
      unsigned WideWidth = C.getBitWidth() + 1;
      APInt AbsCExt = AbsC.zext(WideWidth);
      APInt p2FloorExt = APInt(WideWidth, 1) << floor;
      APInt p2CeilExt = APInt(WideWidth, 1) << ceil;
      
      APInt distFloor = AbsCExt - p2FloorExt;
      APInt distCeil = p2CeilExt - AbsCExt;
      
      unsigned k;
      APInt P;
      if (distFloor.ule(distCeil) || ceil >= C.getBitWidth()) {
        k = floor;
        P = p2FloorExt.trunc(C.getBitWidth());
      } else {
        k = ceil;
        P = p2CeilExt.trunc(C.getBitWidth());
      }

      Value *Term = Builder.CreateShl(X, ConstantInt::get(Ty, k));
      if (Neg) Term = Builder.CreateNeg(Term);

      APInt R = C - (Neg ? -P : P);
      return Builder.CreateAdd(Term, decomposeMul(X, R, Builder, Ty));
    }

    bool runOnFunction(Function &F) override {
      bool Changed = false;
      for (auto &BB : F) {
        for (auto I = BB.begin(); I != BB.end(); ) {
          Instruction &Inst = *I++;
          unsigned Opcode = Inst.getOpcode();

          if (Opcode != Instruction::Mul && Opcode != Instruction::UDiv && 
              Opcode != Instruction::SDiv && Opcode != Instruction::URem)
            continue;

          Value *Op0 = Inst.getOperand(0);
          Value *Op1 = Inst.getOperand(1);
          ConstantInt *CI = dyn_cast<ConstantInt>(Op1);
          
          // Mul is commutative, constant could be Op0
          if (!CI && Opcode == Instruction::Mul) {
            CI = dyn_cast<ConstantInt>(Op0);
            if (CI) std::swap(Op0, Op1);
          }

          if (!CI) continue;
          APInt C = CI->getValue();
          IRBuilder<> Builder(&Inst);
          Type *Ty = Inst.getType();
          Value *NewVal = nullptr;

          if (Opcode == Instruction::Mul) {
            NewVal = decomposeMul(Op0, C, Builder, Ty);
          } else if (C.isPowerOf2()) {
            uint64_t k = C.logBase2();
            if (Opcode == Instruction::UDiv) {
              NewVal = Builder.CreateLShr(Op0, ConstantInt::get(Ty, k));
            } else if (Opcode == Instruction::URem) {
              NewVal = Builder.CreateAnd(Op0, ConstantInt::get(Ty, C - 1));
            } else if (Opcode == Instruction::SDiv) {
              if (k == 0) {
                NewVal = Op0; // Should not happen with well-formed IR
              } else {
                // SDiv by power of 2: (X + (X < 0 ? (2^k - 1) : 0)) >> k
                Value *ShiftAmt = ConstantInt::get(Ty, Ty->getIntegerBitWidth() - 1);
                Value *IsNeg = Builder.CreateAShr(Op0, ShiftAmt);
                Value *Bias = Builder.CreateAnd(IsNeg, ConstantInt::get(Ty, C - 1));
                Value *Add = Builder.CreateAdd(Op0, Bias);
                NewVal = Builder.CreateAShr(Add, ConstantInt::get(Ty, k));
              }
            }
          }

          if (NewVal) {
            Inst.replaceAllUsesWith(NewVal);
            Inst.eraseFromParent();
            Changed = true;
          }
        }
      }
      return Changed;
    }
  };
}

char StrengthReductionPass::ID = 0;

FunctionPass *createMulToShiftPass() {
  return new StrengthReductionPass();
}
