#include "klee/Support/ErrorHandling.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <map>
#include <set>
#include <vector>

using namespace llvm;

namespace {
struct ObjectInfo {
  Value *OriginalValue;
  Type *AllocatedType;
  uint64_t Size;
  uint64_t Offset;
};

struct ObjectLinearizationPass : public ModulePass {
  static char ID;
  ObjectLinearizationPass() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    const DataLayout &DL = M.getDataLayout();
    std::vector<ObjectInfo> Objects;
    uint64_t TotalSize = 0;

    auto IsUsedInKleeMakeSymbolic = [](Value *V) {
      std::vector<User *> Worklist(V->user_begin(), V->user_end());
      std::set<User *> Visited;
      while (!Worklist.empty()) {
        User *U = Worklist.back();
        Worklist.pop_back();
        if (!Visited.insert(U).second) continue;

        if (auto *CB = dyn_cast<CallBase>(U)) {
          Function *F = CB->getCalledFunction();
          if (F && F->getName() == "klee_make_symbolic")
            return true;
        } else if (auto *CE = dyn_cast<ConstantExpr>(U)) {
          for (User *CEU : CE->users()) Worklist.push_back(CEU);
        } else if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
           for (User *GU : GEP->users()) Worklist.push_back(GU);
        } else if (auto *BC = dyn_cast<BitCastInst>(U)) {
           for (User *BU : BC->users()) Worklist.push_back(BU);
        } else if (auto *PHI = dyn_cast<PHINode>(U)) {
           for (User *PU : PHI->users()) Worklist.push_back(PU);
        }
      }
      return false;
    };

    // 1. Collect GlobalVariables
    for (GlobalVariable &GV : M.globals()) {
      if (GV.isDeclaration() || GV.isConstant())
        continue;
      if (GV.getName().starts_with("klee") || GV.getName().starts_with("__klee"))
        continue;
      
      Type *Ty = GV.getValueType();
      if (!Ty->isSized() || (!Ty->isStructTy() && !Ty->isArrayTy()))
        continue;

      if (IsUsedInKleeMakeSymbolic(&GV))
        continue;

      uint64_t Size = DL.getTypeAllocSize(Ty);
      if (Size == 0) continue;

      Objects.push_back({&GV, Ty, Size, TotalSize});
      TotalSize += Size;
    }

    // 2. Collect static AllocaInsts
    for (Function &F : M) {
      if (F.isDeclaration()) continue;
      if (F.empty()) continue;
      
      BasicBlock &Entry = F.getEntryBlock();
      for (Instruction &I : Entry) {
        if (auto *AI = dyn_cast<AllocaInst>(&I)) {
          if (AI->isStaticAlloca()) {
            Type *Ty = AI->getAllocatedType();
            if (!Ty->isSized() || (!Ty->isStructTy() && !Ty->isArrayTy()))
              continue;

            if (IsUsedInKleeMakeSymbolic(AI))
              continue;

            uint64_t Size = DL.getTypeAllocSize(Ty);
            if (Size == 0) continue;

            Objects.push_back({AI, Ty, Size, TotalSize});
            TotalSize += Size;
          }
        }
      }
    }

    if (Objects.empty()) return false;

    klee::klee_message("Object-linearization: Linearizing %zu objects into %lu bytes",
                       Objects.size(), TotalSize);

    LLVMContext &Ctx = M.getContext();
    ArrayType *LinearMemTy = ArrayType::get(Type::getInt8Ty(Ctx), TotalSize);
    GlobalVariable *LinearMem = new GlobalVariable(
        M, LinearMemTy, false, GlobalValue::ExternalLinkage,
        ConstantAggregateZero::get(LinearMemTy), "__klee_linear_memory");

    FunctionCallee KleeReportError = M.getOrInsertFunction(
        "klee_report_error", Type::getVoidTy(Ctx),
        PointerType::getUnqual(Ctx), Type::getInt32Ty(Ctx),
        PointerType::getUnqual(Ctx), PointerType::getUnqual(Ctx));

