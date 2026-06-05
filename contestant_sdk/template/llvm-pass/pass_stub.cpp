#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

namespace {

class ContestantPass : public llvm::PassInfoMixin<ContestantPass> {
public:
    llvm::PreservedAnalyses run(llvm::Module &, llvm::ModuleAnalysisManager &) {
        // The baseline submission deliberately keeps the official serial
        // baseline unchanged so it can serve as a judge-pipeline smoke test.
        return llvm::PreservedAnalyses::all();
    }

    static bool isRequired() {
        return true;
    }
};

}  // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION,
        "contestant-pass",
        "0.1",
        [](llvm::PassBuilder &pass_builder) {
            pass_builder.registerPipelineParsingCallback(
                [](llvm::StringRef name, llvm::ModulePassManager &module_pass_manager,
                   llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
                    if (name != "contestant-pass") {
                        return false;
                    }
                    module_pass_manager.addPass(ContestantPass());
                    return true;
                });
        }
    };
}
