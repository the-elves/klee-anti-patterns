#include "klee/Support/ErrorHandling.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <map>
#include <vector>

using namespace llvm;

namespace {
  struct StructBlastingPass : public ModulePass {
    static char ID;
    StructBlastingPass() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
      bool changed = false;
      std::vector<Function*> Functions;
      for (auto &F : M) {
        if (!F.isDeclaration() && F.hasLocalLinkage()) {
          Functions.push_back(&F);
        }
      }

      for (Function *F : Functions) {
        if (promoteFunction(F)) {
          changed = true;
        }
      }
      return changed;
    }

    struct PromotionInfo {
      Argument *Arg;
      std::map<unsigned, Type*> Fields; // Index -> Type
    };

    bool promoteFunction(Function *F) {
      std::vector<PromotionInfo> ToPromote;
      
      for (auto &Arg : F->args()) {
        if (!Arg.getType()->isPointerTy()) continue;
        
        std::map<unsigned, Type*> Fields;
        bool canPromoteArg = true;
        bool hasUses = false;

        for (User *U : Arg.users()) {
          if (isa<DbgInfoIntrinsic>(U)) continue;
          
          hasUses = true;
          if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
            if (!GEP->hasAllConstantIndices() || GEP->getNumIndices() != 2) {
              canPromoteArg = false;
              break;
            }
            auto *Idx1 = dyn_cast<ConstantInt>(GEP->getOperand(1));
            auto *Idx2 = dyn_cast<ConstantInt>(GEP->getOperand(2));
            if (!Idx1 || !Idx1->isZero() || !Idx2) {
              canPromoteArg = false;
              break;
            }
            
            unsigned fieldIdx = Idx2->getZExtValue();
            Fields[fieldIdx] = GEP->getResultElementType();

            for (User *GU : GEP->users()) {
              if (isa<DbgInfoIntrinsic>(GU)) continue;
              if (!isa<LoadInst>(GU)) {
                canPromoteArg = false;
                break;
              }
            }
            if (!canPromoteArg) break;
          } else if (auto *Load = dyn_cast<LoadInst>(U)) {
            Fields[0] = Load->getType();
          } else {
            canPromoteArg = false;
            break;
          }
        }

        if (canPromoteArg && hasUses && !Fields.empty()) {
          ToPromote.push_back({&Arg, Fields});
        }
      }

      if (ToPromote.empty()) return false;

      klee::klee_message("Struct-blasting: Promoting %zu arguments in function %s",
                         ToPromote.size(), F->getName().str().c_str());

      // Create new function signature
      std::vector<Type*> NewParamTypes;
      for (auto &Arg : F->args()) {
        bool promoted = false;
        for (auto &PI : ToPromote) {
          if (PI.Arg == &Arg) {
            for (auto &Pair : PI.Fields) {
              NewParamTypes.push_back(Pair.second);
            }
            promoted = true;
            break;
          }
        }
        if (!promoted) {
          NewParamTypes.push_back(Arg.getType());
        }
      }

      FunctionType *NewFTy = FunctionType::get(F->getReturnType(), NewParamTypes, F->isVarArg());
      Function *NewF = Function::Create(NewFTy, F->getLinkage(), F->getName() + ".blasted", F->getParent());
      
      ValueToValueMapTy VMap;
      auto NewArgIt = NewF->arg_begin();
      for (auto &OldArg : F->args()) {
        bool promoted = false;
        for (auto &PI : ToPromote) {
          if (PI.Arg == &OldArg) {
            promoted = true;
            // Map promoted arguments to the first of the new arguments for this PI.Arg
            // This avoids unmapped values in CloneFunctionInto
            VMap[&OldArg] = &*NewArgIt;
            for (auto &Pair : PI.Fields) {
              NewArgIt->setName(OldArg.getName() + ".f" + std::to_string(Pair.first));
              ++NewArgIt;
            }
            break;
          }
        }
        if (!promoted) {
          VMap[&OldArg] = &*NewArgIt;
          NewArgIt->setName(OldArg.getName());
          ++NewArgIt;
        }
      }

      SmallVector<ReturnInst*, 8> Returns;
      // We use GlobalChanges to handle cross-function references if any
      CloneFunctionInto(NewF, F, VMap, CloneFunctionChangeType::GlobalChanges, Returns);