    for (const auto &Info : Objects) {
      Value *OrigVal = Info.OriginalValue;
      uint64_t Offset = Info.Offset;
      uint64_t ObjSize = Info.Size;

      IRBuilder<> Builder(Ctx);
      Value *Replacement = nullptr;

      if (auto *GV = dyn_cast<GlobalVariable>(OrigVal)) {
        std::vector<Value *> Indices = {
            ConstantInt::get(Type::getInt64Ty(Ctx), 0),
            ConstantInt::get(Type::getInt64Ty(Ctx), Offset)};
        Constant *GEP = ConstantExpr::getGetElementPtr(LinearMemTy, LinearMem, Indices);
        Replacement = ConstantExpr::getBitCast(GEP, GV->getType());
      } else if (auto *AI = dyn_cast<AllocaInst>(OrigVal)) {
        Builder.SetInsertPoint(AI);
        std::vector<Value *> Indices = {
            ConstantInt::get(Type::getInt64Ty(Ctx), 0),
            ConstantInt::get(Type::getInt64Ty(Ctx), Offset)};
        Value *GEP = Builder.CreateInBoundsGEP(LinearMemTy, LinearMem, Indices);
        Replacement = Builder.CreateBitCast(GEP, AI->getType());
      }

      if (!Replacement) continue;

      std::vector<User *> Worklist(OrigVal->user_begin(), OrigVal->user_end());
      std::vector<Instruction *> Accessors;
      std::map<Instruction *, Value *> AccessorToPtr;

      while (!Worklist.empty()) {
        User *U = Worklist.back();
        Worklist.pop_back();

        if (auto *I = dyn_cast<Instruction>(U)) {
          Accessors.push_back(I);
          // For GEPs, the GEP itself is the accessor we want to check
          // For Load/Store, the pointer operand is what we check
        } else if (auto *CE = dyn_cast<ConstantExpr>(U)) {
          for (User *CEU : CE->users()) {
            Worklist.push_back(CEU);
          }
        }
      }

      for (Instruction *I : Accessors) {
        Builder.SetInsertPoint(I);
        Value *ByteOffset = nullptr;
        Value *Ptr = nullptr;

        if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
          if (GEP->getPointerOperand()->stripPointerCasts() == OrigVal)
            Ptr = GEP;
        } else if (auto *LI = dyn_cast<LoadInst>(I)) {
          if (LI->getPointerOperand()->stripPointerCasts() == OrigVal)
            Ptr = LI->getPointerOperand();
        } else if (auto *SI = dyn_cast<StoreInst>(I)) {
          if (SI->getPointerOperand()->stripPointerCasts() == OrigVal)
            Ptr = SI->getPointerOperand();
        }

        if (Ptr) {
          if (auto *GEP = dyn_cast<GetElementPtrInst>(Ptr)) {
            Type *CurrentTy = GEP->getSourceElementType();
            if (!CurrentTy->isSized()) continue;

            auto It = GEP->idx_begin();
            Value *Idx0 = *It++;
            uint64_t srcSize = DL.getTypeAllocSize(CurrentTy);
            Value *idx0_64 = Builder.CreateZExtOrTrunc(Idx0, Type::getInt64Ty(Ctx));
            ByteOffset = Builder.CreateMul(idx0_64, ConstantInt::get(Type::getInt64Ty(Ctx), srcSize));

            for (; It != GEP->idx_end(); ++It) {
              Value *Idx = *It;
              if (auto *ST = dyn_cast<StructType>(CurrentTy)) {
                uint64_t fieldIdx = cast<ConstantInt>(Idx)->getZExtValue();
                uint64_t fieldOffset = DL.getStructLayout(ST)->getElementOffset(fieldIdx);
                ByteOffset = Builder.CreateAdd(ByteOffset, ConstantInt::get(Type::getInt64Ty(Ctx), fieldOffset));
                CurrentTy = ST->getElementType(fieldIdx);
              } else {
                Type *NextTy = GetElementPtrInst::getTypeAtIndex(CurrentTy, Idx);
                if (!NextTy->isSized()) {
                  ByteOffset = nullptr;
                  break;
                }
                uint64_t elemSize = DL.getTypeAllocSize(NextTy);
                Value *idx64 = Builder.CreateZExtOrTrunc(Idx, Type::getInt64Ty(Ctx));
                Value *offset = Builder.CreateMul(idx64, ConstantInt::get(Type::getInt64Ty(Ctx), elemSize));
                ByteOffset = Builder.CreateAdd(ByteOffset, offset);
                CurrentTy = NextTy;
              }
            }
          }
 else {
            ByteOffset = ConstantInt::get(Type::getInt64Ty(Ctx), 0);
          }
        }
        
        if (ByteOffset) {
          Value *Cond = Builder.CreateICmpUGE(ByteOffset, ConstantInt::get(Type::getInt64Ty(Ctx), ObjSize));
          Instruction *ThenTerm = SplitBlockAndInsertIfThen(Cond, I, false);
          Builder.SetInsertPoint(ThenTerm);
          Value *File = Builder.CreateGlobalStringPtr("ObjectLinearization.cpp");
          Value *Line = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
          Value *Msg = Builder.CreateGlobalStringPtr("out of bounds access (linearized)");
          Value *Suffix = Builder.CreateGlobalStringPtr("ptr.err");
          Builder.CreateCall(KleeReportError, {File, Line, Msg, Suffix});
          Builder.CreateUnreachable();
        }
      }

      OrigVal->replaceAllUsesWith(Replacement);
      if (auto *I = dyn_cast<Instruction>(OrigVal)) {
        I->eraseFromParent();
      } else if (auto *GV = dyn_cast<GlobalVariable>(OrigVal)) {
        GV->eraseFromParent();
      }
    }
    return true;
  }
};
}

char ObjectLinearizationPass::ID = 0;

ModulePass *createObjectLinearizationPass() {
  return new ObjectLinearizationPass();
}
