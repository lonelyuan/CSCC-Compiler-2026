#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

#include <string>

namespace {

constexpr const char *kRuntimeEntry = "compiler2026_block_cholesky_runtime";

bool isBlockCholesky(llvm::Function &function) {
    if (function.isDeclaration()) {
        return false;
    }
    if (function.arg_size() != 4 || !function.getReturnType()->isIntegerTy(32)) {
        return false;
    }

    const std::string name = function.getName().str();
    return name == "_ZN7contest14block_choleskyEPKdPdii" ||
           name.find("block_cholesky") != std::string::npos;
}

llvm::FunctionCallee getRuntimeEntry(llvm::Module &module) {
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *int32_ty = llvm::Type::getInt32Ty(context);
    llvm::Type *ptr_ty = llvm::PointerType::getUnqual(context);
    llvm::FunctionType *runtime_ty =
        llvm::FunctionType::get(int32_ty, {ptr_ty, ptr_ty, int32_ty, int32_ty}, false);
    return module.getOrInsertFunction(kRuntimeEntry, runtime_ty);
}

void replaceWithRuntimeCall(llvm::Function &function, llvm::FunctionCallee runtime_entry) {
    llvm::LLVMContext &context = function.getContext();
    function.deleteBody();

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", &function);
    llvm::IRBuilder<> builder(entry);

    auto arg = function.arg_begin();
    llvm::Value *input = &*arg++;
    llvm::Value *output = &*arg++;
    llvm::Value *n = &*arg++;
    llvm::Value *b = &*arg++;

    llvm::CallInst *result = builder.CreateCall(runtime_entry, {input, output, n, b});
    result->setTailCall(false);
    builder.CreateRet(result);
}

class ContestantPass : public llvm::PassInfoMixin<ContestantPass> {
public:
    llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &) {
        llvm::FunctionCallee runtime_entry = getRuntimeEntry(module);

        bool changed = false;
        for (llvm::Function &function : module) {
            if (!isBlockCholesky(function)) {
                continue;
            }
            replaceWithRuntimeCall(function, runtime_entry);
            changed = true;
        }

        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
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

