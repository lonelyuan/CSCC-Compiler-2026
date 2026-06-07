#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char *kBegin = "compiler2026_runtime_begin";
constexpr const char *kAlloc = "compiler2026_runtime_alloc";
constexpr const char *kSubmit = "compiler2026_runtime_submit";
constexpr const char *kSubmitDeps = "compiler2026_runtime_submit_deps";
constexpr const char *kRegisterTask = "compiler2026_runtime_register_task";
constexpr const char *kShouldAsync = "compiler2026_runtime_should_async";
constexpr const char *kWait = "compiler2026_runtime_wait";
constexpr const char *kEnd = "compiler2026_runtime_end";
constexpr int kNoOutputKey = -1;

struct RuntimeApi {
    llvm::FunctionCallee begin;
    llvm::FunctionCallee alloc;
    llvm::FunctionCallee submit;
    llvm::FunctionCallee submit_deps;
    llvm::FunctionCallee register_task;
    llvm::FunctionCallee should_async;
    llvm::FunctionCallee wait;
    llvm::FunctionCallee end;
};

struct TaskIr {
    llvm::StructType *context_ty;
    llvm::Function *trsm_task;
    llvm::Function *madd_task;
};

struct BlockCoordinate {
    llvm::Value *row;
    llvm::Value *col;
    llvm::Value *linear_key;
};

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

RuntimeApi getRuntimeApi(llvm::Module &module) {
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *int32_ty = llvm::Type::getInt32Ty(context);
    llvm::Type *ptr_ty = llvm::PointerType::getUnqual(context);
    llvm::Type *size_ty =
        llvm::IntegerType::get(context, module.getDataLayout().getPointerSizeInBits());

    llvm::FunctionType *alloc_ty = llvm::FunctionType::get(ptr_ty, {size_ty}, false);

    return {
        declareVoidRuntime(module, kBegin, {int32_ty, int32_ty}),
        module.getOrInsertFunction(kAlloc, alloc_ty),
        declareVoidRuntime(module, kSubmit, {ptr_ty, ptr_ty}),
        declareVoidRuntime(module, kSubmitDeps, {ptr_ty, ptr_ty, int32_ty, int32_ty, int32_ty}),
        declareVoidRuntime(module, kRegisterTask, {ptr_ty, ptr_ty}),
        module.getOrInsertFunction(kShouldAsync,
                                   llvm::FunctionType::get(int32_ty, {int32_ty, int32_ty}, false)),
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

llvm::Value *fieldPtr(llvm::IRBuilder<> &builder, llvm::StructType *context_ty,
                      llvm::Value *context, unsigned index) {
    llvm::Value *zero = builder.getInt32(0);
    llvm::Value *field = builder.getInt32(index);
    return builder.CreateInBoundsGEP(context_ty, context, {zero, field});
}

void buildOperatorTaskBody(llvm::Module &module, llvm::StructType *context_ty,
                           llvm::Function *task_fn, llvm::FunctionCallee operator_fn) {
    llvm::LLVMContext &context = module.getContext();
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", task_fn);
    llvm::IRBuilder<> builder(entry);

    llvm::Argument *context_arg = task_fn->arg_begin();
    llvm::Value *arg0 =
        builder.CreateLoad(builder.getPtrTy(), fieldPtr(builder, context_ty, context_arg, 0));
    llvm::Value *arg1 =
        builder.CreateLoad(builder.getPtrTy(), fieldPtr(builder, context_ty, context_arg, 1));
    llvm::Value *arg2 =
        builder.CreateLoad(builder.getPtrTy(), fieldPtr(builder, context_ty, context_arg, 2));
    llvm::Value *arg3 =
        builder.CreateLoad(builder.getInt32Ty(), fieldPtr(builder, context_ty, context_arg, 3));
    llvm::Value *arg4 =
        builder.CreateLoad(builder.getInt32Ty(), fieldPtr(builder, context_ty, context_arg, 4));

    builder.CreateCall(operator_fn, {arg0, arg1, arg2, arg3, arg4});
    builder.CreateRetVoid();
}

TaskIr getTaskIr(llvm::Module &module) {
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *ptr_ty = llvm::PointerType::getUnqual(context);
    llvm::Type *int32_ty = llvm::Type::getInt32Ty(context);

    llvm::StructType *context_ty =
        llvm::StructType::getTypeByName(context, "compiler2026.operator_context");
    if (context_ty == nullptr) {
        context_ty = llvm::StructType::create(context, "compiler2026.operator_context");
        context_ty->setBody({ptr_ty, ptr_ty, ptr_ty, int32_ty, int32_ty});
    }

    llvm::FunctionType *task_ty =
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr_ty}, false);
    llvm::FunctionType *operator_ty =
        llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                {ptr_ty, ptr_ty, ptr_ty, int32_ty, int32_ty}, false);

    llvm::Function *trsm_task = module.getFunction("compiler2026_task_trsm");
    if (trsm_task == nullptr) {
        trsm_task = llvm::Function::Create(task_ty, llvm::GlobalValue::InternalLinkage,
                                           "compiler2026_task_trsm", module);
        trsm_task->addFnAttr("compiler2026.skip");
        buildOperatorTaskBody(module, context_ty, trsm_task,
                              module.getOrInsertFunction("trsm", operator_ty));
    }

    llvm::Function *madd_task = module.getFunction("compiler2026_task_madd");
    if (madd_task == nullptr) {
        madd_task = llvm::Function::Create(task_ty, llvm::GlobalValue::InternalLinkage,
                                           "compiler2026_task_madd", module);
        madd_task->addFnAttr("compiler2026.skip");
        buildOperatorTaskBody(module, context_ty, madd_task,
                              module.getOrInsertFunction("madd", operator_ty));
    }

    return {context_ty, trsm_task, madd_task};
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