      // Fix up the promoted GEPs and Loads in the new function
      for (auto &PI : ToPromote) {
        std::vector<Argument*> NewArgs;
        auto ArgIt = NewF->arg_begin();
        for (auto &A : F->args()) {
          if (&A == PI.Arg) {
            for (size_t i = 0; i < PI.Fields.size(); ++i) {
              NewArgs.push_back(&*ArgIt++);
            }
            break;
          } else {
            bool otherPromoted = false;
            for (auto &op : ToPromote) {
              if (op.Arg == &A) {
                for (size_t j = 0; j < op.Fields.size(); ++j) ArgIt++;
                otherPromoted = true;
                break;
              }
            }
            if (!otherPromoted) ArgIt++;
          }
        }

        std::map<unsigned, Argument*> FieldToNewArg;
        size_t idx = 0;
        for (auto &Pair : PI.Fields) {
          FieldToNewArg[Pair.first] = NewArgs[idx++];
        }

        for (User *U : PI.Arg->users()) {
          if (auto *OldGEP = dyn_cast<GetElementPtrInst>(U)) {
            if (auto *NewGEP = dyn_cast_or_null<GetElementPtrInst>(VMap[OldGEP])) {
              unsigned fIdx = cast<ConstantInt>(NewGEP->getOperand(2))->getZExtValue();
              Argument *NewArg = FieldToNewArg[fIdx];
              std::vector<User*> GUsers(NewGEP->user_begin(), NewGEP->user_end());
              for (User *GU : GUsers) {
                if (auto *Load = dyn_cast<LoadInst>(GU)) {
                  Load->replaceAllUsesWith(NewArg);
                  Load->eraseFromParent();
                }
              }
              NewGEP->eraseFromParent();
            }
          } else if (auto *OldLoad = dyn_cast<LoadInst>(U)) {
            if (auto *NewLoad = dyn_cast_or_null<LoadInst>(VMap[OldLoad])) {
              NewLoad->replaceAllUsesWith(FieldToNewArg[0]);
              NewLoad->eraseFromParent();
            }
          }
        }
      }

      // Update call sites
      std::vector<CallBase*> Callers;
      for (User *U : F->users()) {
        if (auto *CB = dyn_cast<CallBase>(U)) {
          if (CB->getCalledOperand() == F) {
            Callers.push_back(CB);
          }
        }
      }

      for (CallBase *CB : Callers) {
        IRBuilder<> Builder(CB);
        std::vector<Value*> NewArgs;
        auto OldArgIt = F->arg_begin();
        for (unsigned i = 0; i < CB->arg_size(); ++i) {
          Value *V = CB->getArgOperand(i);
          Argument *Arg = &*OldArgIt++;
          bool promoted = false;
          for (auto &PI : ToPromote) {
            if (PI.Arg == Arg) {
              promoted = true;
              Type *StructTy = nullptr;
              for (User *U : Arg->users()) {
                if (auto *G = dyn_cast<GetElementPtrInst>(U)) {
                  StructTy = G->getSourceElementType();
                  break;
                }
              }

              for (auto &Pair : PI.Fields) {
                if (Pair.first == 0 && !StructTy) {
                  // Direct load, no GEP needed
                  NewArgs.push_back(Builder.CreateLoad(Pair.second, V));
                } else {
                  std::vector<Value*> Indices = {
                    ConstantInt::get(Type::getInt32Ty(F->getContext()), 0),
                    ConstantInt::get(Type::getInt32Ty(F->getContext()), Pair.first)
                  };
                  Value *GEP = Builder.CreateInBoundsGEP(StructTy, V, Indices);
                  Value *Load = Builder.CreateLoad(Pair.second, GEP);
                  NewArgs.push_back(Load);
                }
              }
              break;
            }
          }
          if (!promoted) {
            NewArgs.push_back(V);
          }
        }
        CallBase *NewCB = Builder.CreateCall(NewFTy, NewF, NewArgs);
        NewCB->setAttributes(CB->getAttributes()); 
        if (!CB->use_empty()) {
          CB->replaceAllUsesWith(NewCB);
        }
        CB->eraseFromParent();
      }

      if (F->use_empty()) {
        F->eraseFromParent();
      }

      return true;
    }
  };
}

char StructBlastingPass::ID = 0;

ModulePass *createStructBlastingPass() {
  return new StructBlastingPass();
}
