// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#include "backend/gcc/backend.hpp"

#include <libgccjit.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agn::backend::gcc {

using agn::parser::FunctionSignature;
using agn::parser::Type;
using agn::parser::TypeKind;

namespace {

bool isIntKind(TypeKind k) {
    return k == TypeKind::I64 || k == TypeKind::I32 || k == TypeKind::I8 ||
           k == TypeKind::U64 || k == TypeKind::U32 || k == TypeKind::U8;
}

bool isUnsignedType(const Type& t) {
    return t.kind == TypeKind::U64 || t.kind == TypeKind::U32 || t.kind == TypeKind::U8;
}

struct TypedValue {
    gcc_jit_rvalue* value = nullptr;
    Type type;
};

struct LocalVar {
    gcc_jit_lvalue* directSlot = nullptr;
    gcc_jit_rvalue* heapAddr = nullptr;
    Type type;
};

} // namespace

struct GccBackend::Impl {
    agn::parser::TypeChecker& checker;
    MemMode mode;
    gcc_jit_context* ctxt;
    gcc_jit_location* loc = nullptr;

    gcc_jit_type* ptrTy;
    gcc_jit_type* i64Ty;
    gcc_jit_type* i32Ty;
    gcc_jit_type* i8Ty;
    gcc_jit_type* u64Ty;
    gcc_jit_type* u32Ty;
    gcc_jit_type* u8Ty;
    gcc_jit_type* boolTy;
    gcc_jit_type* voidTy;
    gcc_jit_type* doubleTy;

    std::unordered_map<std::string, gcc_jit_struct*> structTypes;
    std::unordered_map<std::string, gcc_jit_function*> functionTable;
    std::unordered_map<std::string, gcc_jit_rvalue*> stringLiterals;
    std::unordered_map<std::string, gcc_jit_function*> rtFnCache;
    int closureCounter = 0;
    int tempCounter = 0;

    gcc_jit_function* curFn = nullptr;
    gcc_jit_block* curBlock = nullptr;
    bool blockTerminated = false;
    gcc_jit_rvalue* curEnv = nullptr;
    Type curReturnType{TypeKind::Void};
    bool curIsMain = false;
    std::string currentModulePrefix;
    std::vector<std::pair<gcc_jit_block*, gcc_jit_block*>> loopStack; // {continueTarget, breakTarget}
    std::unordered_map<std::string, LocalVar> locals;
    std::vector<LocalVar> arcTrackedClosures;
    std::unordered_set<std::string> capturedInCurrentFn;

    Impl(agn::parser::TypeChecker& c, MemMode m, const std::string& name) : checker(c), mode(m) {
        ctxt = gcc_jit_context_acquire();
        gcc_jit_context_set_str_option(ctxt, GCC_JIT_STR_OPTION_PROGNAME, name.c_str());
        gcc_jit_context_set_int_option(ctxt, GCC_JIT_INT_OPTION_OPTIMIZATION_LEVEL, 1);
        gcc_jit_context_add_command_line_option(ctxt, "-fno-stack-protector");

        ptrTy = gcc_jit_type_get_pointer(gcc_jit_context_get_int_type(ctxt, 1, 0));
        i64Ty = gcc_jit_context_get_int_type(ctxt, 8, 1);
        i32Ty = gcc_jit_context_get_int_type(ctxt, 4, 1);
        i8Ty = gcc_jit_context_get_int_type(ctxt, 1, 1);
        u64Ty = gcc_jit_context_get_int_type(ctxt, 8, 0);
        u32Ty = gcc_jit_context_get_int_type(ctxt, 4, 0);
        u8Ty = gcc_jit_context_get_int_type(ctxt, 1, 0);
        boolTy = gcc_jit_context_get_type(ctxt, GCC_JIT_TYPE_BOOL);
        voidTy = gcc_jit_context_get_type(ctxt, GCC_JIT_TYPE_VOID);
        doubleTy = gcc_jit_context_get_type(ctxt, GCC_JIT_TYPE_DOUBLE);
    }

    ~Impl() { gcc_jit_context_release(ctxt); }

    gcc_jit_type* gccType(const Type& t) {
        switch (t.kind) {
            case TypeKind::I64: return i64Ty;
            case TypeKind::U64: return u64Ty;
            case TypeKind::I32: return i32Ty;
            case TypeKind::U32: return u32Ty;
            case TypeKind::I8: return i8Ty;
            case TypeKind::U8: return u8Ty;
            case TypeKind::F64: return doubleTy;
            case TypeKind::Bool: return boolTy;
            case TypeKind::Void: return voidTy;
            case TypeKind::String: return ptrTy;
            case TypeKind::Ptr: return ptrTy;
            case TypeKind::Function: return ptrTy;
            case TypeKind::Struct: {
                auto it = structTypes.find(t.structName);
                return it != structTypes.end() ? gcc_jit_struct_as_type(it->second) : i64Ty;
            }
            case TypeKind::Array:
                return gcc_jit_context_new_array_type(ctxt, loc, gccType(*t.elementType), int(t.arraySize));
            default:
                return i64Ty;
        }
    }

    gcc_jit_rvalue* materialize(gcc_jit_rvalue* val, gcc_jit_type* type) {
        gcc_jit_lvalue* tmp = gcc_jit_function_new_local(curFn, loc, type, ("t" + std::to_string(tempCounter++)).c_str());
        gcc_jit_block_add_assignment(curBlock, loc, tmp, val);
        return gcc_jit_lvalue_as_rvalue(tmp);
    }

    void switchBlock(gcc_jit_block* b) {
        curBlock = b;
        blockTerminated = false;
    }

    void endWithJump(gcc_jit_block* target) {
        if (blockTerminated) return;
        gcc_jit_block_end_with_jump(curBlock, loc, target);
        blockTerminated = true;
    }

    void endWithConditional(gcc_jit_rvalue* cond, gcc_jit_block* onTrue, gcc_jit_block* onFalse) {
        gcc_jit_block_end_with_conditional(curBlock, loc, cond, onTrue, onFalse);
        blockTerminated = true;
    }

    void endWithReturn(gcc_jit_rvalue* v) {
        if (blockTerminated) return;
        gcc_jit_block_end_with_return(curBlock, loc, v);
        blockTerminated = true;
    }

    void endWithVoidReturn() {
        if (blockTerminated) return;
        gcc_jit_block_end_with_void_return(curBlock, loc);
        blockTerminated = true;
    }

    gcc_jit_lvalue* accessLocal(LocalVar& lv) {
        if (lv.directSlot) return lv.directSlot;
        return gcc_jit_rvalue_dereference(lv.heapAddr, loc);
    }

    gcc_jit_rvalue* bitcast(gcc_jit_rvalue* v, gcc_jit_type* to) {
        return gcc_jit_context_new_bitcast(ctxt, loc, v, to);
    }

    gcc_jit_function* getRtFn(const std::string& name, gcc_jit_type* retTy, std::vector<gcc_jit_type*> paramTys) {
        auto it = rtFnCache.find(name);
        if (it != rtFnCache.end()) return it->second;
        std::vector<gcc_jit_param*> params;
        for (auto* pt : paramTys) params.push_back(gcc_jit_context_new_param(ctxt, loc, pt, "a"));
        auto* fn = gcc_jit_context_new_function(ctxt, loc, GCC_JIT_FUNCTION_IMPORTED, retTy, name.c_str(),
                                                 int(params.size()), params.data(), 0);
        rtFnCache[name] = fn;
        return fn;
    }

    gcc_jit_rvalue* callRt(const std::string& name, gcc_jit_type* retTy, std::vector<gcc_jit_type*> paramTys,
                            std::vector<gcc_jit_rvalue*> args) {
        auto* fn = getRtFn(name, retTy, paramTys);
        auto* call = gcc_jit_context_new_call(ctxt, loc, fn, int(args.size()), args.data());
        if (retTy == voidTy) {
            gcc_jit_block_add_eval(curBlock, loc, call);
            return nullptr;
        }
        return materialize(call, retTy);
    }