llvm::Function *cloneForAsync(llvm::Function &function) {
    llvm::Module *module = function.getParent();
    llvm::Function *async_fn = llvm::Function::Create(
        function.getFunctionType(), llvm::GlobalValue::InternalLinkage,
        "compiler2026_async_impl", module);
    async_fn->copyAttributesFrom(&function);
    async_fn->addFnAttr("compiler2026.skip");

    llvm::ValueToValueMapTy value_map;
    auto dest_arg = async_fn->arg_begin();
    for (const llvm::Argument &source_arg : function.args()) {
        dest_arg->setName(source_arg.getName());
        value_map[&source_arg] = &*dest_arg++;
    }

    llvm::SmallVector<llvm::ReturnInst *, 4> returns;
    llvm::CloneFunctionInto(async_fn, &function, value_map,
                            llvm::CloneFunctionChangeType::LocalChangesOnly, returns);
    return async_fn;
}

void insertAsyncDispatch(llvm::Function &function, llvm::Function *async_fn,
                         llvm::FunctionCallee should_async, llvm::Value *n, llvm::Value *b) {
    llvm::LLVMContext &context = function.getContext();
    llvm::BasicBlock &entry = function.getEntryBlock();
    llvm::Instruction *split_point = &*entry.getFirstInsertionPt();
    llvm::BasicBlock *serial_block = entry.splitBasicBlock(split_point, "compiler2026.serial");

    llvm::Instruction *old_branch = entry.getTerminator();
    old_branch->eraseFromParent();

    llvm::BasicBlock *async_block =
        llvm::BasicBlock::Create(context, "compiler2026.async", &function, serial_block);

    llvm::IRBuilder<> entry_builder(&entry);
    llvm::Value *decision = entry_builder.CreateCall(should_async, {n, b});
    llvm::Value *use_async =
        entry_builder.CreateICmpNE(decision, llvm::ConstantInt::get(decision->getType(), 0));
    entry_builder.CreateCondBr(use_async, async_block, serial_block);

    llvm::IRBuilder<> async_builder(async_block);
    std::vector<llvm::Value *> args;
    args.reserve(function.arg_size());
    for (llvm::Argument &arg : function.args()) {
        args.push_back(&arg);
    }
    llvm::CallInst *result = async_builder.CreateCall(async_fn, args);
    async_builder.CreateRet(result);
}

