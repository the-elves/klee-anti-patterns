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
  GlobalVariable *GV;
  Type *AllocatedType;
  uint64_t Size;
  uint64_t Offset;
  unsigned FieldIndex;
};

struct ObjectLinearizationPass : public ModulePass {
  static char ID;
  ObjectLinearizationPass() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    const DataLayout &DL = M.getDataLayout();
    std::vector<ObjectInfo> Objects;
    uint64_t TotalSize = 0;
    std::vector<Type *> FieldTypes;
    std::vector<Constant *> Initializers;

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
      if (!GV.hasLocalLinkage())
        continue;
      if (GV.getName().starts_with("klee") || GV.getName().starts_with("__klee"))
        continue;

      Type *Ty = GV.getValueType();
      if (!Ty->isSized() || (!Ty->isStructTy() && !Ty->isArrayTy()))
        continue;

      if (IsUsedInKleeMakeSymbolic(&GV))
        continue;

      FieldTypes.push_back(Ty);
      Initializers.push_back(GV.getInitializer());
      Objects.push_back({&GV, Ty, 0, 0, (unsigned)FieldTypes.size() - 1});
    }

    if (Objects.empty()) return false;

    LLVMContext &Ctx = M.getContext();
    StructType *LinearMemTy = StructType::get(Ctx, FieldTypes, false);
    const StructLayout *SL = DL.getStructLayout(LinearMemTy);
    TotalSize = SL->getSizeInBytes();

    for (auto &Info : Objects) {
      Info.Size = DL.getTypeAllocSize(Info.AllocatedType);
      Info.Offset = SL->getElementOffset(Info.FieldIndex);
    }

    klee::klee_message("Object-linearization: Linearizing %zu objects into %lu bytes",
                       Objects.size(), TotalSize);

    GlobalVariable *LinearMem = new GlobalVariable(
        M, LinearMemTy, false, GlobalValue::InternalLinkage,
        ConstantStruct::get(LinearMemTy, Initializers), "__klee_linear_memory");

    uint64_t MaxAlign = 1;
    for (const auto &Info : Objects) {
      MaxAlign = std::max(MaxAlign, DL.getABITypeAlign(Info.AllocatedType).value());
    }
    LinearMem->setAlignment(Align(MaxAlign));

    FunctionCallee KleeReportError = M.getOrInsertFunction(
        "klee_report_error", Type::getVoidTy(Ctx),
        PointerType::getUnqual(Ctx), Type::getInt32Ty(Ctx),
        PointerType::getUnqual(Ctx), PointerType::getUnqual(Ctx));

    auto GetPointerOffset = [&](Value *Ptr, Value *Base) -> Value* {
      if (Ptr->stripPointerCasts() == Base->stripPointerCasts())
        return ConstantInt::get(Type::getInt64Ty(Ctx), 0);

      APInt Offset(DL.getIndexSizeInBits(0), 0);
      if (Ptr->stripAndAccumulateInBoundsConstantOffsets(DL, Offset) == Base->stripPointerCasts()) {
        return ConstantInt::get(Type::getInt64Ty(Ctx), Offset.getZExtValue());
      }
      return nullptr;
    };

    for (const auto &Info : Objects) {
      GlobalVariable *OrigVal = Info.GV;
      uint64_t Offset = Info.Offset;
      uint64_t ObjSize = Info.Size;

      IRBuilder<> Builder(Ctx);
      Value *Replacement = nullptr;

      std::vector<Value *> Indices = {
          ConstantInt::get(Type::getInt64Ty(Ctx), 0),
          ConstantInt::get(Type::getInt32Ty(Ctx), Info.FieldIndex)};
      Constant *GEP = ConstantExpr::getGetElementPtr(LinearMemTy, LinearMem, Indices);
      Replacement = ConstantExpr::getBitCast(GEP, OrigVal->getType());

      if (!Replacement) continue;

      std::vector<User *> Worklist(OrigVal->user_begin(), OrigVal->user_end());
      std::set<User *> Visited;
      std::vector<Instruction *> Accessors;

      while (!Worklist.empty()) {
        User *U = Worklist.back();
        Worklist.pop_back();
        if (!Visited.insert(U).second) continue;

        if (auto *I = dyn_cast<Instruction>(U)) {
          Accessors.push_back(I);
          if (isa<GetElementPtrInst>(I) || isa<BitCastInst>(I) || isa<PHINode>(I) || isa<SelectInst>(I)) {
            for (User *UU : I->users()) Worklist.push_back(UU);
          }
        } else if (auto *CE = dyn_cast<ConstantExpr>(U)) {
          for (User *CEU : CE->users()) {
            Worklist.push_back(CEU);
          }
        }
      }

      for (Instruction *I : Accessors) {
        if (isa<PHINode>(I) || isa<SelectInst>(I)) continue;
        Builder.SetInsertPoint(I);
        Builder.SetCurrentDebugLocation(I->getDebugLoc());
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
        } else if (auto *CB = dyn_cast<CallBase>(I)) {
          Function *F = CB->getCalledFunction();
          if (F && F->isDeclaration() && !F->isIntrinsic() && !F->getName().starts_with("klee")) {
            bool needed = false;
            for (unsigned j=0; j<CB->arg_size(); ++j) {
              if (GetPointerOffset(CB->getArgOperand(j), OrigVal)) {
                needed = true;
                break;
              }
            }
            if (needed) {
              Function *ParentF = I->getFunction();
              IRBuilder<> EntryBuilder(&ParentF->getEntryBlock(), ParentF->getEntryBlock().begin());
              AllocaInst *Tmp = EntryBuilder.CreateAlloca(Info.AllocatedType, nullptr, "linear.rebuild." + OrigVal->getName());
              Tmp->setAlignment(DL.getABITypeAlign(Info.AllocatedType));

              Builder.CreateMemCpy(Tmp, Tmp->getAlign(), Replacement, MaybeAlign(DL.getABITypeAlign(Info.AllocatedType)), ObjSize);

              for (unsigned j=0; j<CB->arg_size(); ++j) {
                if (Value *Off = GetPointerOffset(CB->getArgOperand(j), OrigVal)) {
                  Value *Tmp8 = Builder.CreateBitCast(Tmp, Type::getInt8PtrTy(Ctx));
                  Value *NewArg8 = Builder.CreateGEP(Type::getInt8Ty(Ctx), Tmp8, Off);
                  Value *NewArg = Builder.CreateBitCast(NewArg8, CB->getArgOperand(j)->getType());
                  CB->setArgOperand(j, NewArg);
                }
              }

              if (auto *CI = dyn_cast<CallInst>(CB)) {
                if (!CI->doesNotReturn() && !CI->isTerminator()) {
                  Builder.SetInsertPoint(CI->getNextNode());
                  Builder.SetCurrentDebugLocation(CI->getDebugLoc());
                  Builder.CreateMemCpy(Replacement, MaybeAlign(DL.getABITypeAlign(Info.AllocatedType)), Tmp, Tmp->getAlign(), ObjSize);
                }
              } else if (auto *II = dyn_cast<InvokeInst>(CB)) {
                Builder.SetInsertPoint(&II->getNormalDest()->front());
                Builder.SetCurrentDebugLocation(II->getDebugLoc());
                Builder.CreateMemCpy(Replacement, MaybeAlign(DL.getABITypeAlign(Info.AllocatedType)), Tmp, Tmp->getAlign(), ObjSize);
              }
            }
          }
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
          } else {
            ByteOffset = ConstantInt::get(Type::getInt64Ty(Ctx), 0);
          }
        }

        if (ByteOffset) {
          Value *Cond = Builder.CreateICmpUGE(ByteOffset, ConstantInt::get(Type::getInt64Ty(Ctx), ObjSize));
          Instruction *ThenTerm = SplitBlockAndInsertIfThen(Cond, I, true);
          Builder.SetInsertPoint(ThenTerm);
          Builder.SetCurrentDebugLocation(I->getDebugLoc());
          Value *File = Builder.CreateGlobalStringPtr("ObjectLinearization.cpp");
          Value *Line = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
          Value *Msg = Builder.CreateGlobalStringPtr("out of bounds access (linearized)");
          Value *Suffix = Builder.CreateGlobalStringPtr("ptr.err");
          Builder.CreateCall(KleeReportError, {File, Line, Msg, Suffix});
        }
      }

      OrigVal->replaceAllUsesWith(Replacement);
      OrigVal->eraseFromParent();
    }
    return true;
  }
};
}

char ObjectLinearizationPass::ID = 0;

ModulePass *createObjectLinearizationPass() {
  return new ObjectLinearizationPass();
}
