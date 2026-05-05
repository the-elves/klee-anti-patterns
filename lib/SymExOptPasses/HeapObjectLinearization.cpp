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
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/InitializePasses.h"
#include <map>
#include <set>
#include <vector>

using namespace llvm;

namespace {
struct HeapObjectInfo {
  CallBase *CB;
  Type *AllocatedType;
  uint64_t Size;
  uint64_t Offset;
  unsigned FieldIndex;
};

struct HeapObjectLinearizationPass : public ModulePass {
  static char ID;
  HeapObjectLinearizationPass() : ModulePass(ID) {
    PassRegistry &Registry = *PassRegistry::getPassRegistry();
    initializeLazyValueInfoWrapperPassPass(Registry);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LazyValueInfoWrapperPass>();
    ModulePass::getAnalysisUsage(AU);
  }

  bool isAllocationFunction(const Function *F) {
    if (!F) return false;
    StringRef Name = F->getName();
    return Name == "malloc" || Name == "calloc" || Name == "realloc" ||
           Name == "_Znwm" || Name == "_Znam" ||
           Name == "_ZnwmRKSt9nothrow_t" || Name == "_ZnamRKSt9nothrow_t";
  }

  uint64_t getAllocationSize(CallBase *CB, const DataLayout &DL) {
    Function *F = CB->getCalledFunction();
    if (!F) return 0;
    StringRef Name = F->getName();
    if (Name == "malloc" || Name == "_Znwm" || Name == "_Znam" ||
        Name == "_ZnwmRKSt9nothrow_t" || Name == "_ZnamRKSt9nothrow_t") {
      if (CB->arg_size() >= 1) {
        if (auto *CI = dyn_cast<ConstantInt>(CB->getArgOperand(0)))
          return CI->getZExtValue();
      }
    } else if (Name == "calloc") {
      if (CB->arg_size() >= 2) {
        auto *CI1 = dyn_cast<ConstantInt>(CB->getArgOperand(0));
        auto *CI2 = dyn_cast<ConstantInt>(CB->getArgOperand(1));
        if (CI1 && CI2)
          return CI1->getZExtValue() * CI2->getZExtValue();
      }
    } else if (Name == "realloc") {
      if (CB->arg_size() >= 2) {
        if (auto *CI = dyn_cast<ConstantInt>(CB->getArgOperand(1)))
          return CI->getZExtValue();
      }
    }
    return 0;
  }

  bool runOnModule(Module &M) override {
    const DataLayout &DL = M.getDataLayout();
    LLVMContext &Ctx = M.getContext();

    auto isLinkedFunction = [](const Function *F) -> bool {
      if (!F) return false;
      if (F->isDeclaration()) return true;
      if (F->getName().starts_with("klee") || F->getName().starts_with("__klee"))
        return true;
      if (DISubprogram *SP = F->getSubprogram()) {
        StringRef Dir = SP->getDirectory();
        StringRef File = SP->getFilename();
        if (Dir.contains("klee-uclibc") || File.contains("klee-uclibc") ||
            Dir.contains("runtime") || File.contains("runtime") ||
            Dir.contains("build/runtime") || File.contains("build/runtime")) {
          return true;
        }
      }
      return false;
    };

    auto IsUsedInKleeMakeSymbolic = [](Value *V) {
      std::vector<User *> Worklist(V->user_begin(), V->user_end());
      std::set<User *> Visited;
      while (!Worklist.empty()) {
        User *U = Worklist.back();
        Worklist.pop_back();
        if (!Visited.insert(U).second) continue;

        if (auto *CB = dyn_cast<CallBase>(U)) {
          Function *F = CB->getCalledFunction();
          if (F && (F->getName() == "klee_make_symbolic" || F->getName().starts_with("klee_")))
            return true;
        } else if (auto *CE = dyn_cast<ConstantExpr>(U)) {
          for (User *CEU : CE->users()) Worklist.push_back(CEU);
        } else if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
           for (User *GU : GEP->users()) Worklist.push_back(GU);
        } else if (auto *BC = dyn_cast<BitCastInst>(U)) {
           for (User *BU : BC->users()) Worklist.push_back(BU);
        } else if (auto *PHI = dyn_cast<PHINode>(U)) {
           for (User *PU : PHI->users()) Worklist.push_back(PU);
        } else if (auto *SI = dyn_cast<SelectInst>(U)) {
           for (User *SU : SI->users()) Worklist.push_back(SU);
        }
      }
      return false;
    };

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

