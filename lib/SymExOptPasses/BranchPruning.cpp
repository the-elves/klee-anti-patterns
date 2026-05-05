#include "klee/Support/ErrorHandling.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include <set>

using namespace llvm;

namespace {
  struct BranchPruningPass : public ModulePass {
    static char ID;
    BranchPruningPass() : ModulePass(ID) {
      PassRegistry &Registry = *PassRegistry::getPassRegistry();
      initializeLazyValueInfoWrapperPassPass(Registry);
    }

    StringRef getPassName() const override { return "Branch Pruning Pass"; }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<LazyValueInfoWrapperPass>();
    }

    bool isInputDependent(Value *V, std::set<Value*> &Visited) {
      if (!V || Visited.count(V)) return false;
      Visited.insert(V);

      if (isa<Argument>(V)) return true;
      if (isa<LoadInst>(V)) return true;
      if (auto *CI = dyn_cast<CallInst>(V)) {
          if (Function *F = CI->getCalledFunction()) {
              if (F->getName().contains("klee_")) return true;
          }
          return true;
      }

      if (auto *I = dyn_cast<Instruction>(V)) {
        for (Value *Op : I->operands()) {
          if (isInputDependent(Op, Visited)) return true;
        }
      }
      return false;
    }

    bool runOnModule(Module &M) override {
      unsigned totalPruned = 0;
      unsigned totalInstRemoved = 0;
      unsigned totalInputDep = 0;
      bool changed = false;

      for (auto &F : M) {
        if (F.isDeclaration()) continue;
        
        unsigned pruned = 0;
        unsigned instRemoved = 0;
        unsigned inputDep = 0;
        
        if (runOnFunction(F, pruned, instRemoved, inputDep)) {
            changed = true;
            totalPruned += pruned;
            totalInstRemoved += instRemoved;
            totalInputDep += inputDep;
        }
      }

      if (changed) {
          klee::klee_message("Branch-Pruning: Total Pruned=%u, Total InstructionsRemoved=%u, Total InputDependent=%u",
                             totalPruned, totalInstRemoved, totalInputDep);
      }

      return changed;
    }

    bool runOnFunction(Function &F, unsigned &branchesPruned, unsigned &instructionsRemoved, unsigned &inputDependentPruned) {
      LazyValueInfo *LVI = &getAnalysis<LazyValueInfoWrapperPass>(F).getLVI();
      
      unsigned initInstCount = F.getInstructionCount();
      bool changed = false;

      bool functionChanged;
      do {
        functionChanged = false;
        std::vector<BranchInst*> CondBranches;
        for (auto &BB : F) {
          if (auto *BI = dyn_cast<BranchInst>(BB.getTerminator())) {
            if (BI->isConditional()) {
              CondBranches.push_back(BI);
            }
          }
        }

        for (auto *BI : CondBranches) {
          Value *Cond = BI->getCondition();
          
          bool proven = false;
          bool provenVal = false;

          Constant *C = LVI->getConstant(Cond, BI);
          if (C && isa<ConstantInt>(C)) {
            proven = true;
            provenVal = cast<ConstantInt>(C)->isOne();
          } else if (auto *ICI = dyn_cast<ICmpInst>(Cond)) {
            auto res = LVI->getPredicateAt(ICI->getPredicate(), ICI->getOperand(0), ICI->getOperand(1), BI, /*UseBlockValue=*/true);
            if (res == LazyValueInfo::True) {
              proven = true;
              provenVal = true;
            } else if (res == LazyValueInfo::False) {
              proven = true;
              provenVal = false;
            }
          }
          
          if (proven) {
            branchesPruned++;
            std::set<Value*> Visited;
            if (isInputDependent(Cond, Visited)) {
                inputDependentPruned++;
            }

            BasicBlock *KeepBB = BI->getSuccessor(provenVal ? 0 : 1);
            BasicBlock *DeadBB = BI->getSuccessor(provenVal ? 1 : 0);
            
            DeadBB->removePredecessor(BI->getParent());
            
            ReplaceInstWithInst(BI, BranchInst::Create(KeepBB));
            changed = true;
            functionChanged = true;
            break; 
          }
        }
      } while (functionChanged);

      if (changed) {
          removeUnreachableBlocks(F);
          
          bool localChanged = true;
          while (localChanged) {
              localChanged = false;
              for (auto &BB : F) {
                  for (auto I = BB.begin(); I != BB.end();) {
                      Instruction *Inst = &*I++;
                      if (isInstructionTriviallyDead(Inst)) {
                          RecursivelyDeleteTriviallyDeadInstructions(Inst);
                          localChanged = true;
                          break;
                      }
                  }
              }
          }

          unsigned finalInstCount = F.getInstructionCount();
          int diff = (int)initInstCount - (int)finalInstCount;
          instructionsRemoved = (diff > 0) ? (unsigned)diff : 0;
      }

      return changed;
    }
  };
}

char BranchPruningPass::ID = 0;

namespace klee {
ModulePass *createBranchPruningPass() {
  return new BranchPruningPass();
}
}
