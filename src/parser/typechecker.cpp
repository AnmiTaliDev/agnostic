// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#include "parser/typechecker.hpp"
#include "misc/suggest.hpp"

#include <algorithm>
#include <cstdio>

namespace agn::parser {

bool Type::isNumeric() const {
    switch (kind) {
        case TypeKind::I64: case TypeKind::I32: case TypeKind::I8:
        case TypeKind::U64: case TypeKind::U32: case TypeKind::U8:
        case TypeKind::F64:
            return true;
        default:
            return false;
    }
}

bool Type::operator==(const Type& other) const {
    if (kind != other.kind) return false;
    switch (kind) {
        case TypeKind::Ptr:
            return *pointee == *other.pointee;
        case TypeKind::Array:
            return arraySize == other.arraySize && *elementType == *other.elementType;
        case TypeKind::Function: {
            if (paramTypes.size() != other.paramTypes.size()) return false;
            for (size_t i = 0; i < paramTypes.size(); i++) {
                if (!(paramTypes[i] == other.paramTypes[i])) return false;
            }
            return *returnType == *other.returnType;
        }
        case TypeKind::Struct:
            return structName == other.structName;
        default:
            return true;
    }
}

bool Type::canAssignTo(const Type& other) const {
    if (*this == other) return true;
    if (isNumeric() && other.isNumeric()) return true;
    if (kind == TypeKind::Ptr && other.isNumeric()) return true;
    if (kind == TypeKind::Bool && other.isNumeric()) return true;
    if (other.kind == TypeKind::Bool && isNumeric()) return true;
    if (kind == TypeKind::Unknown || other.kind == TypeKind::Unknown) return true;
    return false;
}

std::string Type::toString() const {
    switch (kind) {
        case TypeKind::I64: return "i64";
        case TypeKind::I32: return "i32";
        case TypeKind::I8: return "i8";
        case TypeKind::U64: return "u64";
        case TypeKind::U32: return "u32";
        case TypeKind::U8: return "u8";
        case TypeKind::F64: return "f64";
        case TypeKind::Bool: return "bool";
        case TypeKind::String: return "string";
        case TypeKind::Void: return "void";
        case TypeKind::Ptr: return "*" + pointee->toString();
        case TypeKind::Array: return "[" + elementType->toString() + ";" + std::to_string(arraySize) + "]";
        case TypeKind::Struct: return structName;
        case TypeKind::Function: {
            std::string s = "func(";
            for (size_t i = 0; i < paramTypes.size(); i++) {
                if (i) s += ",";
                s += paramTypes[i].toString();
            }
            s += ")->" + returnType->toString();
            return s;
        }
        default: return "unknown";
    }
}

TypeChecker::TypeChecker(std::string targetOs, std::string targetArch, std::string memMode)
    : targetOs_(std::move(targetOs)), targetArch_(std::move(targetArch)), memMode_(std::move(memMode)) {}

Type TypeChecker::namedType(const std::string& name) {
    if (name == "i64" || name == "int") return Type{TypeKind::I64};
    if (name == "i32") return Type{TypeKind::I32};
    if (name == "i8") return Type{TypeKind::I8};
    if (name == "u64") return Type{TypeKind::U64};
    if (name == "u32") return Type{TypeKind::U32};
    if (name == "u8") return Type{TypeKind::U8};
    if (name == "f64" || name == "float") return Type{TypeKind::F64};
    if (name == "bool") return Type{TypeKind::Bool};
    if (name == "string") return Type{TypeKind::String};
    if (name.empty() || name == "void") return Type{TypeKind::Void};
    if (structs_.count(name)) {
        Type t; t.kind = TypeKind::Struct; t.structName = name;
        return t;
    }
    return Type{TypeKind::Unknown};
}

Type TypeChecker::parseTypeStr(const std::string& s, size_t& pos) {
    if (s.compare(pos, 5, "func(") == 0) {
        pos += 5;
        std::vector<Type> params;
        while (pos < s.size() && s[pos] != ')') {
            params.push_back(parseTypeStr(s, pos));
            if (pos < s.size() && s[pos] == ',') pos++;
        }
        if (pos < s.size()) pos++;
        if (s.compare(pos, 2, "->") == 0) pos += 2;
        Type ret = parseTypeStr(s, pos);

        Type t;
        t.kind = TypeKind::Function;
        t.paramTypes = std::move(params);
        t.returnType = std::make_shared<Type>(ret);
        return t;
    }

    size_t start = pos;
    while (pos < s.size() && s[pos] != ',' && s[pos] != ')') pos++;
    return namedType(s.substr(start, pos - start));
}

Type TypeChecker::resolveType(const std::string& text) {
    if (text.empty()) return Type{TypeKind::Void};
    size_t pos = 0;
    return parseTypeStr(text, pos);
}

void TypeChecker::registerStruct(const ast::StructDecl& decl) {
    std::vector<std::pair<std::string, Type>> fields;
    for (auto& f : decl.fields) fields.emplace_back(f.name, resolveType(f.type));
    structs_[decl.name] = std::move(fields);
}

void TypeChecker::registerFunctionSignature(const std::string& key, const ast::Function& func) {
    FunctionSignature sig;
    if (func.receiver) sig.receiver = std::make_pair(func.receiver->name, resolveType(func.receiver->type));
    for (auto& p : func.params) sig.params.emplace_back(p.name, resolveType(p.type));
    sig.returnType = resolveType(func.returnType);
    functions_[key] = std::move(sig);
}

std::optional<Type> TypeChecker::lookupVar(const std::string& name) {
    for (int i = static_cast<int>(scopeStack_.size()) - 1; i >= 0; i--) {
        auto it = scopeStack_[i].vars.find(name);
        if (it != scopeStack_[i].vars.end()) {
            for (size_t j = i + 1; j < scopeStack_.size(); j++) {
                if (scopeStack_[j].isClosure) {
                    auto& caps = *scopeStack_[j].captureSet;
                    if (std::find(caps.begin(), caps.end(), name) == caps.end()) caps.push_back(name);
                }
            }
            return it->second;
        }
    }
    return std::nullopt;
}

void TypeChecker::declareVar(const std::string& name, const Type& type) {
    scopeStack_.back().vars[name] = type;
}

void TypeChecker::addError(const std::string& message) {
    errors_.push_back(TypeError{message, currentFunction_.empty() ? "global" : currentFunction_,
                                 currentLine_, currentColumn_});
}

std::string TypeChecker::didYouMean(const std::string& name, const std::vector<std::string>& candidates) const {
    auto suggestion = agn::misc::suggestClosest(name, candidates);
    return suggestion ? " (did you mean '" + *suggestion + "'?)" : "";
}

std::vector<std::string> TypeChecker::visibleVarNames() const {
    std::vector<std::string> names;
    for (auto& frame : scopeStack_) {
        for (auto& [name, type] : frame.vars) names.push_back(name);
    }
    return names;
}

std::vector<std::string> TypeChecker::functionNames() const {
    std::vector<std::string> names;
    names.reserve(functions_.size());
    for (auto& [name, sig] : functions_) names.push_back(name);
    return names;
}

std::vector<std::string> TypeChecker::structNames() const {
    std::vector<std::string> names;
    names.reserve(structs_.size());
    for (auto& [name, fields] : structs_) names.push_back(name);
    return names;
}

std::vector<std::string> TypeChecker::fieldNames(const std::string& structName) const {
    std::vector<std::string> names;
    auto it = structs_.find(structName);
    if (it == structs_.end()) return names;
    for (auto& [name, type] : it->second) names.push_back(name);
    return names;
}

static std::string functionKey(const ast::Function& f) {
    return f.receiver ? (f.receiver->type + "." + f.name) : f.name;
}

bool TypeChecker::checkProgram(ast::Program& program) {
    for (auto& s : program.structs) structs_[s.name] = {};
    for (auto& s : program.structs) registerStruct(s);

    for (auto& f : program.functions) registerFunctionSignature(functionKey(f), f);
    for (auto& [modName, mod] : program.modules) {
        moduleNames_.push_back(modName);
        for (auto& f : mod.functions) {
            if (f.isExported) registerFunctionSignature(modName + "." + f.name, f);
        }
    }

    for (auto& f : program.functions) checkFunctionBody(f, functionKey(f));
    for (auto& [modName, mod] : program.modules) {
        for (auto& f : mod.functions) {
            if (f.isExported) checkFunctionBody(f, modName + "." + f.name, modName);
        }
    }

    return errors_.empty();
}

void TypeChecker::checkFunctionBody(ast::Function& func, const std::string& key, const std::string& modulePrefix) {
    currentFunction_ = key;
    currentModulePrefix_ = modulePrefix;
    scopeStack_.clear();
    scopeStack_.push_back(ScopeFrame{});

    auto& sig = functions_.at(key);
    if (func.receiver) declareVar(func.receiver->name, sig.receiver->second);
    for (auto& [name, type] : sig.params) declareVar(name, type);

    returnTypeStack_.push_back(sig.returnType);
    for (auto& stmt : func.body) checkStatement(stmt);
    returnTypeStack_.pop_back();

    scopeStack_.pop_back();
    currentFunction_.clear();
    currentModulePrefix_.clear();
}

void TypeChecker::checkComptimeBody(std::vector<ast::Statement>& body) {
    std::vector<ast::Statement> result;
    for (auto& stmt : body) {
        if (auto* ifs = std::get_if<ast::IfStmt>(&stmt.node)) {
            auto val = evalComptimeCondition(ifs->condition);
            if (!val) {
                addError("comptime condition must be a compile-time constant");
                continue;
            }
            std::vector<ast::Statement> empty;
            std::vector<ast::Statement>* branch = *val ? &ifs->thenBody : (ifs->elseBody ? &*ifs->elseBody : &empty);
            checkComptimeBody(*branch);
            for (auto& s : *branch) result.push_back(std::move(s));
        } else {
            checkStatement(stmt);
            result.push_back(std::move(stmt));
        }
    }
    body = std::move(result);
}

std::optional<std::string> TypeChecker::evalComptimeConstant(const ast::Expression& expr) {
    if (auto* id = std::get_if<ast::IdentifierExpr>(&expr.node)) {
        if (id->name == "TARGET_OS") return targetOs_;
        if (id->name == "TARGET_ARCH") return targetArch_;
        if (id->name == "MEM_MODE") return memMode_;
        return std::nullopt;
    }
    if (auto* str = std::get_if<ast::StringExpr>(&expr.node)) return str->value;
    return std::nullopt;
}

std::optional<bool> TypeChecker::evalComptimeCondition(const ast::Expression& expr) {
    if (auto* bin = std::get_if<ast::BinaryExpr>(&expr.node)) {
        if (bin->op == ast::BinaryOp::And || bin->op == ast::BinaryOp::Or) {
            auto l = evalComptimeCondition(*bin->left);
            auto r = evalComptimeCondition(*bin->right);
            if (!l || !r) return std::nullopt;
            return bin->op == ast::BinaryOp::And ? (*l && *r) : (*l || *r);
        }
        if (bin->op == ast::BinaryOp::Equal || bin->op == ast::BinaryOp::NotEqual) {
            auto l = evalComptimeConstant(*bin->left);
            auto r = evalComptimeConstant(*bin->right);
            if (!l || !r) return std::nullopt;
            bool eq = (*l == *r);
            return bin->op == ast::BinaryOp::Equal ? eq : !eq;
        }
        return std::nullopt;
    }
    if (auto* un = std::get_if<ast::UnaryExpr>(&expr.node); un && un->op == ast::UnaryOp::Not) {
        auto v = evalComptimeCondition(*un->operand);
        return v ? std::optional<bool>(!*v) : std::nullopt;
    }
    return std::nullopt;
}

void TypeChecker::checkStatement(ast::Statement& stmt) {
    currentLine_ = stmt.line;
    currentColumn_ = stmt.column;
    if (auto* n = std::get_if<ast::VarDeclStmt>(&stmt.node)) {
        Type declared = n->varType.empty() ? Type{TypeKind::Unknown} : resolveType(n->varType);
        if (n->value) {
            Type exprType = checkExpression(*n->value);
            if (!declared.canAssignTo(exprType) && !exprType.canAssignTo(declared)) {
                addError("type mismatch in variable '" + n->name + "': declared as " + declared.toString() +
                          ", initialized with " + exprType.toString());
            }
            declareVar(n->name, declared.kind == TypeKind::Unknown ? exprType : declared);
        } else {
            declareVar(n->name, declared);
        }
        return;
    }
    if (auto* n = std::get_if<ast::ArrayDeclStmt>(&stmt.node)) {
        Type elem = resolveType(n->elementType);
        Type arr; arr.kind = TypeKind::Array; arr.elementType = std::make_shared<Type>(elem); arr.arraySize = n->size;
        declareVar(n->name, arr);
        return;
    }
    if (auto* n = std::get_if<ast::AssignmentStmt>(&stmt.node)) {
        Type exprType = checkExpression(n->value);
        auto varType = lookupVar(n->name);
        if (!varType) addError("variable '" + n->name + "' not declared" + didYouMean(n->name, visibleVarNames()));
        else if (!exprType.canAssignTo(*varType)) {
            addError("type mismatch in assignment to '" + n->name + "': expected " + varType->toString() +
                      ", got " + exprType.toString());
        }
        return;
    }
    if (auto* n = std::get_if<ast::ArrayAssignmentStmt>(&stmt.node)) {
        auto varType = lookupVar(n->name);
        if (!varType) { addError("variable '" + n->name + "' not declared" + didYouMean(n->name, visibleVarNames())); return; }
        if (varType->kind != TypeKind::Array) { addError("cannot index into non-array type " + varType->toString()); return; }
        Type indexType = checkExpression(n->index);
        if (!indexType.isNumeric()) addError("array index must be numeric");
        Type valueType = checkExpression(n->value);
        if (!valueType.canAssignTo(*varType->elementType)) {
            addError("type mismatch in array assignment: expected " + varType->elementType->toString() +
                      ", got " + valueType.toString());
        }
        return;
    }
    if (auto* n = std::get_if<ast::PointerAssignmentStmt>(&stmt.node)) {
        Type targetType = checkExpression(n->target);
        if (targetType.kind != TypeKind::Ptr && targetType.kind != TypeKind::Unknown) {
            addError("pointer assignment requires a pointer type, got " + targetType.toString());
        }
        checkExpression(n->value);
        return;
    }
    if (auto* n = std::get_if<ast::FieldAssignmentStmt>(&stmt.node)) {
        Type objType = checkExpression(n->object);
        Type valueType = checkExpression(n->value);
        if (objType.kind != TypeKind::Struct) {
            addError("field assignment requires a struct type, got " + objType.toString());
            return;
        }
        auto& fields = structs_[objType.structName];
        auto it = std::find_if(fields.begin(), fields.end(), [&](auto& f) { return f.first == n->field; });
        if (it == fields.end()) {
            addError("struct '" + objType.structName + "' has no field '" + n->field + "'" +
                      didYouMean(n->field, fieldNames(objType.structName)));
        } else if (!valueType.canAssignTo(it->second)) {
            addError("type mismatch assigning to field '" + n->field + "'");
        }
        return;
    }
    if (auto* n = std::get_if<ast::IfStmt>(&stmt.node)) {
        checkExpression(n->condition);
        for (auto& s : n->thenBody) checkStatement(s);
        if (n->elseBody) for (auto& s : *n->elseBody) checkStatement(s);
        return;
    }
    if (auto* n = std::get_if<ast::ForStmt>(&stmt.node)) {
        if (n->condition) checkExpression(*n->condition);
        loopDepth_++;
        for (auto& s : n->body) checkStatement(s);
        loopDepth_--;
        return;
    }
    if (std::get_if<ast::BreakStmt>(&stmt.node)) {
        if (loopDepth_ == 0) addError("'break' outside of a loop");
        return;
    }
    if (std::get_if<ast::ContinueStmt>(&stmt.node)) {
        if (loopDepth_ == 0) addError("'continue' outside of a loop");
        return;
    }
    if (auto* n = std::get_if<ast::ReturnStmt>(&stmt.node)) {
        Type expected = returnTypeStack_.empty() ? Type{TypeKind::Void} : returnTypeStack_.back();
        if (n->value) {
            Type got = checkExpression(*n->value);
            if (!got.canAssignTo(expected)) {
                addError("return type mismatch: expected " + expected.toString() + ", got " + got.toString());
            }
        } else if (expected.kind != TypeKind::Void) {
            addError("function must return a value of type " + expected.toString());
        }
        return;
    }
    if (auto* n = std::get_if<ast::ExpressionStmt>(&stmt.node)) {
        checkExpression(n->expr);
        return;
    }
    if (std::get_if<ast::InlineAsmStmt>(&stmt.node)) return;
    if (auto* n = std::get_if<ast::ComptimeStmt>(&stmt.node)) {
        checkComptimeBody(n->body);
        return;
    }
}

Type TypeChecker::checkFunctionLiteral(ast::FunctionLiteralExpr& lit) {
    ScopeFrame frame;
    frame.isClosure = true;
    frame.captureSet = &lit.capturedVars;

    std::vector<Type> paramTypes;
    for (auto& p : lit.params) {
        Type t = resolveType(p.type);
        frame.vars[p.name] = t;
        paramTypes.push_back(t);
    }
    scopeStack_.push_back(std::move(frame));

    Type retType = resolveType(lit.returnType);
    returnTypeStack_.push_back(retType);
    int savedLoopDepth = loopDepth_;
    loopDepth_ = 0;
    for (auto& stmt : lit.body) checkStatement(stmt);
    loopDepth_ = savedLoopDepth;
    returnTypeStack_.pop_back();

    scopeStack_.pop_back();

    Type fnType;
    fnType.kind = TypeKind::Function;
    fnType.paramTypes = std::move(paramTypes);
    fnType.returnType = std::make_shared<Type>(retType);
    return fnType;
}

Type TypeChecker::checkExpression(ast::Expression& expr) {
    if (std::get_if<ast::NumberExpr>(&expr.node)) return Type{TypeKind::I64};
    if (std::get_if<ast::FloatExpr>(&expr.node)) return Type{TypeKind::F64};
    if (std::get_if<ast::BoolExpr>(&expr.node)) return Type{TypeKind::Bool};
    if (std::get_if<ast::StringExpr>(&expr.node)) return Type{TypeKind::String};

    if (auto* n = std::get_if<ast::TemplateStringExpr>(&expr.node)) {
        for (auto& part : n->parts) {
            if (auto* e = std::get_if<ast::TemplateExprPart>(&part)) checkExpression(*e->expr);
        }
        return Type{TypeKind::String};
    }

    if (auto* n = std::get_if<ast::IdentifierExpr>(&expr.node)) {
        if (auto v = lookupVar(n->name)) return *v;
        auto it = functions_.find(n->name);
        if (it != functions_.end() && !it->second.receiver) {
            Type t; t.kind = TypeKind::Function;
            for (auto& [pname, ptype] : it->second.params) { (void)pname; t.paramTypes.push_back(ptype); }
            t.returnType = std::make_shared<Type>(it->second.returnType);
            return t;
        }
        auto candidates = visibleVarNames();
        auto fnNames = functionNames();
        candidates.insert(candidates.end(), fnNames.begin(), fnNames.end());
        addError("variable '" + n->name + "' not declared" + didYouMean(n->name, candidates));
        return Type{TypeKind::Unknown};
    }

    if (auto* n = std::get_if<ast::BinaryExpr>(&expr.node)) {
        Type l = checkExpression(*n->left);
        Type r = checkExpression(*n->right);
        switch (n->op) {
            case ast::BinaryOp::Add: case ast::BinaryOp::Sub: case ast::BinaryOp::Mul: case ast::BinaryOp::Div:
                if (!l.isNumeric()) addError("left operand must be numeric, got " + l.toString());
                if (!r.isNumeric()) addError("right operand must be numeric, got " + r.toString());
                return (l.isFloat() || r.isFloat()) ? Type{TypeKind::F64} : l;
            case ast::BinaryOp::Mod:
            case ast::BinaryOp::BitAnd: case ast::BinaryOp::BitOr: case ast::BinaryOp::BitXor:
            case ast::BinaryOp::Shl: case ast::BinaryOp::Shr:
                if (l.isFloat() || r.isFloat()) {
                    addError("'" + l.toString() + "' and '" + r.toString() +
                             "': this operator does not support float operands");
                } else {
                    if (!l.isNumeric()) addError("left operand must be numeric, got " + l.toString());
                    if (!r.isNumeric()) addError("right operand must be numeric, got " + r.toString());
                }
                return l;
            case ast::BinaryOp::Concat:
                return Type{TypeKind::String};
            default:
                return Type{TypeKind::Bool};
        }
    }

    if (auto* n = std::get_if<ast::UnaryExpr>(&expr.node)) {
        Type operand = checkExpression(*n->operand);
        if (n->op == ast::UnaryOp::Neg) {
            if (!operand.isNumeric()) addError("negation operand must be numeric, got " + operand.toString());
            return operand;
        }
        return Type{TypeKind::Bool};
    }

    if (auto* n = std::get_if<ast::CallExpr>(&expr.node)) {
        std::string resolvedName = n->function;
        if (!currentModulePrefix_.empty() && !lookupVar(n->function)) {
            std::string qualified = currentModulePrefix_ + "." + n->function;
            if (functions_.count(qualified)) resolvedName = qualified;
        }

        auto it = functions_.find(resolvedName);
        if (it != functions_.end()) {
            auto& sig = it->second;
            if (n->args.size() != sig.params.size()) {
                addError("function '" + n->function + "' expects " + std::to_string(sig.params.size()) +
                          " arguments, got " + std::to_string(n->args.size()));
            }
            for (size_t i = 0; i < n->args.size() && i < sig.params.size(); i++) {
                Type argType = checkExpression(n->args[i]);
                if (!argType.canAssignTo(sig.params[i].second)) {
                    addError("argument " + std::to_string(i) + " of '" + n->function + "': expected " +
                              sig.params[i].second.toString() + ", got " + argType.toString());
                }
            }
            return sig.returnType;
        }
        auto localType = lookupVar(n->function);
        if (localType && localType->kind == TypeKind::Function) {
            if (n->args.size() != localType->paramTypes.size()) {
                addError("closure call expects " + std::to_string(localType->paramTypes.size()) +
                          " arguments, got " + std::to_string(n->args.size()));
            }
            for (size_t i = 0; i < n->args.size() && i < localType->paramTypes.size(); i++) {
                Type argType = checkExpression(n->args[i]);
                if (!argType.canAssignTo(localType->paramTypes[i])) {
                    addError("argument " + std::to_string(i) + " of closure call has wrong type");
                }
            }
            return *localType->returnType;
        }
        {
            auto candidates = functionNames();
            auto varNames = visibleVarNames();
            candidates.insert(candidates.end(), varNames.begin(), varNames.end());
            addError("function '" + n->function + "' not declared" + didYouMean(n->function, candidates));
        }
        for (auto& a : n->args) checkExpression(a);
        return Type{TypeKind::Unknown};
    }

    if (auto* n = std::get_if<ast::MethodCallExpr>(&expr.node)) {
        auto localType = lookupVar(n->object);
        if (localType && localType->kind == TypeKind::Struct) {
            auto& fields = structs_[localType->structName];
            auto fieldIt = std::find_if(fields.begin(), fields.end(),
                                         [&](auto& f) { return f.first == n->member; });
            if (fieldIt != fields.end() && fieldIt->second.kind == TypeKind::Function) {
                n->kind = ast::MethodCallKind::StructField;
                n->resolvedStructName = localType->structName;
                auto& fnType = fieldIt->second;
                if (n->args.size() != fnType.paramTypes.size()) {
                    addError("field '" + n->member + "' expects " + std::to_string(fnType.paramTypes.size()) +
                              " arguments, got " + std::to_string(n->args.size()));
                }
                for (size_t i = 0; i < n->args.size() && i < fnType.paramTypes.size(); i++) {
                    Type argType = checkExpression(n->args[i]);
                    if (!argType.canAssignTo(fnType.paramTypes[i])) {
                        addError("argument " + std::to_string(i) + " of '" + n->member + "': wrong type");
                    }
                }
                return *fnType.returnType;
            }
        }

        std::string key;
        bool isMethod = localType && localType->kind == TypeKind::Struct;
        if (isMethod) {
            n->kind = ast::MethodCallKind::Method;
            n->resolvedStructName = localType->structName;
            key = localType->structName + "." + n->member;
        } else {
            key = n->object + "." + n->member;
        }

        auto it = functions_.find(key);
        if (it == functions_.end()) {
            bool objectKnown = isMethod ||
                std::find(moduleNames_.begin(), moduleNames_.end(), n->object) != moduleNames_.end();
            std::string suggestion;
            if (objectKnown) {
                std::string prefix = (isMethod ? localType->structName : n->object) + ".";
                std::vector<std::string> members;
                for (auto& fname : functionNames()) {
                    if (fname.rfind(prefix, 0) == 0) members.push_back(fname.substr(prefix.size()));
                }
                if (isMethod) {
                    auto fields = fieldNames(localType->structName);
                    members.insert(members.end(), fields.begin(), fields.end());
                }
                suggestion = didYouMean(n->member, members);
            } else {
                auto candidates = moduleNames_;
                auto varNames = visibleVarNames();
                candidates.insert(candidates.end(), varNames.begin(), varNames.end());
                suggestion = didYouMean(n->object, candidates);
            }
            addError("function '" + key + "' not declared" + suggestion);
            for (auto& a : n->args) checkExpression(a);
            return Type{TypeKind::Unknown};
        }
        auto& sig = it->second;
        if (n->args.size() != sig.params.size()) {
            addError("function '" + key + "' expects " + std::to_string(sig.params.size()) +
                      " arguments, got " + std::to_string(n->args.size()));
        }
        for (size_t i = 0; i < n->args.size() && i < sig.params.size(); i++) {
            Type argType = checkExpression(n->args[i]);
            if (!argType.canAssignTo(sig.params[i].second)) {
                addError("argument " + std::to_string(i) + " of '" + key + "': expected " +
                          sig.params[i].second.toString() + ", got " + argType.toString());
            }
        }
        return sig.returnType;
    }

    if (auto* n = std::get_if<ast::ArrayAccessExpr>(&expr.node)) {
        Type indexType = checkExpression(*n->index);
        if (!indexType.isNumeric()) addError("array index must be numeric, got " + indexType.toString());
        auto varType = lookupVar(n->name);
        if (!varType) {
            addError("variable '" + n->name + "' not declared" + didYouMean(n->name, visibleVarNames()));
            return Type{TypeKind::Unknown};
        }
        if (varType->kind != TypeKind::Array) {
            addError("cannot index into non-array type " + varType->toString());
            return Type{TypeKind::Unknown};
        }
        return *varType->elementType;
    }

    if (auto* n = std::get_if<ast::StringIndexExpr>(&expr.node)) {
        checkExpression(*n->str);
        Type indexType = checkExpression(*n->index);
        if (!indexType.isNumeric()) addError("string index must be numeric, got " + indexType.toString());
        return Type{TypeKind::U8};
    }

    if (auto* n = std::get_if<ast::AddressOfExpr>(&expr.node)) {
        Type inner = checkExpression(*n->operand);
        Type t; t.kind = TypeKind::Ptr; t.pointee = std::make_shared<Type>(inner);
        return t;
    }

    if (auto* n = std::get_if<ast::DerefExpr>(&expr.node)) {
        Type operand = checkExpression(*n->operand);
        if (operand.kind == TypeKind::Ptr) return *operand.pointee;
        addError("cannot dereference non-pointer type " + operand.toString());
        return Type{TypeKind::Unknown};
    }

    if (auto* n = std::get_if<ast::EvalExpr>(&expr.node)) {
        checkExpression(*n->instruction);
        return Type{TypeKind::Unknown};
    }

    if (auto* n = std::get_if<ast::FieldAccessExpr>(&expr.node)) {
        Type objType = checkExpression(*n->object);
        if (objType.kind != TypeKind::Struct) {
            addError("field access requires a struct type, got " + objType.toString());
            return Type{TypeKind::Unknown};
        }
        auto& fields = structs_[objType.structName];
        auto it = std::find_if(fields.begin(), fields.end(), [&](auto& f) { return f.first == n->field; });
        if (it == fields.end()) {
            addError("struct '" + objType.structName + "' has no field '" + n->field + "'" +
                      didYouMean(n->field, fieldNames(objType.structName)));
            return Type{TypeKind::Unknown};
        }
        return it->second;
    }

    if (auto* n = std::get_if<ast::FunctionLiteralExpr>(&expr.node)) {
        return checkFunctionLiteral(*n);
    }

    if (auto* n = std::get_if<ast::StructLiteralExpr>(&expr.node)) {
        if (!structs_.count(n->structName)) {
            addError("struct '" + n->structName + "' not declared" + didYouMean(n->structName, structNames()));
            for (auto& [fname, fexpr] : n->fields) { (void)fname; checkExpression(fexpr); }
            return Type{TypeKind::Unknown};
        }
        auto& fields = structs_[n->structName];
        for (auto& [fieldName, fieldExpr] : n->fields) {
            Type valueType = checkExpression(fieldExpr);
            auto it = std::find_if(fields.begin(), fields.end(), [&](auto& f) { return f.first == fieldName; });
            if (it == fields.end()) {
                addError("struct '" + n->structName + "' has no field '" + fieldName + "'" +
                          didYouMean(fieldName, fieldNames(n->structName)));
            } else if (!valueType.canAssignTo(it->second)) {
                addError("type mismatch for field '" + fieldName + "'");
            }
        }
        Type t; t.kind = TypeKind::Struct; t.structName = n->structName;
        return t;
    }

    return Type{TypeKind::Unknown};
}

void TypeChecker::printErrors() const {
    for (auto& e : errors_) std::fprintf(stderr, "type error in %s: %s\n", e.location.c_str(), e.message.c_str());
}

} // namespace agn::parser
