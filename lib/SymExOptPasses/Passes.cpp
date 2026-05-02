#include "klee/SymExOptPasses/Passes.h"
#include "klee/Support/ErrorHandling.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

namespace llvm {
class FunctionPass;
}

llvm::FunctionPass *createAntiFlatteningPass();

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
    } else {
      klee_warning("Unknown symbolic execution optimization pass: %s",
                   passName.c_str());
    }
  }

  if (added) {
    PM.run(M);
  }
}

} // namespace klee
