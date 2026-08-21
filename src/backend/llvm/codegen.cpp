// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#include "backend/llvm/codegen.hpp"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agn::backend::llvm_backend {

using agn::parser::FunctionSignature;
using agn::parser::Type;
using agn::parser::TypeKind;

namespace {

bool isIntKind(TypeKind k) {
    return k == TypeKind::I64 || k == TypeKind::I32 || k == TypeKind::I8 ||
           k == TypeKind::U64 || k == TypeKind::U32 || k == TypeKind::U8;
}

struct TypedValue {
    llvm::Value* value = nullptr;
    Type type;
};

struct LocalVar {
    llvm::Value* addr;
    Type type;
};

} // namespace

struct Codegen::Impl {
    agn::parser::TypeChecker& checker;
    MemMode mode;
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> mod;
    llvm::IRBuilder<> builder;

    llvm::Type* ptrTy;
    llvm::Type* i64Ty;
    llvm::Type* i32Ty;
    llvm::Type* i8Ty;
    llvm::Type* i1Ty;
    llvm::Type* voidTy;
    llvm::Type* doubleTy;

    std::unordered_map<std::string, llvm::StructType*> structTypes;
    std::unordered_map<std::string, llvm::Function*> functionTable;
    std::unordered_map<std::string, llvm::GlobalVariable*> stringLiterals;
    int closureCounter = 0;

    llvm::Function* curFn = nullptr;
    llvm::Value* curEnv = nullptr;
    Type curReturnType{TypeKind::Void};
    bool curIsMain = false;
    std::string currentModulePrefix;
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> loopStack; // {continueTarget, breakTarget}
    std::unordered_map<std::string, LocalVar> locals;
    std::vector<llvm::Value*> arcTrackedClosures;
    std::unordered_set<std::string> capturedInCurrentFn;

    Impl(agn::parser::TypeChecker& c, MemMode m, const std::string& name)
        : checker(c), mode(m), builder(ctx) {
        mod = std::make_unique<llvm::Module>(name, ctx);
        ptrTy = llvm::PointerType::get(ctx, 0);
        i64Ty = llvm::Type::getInt64Ty(ctx);
        i32Ty = llvm::Type::getInt32Ty(ctx);
        i8Ty = llvm::Type::getInt8Ty(ctx);
        i1Ty = llvm::Type::getInt1Ty(ctx);
        voidTy = llvm::Type::getVoidTy(ctx);
        doubleTy = llvm::Type::getDoubleTy(ctx);
    }

    llvm::Type* llvmType(const Type& t) {
        switch (t.kind) {
            case TypeKind::I64: case TypeKind::U64: return i64Ty;
            case TypeKind::I32: case TypeKind::U32: return i32Ty;
            case TypeKind::I8: case TypeKind::U8: return i8Ty;
            case TypeKind::F64: return doubleTy;
            case TypeKind::Bool: return i1Ty;
            case TypeKind::Void: return voidTy;
            case TypeKind::String: return ptrTy;
            case TypeKind::Ptr: return ptrTy;
            case TypeKind::Function: return ptrTy;
            case TypeKind::Struct: {
                auto it = structTypes.find(t.structName);
                return it != structTypes.end() ? static_cast<llvm::Type*>(it->second)
                                                : static_cast<llvm::Type*>(i64Ty);
            }
            case TypeKind::Array:
                return llvm::ArrayType::get(llvmType(*t.elementType), t.arraySize);
            default:
                return i64Ty;
        }
    }

    static bool isUnsignedType(const Type& t) {
        return t.kind == TypeKind::U64 || t.kind == TypeKind::U32 || t.kind == TypeKind::U8;
    }

    llvm::Value* coerceValue(const TypedValue& v, const Type& target) {
        if (target.kind == TypeKind::Struct || target.kind == TypeKind::Array ||
            target.kind == TypeKind::String || target.kind == TypeKind::Ptr ||
            target.kind == TypeKind::Function || target.kind == TypeKind::Unknown) {
            return v.value;
        }
        if (target.kind == TypeKind::Bool) {
            if (v.type.kind == TypeKind::Bool) return v.value;
            return builder.CreateICmpNE(v.value, llvm::Constant::getNullValue(v.value->getType()));
        }
        if (v.type.kind == TypeKind::Bool && isIntKind(target.kind)) {
            return builder.CreateZExt(v.value, llvmType(target));
        }
        if (target.kind == TypeKind::F64) {
            if (v.type.kind == TypeKind::F64) return v.value;
            if (v.type.kind == TypeKind::Bool) return builder.CreateUIToFP(v.value, doubleTy);
            if (isIntKind(v.type.kind)) {
                return isUnsignedType(v.type) ? builder.CreateUIToFP(v.value, doubleTy)
                                               : builder.CreateSIToFP(v.value, doubleTy);
            }
            return v.value;
        }
        if (isIntKind(target.kind) && v.type.kind == TypeKind::F64) {
            return isUnsignedType(target) ? builder.CreateFPToUI(v.value, llvmType(target))
                                           : builder.CreateFPToSI(v.value, llvmType(target));
        }
        if (isIntKind(target.kind) && isIntKind(v.type.kind)) {
            auto* from = llvmType(v.type);
            auto* to = llvmType(target);
            if (from == to) return v.value;
            if (from->getIntegerBitWidth() < to->getIntegerBitWidth()) {
                return isUnsignedType(v.type) ? builder.CreateZExt(v.value, to) : builder.CreateSExt(v.value, to);
            }
            return builder.CreateTrunc(v.value, to);
        }
        return v.value;
    }

    llvm::Value* toCond(const TypedValue& v) {
        if (v.type.kind == TypeKind::Bool) return v.value;
        return builder.CreateICmpNE(v.value, llvm::Constant::getNullValue(v.value->getType()));
    }

    llvm::Value* toBufferPtr(const TypedValue& v) {
        if (v.type.kind == TypeKind::Ptr) return v.value;
        return builder.CreateIntToPtr(toI64(v), ptrTy);
    }

    llvm::Value* toI64(const TypedValue& v) {
        if (v.type.kind == TypeKind::Bool) return builder.CreateZExt(v.value, i64Ty);
        if (v.type.kind == TypeKind::F64) return builder.CreateFPToSI(v.value, i64Ty);
        if (!isIntKind(v.type.kind)) return v.value;
        auto* from = llvmType(v.type);
        if (from == i64Ty) return v.value;
        return isUnsignedType(v.type) ? builder.CreateZExt(v.value, i64Ty) : builder.CreateSExt(v.value, i64Ty);
    }

    void declareStructs() {
        for (auto& [name, fields] : checker.structs()) {
            (void)fields;
            structTypes[name] = llvm::StructType::create(ctx, "struct." + name);
        }
        for (auto& [name, fields] : checker.structs()) {
            std::vector<llvm::Type*> fieldTypes;
            for (auto& field : fields) fieldTypes.push_back(llvmType(field.second));
            structTypes[name]->setBody(fieldTypes);
        }
    }

    llvm::Constant* getStringLiteral(const std::string& s) {
        auto it = stringLiterals.find(s);
        if (it != stringLiterals.end()) return it->second;
        auto* data = llvm::ConstantDataArray::getString(ctx, s, true);
        auto* gv = new llvm::GlobalVariable(*mod, data->getType(), true, llvm::GlobalValue::PrivateLinkage,
                                             data, "str");
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        stringLiterals[s] = gv;
        return gv;
    }

    static std::string funcKey(const ast::Function& f) {
        return f.receiver ? (f.receiver->type + "." + f.name) : f.name;
    }

    static std::string mangleName(const std::string& key) {
        std::string out = "agn_" + key;
        for (auto& c : out) if (c == '.') c = '_';
        return out;
    }

    llvm::FunctionType* fnTypeFor(const FunctionSignature& sig, bool withHiddenFirst) {
        std::vector<llvm::Type*> params;
        if (withHiddenFirst) params.push_back(ptrTy);
        for (auto& p : sig.params) params.push_back(llvmType(p.second));
        return llvm::FunctionType::get(llvmType(sig.returnType), params, false);
    }

