#include "klee/SymExOptPasses/Passes.h"
#include "klee/Support/ErrorHandling.h"
#include "llvm/CodeGen/IntrinsicLowering.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
class FunctionPass;
}

llvm::FunctionPass *createAntiFlatteningPass();
llvm::ModulePass *createStructBlastingPass();
llvm::ModulePass *createObjectLinearizationPass();
llvm::ModulePass *createHeapObjectLinearizationPass();
llvm::FunctionPass *createMulToShiftPass();

namespace klee {

void applySymExOptPasses(llvm::Module &M,
                         const std::vector<std::string> &passes) {
  if (passes.empty())
    return;

  llvm::legacy::PassManager PM;
  bool added = false;

  for (const auto &passName : passes) {
    if (passName == "dummy-pass") {
      klee_message("Running dummy symbolic execution optimization pass");
      added = true;
    } else if (passName == "anti-flattening") {
      klee_message("Running anti-flattening symbolic execution optimization pass");
      PM.add(createAntiFlatteningPass());
      added = true;
    } else if (passName == "struct-blasting") {
      klee_message("Running struct-blasting symbolic execution optimization pass");
      PM.add(createStructBlastingPass());
      added = true;
    } else if (passName == "object-linearization") {
      klee_message("Running object-linearization symbolic execution optimization pass");
      PM.add(createObjectLinearizationPass());
      added = true;
    } else if (passName == "heap-object-linearization") {
      klee_message("Running heap-object-linearization symbolic execution optimization pass");
      PM.add(createHeapObjectLinearizationPass());
      added = true;
    } else if (passName == "mul-to-shift") {
      klee_message("Running mul-to-shift symbolic execution optimization pass");
      PM.add(createMulToShiftPass());
      added = true;
    } else {
      klee_warning("Unknown symbolic execution optimization pass: %s",
                   passName.c_str());
    }
  }

  if (added) {
    PM.run(M);

    // Lower intrinsics introduced by the passes (e.g., llvm.memcpy)
    // as they might be run after KLEE's own IntrinsicCleanerPass.
    llvm::IntrinsicLowering IL(M.getDataLayout());
    for (auto &F : M) {
      for (auto &BB : F) {
        for (auto I = BB.begin(); I != BB.end();) {
          llvm::Instruction *Inst = &*I++;
          if (auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(Inst)) {
            switch (II->getIntrinsicID()) {
            case llvm::Intrinsic::memcpy:
            case llvm::Intrinsic::memmove:
            case llvm::Intrinsic::memset:
              IL.LowerIntrinsicCall(II);
              break;
            default:
              break;
            }
          }
        }
      }
    }
  }
}

void printSymExOptPasses() {
  llvm::outs() << "Available symbolic execution optimization passes:\n"
               << "  dummy-pass\n"
               << "  anti-flattening\n"
               << "  struct-blasting\n"
               << "  object-linearization\n"
               << "  heap-object-linearization\n"
               << "  mul-to-shift\n";
}

} // namespace klee