void replaceOperatorCallWithTaskSubmit(llvm::CallBase *call, RuntimeApi &runtime,
                                       TaskIr &task_ir, llvm::Function *task_fn) {
    llvm::Module *module = call->getModule();
    llvm::IRBuilder<> builder(call);
    const llvm::DataLayout &layout = module->getDataLayout();
    const uint64_t context_size = layout.getTypeAllocSize(task_ir.context_ty).getFixedValue();
    llvm::Type *size_ty =
        llvm::IntegerType::get(module->getContext(), layout.getPointerSizeInBits());

    llvm::Value *context = builder.CreateCall(
        runtime.alloc, {llvm::ConstantInt::get(size_ty, context_size)});

    for (unsigned i = 0; i < 5; ++i) {
        builder.CreateStore(call->getArgOperand(i), fieldPtr(builder, task_ir.context_ty, context, i));
    }

    builder.CreateCall(runtime.submit, {task_fn, context});
    call->eraseFromParent();
}

std::optional<llvm::Value *> buildLinearElementOffset(llvm::IRBuilder<> &builder,
                                                      llvm::Value *ptr,
                                                      llvm::Type *offset_ty) {
    auto *gep = llvm::dyn_cast<llvm::GEPOperator>(ptr->stripPointerCasts());
    if (gep == nullptr || gep->getNumIndices() != 1) {
        return std::nullopt;
    }

    llvm::Value *offset = builder.CreateSExtOrTrunc(gep->getOperand(1), offset_ty);
    llvm::Value *base = gep->getPointerOperand()->stripPointerCasts();
    if (llvm::isa<llvm::GEPOperator>(base)) {
        std::optional<llvm::Value *> base_offset =
            buildLinearElementOffset(builder, base, offset_ty);
        if (!base_offset) {
            return std::nullopt;
        }
        offset = builder.CreateAdd(*base_offset, offset);
    }
    return offset;
}

std::optional<BlockCoordinate> buildBlockCoordinate(llvm::IRBuilder<> &builder,
                                                   llvm::Value *ptr,
                                                   llvm::Value *n, llvm::Value *b) {
    llvm::Module *module = builder.GetInsertBlock()->getModule();
    llvm::Type *offset_ty = llvm::IntegerType::get(
        module->getContext(), module->getDataLayout().getPointerSizeInBits());
    std::optional<llvm::Value *> offset = buildLinearElementOffset(builder, ptr, offset_ty);
    if (!offset) {
        return std::nullopt;
    }

    llvm::Value *wide_n = builder.CreateSExtOrTrunc(n, (*offset)->getType());
    llvm::Value *wide_b = builder.CreateSExtOrTrunc(b, (*offset)->getType());
    llvm::Value *element_row = builder.CreateSDiv(*offset, wide_n);
    llvm::Value *element_col = builder.CreateSRem(*offset, wide_n);
    llvm::Value *block_row = builder.CreateSDiv(element_row, wide_b);
    llvm::Value *block_col = builder.CreateSDiv(element_col, wide_b);
    llvm::Value *block_count = builder.CreateSDiv(wide_n, wide_b);
    llvm::Value *linear_key = builder.CreateAdd(
        builder.CreateMul(block_row, block_count), block_col);
    return BlockCoordinate{
        block_row,
        block_col,
        builder.CreateTruncOrBitCast(linear_key, builder.getInt32Ty()),
    };
}

bool replaceOperatorCallWithDagSubmit(llvm::CallBase *call, RuntimeApi &runtime,
                                      TaskIr &task_ir, llvm::Function *task_fn,
                                      llvm::Value *n, llvm::Value *b,
                                      llvm::ArrayRef<unsigned> dep_args, int output_arg) {
    llvm::Module *module = call->getModule();
    llvm::IRBuilder<> builder(call);

    std::vector<llvm::Value *> dep_keys;
    dep_keys.reserve(2);
    for (unsigned arg_index : dep_args) {
        std::optional<BlockCoordinate> coord =
            buildBlockCoordinate(builder, call->getArgOperand(arg_index), n, b);
        if (!coord) {
            return false;
        }
        dep_keys.push_back(coord->linear_key);
    }

    while (dep_keys.size() < 2) {
        dep_keys.push_back(builder.getInt32(kNoOutputKey));
    }

    llvm::Value *output_key = builder.getInt32(kNoOutputKey);
    if (output_arg >= 0) {
        std::optional<BlockCoordinate> output_coord =
            buildBlockCoordinate(builder, call->getArgOperand(static_cast<unsigned>(output_arg)),
                                 n, b);
        if (!output_coord) {
            return false;
        }
        output_key = output_coord->linear_key;
    }

    const llvm::DataLayout &layout = module->getDataLayout();
    const uint64_t context_size = layout.getTypeAllocSize(task_ir.context_ty).getFixedValue();
    llvm::Type *size_ty =
        llvm::IntegerType::get(module->getContext(), layout.getPointerSizeInBits());

    llvm::Value *context = builder.CreateCall(
        runtime.alloc, {llvm::ConstantInt::get(size_ty, context_size)});

    for (unsigned i = 0; i < 5; ++i) {
        builder.CreateStore(call->getArgOperand(i), fieldPtr(builder, task_ir.context_ty, context, i));
    }

    builder.CreateCall(runtime.submit_deps,
                       {task_fn, context, dep_keys[0], dep_keys[1], output_key});
    call->eraseFromParent();
    return true;
}

