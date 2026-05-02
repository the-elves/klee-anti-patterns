#ifndef KLEE_SYMEXOPTPASSES_PASSES_H
#define KLEE_SYMEXOPTPASSES_PASSES_H

#include <string>
#include <vector>

namespace llvm {
class Module;
}

namespace klee {
void applySymExOptPasses(llvm::Module &M,
                         const std::vector<std::string> &passes);
void printSymExOptPasses();
}

#endif /* KLEE_SYMEXOPTPASSES_PASSES_H */
