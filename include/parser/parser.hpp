// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#pragma once

#include "ast/ast.hpp"
#include "lexer/token.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agn::parser {

class Parser {
public:
    Parser(std::vector<lexer::Token> tokens, std::string file, std::string source);

    ast::Program parse();

private:
    const lexer::Token& current() const;
    const lexer::Token& peek(size_t offset) const;
    void advance();
    void skipNewlines();
    void expect(lexer::TokenKind kind);
    [[noreturn]] void error(const std::string& message) const;

    ast::Function parseFunction();
    ast::StructDecl parseStructDecl();
    std::vector<ast::Parameter> parseParamList();
    std::string parseOptionalReturnType();

    ast::Statement parseStatement();
    ast::Statement parseStatementBody();
    ast::Statement parseVarDecl();
    ast::Statement parseAssignmentLike();
    ast::Statement parsePointerAssignment();
    ast::Statement parseIf();
    ast::Statement parseFor();
    ast::Statement parseReturn();
    ast::Statement parseAsm();
    ast::Statement parseComptime();
    std::vector<ast::Statement> parseBlock();
    std::vector<ast::AsmPart> parseAsmInterpolation(const std::string& code);

    ast::Expression parseExpression();
    ast::Expression parseOr();
    ast::Expression parseAnd();
    ast::Expression parseBitOr();
    ast::Expression parseBitXor();
    ast::Expression parseBitAnd();
    ast::Expression parseEquality();
    ast::Expression parseComparison();
    ast::Expression parseShift();
    ast::Expression parseAdditive();
    ast::Expression parseMultiplicative();
    ast::Expression parseUnary();
    ast::Expression parsePrimary();
    ast::Expression parseTemplateString(const std::string& s);
    std::pair<std::string, std::optional<ast::FormatSpec>> parseFormatSpec(const std::string& exprStr);

    std::optional<std::string> tryParseType();
    std::string parseFunctionTypeString();
    std::string parseGenericArgsString();
    void consumeClosingAngle();
    bool looksLikeGenericArgsHead() const;

    std::vector<lexer::Token> tokens_;
    size_t position_ = 0;
    std::string file_;
    std::string source_;
    bool noStructLiteral_ = false;
};

} // namespace agn::parser
