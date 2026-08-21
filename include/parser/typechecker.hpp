// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#pragma once

#include "ast/ast.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agn::parser {

enum class TypeKind { I64, I32, I8, U64, U32, U8, F64, Bool, String, Ptr, Array, Function, Struct, Void, Unknown };

struct Type {
    Type() = default;
    Type(TypeKind k) : kind(k) {}

    TypeKind kind = TypeKind::Unknown;
    std::shared_ptr<Type> pointee;
    std::shared_ptr<Type> elementType;
    size_t arraySize = 0;
    std::vector<Type> paramTypes;
    std::shared_ptr<Type> returnType;
    std::string structName;

    bool isNumeric() const;
    bool isFloat() const { return kind == TypeKind::F64; }
    bool canAssignTo(const Type& other) const;
    std::string toString() const;
    bool operator==(const Type& other) const;
};

struct TypeError {
    std::string message;
    std::string location;
    size_t line = 0;
    size_t column = 0;
};

struct FunctionSignature {
    std::optional<std::pair<std::string, Type>> receiver;
    std::vector<std::pair<std::string, Type>> params;
    Type returnType;
};

class TypeChecker {
public:
    TypeChecker(std::string targetOs = "linux", std::string targetArch = "x86_64",
                std::string memMode = "arc");

    bool checkProgram(ast::Program& program);
    const std::vector<TypeError>& errors() const { return errors_; }
    void printErrors() const;

    Type resolveTypeString(const std::string& text) { return resolveType(text); }
    const std::unordered_map<std::string, std::vector<std::pair<std::string, Type>>>& structs() const { return structs_; }
    const std::unordered_map<std::string, FunctionSignature>& functions() const { return functions_; }

private:
    struct ScopeFrame {
        std::unordered_map<std::string, Type> vars;
        bool isClosure = false;
        std::vector<std::string>* captureSet = nullptr;
    };

    Type resolveType(const std::string& text);
    Type parseTypeStr(const std::string& s, size_t& pos);
    Type namedType(const std::string& name);

    void registerStruct(const ast::StructDecl& decl);
    void registerFunctionSignature(const std::string& key, const ast::Function& func);

    void checkFunctionBody(ast::Function& func, const std::string& key, const std::string& modulePrefix = "");
    void checkStatement(ast::Statement& stmt);
    void checkComptimeBody(std::vector<ast::Statement>& body);
    Type checkExpression(ast::Expression& expr);
    Type checkFunctionLiteral(ast::FunctionLiteralExpr& lit);

    std::optional<Type> lookupVar(const std::string& name);
    void declareVar(const std::string& name, const Type& type);

    std::optional<bool> evalComptimeCondition(const ast::Expression& cond);
    std::optional<std::string> evalComptimeConstant(const ast::Expression& expr);

    void addError(const std::string& message);
    std::string didYouMean(const std::string& name, const std::vector<std::string>& candidates) const;
    std::vector<std::string> visibleVarNames() const;
    std::vector<std::string> functionNames() const;
    std::vector<std::string> structNames() const;
    std::vector<std::string> fieldNames(const std::string& structName) const;

    std::unordered_map<std::string, std::vector<std::pair<std::string, Type>>> structs_;
    std::unordered_map<std::string, FunctionSignature> functions_;
    std::vector<std::string> moduleNames_;
    std::vector<ScopeFrame> scopeStack_;
    std::vector<Type> returnTypeStack_;
    int loopDepth_ = 0;
    std::vector<TypeError> errors_;
    std::string currentFunction_;
    std::string currentModulePrefix_;
    size_t currentLine_ = 0;
    size_t currentColumn_ = 0;

    std::string targetOs_;
    std::string targetArch_;
    std::string memMode_;
};

} // namespace agn::parser
