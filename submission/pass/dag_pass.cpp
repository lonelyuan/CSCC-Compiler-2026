#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"

#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char *kBegin = "compiler2026_runtime_begin";
constexpr const char *kSubmitTrsm = "compiler2026_runtime_submit_trsm";
constexpr const char *kSubmitMadd = "compiler2026_runtime_submit_madd";
constexpr const char *kWait = "compiler2026_runtime_wait";
constexpr const char *kEnd = "compiler2026_runtime_end";
constexpr int kAsyncMinBlockSize = 64;

bool isBlockCholesky(llvm::Function &function) {
    if (function.isDeclaration()) {
        return false;
    }
    if (function.hasFnAttribute("compiler2026.skip")) {
        return false;
    }
    if (function.arg_size() != 4 || !function.getReturnType()->isIntegerTy(32)) {
        return false;
    }

    const std::string name = function.getName().str();
    return name == "_ZN7contest14block_choleskyEPKdPdii" ||
           name.find("block_cholesky") != std::string::npos;
}

llvm::FunctionCallee declareVoidRuntime(llvm::Module &module, const char *name,
                                        llvm::ArrayRef<llvm::Type *> args) {
    llvm::FunctionType *fn_ty =
        llvm::FunctionType::get(llvm::Type::getVoidTy(module.getContext()), args, false);
    return module.getOrInsertFunction(name, fn_ty);
}

struct RuntimeApi {
    llvm::FunctionCallee begin;
    llvm::FunctionCallee submit_trsm;
    llvm::FunctionCallee submit_madd;
    llvm::FunctionCallee wait;
    llvm::FunctionCallee end;
};

RuntimeApi getRuntimeApi(llvm::Module &module) {
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *int32_ty = llvm::Type::getInt32Ty(context);
    llvm::Type *ptr_ty = llvm::PointerType::getUnqual(context);

    return {
        declareVoidRuntime(module, kBegin, {int32_ty, int32_ty}),
        declareVoidRuntime(module, kSubmitTrsm, {ptr_ty, ptr_ty, ptr_ty, int32_ty, int32_ty}),
        declareVoidRuntime(module, kSubmitMadd, {ptr_ty, ptr_ty, ptr_ty, int32_ty, int32_ty}),
        declareVoidRuntime(module, kWait, {}),
        declareVoidRuntime(module, kEnd, {}),
    };
}

llvm::StringRef getCalledName(llvm::CallBase &call) {
    llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr) {
        return {};
    }
    return callee->getName();
}

void addLoopExitWaits(llvm::Loop *loop, llvm::FunctionCallee wait,
                      std::set<llvm::BasicBlock *> &wait_blocks) {
    if (loop == nullptr) {
        return;
    }

    llvm::SmallVector<llvm::BasicBlock *, 4> exits;
    loop->getExitBlocks(exits);
    for (llvm::BasicBlock *exit : exits) {
        if (!wait_blocks.insert(exit).second) {
            continue;
        }
        llvm::IRBuilder<> builder(&*exit->getFirstInsertionPt());
        builder.CreateCall(wait);
    }
}

llvm::Function *cloneSerialFallback(llvm::Function &function) {
    llvm::Module *module = function.getParent();
    llvm::Function *fallback = llvm::Function::Create(
        function.getFunctionType(), llvm::GlobalValue::InternalLinkage,
        "compiler2026_serial_fallback", module);
    fallback->copyAttributesFrom(&function);
    fallback->addFnAttr("compiler2026.skip");

    llvm::ValueToValueMapTy value_map;
    auto dest_arg = fallback->arg_begin();
    for (const llvm::Argument &source_arg : function.args()) {
        dest_arg->setName(source_arg.getName());
        value_map[&source_arg] = &*dest_arg++;
    }

    llvm::SmallVector<llvm::ReturnInst *, 4> returns;
    llvm::CloneFunctionInto(fallback, &function, value_map,
                            llvm::CloneFunctionChangeType::LocalChangesOnly, returns);
    return fallback;
}