void transformAsyncFunction(llvm::Function &async_fn, RuntimeApi &runtime, TaskIr &task_ir) {
    llvm::DominatorTree dominator_tree(async_fn);
    llvm::LoopInfo loop_info(dominator_tree);

    auto arg = async_fn.arg_begin();
    (void)&*arg++;
    (void)&*arg++;
    llvm::Value *n = &*arg++;
    llvm::Value *b = &*arg++;

    llvm::IRBuilder<> entry_builder(&*async_fn.getEntryBlock().getFirstInsertionPt());
    entry_builder.CreateCall(runtime.begin, {n, b});
    entry_builder.CreateCall(runtime.register_task,
                             {task_ir.trsm_task,
                              entry_builder.CreateGlobalStringPtr("trsm")});
    entry_builder.CreateCall(runtime.register_task,
                             {task_ir.madd_task,
                              entry_builder.CreateGlobalStringPtr("madd")});

    std::vector<llvm::CallBase *> trsm_calls;
    std::vector<llvm::CallBase *> madd_calls;
    for (llvm::BasicBlock &block : async_fn) {
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
        if (!replaceOperatorCallWithDagSubmit(call, runtime, task_ir, task_ir.trsm_task,
                                              n, b, {}, 0)) {
            llvm::Loop *loop = loop_info.getLoopFor(call->getParent());
            addLoopExitWaits(loop, runtime.wait, wait_blocks);
            replaceOperatorCallWithTaskSubmit(call, runtime, task_ir, task_ir.trsm_task);
        }
    }

    for (llvm::CallBase *call : madd_calls) {
        llvm::Loop *loop = loop_info.getLoopFor(call->getParent());
        llvm::Loop *sync_loop =
            (loop != nullptr && loop->getParentLoop() != nullptr) ? loop->getParentLoop() : loop;
        addLoopExitWaits(sync_loop, runtime.wait, wait_blocks);
        // Panel-local scheduling waits before the next panel, so madd outputs
        // have no downstream async consumers in the current DAG scope.
        if (!replaceOperatorCallWithDagSubmit(call, runtime, task_ir, task_ir.madd_task,
                                              n, b, {0, 1}, kNoOutputKey)) {
            replaceOperatorCallWithTaskSubmit(call, runtime, task_ir, task_ir.madd_task);
        }
    }

    for (llvm::BasicBlock &block : async_fn) {
        if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())) {
            llvm::IRBuilder<> builder(ret);
            builder.CreateCall(runtime.end);
        }
    }
}

class OperatorDagPass : public llvm::PassInfoMixin<OperatorDagPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function &function,
                                llvm::FunctionAnalysisManager &) {
        if (!isBlockCholesky(function)) {
            return llvm::PreservedAnalyses::all();
        }

        llvm::Module *module = function.getParent();
        RuntimeApi runtime = getRuntimeApi(*module);
        TaskIr task_ir = getTaskIr(*module);

        auto arg = function.arg_begin();
        (void)&*arg++;
        (void)&*arg++;
        llvm::Value *n = &*arg++;
        llvm::Value *b = &*arg++;

        llvm::Function *async_fn = cloneForAsync(function);
        transformAsyncFunction(*async_fn, runtime, task_ir);
        insertAsyncDispatch(function, async_fn, runtime.should_async, n, b);

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
        "0.4",
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