    void declareFunctions(ast::Program& program) {
        for (auto& f : program.functions) {
            std::string key = funcKey(f);
            auto& sig = checker.functions().at(key);
            if (f.name == "main" && !f.receiver) {
                auto* fnType = llvm::FunctionType::get(i32Ty, {}, false);
                functionTable[key] = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, "main", mod.get());
                continue;
            }
            auto* fnType = fnTypeFor(sig, true);
            functionTable[key] =
                llvm::Function::Create(fnType, llvm::Function::InternalLinkage, mangleName(key), mod.get());
        }

        for (auto& [modName, module] : program.modules) {
            if (modName == "stdio" || modName == "os") continue;
            for (auto& f : module.functions) {
                if (!f.isExported) continue;
                std::string key = modName + "." + f.name;
                auto& sig = checker.functions().at(key);
                auto* fnType = fnTypeFor(sig, true);
                functionTable[key] =
                    llvm::Function::Create(fnType, llvm::Function::InternalLinkage, mangleName(key), mod.get());
            }
        }
    }

    void defineAllFunctionBodies(ast::Program& program) {
        for (auto& f : program.functions) defineFunction(f, "");
        for (auto& [modName, module] : program.modules) {
            if (modName == "stdio" || modName == "os") continue;
            for (auto& f : module.functions) {
                if (f.isExported) defineFunction(f, modName);
            }
        }
    }

    bool currentBlockTerminated() { return builder.GetInsertBlock()->getTerminator() != nullptr; }

    llvm::FunctionCallee getRtFn(const std::string& name, llvm::FunctionType* ty) {
        return mod->getOrInsertFunction(name, ty);
    }

    void callRtRetain(llvm::Value* ptr) {
        if (mode != MemMode::Arc) return;
        auto* fnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
        builder.CreateCall(getRtFn("agn_rt_retain", fnTy), {ptr});
    }

    void callRtRelease(llvm::Value* ptr) {
        if (mode != MemMode::Arc) return;
        auto* fnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
        builder.CreateCall(getRtFn("agn_rt_release", fnTy), {ptr});
    }

    void callOrcEnterRegion() {
        if (mode != MemMode::Orc) return;
        auto* fnTy = llvm::FunctionType::get(voidTy, {}, false);
        builder.CreateCall(getRtFn("agn_rt_orc_enter", fnTy), {});
    }

    void callOrcExitRegion() {
        if (mode != MemMode::Orc) return;
        auto* fnTy = llvm::FunctionType::get(voidTy, {}, false);
        builder.CreateCall(getRtFn("agn_rt_orc_exit", fnTy), {});
    }

    llvm::Value* callRtAlloc(llvm::Value* size) {
        auto* fnTy = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
        return builder.CreateCall(getRtFn("agn_rt_alloc", fnTy), {size});
    }

    void releaseArcLocals() {
        for (auto* addr : arcTrackedClosures) {
            auto* val = builder.CreateLoad(ptrTy, addr);
            callRtRelease(val);
        }
    }

    void emitDefaultReturn() {
        releaseArcLocals();
        callOrcExitRegion();
        if (curIsMain) builder.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
        else if (curReturnType.kind == TypeKind::Void) builder.CreateRetVoid();
        else builder.CreateRet(llvm::Constant::getNullValue(llvmType(curReturnType)));
    }

    // retain only when aliasing an existing binding; calls/literals already own their one ref
    bool needsRetainOnBind(ast::Expression& e) {
        if (auto* id = std::get_if<ast::IdentifierExpr>(&e.node)) return locals.count(id->name) != 0;
        if (std::get_if<ast::FieldAccessExpr>(&e.node)) return true;
        if (std::get_if<ast::ArrayAccessExpr>(&e.node)) return true;
        return false;
    }

    unsigned long typeSize(const Type& t) {
        switch (t.kind) {
            case TypeKind::I64: case TypeKind::U64: case TypeKind::String:
            case TypeKind::Ptr: case TypeKind::Function:
                return 8;
            case TypeKind::I32: case TypeKind::U32: return 4;
            case TypeKind::I8: case TypeKind::U8: case TypeKind::Bool: return 1;
            case TypeKind::Array: return typeSize(*t.elementType) * t.arraySize;
            case TypeKind::Struct: {
                unsigned long total = 0;
                auto it = checker.structs().find(t.structName);
                if (it != checker.structs().end()) for (auto& f : it->second) total += typeSize(f.second);
                return total;
            }
            default: return 8;
        }
    }

    // captured locals must outlive this stack frame, so box them on the heap instead
    llvm::Value* allocSlot(const std::string& name, const Type& type) {
        if (capturedInCurrentFn.count(name)) return callRtAlloc(llvm::ConstantInt::get(i64Ty, typeSize(type)));
        return builder.CreateAlloca(llvmType(type), nullptr, name);
    }

    void collectCapturedExpr(ast::Expression& expr, std::unordered_set<std::string>& out) {
        if (auto* n = std::get_if<ast::TemplateStringExpr>(&expr.node)) {
            for (auto& part : n->parts) {
                if (auto* e = std::get_if<ast::TemplateExprPart>(&part)) collectCapturedExpr(*e->expr, out);
            }
        } else if (auto* n = std::get_if<ast::BinaryExpr>(&expr.node)) {
            collectCapturedExpr(*n->left, out);
            collectCapturedExpr(*n->right, out);
        } else if (auto* n = std::get_if<ast::UnaryExpr>(&expr.node)) {
            collectCapturedExpr(*n->operand, out);
        } else if (auto* n = std::get_if<ast::CallExpr>(&expr.node)) {
            for (auto& a : n->args) collectCapturedExpr(a, out);
        } else if (auto* n = std::get_if<ast::MethodCallExpr>(&expr.node)) {
            for (auto& a : n->args) collectCapturedExpr(a, out);
        } else if (auto* n = std::get_if<ast::ArrayAccessExpr>(&expr.node)) {
            collectCapturedExpr(*n->index, out);
        } else if (auto* n = std::get_if<ast::StringIndexExpr>(&expr.node)) {
            collectCapturedExpr(*n->str, out);
            collectCapturedExpr(*n->index, out);
        } else if (auto* n = std::get_if<ast::AddressOfExpr>(&expr.node)) {
            collectCapturedExpr(*n->operand, out);
        } else if (auto* n = std::get_if<ast::DerefExpr>(&expr.node)) {
            collectCapturedExpr(*n->operand, out);
        } else if (auto* n = std::get_if<ast::EvalExpr>(&expr.node)) {
            collectCapturedExpr(*n->instruction, out);
        } else if (auto* n = std::get_if<ast::FieldAccessExpr>(&expr.node)) {
            collectCapturedExpr(*n->object, out);
        } else if (auto* n = std::get_if<ast::FunctionLiteralExpr>(&expr.node)) {
            for (auto& name : n->capturedVars) out.insert(name);
        } else if (auto* n = std::get_if<ast::StructLiteralExpr>(&expr.node)) {
            for (auto& [fname, fexpr] : n->fields) { (void)fname; collectCapturedExpr(fexpr, out); }
        }
    }

    void collectCapturedStmt(ast::Statement& stmt, std::unordered_set<std::string>& out) {
        if (auto* n = std::get_if<ast::VarDeclStmt>(&stmt.node)) {
            if (n->value) collectCapturedExpr(*n->value, out);
        } else if (auto* n = std::get_if<ast::AssignmentStmt>(&stmt.node)) {
            collectCapturedExpr(n->value, out);
        } else if (auto* n = std::get_if<ast::ArrayAssignmentStmt>(&stmt.node)) {
            collectCapturedExpr(n->index, out);
            collectCapturedExpr(n->value, out);
        } else if (auto* n = std::get_if<ast::PointerAssignmentStmt>(&stmt.node)) {
            collectCapturedExpr(n->target, out);
            collectCapturedExpr(n->value, out);
        } else if (auto* n = std::get_if<ast::FieldAssignmentStmt>(&stmt.node)) {
            collectCapturedExpr(n->object, out);
            collectCapturedExpr(n->value, out);
        } else if (auto* n = std::get_if<ast::IfStmt>(&stmt.node)) {
            collectCapturedExpr(n->condition, out);
            for (auto& s : n->thenBody) collectCapturedStmt(s, out);
            if (n->elseBody) for (auto& s : *n->elseBody) collectCapturedStmt(s, out);
        } else if (auto* n = std::get_if<ast::ForStmt>(&stmt.node)) {
            if (n->condition) collectCapturedExpr(*n->condition, out);
            for (auto& s : n->body) collectCapturedStmt(s, out);
        } else if (auto* n = std::get_if<ast::ReturnStmt>(&stmt.node)) {
            if (n->value) collectCapturedExpr(*n->value, out);
        } else if (auto* n = std::get_if<ast::ExpressionStmt>(&stmt.node)) {
            collectCapturedExpr(n->expr, out);
        } else if (auto* n = std::get_if<ast::ComptimeStmt>(&stmt.node)) {
            for (auto& s : n->body) collectCapturedStmt(s, out);
        }
    }

    std::unordered_set<std::string> collectCapturedNames(std::vector<ast::Statement>& body) {
        std::unordered_set<std::string> out;
        for (auto& stmt : body) collectCapturedStmt(stmt, out);
        return out;
    }

    void defineFunction(ast::Function& f, const std::string& modulePrefix) {
        std::string key = modulePrefix.empty() ? funcKey(f) : modulePrefix + "." + f.name;
        llvm::Function* fn = functionTable.at(key);
        bool isMain = (f.name == "main" && !f.receiver && modulePrefix.empty());
        auto& sig = checker.functions().at(key);

        auto* savedFn = curFn;
        auto* savedEnv = curEnv;
        Type savedRet = curReturnType;
        bool savedIsMain = curIsMain;
        std::string savedModulePrefix = currentModulePrefix;
        auto savedLocals = std::move(locals);
        auto savedArc = std::move(arcTrackedClosures);
        auto savedCaptured = std::move(capturedInCurrentFn);
        auto savedInsertBB = builder.GetInsertBlock();
        auto savedInsertPt = builder.GetInsertPoint();

        locals.clear();
        arcTrackedClosures.clear();
        capturedInCurrentFn = collectCapturedNames(f.body);
        curFn = fn;
        curReturnType = sig.returnType;
        curIsMain = isMain;
        currentModulePrefix = modulePrefix;

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        builder.SetInsertPoint(entry);
        callOrcEnterRegion();

        auto argIt = fn->arg_begin();
        if (isMain) {
            curEnv = nullptr;
        } else if (f.receiver) {
            llvm::Argument* recvArg = &*argIt++;
            recvArg->setName(f.receiver->name);
            locals[f.receiver->name] = LocalVar{recvArg, sig.receiver->second};
            curEnv = nullptr;
        } else {
            llvm::Argument* envArg = &*argIt++;
            envArg->setName("env");
            curEnv = envArg;
        }

        for (auto& p : f.params) {
            llvm::Argument* argVal = &*argIt++;
            argVal->setName(p.name);
            Type pType = checker.resolveTypeString(p.type);
            auto* slot = allocSlot(p.name, pType);
            builder.CreateStore(argVal, slot);
            locals[p.name] = LocalVar{slot, pType};
        }

        for (auto& stmt : f.body) genStatement(stmt);
        if (!currentBlockTerminated()) emitDefaultReturn();

        curFn = savedFn;
        curEnv = savedEnv;
        curReturnType = savedRet;
        curIsMain = savedIsMain;
        currentModulePrefix = savedModulePrefix;
        locals = std::move(savedLocals);
        arcTrackedClosures = std::move(savedArc);
        capturedInCurrentFn = std::move(savedCaptured);
        if (savedInsertBB) builder.SetInsertPoint(savedInsertBB, savedInsertPt);
    }

    TypedValue genFunctionLiteral(ast::FunctionLiteralExpr& lit) {
        struct CapInfo { std::string name; Type type; llvm::Value* addr; };
        std::vector<CapInfo> caps;
        for (auto& name : lit.capturedVars) {
            auto& lv = locals.at(name);
            caps.push_back(CapInfo{name, lv.type, lv.addr});
        }

        std::vector<Type> paramTypes;
        for (auto& p : lit.params) paramTypes.push_back(checker.resolveTypeString(p.type));
        Type retType = checker.resolveTypeString(lit.returnType);

        std::string llvmFnName = "agn_closure_" + std::to_string(closureCounter++);
        std::vector<llvm::Type*> llvmParams;
        llvmParams.push_back(ptrTy);
        for (auto& pt : paramTypes) llvmParams.push_back(llvmType(pt));
        auto* fnType = llvm::FunctionType::get(llvmType(retType), llvmParams, false);
        auto* fn = llvm::Function::Create(fnType, llvm::Function::InternalLinkage, llvmFnName, mod.get());

        auto* savedFn = curFn;
        auto* savedEnv = curEnv;
        Type savedRet = curReturnType;
        bool savedIsMain = curIsMain;
        auto savedLocals = std::move(locals);
        auto savedArc = std::move(arcTrackedClosures);
        auto savedCaptured = std::move(capturedInCurrentFn);
        auto savedInsertBB = builder.GetInsertBlock();
        auto savedInsertPt = builder.GetInsertPoint();

        locals.clear();
        arcTrackedClosures.clear();
        capturedInCurrentFn = collectCapturedNames(lit.body);
        curFn = fn;
        curReturnType = retType;
        curIsMain = false;

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        builder.SetInsertPoint(entry);
        callOrcEnterRegion();

        auto argIt = fn->arg_begin();
        llvm::Argument* envArg = &*argIt++;
        envArg->setName("env");
        curEnv = envArg;

        for (size_t i = 0; i < caps.size(); i++) {
            auto* slotPtr = builder.CreateGEP(ptrTy, envArg, {llvm::ConstantInt::get(i64Ty, int64_t(i + 1))});
            auto* addr = builder.CreateLoad(ptrTy, slotPtr);
            locals[caps[i].name] = LocalVar{addr, caps[i].type};
        }
        for (size_t i = 0; i < lit.params.size(); i++) {
            llvm::Argument* argVal = &*argIt++;
            argVal->setName(lit.params[i].name);
            auto* slot = allocSlot(lit.params[i].name, paramTypes[i]);
            builder.CreateStore(argVal, slot);
            locals[lit.params[i].name] = LocalVar{slot, paramTypes[i]};
        }

        for (auto& stmt : lit.body) genStatement(stmt);
        if (!currentBlockTerminated()) emitDefaultReturn();

        curFn = savedFn;
        curEnv = savedEnv;
        curReturnType = savedRet;
        curIsMain = savedIsMain;
        locals = std::move(savedLocals);
        arcTrackedClosures = std::move(savedArc);
        capturedInCurrentFn = std::move(savedCaptured);
        if (savedInsertBB) builder.SetInsertPoint(savedInsertBB, savedInsertPt);

        unsigned long totalSlots = 1 + caps.size();
        llvm::Value* obj = callRtAlloc(llvm::ConstantInt::get(i64Ty, totalSlots * 8));
        builder.CreateStore(fn, obj);
        for (size_t i = 0; i < caps.size(); i++) {
            auto* slotPtr = builder.CreateGEP(ptrTy, obj, {llvm::ConstantInt::get(i64Ty, int64_t(i + 1))});
            builder.CreateStore(caps[i].addr, slotPtr);
        }

        Type fnValType;
        fnValType.kind = TypeKind::Function;
        fnValType.paramTypes = paramTypes;
        fnValType.returnType = std::make_shared<Type>(retType);
        return TypedValue{obj, fnValType};
    }

    TypedValue wrapNamedFunctionAsValue(const std::string& name, const FunctionSignature& sig) {
        llvm::Function* fn = functionTable.at(name);
        llvm::Value* obj = callRtAlloc(llvm::ConstantInt::get(i64Ty, 8));
        builder.CreateStore(fn, obj);
        Type t;
        t.kind = TypeKind::Function;
        for (auto& p : sig.params) t.paramTypes.push_back(p.second);
        t.returnType = std::make_shared<Type>(sig.returnType);
        return TypedValue{obj, t};
    }

    TypedValue genStdioCall(const std::string& member, std::vector<ast::Expression>& args) {
        auto rt = [&](const std::string& name, llvm::Type* retTy, std::vector<llvm::Type*> paramTys,
                      std::vector<llvm::Value*> callArgs) {
            auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
            return builder.CreateCall(getRtFn(name, fnTy), callArgs);
        };

        if (member == "Print" || member == "Println") {
            auto val = genExpr(args[0]);
            if (val.type.kind == TypeKind::String) {
                rt(member == "Println" ? "agn_rt_println_str" : "agn_rt_print_str", voidTy, {ptrTy}, {val.value});
            } else if (val.type.isFloat()) {
                rt(member == "Println" ? "agn_rt_println_float" : "agn_rt_print_float", voidTy, {doubleTy},
                   {coerceValue(val, Type{TypeKind::F64})});
            } else {
                rt(member == "Println" ? "agn_rt_println_int" : "agn_rt_print_int", voidTy, {i64Ty}, {toI64(val)});
            }
            return TypedValue{llvm::ConstantInt::get(i64Ty, 0), Type{TypeKind::Void}};
        }
        if (member == "PrintStr") {
            rt("agn_rt_print_str", voidTy, {ptrTy}, {genExpr(args[0]).value});
            return TypedValue{llvm::ConstantInt::get(i64Ty, 0), Type{TypeKind::Void}};
        }
        if (member == "PrintlnStr") {
            rt("agn_rt_println_str", voidTy, {ptrTy}, {genExpr(args[0]).value});
            return TypedValue{llvm::ConstantInt::get(i64Ty, 0), Type{TypeKind::Void}};
        }
        if (member == "PrintChar") {
            rt("agn_rt_print_char", voidTy, {i64Ty}, {toI64(genExpr(args[0]))});
            return TypedValue{llvm::ConstantInt::get(i64Ty, 0), Type{TypeKind::Void}};
        }
        if (member == "ReadInt") {
            auto* call = rt("agn_rt_read_int", i64Ty, {}, {});
            return TypedValue{call, Type{TypeKind::I64}};
        }
        if (member == "ReadChar") {
            auto* call = rt("agn_rt_read_char", i64Ty, {}, {});
            return TypedValue{call, Type{TypeKind::I64}};
        }
        if (member == "ReadLine") {
            auto bufVal = genExpr(args[0]);
            auto lenVal = genExpr(args[1]);
            auto* bufPtr = toBufferPtr(bufVal);
            auto* call = rt("agn_rt_read_line", i64Ty, {ptrTy, i64Ty}, {bufPtr, toI64(lenVal)});
            return TypedValue{call, Type{TypeKind::I64}};
        }
        if (member == "Flush") {
            rt("agn_rt_flush", voidTy, {}, {});
            return TypedValue{llvm::ConstantInt::get(i64Ty, 0), Type{TypeKind::Void}};
        }
        return TypedValue{llvm::ConstantInt::get(i64Ty, 0), Type{TypeKind::Unknown}};
    }

    TypedValue wrapOptionInt(llvm::Value* rawValue, const std::string& qualifiedFnName) {
        Type retType = checker.functions().at(qualifiedFnName).returnType;
        auto* structTy = structTypes.at(retType.structName);
        auto* someFlag = builder.CreateZExt(
            builder.CreateICmpSGE(rawValue, llvm::ConstantInt::get(i64Ty, 0)), i64Ty);
        llvm::Value* agg = llvm::UndefValue::get(structTy);
        agg = builder.CreateInsertValue(agg, someFlag, 0);
        agg = builder.CreateInsertValue(agg, rawValue, 1);
        return TypedValue{agg, retType};
    }

    TypedValue genStringCall(const std::string& member, std::vector<ast::Expression>& args) {
        auto rt = [&](const std::string& name, llvm::Type* retTy, std::vector<llvm::Type*> paramTys,
                      std::vector<llvm::Value*> callArgs) {
            auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
            return builder.CreateCall(getRtFn(name, fnTy), callArgs);
        };

        if (member == "len") {
            auto* call = rt("agn_rt_strlen", i64Ty, {ptrTy}, {genExpr(args[0]).value});
            return TypedValue{call, Type{TypeKind::I64}};
        }
        if (member == "compare") {
            auto* call = rt("agn_rt_strcmp", i64Ty, {ptrTy, ptrTy},
                             {genExpr(args[0]).value, genExpr(args[1]).value});
            return TypedValue{call, Type{TypeKind::I64}};
        }
        if (member == "concat") {
            auto* s1 = genExpr(args[0]).value;
            auto* s2 = genExpr(args[1]).value;
            auto* len1 = rt("agn_rt_strlen", i64Ty, {ptrTy}, {s1});
            auto* len2 = rt("agn_rt_strlen", i64Ty, {ptrTy}, {s2});
            auto* total = builder.CreateAdd(builder.CreateAdd(len1, len2), llvm::ConstantInt::get(i64Ty, 1));
            auto* buf = callRtAlloc(total);
            rt("agn_rt_memcpy", voidTy, {ptrTy, ptrTy, i64Ty}, {buf, s1, len1});
            auto* tail = builder.CreateGEP(i8Ty, buf, {len1});
            rt("agn_rt_memcpy", voidTy, {ptrTy, ptrTy, i64Ty}, {tail, s2, len2});
            auto* end = builder.CreateGEP(i8Ty, buf, {builder.CreateAdd(len1, len2)});
            builder.CreateStore(llvm::ConstantInt::get(i8Ty, 0), end);
            return TypedValue{buf, Type{TypeKind::String}};
        }
        if (member == "indexOf") {
            auto* call = rt("agn_rt_index_of", i64Ty, {ptrTy, ptrTy},
                             {genExpr(args[0]).value, genExpr(args[1]).value});
            return wrapOptionInt(call, "string.indexOf");
        }
        if (member == "contains") {
            auto* call = rt("agn_rt_contains", i64Ty, {ptrTy, ptrTy},
                             {genExpr(args[0]).value, genExpr(args[1]).value});
            return TypedValue{call, Type{TypeKind::I64}};
        }
        if (member == "startsWith") {
            auto* call = rt("agn_rt_starts_with", i64Ty, {ptrTy, ptrTy},
                             {genExpr(args[0]).value, genExpr(args[1]).value});
            return TypedValue{call, Type{TypeKind::I64}};
        }
        if (member == "endsWith") {
            auto* call = rt("agn_rt_ends_with", i64Ty, {ptrTy, ptrTy},
                             {genExpr(args[0]).value, genExpr(args[1]).value});
            return TypedValue{call, Type{TypeKind::I64}};
        }
        if (member == "charAt") {
            auto* call = rt("agn_rt_char_at", i64Ty, {ptrTy, i64Ty},
                             {genExpr(args[0]).value, toI64(genExpr(args[1]))});
            return wrapOptionInt(call, "string.charAt");
        }
        if (member == "substr") {
            auto* call = rt("agn_rt_substr", ptrTy, {ptrTy, i64Ty, i64Ty},
                             {genExpr(args[0]).value, toI64(genExpr(args[1])), toI64(genExpr(args[2]))});
            return TypedValue{call, Type{TypeKind::String}};
        }
        if (member == "toUpper") {
            auto* call = rt("agn_rt_to_upper", ptrTy, {ptrTy}, {genExpr(args[0]).value});
            return TypedValue{call, Type{TypeKind::String}};
        }
        if (member == "toLower") {
            auto* call = rt("agn_rt_to_lower", ptrTy, {ptrTy}, {genExpr(args[0]).value});
            return TypedValue{call, Type{TypeKind::String}};
        }
        return TypedValue{llvm::ConstantInt::get(i64Ty, 0), Type{TypeKind::Unknown}};
    }

    TypedValue genOsCall(const std::string& member, std::vector<ast::Expression>& args) {
        auto rt = [&](const std::string& name, llvm::Type* retTy, std::vector<llvm::Type*> paramTys,
                      std::vector<llvm::Value*> callArgs) {
            auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
            return builder.CreateCall(getRtFn(name, fnTy), callArgs);
        };

        if (member == "ArgCount") {
            auto* call = rt("agn_rt_argc", i64Ty, {}, {});
            return TypedValue{call, Type{TypeKind::I64}};
        }
        if (member == "Arg") {
            auto* call = rt("agn_rt_argv", ptrTy, {i64Ty}, {toI64(genExpr(args[0]))});
            return TypedValue{call, Type{TypeKind::String}};
        }
        if (member == "OpenRead") {
            auto* call = rt("agn_rt_open_read", i64Ty, {ptrTy}, {genExpr(args[0]).value});
            return wrapOptionInt(call, "os.OpenRead");
        }
        if (member == "OpenCreate") {
            auto* call = rt("agn_rt_open_create", i64Ty, {ptrTy}, {genExpr(args[0]).value});
            return wrapOptionInt(call, "os.OpenCreate");
        }
        if (member == "Close") {
            auto* call = rt("agn_rt_close", i64Ty, {i64Ty}, {toI64(genExpr(args[0]))});
            return TypedValue{call, Type{TypeKind::I64}};
        }
        if (member == "ReadFd") {
            auto bufVal = genExpr(args[1]);
            auto* bufPtr = toBufferPtr(bufVal);
            auto* call = rt("agn_rt_read_fd", i64Ty, {i64Ty, ptrTy, i64Ty},
                             {toI64(genExpr(args[0])), bufPtr, toI64(genExpr(args[2]))});
            return wrapOptionInt(call, "os.ReadFd");
        }
        if (member == "WriteFd") {
            auto* data = genExpr(args[1]).value;
            auto* len = rt("agn_rt_strlen", i64Ty, {ptrTy}, {data});
            auto* call = rt("agn_rt_write_fd", i64Ty, {i64Ty, ptrTy, i64Ty},
                             {toI64(genExpr(args[0])), data, len});
            return wrapOptionInt(call, "os.WriteFd");
        }
        if (member == "Exit") {
            rt("agn_rt_exit", voidTy, {i64Ty}, {toI64(genExpr(args[0]))});
            builder.CreateUnreachable();
            return TypedValue{llvm::ConstantInt::get(i64Ty, 0), Type{TypeKind::Void}};
        }
        return TypedValue{llvm::ConstantInt::get(i64Ty, 0), Type{TypeKind::Unknown}};
    }

    void appendToBuffer(llvm::Value* buf, llvm::Value* posAlloca, llvm::Value* src, llvm::Value* len) {
        auto* pos = builder.CreateLoad(i64Ty, posAlloca);
        auto* dest = builder.CreateGEP(i8Ty, buf, {pos});
        auto* memcpyTy = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i64Ty}, false);
        builder.CreateCall(getRtFn("agn_rt_memcpy", memcpyTy), {dest, src, len});
        builder.CreateStore(builder.CreateAdd(pos, len), posAlloca);
    }

    TypedValue genTemplateString(ast::TemplateStringExpr& tmpl) {
        auto* buf = builder.CreateAlloca(i8Ty, llvm::ConstantInt::get(i64Ty, 1024), "tmplbuf");
        auto* posAlloca = builder.CreateAlloca(i64Ty, nullptr, "tmplpos");
        builder.CreateStore(llvm::ConstantInt::get(i64Ty, 0), posAlloca);

        for (auto& part : tmpl.parts) {
            if (auto* lit = std::get_if<ast::TemplateLiteralPart>(&part)) {
                auto* g = getStringLiteral(lit->text);
                appendToBuffer(buf, posAlloca, g, llvm::ConstantInt::get(i64Ty, lit->text.size()));
                continue;
            }
            auto* e = std::get_if<ast::TemplateExprPart>(&part);
            auto val = genExpr(*e->expr);
            if (val.type.kind == TypeKind::String) {
                auto* fnTy = llvm::FunctionType::get(i64Ty, {ptrTy}, false);
                auto* len = builder.CreateCall(getRtFn("agn_rt_strlen", fnTy), {val.value});
                appendToBuffer(buf, posAlloca, val.value, len);
                continue;
            }

            long width = 0;
            bool padZero = false;
            bool hex = false, upper = false;
            if (e->format) {
                width = static_cast<long>(e->format->width.value_or(0));
                padZero = e->format->padding == '0';
                hex = e->format->formatType == ast::FormatType::Hex || e->format->formatType == ast::FormatType::HexUpper;
                upper = e->format->formatType == ast::FormatType::HexUpper;
            }

            auto* pos = builder.CreateLoad(i64Ty, posAlloca);
            auto* dest = builder.CreateGEP(i8Ty, buf, {pos});
            llvm::Value* written;
            if (hex) {
                auto* fnTy = llvm::FunctionType::get(i64Ty, {ptrTy, i64Ty, i64Ty, i64Ty, i64Ty}, false);
                written = builder.CreateCall(getRtFn("agn_rt_format_hex", fnTy),
                                              {dest, toI64(val), llvm::ConstantInt::get(i64Ty, width),
                                               llvm::ConstantInt::get(i64Ty, padZero ? 1 : 0),
                                               llvm::ConstantInt::get(i64Ty, upper ? 1 : 0)});
            } else {
                auto* fnTy = llvm::FunctionType::get(i64Ty, {ptrTy, i64Ty, i64Ty, i64Ty}, false);
                written = builder.CreateCall(getRtFn("agn_rt_format_int", fnTy),
                                              {dest, toI64(val), llvm::ConstantInt::get(i64Ty, width),
                                               llvm::ConstantInt::get(i64Ty, padZero ? 1 : 0)});
            }
            builder.CreateStore(builder.CreateAdd(pos, written), posAlloca);
        }

        auto* pos = builder.CreateLoad(i64Ty, posAlloca);
        auto* endPtr = builder.CreateGEP(i8Ty, buf, {pos});
        builder.CreateStore(llvm::ConstantInt::get(i8Ty, 0), endPtr);
        return TypedValue{buf, Type{TypeKind::String}};
    }

    TypedValue genConcat(TypedValue& lhs, TypedValue& rhs) {
        auto* strlenTy = llvm::FunctionType::get(i64Ty, {ptrTy}, false);
        auto* memcpyTy = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i64Ty}, false);
        auto* buf = builder.CreateAlloca(i8Ty, llvm::ConstantInt::get(i64Ty, 1024), "concatbuf");
        auto* lenA = builder.CreateCall(getRtFn("agn_rt_strlen", strlenTy), {lhs.value});
        builder.CreateCall(getRtFn("agn_rt_memcpy", memcpyTy), {buf, lhs.value, lenA});
        auto* dest2 = builder.CreateGEP(i8Ty, buf, {lenA});
        auto* lenB = builder.CreateCall(getRtFn("agn_rt_strlen", strlenTy), {rhs.value});
        builder.CreateCall(getRtFn("agn_rt_memcpy", memcpyTy), {dest2, rhs.value, lenB});
        auto* endPtr = builder.CreateGEP(i8Ty, buf, {builder.CreateAdd(lenA, lenB)});
        builder.CreateStore(llvm::ConstantInt::get(i8Ty, 0), endPtr);
        return TypedValue{buf, Type{TypeKind::String}};
    }

    TypedValue genExpr(ast::Expression& expr) {
        if (auto* n = std::get_if<ast::NumberExpr>(&expr.node)) {
            return TypedValue{llvm::ConstantInt::get(i64Ty, uint64_t(n->value), true), Type{TypeKind::I64}};
        }
        if (auto* n = std::get_if<ast::FloatExpr>(&expr.node)) {
            return TypedValue{llvm::ConstantFP::get(doubleTy, n->value), Type{TypeKind::F64}};
        }
        if (auto* n = std::get_if<ast::StringExpr>(&expr.node)) {
            return TypedValue{getStringLiteral(n->value), Type{TypeKind::String}};
        }
        if (auto* n = std::get_if<ast::TemplateStringExpr>(&expr.node)) {
            return genTemplateString(*n);
        }
        if (auto* n = std::get_if<ast::IdentifierExpr>(&expr.node)) {
            auto it = locals.find(n->name);
            if (it != locals.end()) {
                auto* v = builder.CreateLoad(llvmType(it->second.type), it->second.addr);
                return TypedValue{v, it->second.type};
            }
            auto fit = checker.functions().find(n->name);
            if (fit != checker.functions().end() && !fit->second.receiver) {
                return wrapNamedFunctionAsValue(n->name, fit->second);
            }
            return TypedValue{llvm::ConstantInt::get(i64Ty, 0), Type{TypeKind::Unknown}};
        }
        if (auto* n = std::get_if<ast::BinaryExpr>(&expr.node)) {
            if (n->op == ast::BinaryOp::Concat) {
                auto l = genExpr(*n->left);
                auto r = genExpr(*n->right);
                return genConcat(l, r);
            }
            auto l = genExpr(*n->left);
            auto r = genExpr(*n->right);

            if (l.type.isFloat() || r.type.isFloat()) {
                llvm::Value* lv = coerceValue(l, Type{TypeKind::F64});
                llvm::Value* rv = coerceValue(r, Type{TypeKind::F64});
                switch (n->op) {
                    case ast::BinaryOp::Add: return TypedValue{builder.CreateFAdd(lv, rv), Type{TypeKind::F64}};
                    case ast::BinaryOp::Sub: return TypedValue{builder.CreateFSub(lv, rv), Type{TypeKind::F64}};
                    case ast::BinaryOp::Mul: return TypedValue{builder.CreateFMul(lv, rv), Type{TypeKind::F64}};
                    case ast::BinaryOp::Div: return TypedValue{builder.CreateFDiv(lv, rv), Type{TypeKind::F64}};
                    case ast::BinaryOp::Equal: return TypedValue{builder.CreateFCmpOEQ(lv, rv), Type{TypeKind::Bool}};
                    case ast::BinaryOp::NotEqual: return TypedValue{builder.CreateFCmpONE(lv, rv), Type{TypeKind::Bool}};
                    case ast::BinaryOp::Less: return TypedValue{builder.CreateFCmpOLT(lv, rv), Type{TypeKind::Bool}};
                    case ast::BinaryOp::LessEqual: return TypedValue{builder.CreateFCmpOLE(lv, rv), Type{TypeKind::Bool}};
                    case ast::BinaryOp::Greater: return TypedValue{builder.CreateFCmpOGT(lv, rv), Type{TypeKind::Bool}};
                    case ast::BinaryOp::GreaterEqual:
                        return TypedValue{builder.CreateFCmpOGE(lv, rv), Type{TypeKind::Bool}};
                    default:
                        return TypedValue{lv, Type{TypeKind::F64}};
                }
            }

            bool bothUnsigned = isUnsignedType(l.type) && isUnsignedType(r.type);

            llvm::Type* wide = llvmType(l.type);
            if (isIntKind(l.type.kind) && isIntKind(r.type.kind)) {
                auto* lt = llvmType(l.type);
                auto* rt = llvmType(r.type);
                wide = lt->getIntegerBitWidth() >= rt->getIntegerBitWidth() ? lt : rt;
                l.value = coerceValue(l, l.type.kind == TypeKind::I64 || l.type.kind == TypeKind::U64
                                             ? Type{bothUnsigned ? TypeKind::U64 : TypeKind::I64}
                                             : l.type);
            }
            auto promote = [&](TypedValue& v) {
                auto* from = v.value->getType();
                if (from == wide) return v.value;
                return isUnsignedType(v.type) ? builder.CreateZExt(v.value, wide) : builder.CreateSExt(v.value, wide);
            };
            llvm::Value* lv = isIntKind(l.type.kind) ? promote(l) : l.value;
            llvm::Value* rv = isIntKind(r.type.kind) ? promote(r) : r.value;
            Type resultType = bothUnsigned ? Type{TypeKind::U64} : l.type;
            if (isIntKind(l.type.kind) && isIntKind(r.type.kind)) resultType = l.type;

            switch (n->op) {
                case ast::BinaryOp::Add: return TypedValue{builder.CreateAdd(lv, rv), resultType};
                case ast::BinaryOp::Sub: return TypedValue{builder.CreateSub(lv, rv), resultType};
                case ast::BinaryOp::Mul: return TypedValue{builder.CreateMul(lv, rv), resultType};
                case ast::BinaryOp::Div:
                    return TypedValue{bothUnsigned ? builder.CreateUDiv(lv, rv) : builder.CreateSDiv(lv, rv), resultType};
                case ast::BinaryOp::Mod:
                    return TypedValue{bothUnsigned ? builder.CreateURem(lv, rv) : builder.CreateSRem(lv, rv), resultType};
                case ast::BinaryOp::BitAnd: return TypedValue{builder.CreateAnd(lv, rv), resultType};
                case ast::BinaryOp::BitOr: return TypedValue{builder.CreateOr(lv, rv), resultType};
                case ast::BinaryOp::BitXor: return TypedValue{builder.CreateXor(lv, rv), resultType};
                case ast::BinaryOp::Shl: return TypedValue{builder.CreateShl(lv, rv), resultType};
                case ast::BinaryOp::Shr: return TypedValue{builder.CreateAShr(lv, rv), resultType};
                case ast::BinaryOp::Equal: return TypedValue{builder.CreateICmpEQ(lv, rv), Type{TypeKind::Bool}};
                case ast::BinaryOp::NotEqual: return TypedValue{builder.CreateICmpNE(lv, rv), Type{TypeKind::Bool}};
                case ast::BinaryOp::Less:
                    return TypedValue{bothUnsigned ? builder.CreateICmpULT(lv, rv) : builder.CreateICmpSLT(lv, rv),
                                       Type{TypeKind::Bool}};
                case ast::BinaryOp::LessEqual:
                    return TypedValue{bothUnsigned ? builder.CreateICmpULE(lv, rv) : builder.CreateICmpSLE(lv, rv),
                                       Type{TypeKind::Bool}};
                case ast::BinaryOp::Greater:
                    return TypedValue{bothUnsigned ? builder.CreateICmpUGT(lv, rv) : builder.CreateICmpSGT(lv, rv),
                                       Type{TypeKind::Bool}};
                case ast::BinaryOp::GreaterEqual:
                    return TypedValue{bothUnsigned ? builder.CreateICmpUGE(lv, rv) : builder.CreateICmpSGE(lv, rv),
                                       Type{TypeKind::Bool}};
                case ast::BinaryOp::And:
                    return TypedValue{builder.CreateAnd(toCond(l), toCond(r)), Type{TypeKind::Bool}};
                case ast::BinaryOp::Or:
                    return TypedValue{builder.CreateOr(toCond(l), toCond(r)), Type{TypeKind::Bool}};
                default:
                    return TypedValue{lv, resultType};
            }
        }
        if (auto* n = std::get_if<ast::UnaryExpr>(&expr.node)) {
            auto v = genExpr(*n->operand);
            if (n->op == ast::UnaryOp::Neg) {
                if (v.type.isFloat()) return TypedValue{builder.CreateFNeg(v.value), v.type};
                return TypedValue{builder.CreateNeg(v.value), v.type};
            }
            return TypedValue{builder.CreateNot(toCond(v)), Type{TypeKind::Bool}};
        }
        if (auto* n = std::get_if<ast::CallExpr>(&expr.node)) {
            return genCall(n->function, n->args);
        }
        if (auto* n = std::get_if<ast::MethodCallExpr>(&expr.node)) {
            return genMethodCall(*n);
        }
        if (auto* n = std::get_if<ast::ArrayAccessExpr>(&expr.node)) {
            auto& lv = locals.at(n->name);
            auto idx = genExpr(*n->index);
            auto* arrTy = llvmType(lv.type);
            auto* elemPtr = builder.CreateGEP(arrTy, lv.addr, {llvm::ConstantInt::get(i64Ty, 0), toI64(idx)});
            auto* loaded = builder.CreateLoad(llvmType(*lv.type.elementType), elemPtr);
            return TypedValue{loaded, *lv.type.elementType};
        }
        if (auto* n = std::get_if<ast::StringIndexExpr>(&expr.node)) {
            auto str = genExpr(*n->str);
            auto idx = genExpr(*n->index);
            auto* bytePtr = builder.CreateGEP(i8Ty, str.value, {toI64(idx)});
            auto* byte = builder.CreateLoad(i8Ty, bytePtr);
            return TypedValue{byte, Type{TypeKind::U8}};
        }
        if (auto* n = std::get_if<ast::AddressOfExpr>(&expr.node)) {
            if (auto* id = std::get_if<ast::IdentifierExpr>(&n->operand->node)) {
                auto& lv = locals.at(id->name);
                Type ptrType{TypeKind::Ptr};
                ptrType.pointee = std::make_shared<Type>(lv.type);
                return TypedValue{lv.addr, ptrType};
            }
            auto v = genExpr(*n->operand);
            Type ptrType{TypeKind::Ptr};
            ptrType.pointee = std::make_shared<Type>(v.type);
            return TypedValue{v.value, ptrType};
        }
        if (auto* n = std::get_if<ast::DerefExpr>(&expr.node)) {
            auto v = genExpr(*n->operand);
            Type pointee = v.type.pointee ? *v.type.pointee : Type{TypeKind::I64};
            auto* loaded = builder.CreateLoad(llvmType(pointee), v.value);
            return TypedValue{loaded, pointee};
        }
        if (std::get_if<ast::EvalExpr>(&expr.node)) {
            std::fprintf(stderr, "error: 'eval' is not supported by the llvm backend "
                                  "(it only has meaning as an nvm inline-asm escape hatch)\n");
            std::exit(1);
        }
        if (auto* n = std::get_if<ast::FieldAccessExpr>(&expr.node)) {
            auto* id = std::get_if<ast::IdentifierExpr>(&n->object->node);
            auto& lv = locals.at(id->name);
            auto& fields = checker.structs().at(lv.type.structName);
            size_t idx = 0;
            Type fieldType{TypeKind::Unknown};
            for (size_t i = 0; i < fields.size(); i++) {
                if (fields[i].first == n->field) { idx = i; fieldType = fields[i].second; break; }
            }
            auto* structTy = structTypes.at(lv.type.structName);
            auto* fieldPtr = builder.CreateStructGEP(structTy, lv.addr, unsigned(idx));
            auto* loaded = builder.CreateLoad(llvmType(fieldType), fieldPtr);
            return TypedValue{loaded, fieldType};
        }
        if (auto* n = std::get_if<ast::FunctionLiteralExpr>(&expr.node)) {
            return genFunctionLiteral(*n);
        }
        if (auto* n = std::get_if<ast::StructLiteralExpr>(&expr.node)) {
            auto& fields = checker.structs().at(n->structName);
            auto* structTy = structTypes.at(n->structName);
            llvm::Value* agg = llvm::UndefValue::get(structTy);
            for (auto& [fname, fexpr] : n->fields) {
                for (size_t i = 0; i < fields.size(); i++) {
                    if (fields[i].first == fname) {
                        auto val = genExpr(fexpr);
                        auto* coerced = coerceValue(val, fields[i].second);
                        agg = builder.CreateInsertValue(agg, coerced, unsigned(i));
                        break;
                    }
                }
            }
            Type t{TypeKind::Struct};
            t.structName = n->structName;
            return TypedValue{agg, t};
        }
        return TypedValue{llvm::ConstantInt::get(i64Ty, 0), Type{TypeKind::Unknown}};
    }

    TypedValue genCall(const std::string& name, std::vector<ast::Expression>& argExprs) {
        if (currentModulePrefix == "string" &&
            (name == "len" || name == "compare" || name == "concat" || name == "indexOf" ||
             name == "contains" || name == "startsWith" || name == "endsWith" || name == "charAt" ||
             name == "substr" || name == "toUpper" || name == "toLower")) {
            return genStringCall(name, argExprs);
        }

        std::string resolvedName = name;
        if (!currentModulePrefix.empty() && locals.find(name) == locals.end()) {
            std::string qualified = currentModulePrefix + "." + name;
            if (checker.functions().count(qualified)) resolvedName = qualified;
        }

        auto fit = checker.functions().find(resolvedName);
        if (fit != checker.functions().end()) {
            llvm::Function* fn = functionTable.at(resolvedName);
            std::vector<llvm::Value*> args;
            args.push_back(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)));
            for (size_t i = 0; i < argExprs.size(); i++) {
                auto v = genExpr(argExprs[i]);
                args.push_back(coerceValue(v, fit->second.params[i].second));
            }
            auto* call = builder.CreateCall(fn, args);
            return TypedValue{call, fit->second.returnType};
        }

        auto& lv = locals.at(name);
        auto* closurePtr = lv.addr ? builder.CreateLoad(ptrTy, lv.addr) : nullptr;
        auto* codePtr = builder.CreateLoad(ptrTy, closurePtr);
        std::vector<llvm::Type*> llvmParams{ptrTy};
        for (auto& pt : lv.type.paramTypes) llvmParams.push_back(llvmType(pt));
        auto* fnType = llvm::FunctionType::get(llvmType(*lv.type.returnType), llvmParams, false);
        std::vector<llvm::Value*> args{closurePtr};
        for (size_t i = 0; i < argExprs.size(); i++) {
            auto v = genExpr(argExprs[i]);
            args.push_back(coerceValue(v, lv.type.paramTypes[i]));
        }
        auto* call = builder.CreateCall(fnType, codePtr, args);
        return TypedValue{call, *lv.type.returnType};
    }

    TypedValue genMethodCall(ast::MethodCallExpr& expr) {
        if (expr.object == "stdio") return genStdioCall(expr.member, expr.args);
        if (expr.object == "os") return genOsCall(expr.member, expr.args);
        if (expr.object == "string" &&
            (expr.member == "len" || expr.member == "compare" || expr.member == "concat" ||
             expr.member == "indexOf" || expr.member == "contains" || expr.member == "startsWith" ||
             expr.member == "endsWith" || expr.member == "charAt" || expr.member == "substr" ||
             expr.member == "toUpper" || expr.member == "toLower")) {
            return genStringCall(expr.member, expr.args);
        }

        if (expr.kind == ast::MethodCallKind::StructField) {
            auto& local = locals.at(expr.object);
            auto& fields = checker.structs().at(expr.resolvedStructName);
            auto fieldIt = std::find_if(fields.begin(), fields.end(),
                                         [&](auto& f) { return f.first == expr.member; });
            Type fieldType = fieldIt->second;
            auto* structTy = structTypes.at(expr.resolvedStructName);
            unsigned index = unsigned(fieldIt - fields.begin());
            auto* fieldPtr = builder.CreateStructGEP(structTy, local.addr, index);
            auto* closurePtr = builder.CreateLoad(ptrTy, fieldPtr);
            auto* codePtr = builder.CreateLoad(ptrTy, closurePtr);
            std::vector<llvm::Type*> llvmParams{ptrTy};
            for (auto& pt : fieldType.paramTypes) llvmParams.push_back(llvmType(pt));
            auto* fnType = llvm::FunctionType::get(llvmType(*fieldType.returnType), llvmParams, false);
            std::vector<llvm::Value*> args{closurePtr};
            for (size_t a = 0; a < expr.args.size(); a++) {
                auto v = genExpr(expr.args[a]);
                args.push_back(coerceValue(v, fieldType.paramTypes[a]));
            }
            auto* call = builder.CreateCall(fnType, codePtr, args);
            return TypedValue{call, *fieldType.returnType};
        }

        bool isMethod = expr.kind == ast::MethodCallKind::Method;
        std::string key = isMethod ? expr.resolvedStructName + "." + expr.member : expr.object + "." + expr.member;

        auto& sig = checker.functions().at(key);
        llvm::Function* fn = functionTable.at(key);
        std::vector<llvm::Value*> args;
        if (isMethod) args.push_back(locals.at(expr.object).addr);
        else args.push_back(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)));

        for (size_t i = 0; i < expr.args.size(); i++) {
            auto v = genExpr(expr.args[i]);
            args.push_back(coerceValue(v, sig.params[i].second));
        }
        auto* call = builder.CreateCall(fn, args);
        return TypedValue{call, sig.returnType};
    }

    void genStatement(ast::Statement& stmt) {
        if (currentBlockTerminated()) return;
        if (auto* n = std::get_if<ast::VarDeclStmt>(&stmt.node)) {
            Type declared = n->varType.empty() ? Type{TypeKind::Unknown} : checker.resolveTypeString(n->varType);
            if (n->value) {
                auto val = genExpr(*n->value);
                Type finalType = declared.kind == TypeKind::Unknown ? val.type : declared;
                auto* slot = allocSlot(n->name, finalType);
                auto* toStore = coerceValue(val, finalType);
                builder.CreateStore(toStore, slot);
                locals[n->name] = LocalVar{slot, finalType};
                if (finalType.kind == TypeKind::Function) {
                    if (!capturedInCurrentFn.count(n->name)) arcTrackedClosures.push_back(slot);
                    if (needsRetainOnBind(*n->value)) callRtRetain(toStore);
                }
            } else {
                auto* slot = allocSlot(n->name, declared);
                builder.CreateStore(llvm::Constant::getNullValue(llvmType(declared)), slot);
                locals[n->name] = LocalVar{slot, declared};
                if (declared.kind == TypeKind::Function && !capturedInCurrentFn.count(n->name)) {
                    arcTrackedClosures.push_back(slot);
                }
            }
            return;
        }
        if (auto* n = std::get_if<ast::ArrayDeclStmt>(&stmt.node)) {
            Type elem = checker.resolveTypeString(n->elementType);
            Type arr{TypeKind::Array};
            arr.elementType = std::make_shared<Type>(elem);
            arr.arraySize = n->size;
            auto* slot = builder.CreateAlloca(llvmType(arr), nullptr, n->name);
            locals[n->name] = LocalVar{slot, arr};
            return;
        }
        if (auto* n = std::get_if<ast::AssignmentStmt>(&stmt.node)) {
            auto& lv = locals.at(n->name);
            auto val = genExpr(n->value);
            auto* toStore = coerceValue(val, lv.type);
            if (lv.type.kind == TypeKind::Function) {
                auto* oldVal = builder.CreateLoad(ptrTy, lv.addr);
                callRtRelease(oldVal);
            }
            builder.CreateStore(toStore, lv.addr);
            if (lv.type.kind == TypeKind::Function && needsRetainOnBind(n->value)) {
                callRtRetain(toStore);
            }
            return;
        }
        if (auto* n = std::get_if<ast::ArrayAssignmentStmt>(&stmt.node)) {
            auto& lv = locals.at(n->name);
            auto idx = genExpr(n->index);
            auto val = genExpr(n->value);
            auto* arrTy = llvmType(lv.type);
            auto* elemPtr = builder.CreateGEP(arrTy, lv.addr, {llvm::ConstantInt::get(i64Ty, 0), toI64(idx)});
            builder.CreateStore(coerceValue(val, *lv.type.elementType), elemPtr);
            return;
        }
        if (auto* n = std::get_if<ast::PointerAssignmentStmt>(&stmt.node)) {
            auto target = genExpr(n->target);
            auto val = genExpr(n->value);
            Type pointee = target.type.pointee ? *target.type.pointee : Type{TypeKind::I64};
            builder.CreateStore(coerceValue(val, pointee), target.value);
            return;
        }
        if (auto* n = std::get_if<ast::FieldAssignmentStmt>(&stmt.node)) {
            auto* id = std::get_if<ast::IdentifierExpr>(&n->object.node);
            auto& lv = locals.at(id->name);
            auto& fields = checker.structs().at(lv.type.structName);
            size_t idx = 0;
            Type fieldType{TypeKind::Unknown};
            for (size_t i = 0; i < fields.size(); i++) {
                if (fields[i].first == n->field) { idx = i; fieldType = fields[i].second; break; }
            }
            auto* structTy = structTypes.at(lv.type.structName);
            auto* fieldPtr = builder.CreateStructGEP(structTy, lv.addr, unsigned(idx));
            auto val = genExpr(n->value);
            builder.CreateStore(coerceValue(val, fieldType), fieldPtr);
            return;
        }
        if (auto* n = std::get_if<ast::IfStmt>(&stmt.node)) {
            auto cond = genExpr(n->condition);
            auto* condVal = toCond(cond);
            auto* thenBB = llvm::BasicBlock::Create(ctx, "then", curFn);
            auto* endBB = llvm::BasicBlock::Create(ctx, "endif", curFn);
            llvm::BasicBlock* elseBB = n->elseBody ? llvm::BasicBlock::Create(ctx, "else", curFn) : nullptr;
            builder.CreateCondBr(condVal, thenBB, elseBB ? elseBB : endBB);

            builder.SetInsertPoint(thenBB);
            for (auto& s : n->thenBody) genStatement(s);
            if (!currentBlockTerminated()) builder.CreateBr(endBB);

            if (elseBB) {
                builder.SetInsertPoint(elseBB);
                for (auto& s : *n->elseBody) genStatement(s);
                if (!currentBlockTerminated()) builder.CreateBr(endBB);
            }
            builder.SetInsertPoint(endBB);
            return;
        }
        if (auto* n = std::get_if<ast::ForStmt>(&stmt.node)) {
            auto* condBB = llvm::BasicBlock::Create(ctx, "forcond", curFn);
            auto* bodyBB = llvm::BasicBlock::Create(ctx, "forbody", curFn);
            auto* endBB = llvm::BasicBlock::Create(ctx, "forend", curFn);
            builder.CreateBr(condBB);

            builder.SetInsertPoint(condBB);
            if (n->condition) {
                auto cond = genExpr(*n->condition);
                builder.CreateCondBr(toCond(cond), bodyBB, endBB);
            } else {
                builder.CreateBr(bodyBB);
            }

            builder.SetInsertPoint(bodyBB);
            loopStack.push_back({condBB, endBB});
            for (auto& s : n->body) genStatement(s);
            loopStack.pop_back();
            if (!currentBlockTerminated()) builder.CreateBr(condBB);

            builder.SetInsertPoint(endBB);
            return;
        }
        if (std::get_if<ast::BreakStmt>(&stmt.node)) {
            builder.CreateBr(loopStack.back().second);
            return;
        }
        if (std::get_if<ast::ContinueStmt>(&stmt.node)) {
            builder.CreateBr(loopStack.back().first);
            return;
        }
        if (auto* n = std::get_if<ast::ReturnStmt>(&stmt.node)) {
            if (n->value) {
                auto val = genExpr(*n->value);
                auto* toRet = coerceValue(val, curReturnType);
                if (curReturnType.kind == TypeKind::Function) callRtRetain(toRet);
                releaseArcLocals();
                callOrcExitRegion();
                if (curIsMain) builder.CreateRet(builder.CreateTrunc(toRet, i32Ty));
                else builder.CreateRet(toRet);
            } else {
                releaseArcLocals();
                callOrcExitRegion();
                if (curIsMain) builder.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
                else builder.CreateRetVoid();
            }
            return;
        }
        if (auto* n = std::get_if<ast::ExpressionStmt>(&stmt.node)) {
            genExpr(n->expr);
            return;
        }
        if (std::get_if<ast::InlineAsmStmt>(&stmt.node)) return;
        if (auto* n = std::get_if<ast::ComptimeStmt>(&stmt.node)) {
            for (auto& s : n->body) genStatement(s);
            return;
        }
    }
};