llvm::BasicBlock *insertSerialFallbackGuard(llvm::Function &function,
                                            llvm::Function *fallback,
                                            llvm::Value *b) {
    llvm::LLVMContext &context = function.getContext();
    llvm::BasicBlock &entry = function.getEntryBlock();
    llvm::Instruction *split_point = &*entry.getFirstInsertionPt();
    llvm::BasicBlock *optimized_block =
        entry.splitBasicBlock(split_point, "compiler2026.optimized");

    llvm::Instruction *old_branch = entry.getTerminator();
    old_branch->eraseFromParent();

    llvm::BasicBlock *fallback_block =
        llvm::BasicBlock::Create(context, "compiler2026.serial_fallback", &function,
                                 optimized_block);

    llvm::IRBuilder<> entry_builder(&entry);
    llvm::Value *use_fallback = entry_builder.CreateICmpSLT(
        b, llvm::ConstantInt::get(b->getType(), kAsyncMinBlockSize));
    entry_builder.CreateCondBr(use_fallback, fallback_block, optimized_block);

    llvm::IRBuilder<> fallback_builder(fallback_block);
    std::vector<llvm::Value *> args;
    args.reserve(function.arg_size());
    for (llvm::Argument &arg : function.args()) {
        args.push_back(&arg);
    }
    llvm::CallInst *result = fallback_builder.CreateCall(fallback, args);
    fallback_builder.CreateRet(result);
    return optimized_block;
}

class OperatorDagPass : public llvm::PassInfoMixin<OperatorDagPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function &function,
                                llvm::FunctionAnalysisManager &analysis_manager) {
        if (!isBlockCholesky(function)) {
            return llvm::PreservedAnalyses::all();
        }

        llvm::Module *module = function.getParent();
        RuntimeApi runtime = getRuntimeApi(*module);
        llvm::LoopInfo &loop_info = analysis_manager.getResult<llvm::LoopAnalysis>(function);

        auto arg = function.arg_begin();
        (void)&*arg++;
        (void)&*arg++;
        llvm::Value *n = &*arg++;
        llvm::Value *b = &*arg++;

        llvm::Function *serial_fallback = cloneSerialFallback(function);
        llvm::BasicBlock *optimized_block = insertSerialFallbackGuard(function, serial_fallback, b);

        llvm::IRBuilder<> entry_builder(&*optimized_block->getFirstInsertionPt());
        entry_builder.CreateCall(runtime.begin, {n, b});

        std::vector<llvm::CallBase *> trsm_calls;
        std::vector<llvm::CallBase *> madd_calls;
        for (llvm::BasicBlock &block : function) {
            for (llvm::Instruction &instruction : block) {
                auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (call == nullptr) {
                    continue;
                }

                const llvm::StringRef callee = getCalledName(*call);
                if (callee == "trsm") {
                    trsm_calls.push_back(call);
                } else if (callee == "madd") {
                    madd_calls.push_back(call);
                }
            }
        }

        std::set<llvm::BasicBlock *> wait_blocks;

        for (llvm::CallBase *call : trsm_calls) {
            llvm::Loop *loop = loop_info.getLoopFor(call->getParent());
            addLoopExitWaits(loop, runtime.wait, wait_blocks);

            llvm::IRBuilder<> builder(call);
            builder.CreateCall(runtime.submit_trsm,
                               {call->getArgOperand(0), call->getArgOperand(1),
                                call->getArgOperand(2), call->getArgOperand(3),
                                call->getArgOperand(4)});
            call->eraseFromParent();
        }

        for (llvm::CallBase *call : madd_calls) {
            llvm::Loop *loop = loop_info.getLoopFor(call->getParent());
            llvm::Loop *sync_loop = (loop != nullptr && loop->getParentLoop() != nullptr)
                                        ? loop->getParentLoop()
                                        : loop;
            addLoopExitWaits(sync_loop, runtime.wait, wait_blocks);

            llvm::IRBuilder<> builder(call);
            builder.CreateCall(runtime.submit_madd,
                               {call->getArgOperand(0), call->getArgOperand(1),
                                call->getArgOperand(2), call->getArgOperand(3),
                                call->getArgOperand(4)});
            call->eraseFromParent();
        }

        for (llvm::BasicBlock &block : function) {
            if (block.getName() == "compiler2026.serial_fallback") {
                continue;
            }
            if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())) {
                llvm::IRBuilder<> builder(ret);
                builder.CreateCall(runtime.end);
            }
        }

        return llvm::PreservedAnalyses::none();
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
        "0.2",
        [](llvm::PassBuilder &pass_builder) {
            pass_builder.registerPipelineParsingCallback(
                [](llvm::StringRef name, llvm::ModulePassManager &module_pass_manager,
                   llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
                    if (name != "contestant-pass") {
                        return false;
                    }
                    llvm::FunctionPassManager function_pass_manager;
                    function_pass_manager.addPass(OperatorDagPass());
                    module_pass_manager.addPass(
                        llvm::createModuleToFunctionPassAdaptor(std::move(function_pass_manager)));
                    return true;
                });
        }
    };
}
