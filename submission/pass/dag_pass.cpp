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

#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char *kBegin = "compiler2026_runtime_begin";
constexpr const char *kAlloc = "compiler2026_runtime_alloc";
constexpr const char *kSubmit = "compiler2026_runtime_submit";
constexpr const char *kSubmitDeps = "compiler2026_runtime_submit_deps";
constexpr const char *kSubmitDeps3 = "compiler2026_runtime_submit_deps3";
constexpr const char *kSubmitDeps3Priority =
    "compiler2026_runtime_submit_deps3_priority";
constexpr const char *kRegisterTask = "compiler2026_runtime_register_task";
constexpr const char *kShouldAsync = "compiler2026_runtime_should_async";
constexpr const char *kWait = "compiler2026_runtime_wait";
constexpr const char *kWaitKey = "compiler2026_runtime_wait_key";
constexpr const char *kEnd = "compiler2026_runtime_end";
constexpr const char *kTileDagAnnotation =
    "compiler2026.graph.block_cholesky.tile_dag.v1";
constexpr int kNoOutputKey = -1;

struct RuntimeApi {
    llvm::FunctionCallee begin;
    llvm::FunctionCallee alloc;
    llvm::FunctionCallee submit;
    llvm::FunctionCallee submit_deps;
    llvm::FunctionCallee submit_deps3;
    llvm::FunctionCallee submit_deps3_priority;
    llvm::FunctionCallee register_task;
    llvm::FunctionCallee should_async;
    llvm::FunctionCallee wait;
    llvm::FunctionCallee end;
};

struct TaskIr {
    llvm::StructType *context_ty;
    llvm::Function *cholesky_task;
    llvm::Function *trsm_task;
    llvm::Function *madd_task;
};

struct BlockCoordinate {
    llvm::Value *row;
    llvm::Value *col;
    llvm::Value *linear_key;
};