    auto ProcessReplacement = [&](Value *OrigVal, Value *Replacement, uint64_t ObjSize, Type *AllocatedType) {
      std::vector<User *> Worklist(OrigVal->user_begin(), OrigVal->user_end());
      std::set<User *> Visited;
      std::vector<Instruction *> Accessors;

      IRBuilder<> Builder(Ctx);

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
          for (User *CEU : CE->users()) Worklist.push_back(CEU);
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
          Function *CalledF = CB->getCalledFunction();
          if (CalledF && isLinkedFunction(CalledF)) {
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
              AllocaInst *Tmp = EntryBuilder.CreateAlloca(AllocatedType, nullptr, "heap.linear.rebuild." + OrigVal->getName());
              Tmp->setAlignment(DL.getABITypeAlign(AllocatedType));

              Builder.CreateMemCpy(Tmp, Tmp->getAlign(), Replacement, MaybeAlign(DL.getABITypeAlign(AllocatedType)), ObjSize);

              for (unsigned j=0; j<CB->arg_size(); ++j) {
                if (Value *Off = GetPointerOffset(CB->getArgOperand(j), OrigVal)) {
                  Value *Tmp8 = Builder.CreateBitCast(Tmp, PointerType::getUnqual(Ctx));
                  Value *NewArg8 = Builder.CreateGEP(Type::getInt8Ty(Ctx), Tmp8, Off);
                  Value *NewArg = Builder.CreateBitCast(NewArg8, CB->getArgOperand(j)->getType());
                  CB->setArgOperand(j, NewArg);
                }
              }

              if (auto *CI = dyn_cast<CallInst>(CB)) {
                if (!CI->doesNotReturn() && !CI->isTerminator()) {
                  Builder.SetInsertPoint(CI->getNextNode());
                  Builder.SetCurrentDebugLocation(CI->getDebugLoc());
                  Builder.CreateMemCpy(Replacement, MaybeAlign(DL.getABITypeAlign(AllocatedType)), Tmp, Tmp->getAlign(), ObjSize);
                }
              } else if (auto *II = dyn_cast<InvokeInst>(CB)) {
                Builder.SetInsertPoint(&II->getNormalDest()->front());
                Builder.SetCurrentDebugLocation(II->getDebugLoc());
                Builder.CreateMemCpy(Replacement, MaybeAlign(DL.getABITypeAlign(AllocatedType)), Tmp, Tmp->getAlign(), ObjSize);
              }
            }
          }
        }

        if (Ptr) {
          if (auto *GEP = dyn_cast<GetElementPtrInst>(Ptr)) {
            Type *CurrentTy = GEP->getSourceElementType();
            if (CurrentTy->isSized()) {
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
          } else {
            ByteOffset = ConstantInt::get(Type::getInt64Ty(Ctx), 0);
          }
        }

        if (ByteOffset) {
          Value *Cond = Builder.CreateICmpUGE(ByteOffset, ConstantInt::get(Type::getInt64Ty(Ctx), ObjSize));
          Instruction *ThenTerm = SplitBlockAndInsertIfThen(Cond, I, true);
          Builder.SetInsertPoint(ThenTerm);
          Builder.SetCurrentDebugLocation(I->getDebugLoc());
          Value *File = Builder.CreateGlobalStringPtr("HeapObjectLinearization.cpp");
          Value *Line = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
          Value *Msg = Builder.CreateGlobalStringPtr("out of bounds access (heap linearized)");
          Value *Suffix = Builder.CreateGlobalStringPtr("ptr.err");
          Builder.CreateCall(KleeReportError, {File, Line, Msg, Suffix});
        }
      }

