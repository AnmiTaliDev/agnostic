// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#pragma once

#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace agn::ast {

enum class BinaryOp {
    Add, Sub, Mul, Div, Mod,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    And, Or, Concat,
    BitAnd, BitOr, BitXor, Shl, Shr,
};

enum class UnaryOp { Neg, Not };

enum class FormatType { Decimal, Hex, HexUpper, String, Auto };

struct FormatSpec {
    std::optional<size_t> width;
    std::optional<size_t> precision;
    FormatType formatType = FormatType::Auto;
    char padding = ' ';
};

struct AsmPart {
    enum class Kind { Literal, Variable } kind;
    std::string text;
};

struct Parameter {
    std::string name;
    std::string type;
};

struct Expression;
struct Statement;

struct TemplateLiteralPart { std::string text; };
struct TemplateExprPart {
    std::unique_ptr<Expression> expr;
    std::optional<FormatSpec> format;
};
using TemplateStringPart = std::variant<TemplateLiteralPart, TemplateExprPart>;

struct NumberExpr { int64_t value; };
struct FloatExpr { double value; };
struct StringExpr { std::string value; };
struct TemplateStringExpr { std::vector<TemplateStringPart> parts; };
struct IdentifierExpr { std::string name; };

struct BinaryExpr {
    BinaryOp op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
};

struct UnaryExpr {
    UnaryOp op;
    std::unique_ptr<Expression> operand;
};

struct CallExpr {
    std::string function;
    std::vector<Expression> args;
};

enum class MethodCallKind { ModuleFunction, Method, StructField };

struct MethodCallExpr {
    std::string object;
    std::string member;
    std::vector<Expression> args;

    MethodCallKind kind = MethodCallKind::ModuleFunction;
    std::string resolvedStructName;
};

struct ArrayAccessExpr {
    std::string name;
    std::unique_ptr<Expression> index;
};

struct StringIndexExpr {
    std::unique_ptr<Expression> str;
    std::unique_ptr<Expression> index;
};

struct AddressOfExpr { std::unique_ptr<Expression> operand; };
struct DerefExpr { std::unique_ptr<Expression> operand; };
struct EvalExpr { std::unique_ptr<Expression> instruction; };

struct FieldAccessExpr {
    std::unique_ptr<Expression> object;
    std::string field;
};

struct FunctionLiteralExpr {
    std::vector<Parameter> params;
    std::string returnType;
    std::vector<Statement> body;
    std::vector<std::string> capturedVars;
};

struct StructLiteralExpr {
    std::string structName;
    std::vector<std::pair<std::string, Expression>> fields;
};

struct Expression {
    std::variant<
        NumberExpr, FloatExpr, StringExpr, TemplateStringExpr, IdentifierExpr,
        BinaryExpr, UnaryExpr, CallExpr, MethodCallExpr, ArrayAccessExpr,
        StringIndexExpr, AddressOfExpr, DerefExpr, EvalExpr, FieldAccessExpr,
        FunctionLiteralExpr, StructLiteralExpr> node;
};

struct VarDeclStmt {
    std::string name;
    std::string varType;
    std::optional<Expression> value;
};

struct ArrayDeclStmt {
    std::string name;
    std::string elementType;
    size_t size;
};

struct AssignmentStmt { std::string name; Expression value; };

struct ArrayAssignmentStmt {
    std::string name;
    Expression index;
    Expression value;
};

struct PointerAssignmentStmt { Expression target; Expression value; };

struct FieldAssignmentStmt {
    Expression object;
    std::string field;
    Expression value;
};

struct IfStmt {
    Expression condition;
    std::vector<Statement> thenBody;
    std::optional<std::vector<Statement>> elseBody;
};

struct ForStmt {
    std::optional<Expression> condition;
    std::vector<Statement> body;
};

struct ReturnStmt { std::optional<Expression> value; };
struct ExpressionStmt { Expression expr; };
struct InlineAsmStmt { std::vector<AsmPart> parts; };

struct ComptimeStmt { std::vector<Statement> body; };

struct BreakStmt {};
struct ContinueStmt {};

struct Statement {
    std::variant<
        VarDeclStmt, ArrayDeclStmt, AssignmentStmt, ArrayAssignmentStmt,
        PointerAssignmentStmt, FieldAssignmentStmt, IfStmt, ForStmt,
        ReturnStmt, ExpressionStmt, InlineAsmStmt, ComptimeStmt,
        BreakStmt, ContinueStmt> node;
    size_t line = 0;
    size_t column = 0;
};

struct Function {
    std::string name;
    std::optional<Parameter> receiver;
    std::vector<Parameter> params;
    std::string returnType;
    std::vector<Statement> body;
    bool isExported = false;
};

struct StructDecl {
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<Parameter> fields;
};

struct Import {
    std::string path;
    std::optional<std::string> alias;
};

struct Module {
    std::string name;
    std::vector<Function> functions;
};

struct Program {
    std::string package;
    std::vector<Import> imports;
    std::vector<Function> functions;
    std::vector<StructDecl> structs;
    std::unordered_map<std::string, Module> modules;
};

// TODO: generics
// TODO: pattern matching

} // namespace agn::ast