enum class SemanticPriority {
    none,
    cholesky,
    trsm,
    madd,
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

bool crossPanelDagEnabledFromEnv() {
    const char *env = std::getenv("COMPILER2026_ENABLE_CROSS_PANEL_DAG");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

bool crossPanelSyncCholeskyEnabledFromEnv() {
    const char *env = std::getenv("COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

bool criticalPriorityEnabledFromEnv() {
    const char *env = std::getenv("COMPILER2026_DAG_CRITICAL_PRIORITY");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

bool hasFunctionAnnotation(llvm::Function &function, llvm::StringRef annotation) {
    llvm::GlobalVariable *annotations =
        function.getParent()->getGlobalVariable("llvm.global.annotations");
    if (annotations == nullptr || !annotations->hasInitializer()) {
        return false;
    }

    auto *entries = llvm::dyn_cast<llvm::ConstantArray>(annotations->getInitializer());
    if (entries == nullptr) {
        return false;
    }

    for (llvm::Value *entry_value : entries->operands()) {
        auto *entry = llvm::dyn_cast<llvm::ConstantStruct>(entry_value);
        if (entry == nullptr || entry->getNumOperands() < 2 ||
            entry->getOperand(0)->stripPointerCasts() != &function) {
            continue;
        }

        auto *text_global = llvm::dyn_cast<llvm::GlobalVariable>(
            entry->getOperand(1)->stripPointerCasts());
        if (text_global == nullptr || !text_global->hasInitializer()) {
            continue;
        }
        auto *text =
            llvm::dyn_cast<llvm::ConstantDataSequential>(text_global->getInitializer());
        if (text != nullptr && text->isCString() && text->getAsCString() == annotation) {
            return true;
        }
    }
    return false;
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
        declareVoidRuntime(module, kSubmitDeps3,
                           {ptr_ty, ptr_ty, int32_ty, int32_ty, int32_ty, int32_ty}),
        declareVoidRuntime(module, kSubmitDeps3Priority,
                           {ptr_ty, ptr_ty, int32_ty, int32_ty, int32_ty, int32_ty,
                            int32_ty}),
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

void buildCholeskyTaskBody(llvm::Module &module, llvm::StructType *context_ty,
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
        builder.CreateLoad(builder.getInt32Ty(), fieldPtr(builder, context_ty, context_arg, 3));
    llvm::Value *arg3 =
        builder.CreateLoad(builder.getInt32Ty(), fieldPtr(builder, context_ty, context_arg, 4));

    builder.CreateCall(operator_fn, {arg0, arg1, arg2, arg3});
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

    return {context_ty, nullptr, trsm_task, madd_task};
}

llvm::Function *getCholeskyTask(llvm::Module &module, llvm::StructType *context_ty) {
    llvm::Function *cholesky_task = module.getFunction("compiler2026_task_cholesky");
    if (cholesky_task != nullptr) {
        return cholesky_task;
    }

    llvm::LLVMContext &context = module.getContext();
    llvm::Type *ptr_ty = llvm::PointerType::getUnqual(context);
    llvm::Type *int32_ty = llvm::Type::getInt32Ty(context);
    llvm::FunctionType *task_ty =
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr_ty}, false);
    llvm::FunctionType *cholesky_ty =
        llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                {ptr_ty, ptr_ty, int32_ty, int32_ty}, false);

    cholesky_task = llvm::Function::Create(task_ty, llvm::GlobalValue::InternalLinkage,
                                           "compiler2026_task_cholesky", module);
    cholesky_task->addFnAttr("compiler2026.skip");
    buildCholeskyTaskBody(module, context_ty, cholesky_task,
                          module.getOrInsertFunction("cholesky", cholesky_ty));
    return cholesky_task;
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

llvm::Loop *outermostLoop(llvm::Loop *loop) {
    if (loop == nullptr) {
        return nullptr;
    }
    while (loop->getParentLoop() != nullptr) {
        loop = loop->getParentLoop();
    }
    return loop;
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
    async_fn->setDSOLocal(true);
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

bool canBuildLinearElementOffset(llvm::Value *ptr) {
    auto *gep = llvm::dyn_cast<llvm::GEPOperator>(ptr->stripPointerCasts());
    if (gep == nullptr || gep->getNumIndices() != 1) {
        return false;
    }

    llvm::Value *base = gep->getPointerOperand()->stripPointerCasts();
    return !llvm::isa<llvm::GEPOperator>(base) || canBuildLinearElementOffset(base);
}

bool canBuildBlockCoordinate(llvm::Value *ptr, llvm::Value *semantic_base = nullptr) {
    if (semantic_base != nullptr) {
        return ptr->getType()->isPointerTy() && semantic_base->getType()->isPointerTy();
    }
    return canBuildLinearElementOffset(ptr);
}

std::optional<BlockCoordinate> buildBlockCoordinate(llvm::IRBuilder<> &builder,
                                                   llvm::Value *ptr,
                                                   llvm::Value *n, llvm::Value *b,
                                                   llvm::Value *semantic_base = nullptr) {
    llvm::Module *module = builder.GetInsertBlock()->getModule();
    llvm::Type *offset_ty = llvm::IntegerType::get(
        module->getContext(), module->getDataLayout().getPointerSizeInBits());
    llvm::Value *element_offset = nullptr;
    if (semantic_base != nullptr && ptr->getType()->isPointerTy() &&
        semantic_base->getType()->isPointerTy()) {
        // tile_dag.v1 guarantees that operator tile pointers are derived from
        // the row-major double matrix L. Pointer subtraction therefore remains
        // valid even after Clang folds the original GEP chain into PHI nodes.
        llvm::Value *ptr_int = builder.CreatePtrToInt(ptr, offset_ty);
        llvm::Value *base_int = builder.CreatePtrToInt(semantic_base, offset_ty);
        llvm::Value *byte_offset = builder.CreateSub(ptr_int, base_int);
        element_offset = builder.CreateSDiv(
            byte_offset, llvm::ConstantInt::get(offset_ty, sizeof(double)));
    } else {
        std::optional<llvm::Value *> offset =
            buildLinearElementOffset(builder, ptr, offset_ty);
        if (!offset) {
            return std::nullopt;
        }
        element_offset = *offset;
    }

    llvm::Value *wide_n = builder.CreateSExtOrTrunc(n, element_offset->getType());
    llvm::Value *wide_b = builder.CreateSExtOrTrunc(b, element_offset->getType());
    llvm::Value *element_row = builder.CreateSDiv(element_offset, wide_n);
    llvm::Value *element_col = builder.CreateSRem(element_offset, wide_n);
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

bool insertWaitForBlockKey(llvm::CallBase *call, llvm::Value *n, llvm::Value *b,
                           unsigned arg_index, llvm::Value *semantic_base = nullptr) {
    if (arg_index >= call->arg_size()) {
        return false;
    }

    llvm::IRBuilder<> builder(call);
    std::optional<BlockCoordinate> coord =
        buildBlockCoordinate(builder, call->getArgOperand(arg_index), n, b,
                             semantic_base);
    if (!coord) {
        return false;
    }

    llvm::FunctionCallee wait_key =
        declareVoidRuntime(*call->getModule(), kWaitKey, {builder.getInt32Ty()});
    builder.CreateCall(wait_key, {coord->linear_key});
    return true;
}

bool replaceOperatorCallWithDagSubmit(llvm::CallBase *call, RuntimeApi &runtime,
                                      TaskIr &task_ir, llvm::Function *task_fn,
                                      llvm::Value *n, llvm::Value *b,
                                      llvm::ArrayRef<unsigned> dep_args, int output_arg,
                                      SemanticPriority semantic_priority =
                                          SemanticPriority::none,
                                      llvm::Value *semantic_base = nullptr) {
    llvm::Module *module = call->getModule();
    llvm::IRBuilder<> builder(call);

    std::vector<llvm::Value *> dep_keys;
    std::vector<BlockCoordinate> dep_coords;
    dep_keys.reserve(3);
    dep_coords.reserve(3);
    for (unsigned arg_index : dep_args) {
        std::optional<BlockCoordinate> coord =
            buildBlockCoordinate(builder, call->getArgOperand(arg_index), n, b,
                                 semantic_base);
        if (!coord) {
            return false;
        }
        dep_keys.push_back(coord->linear_key);
        dep_coords.push_back(*coord);
    }

    while (dep_keys.size() < 2) {
        dep_keys.push_back(builder.getInt32(kNoOutputKey));
    }

    llvm::Value *output_key = builder.getInt32(kNoOutputKey);
    std::optional<BlockCoordinate> output_coord;
    if (output_arg >= 0) {
        output_coord =
            buildBlockCoordinate(builder, call->getArgOperand(static_cast<unsigned>(output_arg)),
                                 n, b, semantic_base);
        if (!output_coord) {
            return false;
        }
        output_key = output_coord->linear_key;
    }

    llvm::Value *priority = nullptr;
    switch (semantic_priority) {
    case SemanticPriority::none:
        break;
    case SemanticPriority::cholesky:
        priority = builder.getInt32(3);
        break;
    case SemanticPriority::trsm:
        if (!output_coord) {
            return false;
        }
        priority = builder.CreateSelect(
            builder.CreateICmpEQ(output_coord->row,
                                 builder.CreateAdd(output_coord->col,
                                                   llvm::ConstantInt::get(
                                                       output_coord->col->getType(), 1))),
            builder.getInt32(2), builder.getInt32(1));
        break;
    case SemanticPriority::madd:
        if (!output_coord || dep_coords.empty()) {
            return false;
        }
        llvm::Value *updates_next_panel = builder.CreateICmpEQ(
            output_coord->col,
            builder.CreateAdd(dep_coords.front().col,
                              llvm::ConstantInt::get(
                                  dep_coords.front().col->getType(), 1)));
        llvm::Value *updates_diagonal =
            builder.CreateICmpEQ(output_coord->row, output_coord->col);
        priority = builder.CreateSelect(
            updates_next_panel,
            builder.CreateSelect(updates_diagonal, builder.getInt32(3),
                                 builder.getInt32(1)),
            builder.getInt32(0));
        break;
    }

    const llvm::DataLayout &layout = module->getDataLayout();
    const uint64_t context_size = layout.getTypeAllocSize(task_ir.context_ty).getFixedValue();
    llvm::Type *size_ty =
        llvm::IntegerType::get(module->getContext(), layout.getPointerSizeInBits());

    llvm::Value *context = builder.CreateCall(
        runtime.alloc, {llvm::ConstantInt::get(size_ty, context_size)});

    if (call->arg_size() == 4) {
        builder.CreateStore(call->getArgOperand(0), fieldPtr(builder, task_ir.context_ty, context, 0));
        builder.CreateStore(call->getArgOperand(1), fieldPtr(builder, task_ir.context_ty, context, 1));
        builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()),
                            fieldPtr(builder, task_ir.context_ty, context, 2));
        builder.CreateStore(call->getArgOperand(2), fieldPtr(builder, task_ir.context_ty, context, 3));
        builder.CreateStore(call->getArgOperand(3), fieldPtr(builder, task_ir.context_ty, context, 4));
    } else {
        for (unsigned i = 0; i < 5; ++i) {
            builder.CreateStore(call->getArgOperand(i),
                                fieldPtr(builder, task_ir.context_ty, context, i));
        }
    }

    if (priority != nullptr) {
        while (dep_keys.size() < 3) {
            dep_keys.push_back(builder.getInt32(kNoOutputKey));
        }
        builder.CreateCall(runtime.submit_deps3_priority,
                           {task_fn, context, dep_keys[0], dep_keys[1], dep_keys[2],
                            output_key, priority});
    } else if (dep_keys.size() > 2) {
        builder.CreateCall(runtime.submit_deps3,
                           {task_fn, context, dep_keys[0], dep_keys[1], dep_keys[2],
                            output_key});
    } else {
        builder.CreateCall(runtime.submit_deps,
                           {task_fn, context, dep_keys[0], dep_keys[1], output_key});
    }
    call->eraseFromParent();
    return true;
}

bool callsHaveCoordinates(llvm::ArrayRef<llvm::CallBase *> calls,
                          llvm::ArrayRef<unsigned> arg_indices,
                          llvm::Value *semantic_base = nullptr) {
    for (llvm::CallBase *call : calls) {
        for (unsigned arg_index : arg_indices) {
            if (arg_index >= call->arg_size() ||
                !canBuildBlockCoordinate(call->getArgOperand(arg_index), semantic_base)) {
                return false;
            }
        }
    }
    return true;
}

void transformAsyncFunction(llvm::Function &async_fn, RuntimeApi &runtime, TaskIr &task_ir,
                            bool has_tile_dag_annotation) {
    llvm::DominatorTree dominator_tree(async_fn);
    llvm::LoopInfo loop_info(dominator_tree);

    auto arg = async_fn.arg_begin();
    (void)&*arg++;
    llvm::Value *matrix_l = &*arg++;
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

    std::vector<llvm::CallBase *> cholesky_calls;
    std::vector<llvm::CallBase *> trsm_calls;
    std::vector<llvm::CallBase *> madd_calls;
    for (llvm::BasicBlock &block : async_fn) {
        for (llvm::Instruction &instruction : block) {
            auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if (call == nullptr) {
                continue;
            }

            const llvm::StringRef callee = getCalledName(*call);
            if (callee == "cholesky") {
                cholesky_calls.push_back(call);
            } else if (callee == "trsm") {
                trsm_calls.push_back(call);
            } else if (callee == "madd") {
                madd_calls.push_back(call);
            }
        }
    }

    std::set<llvm::BasicBlock *> wait_blocks;
    const bool cross_panel_enabled = crossPanelDagEnabledFromEnv();
    const bool sync_cholesky = crossPanelSyncCholeskyEnabledFromEnv();
    const bool use_semantic_priority =
        cross_panel_enabled && has_tile_dag_annotation && criticalPriorityEnabledFromEnv();
    llvm::Value *semantic_base =
        cross_panel_enabled && has_tile_dag_annotation ? matrix_l : nullptr;
    const bool can_cross_panel_sync =
        cross_panel_enabled &&
        sync_cholesky &&
        !cholesky_calls.empty() &&
        callsHaveCoordinates(cholesky_calls, {0}, semantic_base) &&
        callsHaveCoordinates(trsm_calls, {0, 1, 2}, semantic_base) &&
        callsHaveCoordinates(madd_calls, {0, 1, 2}, semantic_base);
    const bool can_cross_panel_task =
        cross_panel_enabled &&
        !sync_cholesky &&
        !cholesky_calls.empty() &&
        callsHaveCoordinates(cholesky_calls, {0, 1}, semantic_base) &&
        callsHaveCoordinates(trsm_calls, {0, 1, 2}, semantic_base) &&
        callsHaveCoordinates(madd_calls, {0, 1, 2}, semantic_base);

    if (can_cross_panel_sync) {
        for (llvm::CallBase *call : cholesky_calls) {
            if (!insertWaitForBlockKey(call, n, b, 0, semantic_base)) {
                return;
            }
        }

        for (llvm::CallBase *call : trsm_calls) {
            replaceOperatorCallWithDagSubmit(call, runtime, task_ir, task_ir.trsm_task,
                                             n, b, {0, 1}, 2,
                                             use_semantic_priority
                                                 ? SemanticPriority::trsm
                                                 : SemanticPriority::none,
                                             semantic_base);
        }

        for (llvm::CallBase *call : madd_calls) {
            llvm::Loop *outer_loop = outermostLoop(loop_info.getLoopFor(call->getParent()));
            replaceOperatorCallWithDagSubmit(call, runtime, task_ir, task_ir.madd_task,
                                             n, b, {0, 1, 2}, 2,
                                             use_semantic_priority
                                                 ? SemanticPriority::madd
                                                 : SemanticPriority::none,
                                             semantic_base);
            addLoopExitWaits(outer_loop, runtime.wait, wait_blocks);
        }
    } else if (can_cross_panel_task) {
        task_ir.cholesky_task = getCholeskyTask(*async_fn.getParent(), task_ir.context_ty);
        entry_builder.CreateCall(runtime.register_task,
                                 {task_ir.cholesky_task,
                                  entry_builder.CreateGlobalStringPtr("cholesky")});

        for (llvm::CallBase *call : cholesky_calls) {
            replaceOperatorCallWithDagSubmit(call, runtime, task_ir, task_ir.cholesky_task,
                                             n, b, {0}, 1,
                                             use_semantic_priority
                                                 ? SemanticPriority::cholesky
                                                 : SemanticPriority::none,
                                             semantic_base);
        }

        for (llvm::CallBase *call : trsm_calls) {
            replaceOperatorCallWithDagSubmit(call, runtime, task_ir, task_ir.trsm_task,
                                             n, b, {0, 1}, 2,
                                             use_semantic_priority
                                                 ? SemanticPriority::trsm
                                                 : SemanticPriority::none,
                                             semantic_base);
        }

        for (llvm::CallBase *call : madd_calls) {
            llvm::Loop *outer_loop = outermostLoop(loop_info.getLoopFor(call->getParent()));
            replaceOperatorCallWithDagSubmit(call, runtime, task_ir, task_ir.madd_task,
                                             n, b, {0, 1, 2}, 2,
                                             use_semantic_priority
                                                 ? SemanticPriority::madd
                                                 : SemanticPriority::none,
                                             semantic_base);
            addLoopExitWaits(outer_loop, runtime.wait, wait_blocks);
        }
    } else {
        for (llvm::CallBase *call : trsm_calls) {
            if (!replaceOperatorCallWithDagSubmit(call, runtime, task_ir, task_ir.trsm_task,
                                                  n, b, {}, 0, SemanticPriority::none,
                                                  semantic_base)) {
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
                                                  n, b, {0, 1}, kNoOutputKey,
                                                  SemanticPriority::none, semantic_base)) {
                replaceOperatorCallWithTaskSubmit(call, runtime, task_ir, task_ir.madd_task);
            }
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

        const bool has_tile_dag_annotation =
            hasFunctionAnnotation(function, kTileDagAnnotation);
        llvm::Function *async_fn = cloneForAsync(function);
        transformAsyncFunction(*async_fn, runtime, task_ir, has_tile_dag_annotation);
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