Codegen::Codegen(agn::parser::TypeChecker& checker, MemMode mode, const std::string& moduleName)
    : impl_(std::make_unique<Impl>(checker, mode, moduleName)) {}

Codegen::~Codegen() = default;

void Codegen::generate(agn::ast::Program& program) {
    impl_->declareStructs();
    impl_->declareFunctions(program);
    impl_->defineAllFunctionBodies(program);
}

void Codegen::dumpIR() const { impl_->mod->print(llvm::errs(), nullptr); }

bool Codegen::emitObjectFile(const std::string& path, std::string& errorOut) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    std::string lookupError;
#if LLVM_VERSION_MAJOR >= 19
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupError);
#else
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple.str(), lookupError);
#endif
    if (!target) {
        errorOut = lookupError;
        return false;
    }

    llvm::TargetOptions opts;
#if LLVM_VERSION_MAJOR >= 19
    auto* machine = target->createTargetMachine(triple, "generic", "", opts,
                                                 llvm::Reloc::PIC_, std::nullopt, llvm::CodeGenOptLevel::Default);
#else
    auto* machine = target->createTargetMachine(triple.str(), "generic", "", opts,
                                                 llvm::Reloc::PIC_, std::nullopt, llvm::CodeGenOptLevel::Default);
#endif
    if (!machine) {
        errorOut = "failed to create target machine";
        return false;
    }

#if LLVM_VERSION_MAJOR >= 19
    impl_->mod->setTargetTriple(triple);
#else
    impl_->mod->setTargetTriple(triple.str());
#endif
    impl_->mod->setDataLayout(machine->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);
    if (ec) {
        errorOut = ec.message();
        delete machine;
        return false;
    }

    llvm::legacy::PassManager pass;
    if (machine->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        errorOut = "target machine can't emit object files";
        delete machine;
        return false;
    }

    std::string verifyErrors;
    llvm::raw_string_ostream verifyStream(verifyErrors);
    if (llvm::verifyModule(*impl_->mod, &verifyStream)) {
        errorOut = "module verification failed: " + verifyErrors;
        delete machine;
        return false;
    }

    pass.run(*impl_->mod);
    dest.flush();
    delete machine;
    return true;
}

} // namespace agn::backend::llvm_backend