    void callRtRetain(gcc_jit_rvalue* ptr) {
        if (mode != MemMode::Arc) return;
        callRt("agn_rt_retain", voidTy, {ptrTy}, {ptr});
    }

    void callRtRelease(gcc_jit_rvalue* ptr) {
        if (mode != MemMode::Arc) return;
        callRt("agn_rt_release", voidTy, {ptrTy}, {ptr});
    }

    void callOrcEnterRegion() {
        if (mode != MemMode::Orc) return;
        callRt("agn_rt_orc_enter", voidTy, {}, {});
    }

    void callOrcExitRegion() {
        if (mode != MemMode::Orc) return;
        callRt("agn_rt_orc_exit", voidTy, {}, {});
    }

    gcc_jit_rvalue* callRtAlloc(gcc_jit_rvalue* size) {
        return callRt("agn_rt_alloc", ptrTy, {i64Ty}, {size});
    }

    gcc_jit_rvalue* constI64(long v) { return gcc_jit_context_new_rvalue_from_long(ctxt, i64Ty, v); }

    void releaseArcLocals() {
        for (auto& lv : arcTrackedClosures) {
            auto* val = gcc_jit_lvalue_as_rvalue(accessLocal(lv));
            callRtRelease(val);
        }
    }

    void emitDefaultReturn() {
        releaseArcLocals();
        callOrcExitRegion();
        if (curIsMain) {
            endWithReturn(gcc_jit_context_new_rvalue_from_int(ctxt, i32Ty, 0));
        } else if (curReturnType.kind == TypeKind::Void) {
            endWithVoidReturn();
        } else if (curReturnType.kind == TypeKind::Struct || curReturnType.kind == TypeKind::Array) {
            auto* tmp = gcc_jit_function_new_local(curFn, loc, gccType(curReturnType), "unreachable_ret");
            endWithReturn(gcc_jit_lvalue_as_rvalue(tmp));
        } else if (curReturnType.kind == TypeKind::F64) {
            endWithReturn(gcc_jit_context_new_rvalue_from_double(ctxt, doubleTy, 0.0));
        } else {
            endWithReturn(gcc_jit_context_new_rvalue_from_int(ctxt, gccType(curReturnType), 0));
        }
    }

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