      OrigVal->replaceAllUsesWith(Replacement);
      if (auto *I = dyn_cast<Instruction>(OrigVal)) I->eraseFromParent();
    };

    // Identify all candidate heap objects (mallocs with constant size)
    std::vector<Value *> CandidateObjects;
    for (Function &F : M) {
      if (F.isDeclaration() || F.empty()) continue;
      if (isLinkedFunction(&F)) continue;
      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          if (auto *CB = dyn_cast<CallBase>(&I)) {
            if (isAllocationFunction(CB->getCalledFunction())) {
              uint64_t size = getAllocationSize(CB, DL);
              if (size > 0) {
                if (!IsUsedInKleeMakeSymbolic(CB)) {
                  CandidateObjects.push_back(CB);
                }
              }
            }
          }
        }
      }
    }

    if (CandidateObjects.empty()) return false;

    // Identify which candidate objects are actually aliased using LazyValueInfo
    std::set<Value *> AliasedObjects;
    for (Function &F : M) {
      if (F.isDeclaration() || F.empty()) continue;
      if (isLinkedFunction(&F)) continue;

      LazyValueInfo &LVI = getAnalysis<LazyValueInfoWrapperPass>(F).getLVI();
      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          std::vector<Value *> Ptrs;
          if (I.getType()->isPointerTy()) Ptrs.push_back(&I);
          if (auto *LI = dyn_cast<LoadInst>(&I)) Ptrs.push_back(LI->getPointerOperand());
          if (auto *SI = dyn_cast<StoreInst>(&I)) Ptrs.push_back(SI->getPointerOperand());
          
          if (auto *CB = dyn_cast<CallBase>(&I)) {
              for (unsigned i = 0; i < CB->arg_size(); ++i) {
                  Value *Arg = CB->getArgOperand(i);
                  if (Arg->getType()->isPointerTy()) Ptrs.push_back(Arg);
              }
          }

          for (Value *Ptr : Ptrs) {
              Value *U = getUnderlyingObject(Ptr);
              bool isCandidate = std::find(CandidateObjects.begin(), CandidateObjects.end(), U) != CandidateObjects.end();
              
              for (Value *C : CandidateObjects) {
                  if (C == U || C == Ptr) continue;
                  
                  // Use LVI to check if Ptr could possibly equal C.
                  LazyValueInfo::Tristate TS = LVI.getPredicateAt(ICmpInst::ICMP_EQ, Ptr, C, &I, true);

                  if (TS == LazyValueInfo::False) {
                      continue;
                  }

                  AliasedObjects.insert(C);
                  if (isCandidate) AliasedObjects.insert(U);
              }
          }
        }
      }
    }

    if (AliasedObjects.empty()) return false;

    // Global Heap Linearization
    std::vector<HeapObjectInfo> GlobalHeapObjects;
    std::vector<Type *> HeapFieldTypes;

    for (Value *V : CandidateObjects) {
      if (AliasedObjects.find(V) == AliasedObjects.end()) continue;
      CallBase *CB = cast<CallBase>(V);
      uint64_t size = getAllocationSize(CB, DL);
      Type *Ty = ArrayType::get(Type::getInt8Ty(Ctx), size);
      HeapFieldTypes.push_back(Ty);
      GlobalHeapObjects.push_back({CB, Ty, size, 0, (unsigned)HeapFieldTypes.size() - 1});
    }

    if (!GlobalHeapObjects.empty()) {
      StructType *LinearHeapTy = StructType::get(Ctx, HeapFieldTypes, false);
      const StructLayout *SL = DL.getStructLayout(LinearHeapTy);

      for (auto &Info : GlobalHeapObjects) {
        Info.Offset = SL->getElementOffset(Info.FieldIndex);
      }

      klee::klee_message("Heap-Object-linearization: Linearizing %zu heap objects into %lu bytes",
                         GlobalHeapObjects.size(), (uint64_t)SL->getSizeInBytes());

      GlobalVariable *LinearHeap = new GlobalVariable(
          M, LinearHeapTy, false, GlobalValue::InternalLinkage,
          ConstantAggregateZero::get(LinearHeapTy), "__klee_linear_heap_memory");

      uint64_t MaxAlign = 1;
      for (const auto &Info : GlobalHeapObjects) {
        MaxAlign = std::max(MaxAlign, DL.getABITypeAlign(Info.AllocatedType).value());
      }
      LinearHeap->setAlignment(Align(MaxAlign));

      for (const auto &Info : GlobalHeapObjects) {
        CallBase *OrigVal = Info.CB;
        std::vector<Value *> Indices = {
            ConstantInt::get(Type::getInt64Ty(Ctx), 0),
            ConstantInt::get(Type::getInt32Ty(Ctx), Info.FieldIndex)};
        Constant *GEP = ConstantExpr::getGetElementPtr(LinearHeapTy, LinearHeap, Indices);
        Value *Replacement = ConstantExpr::getBitCast(GEP, OrigVal->getType());
        ProcessReplacement(OrigVal, Replacement, Info.Size, Info.AllocatedType);
      }
      return true;
    }

    return false;
  }
};
}

char HeapObjectLinearizationPass::ID = 0;

ModulePass *createHeapObjectLinearizationPass() {
  return new HeapObjectLinearizationPass();
}