    LocalVar allocSlot(const std::string& name, const Type& type) {
        if (capturedInCurrentFn.count(name)) {
            auto* raw = callRtAlloc(constI64(long(typeSize(type))));
            auto* typed = bitcast(raw, gcc_jit_type_get_pointer(gccType(type)));
            auto* stash = gcc_jit_function_new_local(curFn, loc, gcc_jit_type_get_pointer(gccType(type)),
                                                       (name + "_box").c_str());
            gcc_jit_block_add_assignment(curBlock, loc, stash, typed);
            return LocalVar{nullptr, gcc_jit_lvalue_as_rvalue(stash), type};
        }
        auto* slot = gcc_jit_function_new_local(curFn, loc, gccType(type), name.c_str());
        return LocalVar{slot, nullptr, type};
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

    static std::string funcKey(const ast::Function& f) {
        return f.receiver ? (f.receiver->type + "." + f.name) : f.name;
    }

    static std::string mangleName(const std::string& key) {
        std::string out = "agn_" + key;
        for (auto& c : out) if (c == '.') c = '_';
        return out;
    }

    void declareStructs() {
        for (auto& [name, fields] : checker.structs()) {
            (void)fields;
            structTypes[name] = gcc_jit_context_new_opaque_struct(ctxt, loc, ("struct_" + name).c_str());
        }
        for (auto& [name, fields] : checker.structs()) {
            std::vector<gcc_jit_field*> gfields;
            for (auto& field : fields) {
                gfields.push_back(gcc_jit_context_new_field(ctxt, loc, gccType(field.second), field.first.c_str()));
            }
            gcc_jit_struct_set_fields(structTypes[name], loc, int(gfields.size()), gfields.data());
        }
    }

    gcc_jit_rvalue* getStringLiteral(const std::string& s) {
        auto it = stringLiterals.find(s);
        if (it != stringLiterals.end()) return it->second;
        auto* lit = gcc_jit_context_new_string_literal(ctxt, s.c_str());
        auto* cast = bitcast(lit, ptrTy);
        stringLiterals[s] = cast;
        return cast;
    }

    gcc_jit_type* fnReturnType(const FunctionSignature& sig) { return gccType(sig.returnType); }

    void declareFunctions(ast::Program& program) {
        for (auto& f : program.functions) {
            std::string key = funcKey(f);
            auto& sig = checker.functions().at(key);
            if (f.name == "main" && !f.receiver) {
                auto* fn = gcc_jit_context_new_function(ctxt, loc, GCC_JIT_FUNCTION_EXPORTED, i32Ty, "main", 0,
                                                          nullptr, 0);
                functionTable[key] = fn;
                continue;
            }
            std::vector<gcc_jit_param*> params;
            params.push_back(gcc_jit_context_new_param(ctxt, loc, ptrTy, "env"));
            for (auto& p : sig.params) params.push_back(gcc_jit_context_new_param(ctxt, loc, gccType(p.second), "p"));
            auto* fn = gcc_jit_context_new_function(ctxt, loc, GCC_JIT_FUNCTION_INTERNAL, fnReturnType(sig),
                                                      mangleName(key).c_str(), int(params.size()), params.data(), 0);
            functionTable[key] = fn;
        }

        for (auto& [modName, module] : program.modules) {
            if (modName == "stdio" || modName == "os") continue;
            for (auto& f : module.functions) {
                if (!f.isExported) continue;
                std::string key = modName + "." + f.name;
                auto& sig = checker.functions().at(key);
                std::vector<gcc_jit_param*> params;
                params.push_back(gcc_jit_context_new_param(ctxt, loc, ptrTy, "env"));
                for (auto& p : sig.params) params.push_back(gcc_jit_context_new_param(ctxt, loc, gccType(p.second), "p"));
                auto* fn = gcc_jit_context_new_function(ctxt, loc, GCC_JIT_FUNCTION_INTERNAL, fnReturnType(sig),
                                                          mangleName(key).c_str(), int(params.size()), params.data(), 0);
                functionTable[key] = fn;
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

    void defineFunction(ast::Function& f, const std::string& modulePrefix) {
        std::string key = modulePrefix.empty() ? funcKey(f) : modulePrefix + "." + f.name;
        gcc_jit_function* fn = functionTable.at(key);
        bool isMain = (f.name == "main" && !f.receiver && modulePrefix.empty());
        auto& sig = checker.functions().at(key);

        auto* savedFn = curFn;
        auto* savedBlock = curBlock;
        bool savedTerminated = blockTerminated;
        auto* savedEnv = curEnv;
        Type savedRet = curReturnType;
        bool savedIsMain = curIsMain;
        std::string savedModulePrefix = currentModulePrefix;
        auto savedLocals = std::move(locals);
        auto savedArc = std::move(arcTrackedClosures);
        auto savedCaptured = std::move(capturedInCurrentFn);

        locals.clear();
        arcTrackedClosures.clear();
        capturedInCurrentFn = collectCapturedNames(f.body);
        curFn = fn;
        curReturnType = sig.returnType;
        curIsMain = isMain;
        currentModulePrefix = modulePrefix;

        switchBlock(gcc_jit_function_new_block(fn, "entry"));
        callOrcEnterRegion();

        int argIndex = 0;
        if (isMain) {
            curEnv = nullptr;
        } else if (f.receiver) {
            auto* recvParam = gcc_jit_function_get_param(fn, argIndex++);
            auto* recvPtr = bitcast(gcc_jit_param_as_rvalue(recvParam),
                                     gcc_jit_type_get_pointer(gccType(sig.receiver->second)));
            locals[f.receiver->name] = LocalVar{nullptr, recvPtr, sig.receiver->second};
            curEnv = nullptr;
        } else {
            auto* envParam = gcc_jit_function_get_param(fn, argIndex++);
            curEnv = gcc_jit_param_as_rvalue(envParam);
        }

        for (auto& p : f.params) {
            auto* param = gcc_jit_function_get_param(fn, argIndex++);
            Type pType = checker.resolveTypeString(p.type);
            LocalVar slot = allocSlot(p.name, pType);
            gcc_jit_block_add_assignment(curBlock, loc, accessLocal(slot), gcc_jit_param_as_rvalue(param));
            locals[p.name] = slot;
        }

        for (auto& stmt : f.body) genStatement(stmt);
        if (!blockTerminated) emitDefaultReturn();

        curFn = savedFn;
        curBlock = savedBlock;
        blockTerminated = savedTerminated;
        curEnv = savedEnv;
        curReturnType = savedRet;
        curIsMain = savedIsMain;
        currentModulePrefix = savedModulePrefix;
        locals = std::move(savedLocals);
        arcTrackedClosures = std::move(savedArc);
        capturedInCurrentFn = std::move(savedCaptured);
    }

    TypedValue genFunctionLiteral(ast::FunctionLiteralExpr& lit) {
        struct CapInfo { std::string name; Type type; gcc_jit_rvalue* addr; };
        std::vector<CapInfo> caps;
        for (auto& name : lit.capturedVars) {
            auto& lv = locals.at(name);
            gcc_jit_rvalue* addr = lv.directSlot ? gcc_jit_lvalue_get_address(lv.directSlot, loc) : lv.heapAddr;
            caps.push_back(CapInfo{name, lv.type, bitcast(addr, ptrTy)});
        }

        std::vector<Type> paramTypes;
        for (auto& p : lit.params) paramTypes.push_back(checker.resolveTypeString(p.type));
        Type retType = checker.resolveTypeString(lit.returnType);

        std::string fnName = "agn_closure_" + std::to_string(closureCounter++);
        std::vector<gcc_jit_param*> params;
        params.push_back(gcc_jit_context_new_param(ctxt, loc, ptrTy, "env"));
        for (size_t i = 0; i < lit.params.size(); i++) {
            params.push_back(gcc_jit_context_new_param(ctxt, loc, gccType(paramTypes[i]), lit.params[i].name.c_str()));
        }
        auto* fn = gcc_jit_context_new_function(ctxt, loc, GCC_JIT_FUNCTION_INTERNAL, gccType(retType),
                                                  fnName.c_str(), int(params.size()), params.data(), 0);

        auto* savedFn = curFn;
        auto* savedBlock = curBlock;
        bool savedTerminated = blockTerminated;
        auto* savedEnv = curEnv;
        Type savedRet = curReturnType;
        bool savedIsMain = curIsMain;
        auto savedLocals = std::move(locals);
        auto savedArc = std::move(arcTrackedClosures);
        auto savedCaptured = std::move(capturedInCurrentFn);

        locals.clear();
        arcTrackedClosures.clear();
        capturedInCurrentFn = collectCapturedNames(lit.body);
        curFn = fn;
        curReturnType = retType;
        curIsMain = false;

        switchBlock(gcc_jit_function_new_block(fn, "entry"));
        callOrcEnterRegion();

        auto* envParam = gcc_jit_function_get_param(fn, 0);
        curEnv = gcc_jit_param_as_rvalue(envParam);

        for (size_t i = 0; i < caps.size(); i++) {
            auto* slotArr = bitcast(curEnv, gcc_jit_type_get_pointer(ptrTy));
            auto* elemLv = gcc_jit_context_new_array_access(ctxt, loc, slotArr, constI64(long(i + 1)));
            auto* rawAddr = materialize(gcc_jit_lvalue_as_rvalue(elemLv), ptrTy);
            auto* typedAddr = bitcast(rawAddr, gcc_jit_type_get_pointer(gccType(caps[i].type)));
            locals[caps[i].name] = LocalVar{nullptr, typedAddr, caps[i].type};
        }
        for (size_t i = 0; i < lit.params.size(); i++) {
            auto* param = gcc_jit_function_get_param(fn, int(i + 1));
            LocalVar slot = allocSlot(lit.params[i].name, paramTypes[i]);
            gcc_jit_block_add_assignment(curBlock, loc, accessLocal(slot), gcc_jit_param_as_rvalue(param));
            locals[lit.params[i].name] = slot;
        }

        for (auto& stmt : lit.body) genStatement(stmt);
        if (!blockTerminated) emitDefaultReturn();

        curFn = savedFn;
        curBlock = savedBlock;
        blockTerminated = savedTerminated;
        curEnv = savedEnv;
        curReturnType = savedRet;
        curIsMain = savedIsMain;
        locals = std::move(savedLocals);
        arcTrackedClosures = std::move(savedArc);
        capturedInCurrentFn = std::move(savedCaptured);

        unsigned long totalSlots = 1 + caps.size();
        gcc_jit_rvalue* obj = callRtAlloc(constI64(long(totalSlots * 8)));
        auto* objArr = bitcast(obj, gcc_jit_type_get_pointer(ptrTy));
        auto* codeSlot = gcc_jit_context_new_array_access(ctxt, loc, objArr, constI64(0));
        gcc_jit_rvalue* codePtr = bitcast(gcc_jit_function_get_address(fn, loc), ptrTy);
        gcc_jit_block_add_assignment(curBlock, loc, codeSlot, codePtr);
        for (size_t i = 0; i < caps.size(); i++) {
            auto* slotN = gcc_jit_context_new_array_access(ctxt, loc, objArr, constI64(long(i + 1)));
            gcc_jit_block_add_assignment(curBlock, loc, slotN, caps[i].addr);
        }

        Type fnValType;
        fnValType.kind = TypeKind::Function;
        fnValType.paramTypes = paramTypes;
        fnValType.returnType = std::make_shared<Type>(retType);
        return TypedValue{obj, fnValType};
    }

    TypedValue wrapNamedFunctionAsValue(const std::string& name, const FunctionSignature& sig) {
        gcc_jit_function* fn = functionTable.at(name);
        gcc_jit_rvalue* obj = callRtAlloc(constI64(8));
        auto* objArr = bitcast(obj, gcc_jit_type_get_pointer(ptrTy));
        auto* codeSlot = gcc_jit_context_new_array_access(ctxt, loc, objArr, constI64(0));
        gcc_jit_block_add_assignment(curBlock, loc, codeSlot, bitcast(gcc_jit_function_get_address(fn, loc), ptrTy));
        Type t;
        t.kind = TypeKind::Function;
        for (auto& p : sig.params) t.paramTypes.push_back(p.second);
        t.returnType = std::make_shared<Type>(sig.returnType);
        return TypedValue{obj, t};
    }

    gcc_jit_rvalue* coerceValue(const TypedValue& v, const Type& target) {
        if (target.kind == TypeKind::Struct || target.kind == TypeKind::Array ||
            target.kind == TypeKind::String || target.kind == TypeKind::Ptr ||
            target.kind == TypeKind::Function || target.kind == TypeKind::Unknown) {
            return v.value;
        }
        if (target.kind == TypeKind::Bool) {
            if (v.type.kind == TypeKind::Bool) return v.value;
            return materialize(gcc_jit_context_new_comparison(ctxt, loc, GCC_JIT_COMPARISON_NE, v.value,
                                                                gcc_jit_context_new_rvalue_from_int(ctxt, gccType(v.type), 0)),
                                boolTy);
        }
        if (v.type.kind == TypeKind::Bool && isIntKind(target.kind)) {
            return materialize(gcc_jit_context_new_cast(ctxt, loc, v.value, gccType(target)), gccType(target));
        }
        if (target.kind == TypeKind::F64) {
            if (v.type.kind == TypeKind::F64) return v.value;
            if (isIntKind(v.type.kind) || v.type.kind == TypeKind::Bool) {
                return materialize(gcc_jit_context_new_cast(ctxt, loc, v.value, doubleTy), doubleTy);
            }
            return v.value;
        }
        if (isIntKind(target.kind) && v.type.kind == TypeKind::F64) {
            return materialize(gcc_jit_context_new_cast(ctxt, loc, v.value, gccType(target)), gccType(target));
        }
        if (isIntKind(target.kind) && isIntKind(v.type.kind)) {
            if (gccType(v.type) == gccType(target)) return v.value;
            return materialize(gcc_jit_context_new_cast(ctxt, loc, v.value, gccType(target)), gccType(target));
        }
        return v.value;
    }

    gcc_jit_rvalue* toCond(const TypedValue& v) {
        if (v.type.kind == TypeKind::Bool) return v.value;
        return materialize(gcc_jit_context_new_comparison(ctxt, loc, GCC_JIT_COMPARISON_NE, v.value,
                                                            gcc_jit_context_new_rvalue_from_int(ctxt, gccType(v.type), 0)),
                            boolTy);
    }

    gcc_jit_rvalue* toI64(const TypedValue& v) {
        if (v.type.kind == TypeKind::F64) {
            return materialize(gcc_jit_context_new_cast(ctxt, loc, v.value, i64Ty), i64Ty);
        }
        if (!isIntKind(v.type.kind) && v.type.kind != TypeKind::Bool) return v.value;
        if (gccType(v.type) == i64Ty) return v.value;
        return materialize(gcc_jit_context_new_cast(ctxt, loc, v.value, i64Ty), i64Ty);
    }

    gcc_jit_rvalue* toBufferPtr(const TypedValue& v) {
        if (v.type.kind == TypeKind::Ptr) return v.value;
        return bitcast(toI64(v), ptrTy);
    }

    TypedValue genStdioCall(const std::string& member, std::vector<ast::Expression>& args) {
        if (member == "Print" || member == "Println") {
            auto val = genExpr(args[0]);
            if (val.type.kind == TypeKind::String) {
                callRt(member == "Println" ? "agn_rt_println_str" : "agn_rt_print_str", voidTy, {ptrTy}, {val.value});
            } else if (val.type.isFloat()) {
                callRt(member == "Println" ? "agn_rt_println_float" : "agn_rt_print_float", voidTy, {doubleTy},
                       {coerceValue(val, Type{TypeKind::F64})});
            } else {
                callRt(member == "Println" ? "agn_rt_println_int" : "agn_rt_print_int", voidTy, {i64Ty}, {toI64(val)});
            }
            return TypedValue{constI64(0), Type{TypeKind::Void}};
        }
        if (member == "PrintStr") {
            callRt("agn_rt_print_str", voidTy, {ptrTy}, {genExpr(args[0]).value});
            return TypedValue{constI64(0), Type{TypeKind::Void}};
        }
        if (member == "PrintlnStr") {
            callRt("agn_rt_println_str", voidTy, {ptrTy}, {genExpr(args[0]).value});
            return TypedValue{constI64(0), Type{TypeKind::Void}};
        }
        if (member == "PrintChar") {
            callRt("agn_rt_print_char", voidTy, {i64Ty}, {toI64(genExpr(args[0]))});
            return TypedValue{constI64(0), Type{TypeKind::Void}};
        }
        if (member == "ReadInt") {
            return TypedValue{callRt("agn_rt_read_int", i64Ty, {}, {}), Type{TypeKind::I64}};
        }
        if (member == "ReadChar") {
            return TypedValue{callRt("agn_rt_read_char", i64Ty, {}, {}), Type{TypeKind::I64}};
        }
        if (member == "ReadLine") {
            auto bufVal = genExpr(args[0]);
            auto lenVal = genExpr(args[1]);
            auto* bufPtr = toBufferPtr(bufVal);
            return TypedValue{callRt("agn_rt_read_line", i64Ty, {ptrTy, i64Ty}, {bufPtr, toI64(lenVal)}), Type{TypeKind::I64}};
        }
        if (member == "Flush") {
            callRt("agn_rt_flush", voidTy, {}, {});
            return TypedValue{constI64(0), Type{TypeKind::Void}};
        }
        return TypedValue{constI64(0), Type{TypeKind::Unknown}};
    }

    TypedValue wrapOptionInt(gcc_jit_rvalue* rawValue, const std::string& qualifiedFnName) {
        Type retType = checker.functions().at(qualifiedFnName).returnType;
        auto* structGcc = structTypes.at(retType.structName);
        auto* tmp = gcc_jit_function_new_local(curFn, loc, gcc_jit_struct_as_type(structGcc), "opt_wrap");
        auto* someFlag = gcc_jit_context_new_comparison(ctxt, loc, GCC_JIT_COMPARISON_GE, rawValue, constI64(0));
        auto* someField = gcc_jit_struct_get_field(structGcc, 0);
        auto* valueField = gcc_jit_struct_get_field(structGcc, 1);
        gcc_jit_block_add_assignment(curBlock, loc, gcc_jit_lvalue_access_field(tmp, loc, someField), someFlag);
        gcc_jit_block_add_assignment(curBlock, loc, gcc_jit_lvalue_access_field(tmp, loc, valueField), rawValue);
        return TypedValue{gcc_jit_lvalue_as_rvalue(tmp), retType};
    }

    TypedValue genStringCall(const std::string& member, std::vector<ast::Expression>& args) {
        if (member == "len") {
            return TypedValue{callRt("agn_rt_strlen", i64Ty, {ptrTy}, {genExpr(args[0]).value}), Type{TypeKind::I64}};
        }
        if (member == "compare") {
            return TypedValue{callRt("agn_rt_strcmp", i64Ty, {ptrTy, ptrTy},
                                      {genExpr(args[0]).value, genExpr(args[1]).value}),
                               Type{TypeKind::I64}};
        }
        if (member == "concat") {
            auto s1 = genExpr(args[0]).value;
            auto s2 = genExpr(args[1]).value;
            return genConcatValues(s1, s2);
        }
        if (member == "indexOf") {
            return wrapOptionInt(callRt("agn_rt_index_of", i64Ty, {ptrTy, ptrTy},
                                         {genExpr(args[0]).value, genExpr(args[1]).value}),
                                  "string.indexOf");
        }
        if (member == "contains") {
            auto* call = callRt("agn_rt_contains", i64Ty, {ptrTy, ptrTy},
                                 {genExpr(args[0]).value, genExpr(args[1]).value});
            return TypedValue{coerceValue({call, Type{TypeKind::I64}}, Type{TypeKind::Bool}), Type{TypeKind::Bool}};
        }
        if (member == "startsWith") {
            auto* call = callRt("agn_rt_starts_with", i64Ty, {ptrTy, ptrTy},
                                 {genExpr(args[0]).value, genExpr(args[1]).value});
            return TypedValue{coerceValue({call, Type{TypeKind::I64}}, Type{TypeKind::Bool}), Type{TypeKind::Bool}};
        }
        if (member == "endsWith") {
            auto* call = callRt("agn_rt_ends_with", i64Ty, {ptrTy, ptrTy},
                                 {genExpr(args[0]).value, genExpr(args[1]).value});
            return TypedValue{coerceValue({call, Type{TypeKind::I64}}, Type{TypeKind::Bool}), Type{TypeKind::Bool}};
        }
        if (member == "charAt") {
            return wrapOptionInt(callRt("agn_rt_char_at", i64Ty, {ptrTy, i64Ty},
                                         {genExpr(args[0]).value, toI64(genExpr(args[1]))}),
                                  "string.charAt");
        }
        if (member == "substr") {
            return TypedValue{callRt("agn_rt_substr", ptrTy, {ptrTy, i64Ty, i64Ty},
                                      {genExpr(args[0]).value, toI64(genExpr(args[1])), toI64(genExpr(args[2]))}),
                               Type{TypeKind::String}};
        }
        if (member == "toUpper") {
            return TypedValue{callRt("agn_rt_to_upper", ptrTy, {ptrTy}, {genExpr(args[0]).value}),
                               Type{TypeKind::String}};
        }
        if (member == "toLower") {
            return TypedValue{callRt("agn_rt_to_lower", ptrTy, {ptrTy}, {genExpr(args[0]).value}),
                               Type{TypeKind::String}};
        }
        return TypedValue{constI64(0), Type{TypeKind::Unknown}};
    }

    TypedValue genOsCall(const std::string& member, std::vector<ast::Expression>& args) {
        if (member == "ArgCount") {
            return TypedValue{callRt("agn_rt_argc", i64Ty, {}, {}), Type{TypeKind::I64}};
        }
        if (member == "Arg") {
            return TypedValue{callRt("agn_rt_argv", ptrTy, {i64Ty}, {toI64(genExpr(args[0]))}), Type{TypeKind::String}};
        }
        if (member == "OpenRead") {
            return wrapOptionInt(callRt("agn_rt_open_read", i64Ty, {ptrTy}, {genExpr(args[0]).value}), "os.OpenRead");
        }
        if (member == "OpenCreate") {
            return wrapOptionInt(callRt("agn_rt_open_create", i64Ty, {ptrTy}, {genExpr(args[0]).value}),
                                  "os.OpenCreate");
        }
        if (member == "Close") {
            return TypedValue{callRt("agn_rt_close", i64Ty, {i64Ty}, {toI64(genExpr(args[0]))}), Type{TypeKind::I64}};
        }
        if (member == "ReadFd") {
            auto bufVal = genExpr(args[1]);
            auto* bufPtr = toBufferPtr(bufVal);
            return wrapOptionInt(callRt("agn_rt_read_fd", i64Ty, {i64Ty, ptrTy, i64Ty},
                                         {toI64(genExpr(args[0])), bufPtr, toI64(genExpr(args[2]))}),
                                  "os.ReadFd");
        }
        if (member == "WriteFd") {
            auto* data = genExpr(args[1]).value;
            auto* len = callRt("agn_rt_strlen", i64Ty, {ptrTy}, {data});
            return wrapOptionInt(
                callRt("agn_rt_write_fd", i64Ty, {i64Ty, ptrTy, i64Ty}, {toI64(genExpr(args[0])), data, len}),
                "os.WriteFd");
        }
        if (member == "Exit") {
            callRt("agn_rt_exit", voidTy, {i64Ty}, {toI64(genExpr(args[0]))});
            emitDefaultReturn();
            return TypedValue{constI64(0), Type{TypeKind::Void}};
        }
        return TypedValue{constI64(0), Type{TypeKind::Unknown}};
    }

    TypedValue genConcatValues(gcc_jit_rvalue* s1, gcc_jit_rvalue* s2) {
        auto* len1 = callRt("agn_rt_strlen", i64Ty, {ptrTy}, {s1});
        auto* len2 = callRt("agn_rt_strlen", i64Ty, {ptrTy}, {s2});
        auto* total = materialize(gcc_jit_context_new_binary_op(ctxt, loc, GCC_JIT_BINARY_OP_PLUS, i64Ty,
                                       gcc_jit_context_new_binary_op(ctxt, loc, GCC_JIT_BINARY_OP_PLUS, i64Ty, len1, len2),
                                       constI64(1)),
                                   i64Ty);
        auto* buf = callRtAlloc(total);
        callRt("agn_rt_memcpy", voidTy, {ptrTy, ptrTy, i64Ty}, {buf, s1, len1});
        auto* tailAddr = materialize(
            gcc_jit_lvalue_get_address(gcc_jit_context_new_array_access(ctxt, loc, bitcast(buf, ptrTy), len1), loc), ptrTy);
        callRt("agn_rt_memcpy", voidTy, {ptrTy, ptrTy, i64Ty}, {tailAddr, s2, len2});
        auto* totalLen = materialize(gcc_jit_context_new_binary_op(ctxt, loc, GCC_JIT_BINARY_OP_PLUS, i64Ty, len1, len2), i64Ty);
        auto* endLv = gcc_jit_context_new_array_access(ctxt, loc, bitcast(buf, ptrTy), totalLen);
        gcc_jit_block_add_assignment(curBlock, loc, endLv, gcc_jit_context_new_rvalue_from_int(ctxt, u8Ty, 0));
        return TypedValue{buf, Type{TypeKind::String}};
    }

    void appendToBuffer(gcc_jit_lvalue* posSlot, gcc_jit_rvalue* buf, gcc_jit_rvalue* src, gcc_jit_rvalue* len) {
        auto* pos = gcc_jit_lvalue_as_rvalue(posSlot);
        auto* destAddr = materialize(
            gcc_jit_lvalue_get_address(gcc_jit_context_new_array_access(ctxt, loc, bitcast(buf, ptrTy), pos), loc), ptrTy);
        callRt("agn_rt_memcpy", voidTy, {ptrTy, ptrTy, i64Ty}, {destAddr, src, len});
        gcc_jit_block_add_assignment(curBlock, loc, posSlot,
                                      gcc_jit_context_new_binary_op(ctxt, loc, GCC_JIT_BINARY_OP_PLUS, i64Ty, pos, len));
    }

    TypedValue genTemplateString(ast::TemplateStringExpr& tmpl) {
        auto* bufSlot = gcc_jit_function_new_local(curFn, loc, gcc_jit_context_new_array_type(ctxt, loc, u8Ty, 1024),
                                                     "tmplbuf");
        auto* buf = bitcast(gcc_jit_lvalue_get_address(bufSlot, loc), ptrTy);
        auto* posSlot = gcc_jit_function_new_local(curFn, loc, i64Ty, "tmplpos");
        gcc_jit_block_add_assignment(curBlock, loc, posSlot, constI64(0));

        for (auto& part : tmpl.parts) {
            if (auto* lit = std::get_if<ast::TemplateLiteralPart>(&part)) {
                appendToBuffer(posSlot, buf, getStringLiteral(lit->text), constI64(long(lit->text.size())));
                continue;
            }
            auto* e = std::get_if<ast::TemplateExprPart>(&part);
            auto val = genExpr(*e->expr);
            if (val.type.kind == TypeKind::String) {
                auto* len = callRt("agn_rt_strlen", i64Ty, {ptrTy}, {val.value});
                appendToBuffer(posSlot, buf, val.value, len);
                continue;
            }

            long width = 0;
            bool padZero = false, hex = false, upper = false;
            if (e->format) {
                width = long(e->format->width.value_or(0));
                padZero = e->format->padding == '0';
                hex = e->format->formatType == ast::FormatType::Hex || e->format->formatType == ast::FormatType::HexUpper;
                upper = e->format->formatType == ast::FormatType::HexUpper;
            }

            auto* pos = gcc_jit_lvalue_as_rvalue(posSlot);
            auto* destAddr = materialize(
                gcc_jit_lvalue_get_address(gcc_jit_context_new_array_access(ctxt, loc, bitcast(buf, ptrTy), pos), loc), ptrTy);
            gcc_jit_rvalue* written;
            if (hex) {
                written = callRt("agn_rt_format_hex", i64Ty, {ptrTy, u64Ty, i64Ty, i64Ty, i64Ty},
                                  {destAddr, toI64(val), constI64(width), constI64(padZero ? 1 : 0), constI64(upper ? 1 : 0)});
            } else {
                written = callRt("agn_rt_format_int", i64Ty, {ptrTy, i64Ty, i64Ty, i64Ty},
                                  {destAddr, toI64(val), constI64(width), constI64(padZero ? 1 : 0)});
            }
            gcc_jit_block_add_assignment(curBlock, loc, posSlot,
                                          gcc_jit_context_new_binary_op(ctxt, loc, GCC_JIT_BINARY_OP_PLUS, i64Ty, pos, written));
        }

        auto* pos = gcc_jit_lvalue_as_rvalue(posSlot);
        auto* endLv = gcc_jit_context_new_array_access(ctxt, loc, bitcast(buf, ptrTy), pos);
        gcc_jit_block_add_assignment(curBlock, loc, endLv, gcc_jit_context_new_rvalue_from_int(ctxt, u8Ty, 0));
        return TypedValue{buf, Type{TypeKind::String}};
    }

    TypedValue genExpr(ast::Expression& expr) {
        if (auto* n = std::get_if<ast::NumberExpr>(&expr.node)) {
            return TypedValue{gcc_jit_context_new_rvalue_from_long(ctxt, i64Ty, long(n->value)), Type{TypeKind::I64}};
        }
        if (auto* n = std::get_if<ast::FloatExpr>(&expr.node)) {
            return TypedValue{gcc_jit_context_new_rvalue_from_double(ctxt, doubleTy, n->value), Type{TypeKind::F64}};
        }
        if (auto* n = std::get_if<ast::BoolExpr>(&expr.node)) {
            return TypedValue{gcc_jit_context_new_rvalue_from_int(ctxt, boolTy, n->value ? 1 : 0), Type{TypeKind::Bool}};
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
                return TypedValue{gcc_jit_lvalue_as_rvalue(accessLocal(it->second)), it->second.type};
            }
            auto fit = checker.functions().find(n->name);
            if (fit != checker.functions().end() && !fit->second.receiver) {
                return wrapNamedFunctionAsValue(n->name, fit->second);
            }
            return TypedValue{constI64(0), Type{TypeKind::Unknown}};
        }
        if (auto* n = std::get_if<ast::BinaryExpr>(&expr.node)) {
            if (n->op == ast::BinaryOp::Concat) {
                auto l = genExpr(*n->left);
                auto r = genExpr(*n->right);
                return genConcatValues(l.value, r.value);
            }
            auto l = genExpr(*n->left);
            auto r = genExpr(*n->right);
            bool eitherFloat = l.type.isFloat() || r.type.isFloat();
            bool bothUnsigned = isUnsignedType(l.type) && isUnsignedType(r.type);
            Type wideType = eitherFloat ? Type{TypeKind::F64}
                            : (isIntKind(l.type.kind) && isIntKind(r.type.kind))
                                 ? (typeSize(l.type) >= typeSize(r.type) ? l.type : r.type)
                                 : l.type;
            gcc_jit_type* wide = gccType(wideType);

            gcc_jit_rvalue* lv = eitherFloat || isIntKind(l.type.kind) ? coerceValue(l, wideType) : l.value;
            gcc_jit_rvalue* rv = eitherFloat || isIntKind(r.type.kind) ? coerceValue(r, wideType) : r.value;
            Type resultType = eitherFloat ? Type{TypeKind::F64} : (bothUnsigned ? Type{TypeKind::U64} : wideType);
            if (!eitherFloat && isIntKind(l.type.kind) && isIntKind(r.type.kind)) resultType = wideType;

            auto binOp = [&](enum gcc_jit_binary_op op) {
                return TypedValue{materialize(gcc_jit_context_new_binary_op(ctxt, loc, op, wide, lv, rv), wide), resultType};
            };
            auto cmp = [&](enum gcc_jit_comparison op) {
                return TypedValue{materialize(gcc_jit_context_new_comparison(ctxt, loc, op, lv, rv), boolTy), Type{TypeKind::Bool}};
            };

            switch (n->op) {
                case ast::BinaryOp::Add: return binOp(GCC_JIT_BINARY_OP_PLUS);
                case ast::BinaryOp::Sub: return binOp(GCC_JIT_BINARY_OP_MINUS);
                case ast::BinaryOp::Mul: return binOp(GCC_JIT_BINARY_OP_MULT);
                case ast::BinaryOp::Div: return binOp(GCC_JIT_BINARY_OP_DIVIDE);
                case ast::BinaryOp::Mod: return binOp(GCC_JIT_BINARY_OP_MODULO);
                case ast::BinaryOp::BitAnd: return binOp(GCC_JIT_BINARY_OP_BITWISE_AND);
                case ast::BinaryOp::BitOr: return binOp(GCC_JIT_BINARY_OP_BITWISE_OR);
                case ast::BinaryOp::BitXor: return binOp(GCC_JIT_BINARY_OP_BITWISE_XOR);
                case ast::BinaryOp::Shl: return binOp(GCC_JIT_BINARY_OP_LSHIFT);
                case ast::BinaryOp::Shr: return binOp(GCC_JIT_BINARY_OP_RSHIFT);
                case ast::BinaryOp::Equal: return cmp(GCC_JIT_COMPARISON_EQ);
                case ast::BinaryOp::NotEqual: return cmp(GCC_JIT_COMPARISON_NE);
                case ast::BinaryOp::Less: return cmp(GCC_JIT_COMPARISON_LT);
                case ast::BinaryOp::LessEqual: return cmp(GCC_JIT_COMPARISON_LE);
                case ast::BinaryOp::Greater: return cmp(GCC_JIT_COMPARISON_GT);
                case ast::BinaryOp::GreaterEqual: return cmp(GCC_JIT_COMPARISON_GE);
                case ast::BinaryOp::And:
                    return TypedValue{materialize(gcc_jit_context_new_binary_op(ctxt, loc, GCC_JIT_BINARY_OP_LOGICAL_AND, boolTy,
                                                                                  toCond(l), toCond(r)), boolTy),
                                       Type{TypeKind::Bool}};
                case ast::BinaryOp::Or:
                    return TypedValue{materialize(gcc_jit_context_new_binary_op(ctxt, loc, GCC_JIT_BINARY_OP_LOGICAL_OR, boolTy,
                                                                                  toCond(l), toCond(r)), boolTy),
                                       Type{TypeKind::Bool}};
                default:
                    return TypedValue{lv, resultType};
            }
        }
        if (auto* n = std::get_if<ast::UnaryExpr>(&expr.node)) {
            auto v = genExpr(*n->operand);
            if (n->op == ast::UnaryOp::Neg) {
                return TypedValue{materialize(gcc_jit_context_new_unary_op(ctxt, loc, GCC_JIT_UNARY_OP_MINUS, gccType(v.type), v.value),
                                               gccType(v.type)),
                                   v.type};
            }
            return TypedValue{materialize(gcc_jit_context_new_unary_op(ctxt, loc, GCC_JIT_UNARY_OP_LOGICAL_NEGATE, boolTy, toCond(v)),
                                           boolTy),
                               Type{TypeKind::Bool}};
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
            auto* elemLv = gcc_jit_context_new_array_access(ctxt, loc, gcc_jit_lvalue_as_rvalue(accessLocal(lv)), toI64(idx));
            return TypedValue{gcc_jit_lvalue_as_rvalue(elemLv), *lv.type.elementType};
        }
        if (auto* n = std::get_if<ast::StringIndexExpr>(&expr.node)) {
            auto str = genExpr(*n->str);
            auto idx = genExpr(*n->index);
            auto* elemLv = gcc_jit_context_new_array_access(ctxt, loc, str.value, toI64(idx));
            return TypedValue{gcc_jit_lvalue_as_rvalue(elemLv), Type{TypeKind::U8}};
        }
        if (auto* n = std::get_if<ast::AddressOfExpr>(&expr.node)) {
            if (auto* id = std::get_if<ast::IdentifierExpr>(&n->operand->node)) {
                auto& lv = locals.at(id->name);
                Type ptrType{TypeKind::Ptr};
                ptrType.pointee = std::make_shared<Type>(lv.type);
                return TypedValue{bitcast(gcc_jit_lvalue_get_address(accessLocal(lv), loc), ptrTy), ptrType};
            }
            auto v = genExpr(*n->operand);
            Type ptrType{TypeKind::Ptr};
            ptrType.pointee = std::make_shared<Type>(v.type);
            return TypedValue{v.value, ptrType};
        }
        if (auto* n = std::get_if<ast::DerefExpr>(&expr.node)) {
            auto v = genExpr(*n->operand);
            Type pointee = v.type.pointee ? *v.type.pointee : Type{TypeKind::I64};
            auto* typed = bitcast(v.value, gcc_jit_type_get_pointer(gccType(pointee)));
            return TypedValue{gcc_jit_lvalue_as_rvalue(gcc_jit_rvalue_dereference(typed, loc)), pointee};
        }
        if (std::get_if<ast::EvalExpr>(&expr.node)) {
            std::fprintf(stderr, "error: 'eval' is not supported by the gcc backend "
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
            auto* field = gcc_jit_struct_get_field(structTypes.at(lv.type.structName), idx);
            auto* fieldLv = gcc_jit_lvalue_access_field(accessLocal(lv), loc, field);
            return TypedValue{gcc_jit_lvalue_as_rvalue(fieldLv), fieldType};
        }
        if (auto* n = std::get_if<ast::FunctionLiteralExpr>(&expr.node)) {
            return genFunctionLiteral(*n);
        }
        if (auto* n = std::get_if<ast::StructLiteralExpr>(&expr.node)) {
            auto& fields = checker.structs().at(n->structName);
            auto* structGcc = structTypes.at(n->structName);
            auto* tmp = gcc_jit_function_new_local(curFn, loc, gcc_jit_struct_as_type(structGcc), "structlit");
            for (auto& [fname, fexpr] : n->fields) {
                for (size_t i = 0; i < fields.size(); i++) {
                    if (fields[i].first != fname) continue;
                    auto val = genExpr(fexpr);
                    auto* coerced = coerceValue(val, fields[i].second);
                    auto* field = gcc_jit_struct_get_field(structGcc, i);
                    gcc_jit_block_add_assignment(curBlock, loc, gcc_jit_lvalue_access_field(tmp, loc, field), coerced);
                    break;
                }
            }
            Type t{TypeKind::Struct};
            t.structName = n->structName;
            return TypedValue{gcc_jit_lvalue_as_rvalue(tmp), t};
        }
        return TypedValue{constI64(0), Type{TypeKind::Unknown}};
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
            gcc_jit_function* fn = functionTable.at(resolvedName);
            std::vector<gcc_jit_rvalue*> args;
            args.push_back(gcc_jit_context_null(ctxt, ptrTy));
            for (size_t i = 0; i < argExprs.size(); i++) {
                auto v = genExpr(argExprs[i]);
                args.push_back(coerceValue(v, fit->second.params[i].second));
            }
            auto* call = gcc_jit_context_new_call(ctxt, loc, fn, int(args.size()), args.data());
            if (fit->second.returnType.kind == TypeKind::Void) {
                gcc_jit_block_add_eval(curBlock, loc, call);
                return TypedValue{constI64(0), Type{TypeKind::Void}};
            }
            return TypedValue{materialize(call, gccType(fit->second.returnType)), fit->second.returnType};
        }

        auto& lv = locals.at(name);
        auto* closureVal = gcc_jit_lvalue_as_rvalue(accessLocal(lv));
        auto* codeSlot = gcc_jit_context_new_array_access(ctxt, loc, bitcast(closureVal, gcc_jit_type_get_pointer(ptrTy)),
                                                            constI64(0));
        auto* rawCode = materialize(gcc_jit_lvalue_as_rvalue(codeSlot), ptrTy);

        std::vector<gcc_jit_type*> paramTys{ptrTy};
        for (auto& pt : lv.type.paramTypes) paramTys.push_back(gccType(pt));
        auto* fnPtrTy = gcc_jit_context_new_function_ptr_type(ctxt, loc, gccType(*lv.type.returnType),
                                                                int(paramTys.size()), paramTys.data(), 0);
        auto* codePtr = bitcast(rawCode, fnPtrTy);

        std::vector<gcc_jit_rvalue*> args{closureVal};
        for (size_t i = 0; i < argExprs.size(); i++) {
            auto v = genExpr(argExprs[i]);
            args.push_back(coerceValue(v, lv.type.paramTypes[i]));
        }
        auto* call = gcc_jit_context_new_call_through_ptr(ctxt, loc, codePtr, int(args.size()), args.data());
        if (lv.type.returnType->kind == TypeKind::Void) {
            gcc_jit_block_add_eval(curBlock, loc, call);
            return TypedValue{constI64(0), Type{TypeKind::Void}};
        }
        return TypedValue{materialize(call, gccType(*lv.type.returnType)), *lv.type.returnType};
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
            auto* field = gcc_jit_struct_get_field(structTypes.at(expr.resolvedStructName),
                                                     size_t(fieldIt - fields.begin()));
            auto* closureVal = gcc_jit_lvalue_as_rvalue(gcc_jit_lvalue_access_field(accessLocal(local), loc, field));
            auto* codeSlot = gcc_jit_context_new_array_access(ctxt, loc, bitcast(closureVal, gcc_jit_type_get_pointer(ptrTy)),
                                                            constI64(0));
            auto* rawCode = materialize(gcc_jit_lvalue_as_rvalue(codeSlot), ptrTy);

            std::vector<gcc_jit_type*> paramTys{ptrTy};
            for (auto& pt : fieldType.paramTypes) paramTys.push_back(gccType(pt));
            auto* fnPtrTy = gcc_jit_context_new_function_ptr_type(ctxt, loc, gccType(*fieldType.returnType),
                                                                    int(paramTys.size()), paramTys.data(), 0);
            auto* codePtr = bitcast(rawCode, fnPtrTy);

            std::vector<gcc_jit_rvalue*> args{closureVal};
            for (size_t a = 0; a < expr.args.size(); a++) {
                auto v = genExpr(expr.args[a]);
                args.push_back(coerceValue(v, fieldType.paramTypes[a]));
            }
            auto* call = gcc_jit_context_new_call_through_ptr(ctxt, loc, codePtr, int(args.size()), args.data());
            if (fieldType.returnType->kind == TypeKind::Void) {
                gcc_jit_block_add_eval(curBlock, loc, call);
                return TypedValue{constI64(0), Type{TypeKind::Void}};
            }
            return TypedValue{materialize(call, gccType(*fieldType.returnType)), *fieldType.returnType};
        }

        bool isMethod = expr.kind == ast::MethodCallKind::Method;
        std::string key = isMethod ? expr.resolvedStructName + "." + expr.member : expr.object + "." + expr.member;

        auto& sig = checker.functions().at(key);
        gcc_jit_function* fn = functionTable.at(key);
        std::vector<gcc_jit_rvalue*> args;
        if (isMethod) {
            auto& lv = locals.at(expr.object);
            args.push_back(bitcast(gcc_jit_lvalue_get_address(accessLocal(lv), loc), ptrTy));
        } else {
            args.push_back(gcc_jit_context_null(ctxt, ptrTy));
        }

        for (size_t i = 0; i < expr.args.size(); i++) {
            auto v = genExpr(expr.args[i]);
            args.push_back(coerceValue(v, sig.params[i].second));
        }
        auto* call = gcc_jit_context_new_call(ctxt, loc, fn, int(args.size()), args.data());
        if (sig.returnType.kind == TypeKind::Void) {
            gcc_jit_block_add_eval(curBlock, loc, call);
            return TypedValue{constI64(0), Type{TypeKind::Void}};
        }
        return TypedValue{materialize(call, gccType(sig.returnType)), sig.returnType};
    }

    void genStatement(ast::Statement& stmt) {
        if (blockTerminated) return;
        if (auto* n = std::get_if<ast::VarDeclStmt>(&stmt.node)) {
            Type declared = n->varType.empty() ? Type{TypeKind::Unknown} : checker.resolveTypeString(n->varType);
            if (n->value) {
                auto val = genExpr(*n->value);
                Type finalType = declared.kind == TypeKind::Unknown ? val.type : declared;
                LocalVar slot = allocSlot(n->name, finalType);
                auto* toStore = coerceValue(val, finalType);
                gcc_jit_block_add_assignment(curBlock, loc, accessLocal(slot), toStore);
                locals[n->name] = slot;
                if (finalType.kind == TypeKind::Function) {
                    if (!capturedInCurrentFn.count(n->name)) arcTrackedClosures.push_back(slot);
                    if (needsRetainOnBind(*n->value)) callRtRetain(toStore);
                }
            } else {
                LocalVar slot = allocSlot(n->name, declared);
                if (declared.kind == TypeKind::F64) {
                    gcc_jit_block_add_assignment(curBlock, loc, accessLocal(slot),
                                                  gcc_jit_context_new_rvalue_from_double(ctxt, doubleTy, 0.0));
                } else if (declared.kind != TypeKind::Struct && declared.kind != TypeKind::Array) {
                    gcc_jit_block_add_assignment(curBlock, loc, accessLocal(slot),
                                                  gcc_jit_context_new_rvalue_from_int(ctxt, gccType(declared), 0));
                }
                locals[n->name] = slot;
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
            auto* slot = gcc_jit_function_new_local(curFn, loc, gccType(arr), n->name.c_str());
            locals[n->name] = LocalVar{slot, nullptr, arr};
            return;
        }
        if (auto* n = std::get_if<ast::AssignmentStmt>(&stmt.node)) {
            auto& lv = locals.at(n->name);
            auto val = genExpr(n->value);
            auto* toStore = coerceValue(val, lv.type);
            if (lv.type.kind == TypeKind::Function) {
                auto* oldVal = gcc_jit_lvalue_as_rvalue(accessLocal(lv));
                callRtRelease(oldVal);
            }
            gcc_jit_block_add_assignment(curBlock, loc, accessLocal(lv), toStore);
            if (lv.type.kind == TypeKind::Function && needsRetainOnBind(n->value)) {
                callRtRetain(toStore);
            }
            return;
        }
        if (auto* n = std::get_if<ast::ArrayAssignmentStmt>(&stmt.node)) {
            auto& lv = locals.at(n->name);
            auto idx = genExpr(n->index);
            auto val = genExpr(n->value);
            auto* elemLv = gcc_jit_context_new_array_access(ctxt, loc, gcc_jit_lvalue_as_rvalue(accessLocal(lv)), toI64(idx));
            gcc_jit_block_add_assignment(curBlock, loc, elemLv, coerceValue(val, *lv.type.elementType));
            return;
        }
        if (auto* n = std::get_if<ast::PointerAssignmentStmt>(&stmt.node)) {
            auto target = genExpr(n->target);
            auto val = genExpr(n->value);
            Type pointee = target.type.pointee ? *target.type.pointee : Type{TypeKind::I64};
            auto* typed = bitcast(target.value, gcc_jit_type_get_pointer(gccType(pointee)));
            gcc_jit_block_add_assignment(curBlock, loc, gcc_jit_rvalue_dereference(typed, loc), coerceValue(val, pointee));
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
            auto* field = gcc_jit_struct_get_field(structTypes.at(lv.type.structName), idx);
            auto val = genExpr(n->value);
            gcc_jit_block_add_assignment(curBlock, loc, gcc_jit_lvalue_access_field(accessLocal(lv), loc, field),
                                          coerceValue(val, fieldType));
            return;
        }
        if (auto* n = std::get_if<ast::IfStmt>(&stmt.node)) {
            auto cond = genExpr(n->condition);
            auto* condVal = toCond(cond);
            auto* thenBB = gcc_jit_function_new_block(curFn, "then");
            auto* endBB = gcc_jit_function_new_block(curFn, "endif");
            gcc_jit_block* elseBB = n->elseBody ? gcc_jit_function_new_block(curFn, "else") : nullptr;
            endWithConditional(condVal, thenBB, elseBB ? elseBB : endBB);

            switchBlock(thenBB);
            for (auto& s : n->thenBody) genStatement(s);
            endWithJump(endBB);

            if (elseBB) {
                switchBlock(elseBB);
                for (auto& s : *n->elseBody) genStatement(s);
                endWithJump(endBB);
            }
            switchBlock(endBB);
            return;
        }
        if (auto* n = std::get_if<ast::ForStmt>(&stmt.node)) {
            auto* condBB = gcc_jit_function_new_block(curFn, "forcond");
            auto* bodyBB = gcc_jit_function_new_block(curFn, "forbody");
            auto* endBB = gcc_jit_function_new_block(curFn, "forend");
            endWithJump(condBB);

            switchBlock(condBB);
            if (n->condition) {
                auto cond = genExpr(*n->condition);
                endWithConditional(toCond(cond), bodyBB, endBB);
            } else {
                endWithJump(bodyBB);
            }

            switchBlock(bodyBB);
            loopStack.push_back({condBB, endBB});
            for (auto& s : n->body) genStatement(s);
            loopStack.pop_back();
            endWithJump(condBB);

            switchBlock(endBB);
            return;
        }
        if (std::get_if<ast::BreakStmt>(&stmt.node)) {
            endWithJump(loopStack.back().second);
            return;
        }
        if (std::get_if<ast::ContinueStmt>(&stmt.node)) {
            endWithJump(loopStack.back().first);
            return;
        }
        if (auto* n = std::get_if<ast::ReturnStmt>(&stmt.node)) {
            if (n->value) {
                auto val = genExpr(*n->value);
                auto* toRet = coerceValue(val, curReturnType);
                if (curReturnType.kind == TypeKind::Function) callRtRetain(toRet);
                releaseArcLocals();
                callOrcExitRegion();
                if (curIsMain) {
                    endWithReturn(materialize(gcc_jit_context_new_cast(ctxt, loc, toRet, i32Ty), i32Ty));
                } else {
                    endWithReturn(toRet);
                }
            } else {
                releaseArcLocals();
                callOrcExitRegion();
                if (curIsMain) endWithReturn(gcc_jit_context_new_rvalue_from_int(ctxt, i32Ty, 0));
                else endWithVoidReturn();
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

GccBackend::GccBackend(agn::parser::TypeChecker& checker, MemMode mode, const std::string& moduleName)
    : impl_(std::make_unique<Impl>(checker, mode, moduleName)) {}

GccBackend::~GccBackend() = default;

void GccBackend::generate(agn::ast::Program& program) {
    impl_->declareStructs();
    impl_->declareFunctions(program);
    impl_->defineAllFunctionBodies(program);
}

bool GccBackend::emitObjectFile(const std::string& path, std::string& errorOut) {
    gcc_jit_context_compile_to_file(impl_->ctxt, GCC_JIT_OUTPUT_KIND_OBJECT_FILE, path.c_str());
    const char* err = gcc_jit_context_get_first_error(impl_->ctxt);
    if (err) {
        errorOut = err;
        return false;
    }
    return true;
}

} // namespace agn::backend::gcc
