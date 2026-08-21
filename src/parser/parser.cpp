// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#include "parser/parser.hpp"
#include "lexer/lexer.hpp"
#include "misc/diagnostic.hpp"

#include <cctype>
#include <cstdlib>

namespace agn::parser {

using lexer::Token;
using lexer::TokenKind;
using agn::misc::CompileError;
using agn::misc::ErrorKind;

namespace {

std::unique_ptr<ast::Expression> box(ast::Expression e) {
    return std::make_unique<ast::Expression>(std::move(e));
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // namespace

Parser::Parser(std::vector<Token> tokens, std::string file, std::string source)
    : tokens_(std::move(tokens)), file_(std::move(file)), source_(std::move(source)) {}

const Token& Parser::current() const { return peek(0); }

const Token& Parser::peek(size_t offset) const {
    static const Token eof{TokenKind::Eof, "", 0};
    size_t idx = position_ + offset;
    return idx < tokens_.size() ? tokens_[idx] : eof;
}

void Parser::advance() { position_++; }

void Parser::skipNewlines() {
    while (current().kind == TokenKind::Newline) advance();
}

void Parser::expect(TokenKind kind) {
    if (current().kind != kind) {
        error(std::string("expected ") + lexer::tokenKindName(kind) +
              ", found " + lexer::tokenKindName(current().kind));
    }
    advance();
}

void Parser::error(const std::string& message) const {
    CompileError err(ErrorKind::Parser, message, file_, current().line, current().column);
    err.withSourceLine(agn::misc::extractSourceLine(source_, current().line));
    err.display();
    std::exit(1);
}

ast::Program Parser::parse() {
    skipNewlines();
    expect(TokenKind::Package);
    if (current().kind != TokenKind::Identifier) error("expected package name");
    std::string package = current().text;
    advance();
    skipNewlines();

    std::vector<ast::Import> imports;
    while (current().kind == TokenKind::Import) {
        advance();
        if (current().kind != TokenKind::String) error("expected import path string");
        imports.push_back(ast::Import{current().text, std::nullopt});
        advance();
        skipNewlines();
    }

    ast::Program program;
    program.package = package;
    program.imports = imports;

    while (current().kind != TokenKind::Eof) {
        skipNewlines();
        if (current().kind == TokenKind::Eof) break;
        if (current().kind == TokenKind::Struct) {
            program.structs.push_back(parseStructDecl());
        } else {
            program.functions.push_back(parseFunction());
        }
    }

    return program;
}

std::optional<std::string> Parser::tryParseType() {
    if (current().kind == TokenKind::Func) return parseFunctionTypeString();
    if (current().kind == TokenKind::Identifier) {
        std::string t = current().text;
        advance();
        if (current().kind == TokenKind::Less) t += parseGenericArgsString();
        return t;
    }
    return std::nullopt;
}

std::string Parser::parseGenericArgsString() {
    expect(TokenKind::Less);
    std::string out = "<";
    bool first = true;
    while (current().kind != TokenKind::Greater && current().kind != TokenKind::RShift) {
        if (!first) out += ",";
        first = false;
        auto t = tryParseType();
        if (!t) error("expected type argument");
        out += *t;
        if (current().kind == TokenKind::Comma) advance();
    }
    consumeClosingAngle();
    out += ">";
    return out;
}

bool Parser::looksLikeGenericArgsHead() const {
    if (current().kind != TokenKind::Less) return false;
    size_t offset = 1;
    int depth = 1;
    while (depth > 0) {
        if (offset > 64) return false;
        TokenKind k = peek(offset).kind;
        if (k == TokenKind::Identifier || k == TokenKind::Comma) {
            // part of a type-argument name or separator
        } else if (k == TokenKind::Less) {
            depth++;
        } else if (k == TokenKind::Greater) {
            depth--;
        } else if (k == TokenKind::RShift) {
            depth -= 2;
        } else {
            return false;
        }
        offset++;
    }
    if (depth != 0) return false;
    return peek(offset).kind == TokenKind::LeftBrace;
}

void Parser::consumeClosingAngle() {
    if (current().kind == TokenKind::Greater) {
        advance();
        return;
    }
    if (current().kind == TokenKind::RShift) {
        tokens_[position_].kind = TokenKind::Greater;
        return;
    }
    error("expected '>' to close generic type argument list");
}

std::string Parser::parseFunctionTypeString() {
    expect(TokenKind::Func);
    expect(TokenKind::LeftParen);
    std::vector<std::string> params;
    while (current().kind != TokenKind::RightParen) {
        auto t = tryParseType();
        if (!t) error("expected parameter type in function type");
        params.push_back(*t);
        if (current().kind == TokenKind::Comma) advance();
    }
    expect(TokenKind::RightParen);

    std::string ret = "void";
    if (current().kind == TokenKind::Arrow) {
        advance();
        auto t = tryParseType();
        if (!t) error("expected return type after '->'");
        ret = *t;
    } else if (auto t = tryParseType()) {
        ret = *t;
    }

    std::string out = "func(";
    for (size_t i = 0; i < params.size(); i++) {
        if (i) out += ",";
        out += params[i];
    }
    out += ")->" + ret;
    return out;
}

std::vector<ast::Parameter> Parser::parseParamList() {
    std::vector<ast::Parameter> params;
    while (current().kind != TokenKind::RightParen) {
        if (current().kind != TokenKind::Identifier) error("expected parameter name");
        std::string name = current().text;
        advance();
        if (current().kind == TokenKind::Colon) advance();
        auto type = tryParseType();
        if (!type) error("expected parameter type");
        params.push_back(ast::Parameter{name, *type});
        if (current().kind == TokenKind::Comma) advance();
    }
    return params;
}

std::string Parser::parseOptionalReturnType() {
    if (current().kind == TokenKind::Arrow) {
        advance();
        auto t = tryParseType();
        return t.value_or("");
    }
    auto t = tryParseType();
    return t.value_or("");
}

std::vector<ast::Statement> Parser::parseBlock() {
    skipNewlines();
    std::vector<ast::Statement> stmts;
    while (current().kind != TokenKind::RightBrace) {
        stmts.push_back(parseStatement());
        skipNewlines();
    }
    expect(TokenKind::RightBrace);
    skipNewlines();
    return stmts;
}

ast::Function Parser::parseFunction() {
    bool isPub = false;
    if (current().kind == TokenKind::Identifier && current().text == "pub") {
        advance();
        isPub = true;
    }
    expect(TokenKind::Func);

    std::optional<ast::Parameter> receiver;
    if (current().kind == TokenKind::LeftParen) {
        advance();
        if (current().kind != TokenKind::Identifier) error("expected receiver name");
        std::string recvName = current().text;
        advance();
        auto recvType = tryParseType();
        if (!recvType) error("expected receiver type");
        expect(TokenKind::RightParen);
        receiver = ast::Parameter{recvName, *recvType};
    }

    if (current().kind != TokenKind::Identifier) error("expected function name");
    std::string name = current().text;
    advance();

    expect(TokenKind::LeftParen);
    auto params = parseParamList();
    expect(TokenKind::RightParen);

    std::string returnType = parseOptionalReturnType();

    skipNewlines();
    expect(TokenKind::LeftBrace);
    auto body = parseBlock();

    bool isExported = isPub || (!name.empty() && std::isupper(static_cast<unsigned char>(name[0])));

    ast::Function func;
    func.name = name;
    func.receiver = receiver;
    func.params = params;
    func.returnType = returnType;
    func.body = std::move(body);
    func.isExported = isExported;
    return func;
}

ast::StructDecl Parser::parseStructDecl() {
    expect(TokenKind::Struct);
    if (current().kind != TokenKind::Identifier) error("expected struct name");
    std::string name = current().text;
    advance();

    std::vector<std::string> typeParams;
    if (current().kind == TokenKind::Less) {
        advance();
        while (current().kind != TokenKind::Greater) {
            if (current().kind != TokenKind::Identifier) error("expected type parameter name");
            typeParams.push_back(current().text);
            advance();
            if (current().kind == TokenKind::Comma) advance();
        }
        expect(TokenKind::Greater);
    }

    skipNewlines();
    expect(TokenKind::LeftBrace);
    skipNewlines();

    std::vector<ast::Parameter> fields;
    while (current().kind != TokenKind::RightBrace) {
        if (current().kind != TokenKind::Identifier) error("expected field name");
        std::string fieldName = current().text;
        advance();
        if (current().kind == TokenKind::Colon) advance();
        auto type = tryParseType();
        if (!type) error("expected field type");
        fields.push_back(ast::Parameter{fieldName, *type});
        if (current().kind == TokenKind::Comma) advance();
        skipNewlines();
    }
    expect(TokenKind::RightBrace);
    skipNewlines();

    return ast::StructDecl{name, typeParams, fields};
}

ast::Statement Parser::parseStatement() {
    size_t line = current().line;
    size_t column = current().column;
    ast::Statement stmt = parseStatementBody();
    stmt.line = line;
    stmt.column = column;
    return stmt;
}

ast::Statement Parser::parseStatementBody() {
    switch (current().kind) {
        case TokenKind::Var: return parseVarDecl();
        case TokenKind::If: return parseIf();
        case TokenKind::For: return parseFor();
        case TokenKind::Return: return parseReturn();
        case TokenKind::Asm: return parseAsm();
        case TokenKind::Comptime: return parseComptime();
        case TokenKind::Break: advance(); return ast::Statement{ast::BreakStmt{}};
        case TokenKind::Continue: advance(); return ast::Statement{ast::ContinueStmt{}};
        case TokenKind::Star: {
            size_t checkPos = position_ + 1;
            while (checkPos < tokens_.size()) {
                const Token& tok = tokens_[checkPos];
                if (tok.kind == TokenKind::Assign) return parsePointerAssignment();
                if (tok.kind == TokenKind::Identifier || tok.kind == TokenKind::LeftParen ||
                    tok.kind == TokenKind::RightParen) {
                    checkPos++;
                } else {
                    break;
                }
            }
            return ast::Statement{ast::ExpressionStmt{parseExpression()}};
        }
        case TokenKind::Identifier: {
            if (peek(1).kind == TokenKind::Assign || peek(1).kind == TokenKind::LBracket) {
                return parseAssignmentLike();
            }
            if (peek(1).kind == TokenKind::Dot && peek(2).kind == TokenKind::Identifier &&
                peek(3).kind == TokenKind::Assign) {
                return parseAssignmentLike();
            }
            return ast::Statement{ast::ExpressionStmt{parseExpression()}};
        }
        default:
            return ast::Statement{ast::ExpressionStmt{parseExpression()}};
    }
}

ast::Statement Parser::parseVarDecl() {
    expect(TokenKind::Var);
    if (current().kind != TokenKind::Identifier) error("expected variable name");
    std::string name = current().text;
    advance();

    if (current().kind == TokenKind::Colon) advance();

    if (current().kind == TokenKind::LBracket) {
        advance();
        if (current().kind != TokenKind::Number) error("expected array size");
        size_t size = static_cast<size_t>(current().number);
        advance();
        expect(TokenKind::RBracket);
        if (current().kind != TokenKind::Identifier) error("expected array element type");
        std::string elementType = current().text;
        advance();
        return ast::Statement{ast::ArrayDeclStmt{name, elementType, size}};
    }

    std::string varType = tryParseType().value_or("");

    std::optional<ast::Expression> value;
    if (current().kind == TokenKind::Assign) {
        advance();
        value = parseExpression();
    }

    return ast::Statement{ast::VarDeclStmt{name, varType, std::move(value)}};
}

ast::Statement Parser::parseAssignmentLike() {
    if (current().kind != TokenKind::Identifier) error("expected variable name");
    std::string name = current().text;
    advance();

    if (current().kind == TokenKind::Dot) {
        advance();
        if (current().kind != TokenKind::Identifier) error("expected field name");
        std::string field = current().text;
        advance();
        expect(TokenKind::Assign);
        ast::Expression value = parseExpression();
        return ast::Statement{ast::FieldAssignmentStmt{
            ast::Expression{ast::IdentifierExpr{name}}, field, std::move(value)}};
    }

    if (current().kind == TokenKind::LBracket) {
        advance();
        ast::Expression index = parseExpression();
        expect(TokenKind::RBracket);
        expect(TokenKind::Assign);
        ast::Expression value = parseExpression();
        return ast::Statement{ast::ArrayAssignmentStmt{name, std::move(index), std::move(value)}};
    }

    expect(TokenKind::Assign);
    ast::Expression value = parseExpression();
    return ast::Statement{ast::AssignmentStmt{name, std::move(value)}};
}

ast::Statement Parser::parsePointerAssignment() {
    expect(TokenKind::Star);
    ast::Expression target = parsePrimary();
    expect(TokenKind::Assign);
    ast::Expression value = parseExpression();
    return ast::Statement{ast::PointerAssignmentStmt{std::move(target), std::move(value)}};
}

ast::Statement Parser::parseIf() {
    expect(TokenKind::If);
    bool saved = noStructLiteral_;
    noStructLiteral_ = true;
    ast::Expression condition = parseExpression();
    noStructLiteral_ = saved;

    skipNewlines();
    expect(TokenKind::LeftBrace);
    auto thenBody = parseBlock();

    std::optional<std::vector<ast::Statement>> elseBody;
    if (current().kind == TokenKind::Else) {
        advance();
        skipNewlines();
        expect(TokenKind::LeftBrace);
        elseBody = parseBlock();
    }

    return ast::Statement{ast::IfStmt{std::move(condition), std::move(thenBody), std::move(elseBody)}};
}

ast::Statement Parser::parseFor() {
    expect(TokenKind::For);

    std::optional<ast::Expression> condition;
    if (current().kind != TokenKind::LeftBrace) {
        bool saved = noStructLiteral_;
        noStructLiteral_ = true;
        condition = parseExpression();
        noStructLiteral_ = saved;
    }

    skipNewlines();
    expect(TokenKind::LeftBrace);
    auto body = parseBlock();

    return ast::Statement{ast::ForStmt{std::move(condition), std::move(body)}};
}

ast::Statement Parser::parseReturn() {
    expect(TokenKind::Return);
    std::optional<ast::Expression> value;
    if (current().kind != TokenKind::Newline && current().kind != TokenKind::RightBrace) {
        value = parseExpression();
    }
    return ast::Statement{ast::ReturnStmt{std::move(value)}};
}

ast::Statement Parser::parseComptime() {
    expect(TokenKind::Comptime);
    skipNewlines();
    expect(TokenKind::LeftBrace);
    auto body = parseBlock();
    return ast::Statement{ast::ComptimeStmt{std::move(body)}};
}

ast::Statement Parser::parseAsm() {
    expect(TokenKind::Asm);

    if (current().kind == TokenKind::String) {
        std::string code = current().text;
        advance();
        return ast::Statement{ast::InlineAsmStmt{parseAsmInterpolation(code)}};
    }

    if (current().kind != TokenKind::LeftBrace) {
        error("expected assembly code string or block after 'asm'");
    }
    advance();
    skipNewlines();

    std::vector<ast::AsmPart> parts;
    std::string currentLine;

    while (current().kind != TokenKind::RightBrace) {
        switch (current().kind) {
            case TokenKind::Dollar: {
                std::string lineBeforeVar = trim(currentLine);
                currentLine.clear();
                advance();
                if (current().kind == TokenKind::LeftParen) {
                    advance();
                    if (current().kind == TokenKind::Identifier) {
                        std::string varName = current().text;
                        if (!lineBeforeVar.empty() && lineBeforeVar != "push") {
                            parts.push_back(ast::AsmPart{ast::AsmPart::Kind::Literal, lineBeforeVar});
                        }
                        parts.push_back(ast::AsmPart{ast::AsmPart::Kind::Variable, varName});
                        advance();
                        expect(TokenKind::RightParen);
                    }
                }
                break;
            }
            case TokenKind::Identifier: {
                if (!currentLine.empty()) currentLine += ' ';
                currentLine += current().text;
                advance();
                break;
            }
            case TokenKind::Number: {
                if (!currentLine.empty()) currentLine += ' ';
                int64_t nVal = current().number;
                std::string numStr;
                bool consumedHex = false;
                if (nVal == 0 && peek(1).kind == TokenKind::Identifier) {
                    const std::string& id = peek(1).text;
                    if (!id.empty() && (id[0] == 'x' || id[0] == 'X')) {
                        numStr = "0" + id;
                        advance();
                        advance();
                        consumedHex = true;
                    }
                }
                if (!consumedHex) {
                    numStr = std::to_string(nVal);
                    advance();
                }
                currentLine += numStr;
                break;
            }
            case TokenKind::Semicolon: {
                while (current().kind != TokenKind::Newline && current().kind != TokenKind::RightBrace &&
                       current().kind != TokenKind::Eof) {
                    advance();
                }
                break;
            }
            case TokenKind::Newline: {
                if (!currentLine.empty()) {
                    parts.push_back(ast::AsmPart{ast::AsmPart::Kind::Literal, currentLine});
                    currentLine.clear();
                }
                parts.push_back(ast::AsmPart{ast::AsmPart::Kind::Literal, "\n"});
                advance();
                break;
            }
            default:
                advance();
                break;
        }
    }

    if (!currentLine.empty()) {
        parts.push_back(ast::AsmPart{ast::AsmPart::Kind::Literal, currentLine});
    }
    expect(TokenKind::RightBrace);
    return ast::Statement{ast::InlineAsmStmt{parts}};
}

std::vector<ast::AsmPart> Parser::parseAsmInterpolation(const std::string& code) {
    std::vector<ast::AsmPart> parts;
    std::string literal;
    for (size_t i = 0; i < code.size(); i++) {
        if (code[i] == '$' && i + 1 < code.size() && code[i + 1] == '(') {
            i++;
            if (!literal.empty()) {
                parts.push_back(ast::AsmPart{ast::AsmPart::Kind::Literal, literal});
                literal.clear();
            }
            std::string varName;
            i++;
            while (i < code.size() && code[i] != ')') {
                varName.push_back(code[i]);
                i++;
            }
            parts.push_back(ast::AsmPart{ast::AsmPart::Kind::Variable, trim(varName)});
        } else {
            literal.push_back(code[i]);
        }
    }
    if (!literal.empty()) parts.push_back(ast::AsmPart{ast::AsmPart::Kind::Literal, literal});
    return parts;
}

ast::Expression Parser::parseExpression() { return parseOr(); }

ast::Expression Parser::parseOr() {
    ast::Expression left = parseAnd();
    while (current().kind == TokenKind::Or) {
        advance();
        ast::Expression right = parseAnd();
        left = ast::Expression{ast::BinaryExpr{ast::BinaryOp::Or, box(std::move(left)), box(std::move(right))}};
    }
    return left;
}

ast::Expression Parser::parseAnd() {
    ast::Expression left = parseBitOr();
    while (current().kind == TokenKind::And) {
        advance();
        ast::Expression right = parseEquality();
        left = ast::Expression{ast::BinaryExpr{ast::BinaryOp::And, box(std::move(left)), box(std::move(right))}};
    }
    return left;
}

ast::Expression Parser::parseBitOr() {
    ast::Expression left = parseBitXor();
    while (current().kind == TokenKind::Pipe) {
        advance();
        ast::Expression right = parseBitXor();
        left = ast::Expression{ast::BinaryExpr{ast::BinaryOp::BitOr, box(std::move(left)), box(std::move(right))}};
    }
    return left;
}

ast::Expression Parser::parseBitXor() {
    ast::Expression left = parseBitAnd();
    while (current().kind == TokenKind::Caret) {
        advance();
        ast::Expression right = parseBitAnd();
        left = ast::Expression{ast::BinaryExpr{ast::BinaryOp::BitXor, box(std::move(left)), box(std::move(right))}};
    }
    return left;
}

ast::Expression Parser::parseBitAnd() {
    ast::Expression left = parseEquality();
    while (current().kind == TokenKind::Ampersand) {
        advance();
        ast::Expression right = parseEquality();
        left = ast::Expression{ast::BinaryExpr{ast::BinaryOp::BitAnd, box(std::move(left)), box(std::move(right))}};
    }
    return left;
}

ast::Expression Parser::parseEquality() {
    ast::Expression left = parseComparison();
    for (;;) {
        ast::BinaryOp op;
        if (current().kind == TokenKind::Equal) op = ast::BinaryOp::Equal;
        else if (current().kind == TokenKind::NotEqual) op = ast::BinaryOp::NotEqual;
        else break;
        advance();
        ast::Expression right = parseComparison();
        left = ast::Expression{ast::BinaryExpr{op, box(std::move(left)), box(std::move(right))}};
    }
    return left;
}

ast::Expression Parser::parseComparison() {
    ast::Expression left = parseShift();
    for (;;) {
        ast::BinaryOp op;
        switch (current().kind) {
            case TokenKind::Less: op = ast::BinaryOp::Less; break;
            case TokenKind::LessEqual: op = ast::BinaryOp::LessEqual; break;
            case TokenKind::Greater: op = ast::BinaryOp::Greater; break;
            case TokenKind::GreaterEqual: op = ast::BinaryOp::GreaterEqual; break;
            default: return left;
        }
        advance();
        ast::Expression right = parseAdditive();
        left = ast::Expression{ast::BinaryExpr{op, box(std::move(left)), box(std::move(right))}};
    }
}

ast::Expression Parser::parseShift() {
    ast::Expression left = parseAdditive();
    for (;;) {
        ast::BinaryOp op;
        if (current().kind == TokenKind::LShift) op = ast::BinaryOp::Shl;
        else if (current().kind == TokenKind::RShift) op = ast::BinaryOp::Shr;
        else break;
        advance();
        ast::Expression right = parseAdditive();
        left = ast::Expression{ast::BinaryExpr{op, box(std::move(left)), box(std::move(right))}};
    }
    return left;
}

ast::Expression Parser::parseAdditive() {
    ast::Expression left = parseMultiplicative();
    for (;;) {
        ast::BinaryOp op;
        if (current().kind == TokenKind::Plus) op = ast::BinaryOp::Add;
        else if (current().kind == TokenKind::Minus) op = ast::BinaryOp::Sub;
        else if (current().kind == TokenKind::DoublePlus) op = ast::BinaryOp::Concat;
        else break;
        advance();
        ast::Expression right = parseMultiplicative();
        left = ast::Expression{ast::BinaryExpr{op, box(std::move(left)), box(std::move(right))}};
    }
    return left;
}

ast::Expression Parser::parseMultiplicative() {
    ast::Expression left = parseUnary();
    for (;;) {
        ast::BinaryOp op;
        if (current().kind == TokenKind::Star) op = ast::BinaryOp::Mul;
        else if (current().kind == TokenKind::Slash) op = ast::BinaryOp::Div;
        else if (current().kind == TokenKind::Percent) op = ast::BinaryOp::Mod;
        else break;
        advance();
        ast::Expression right = parseUnary();
        left = ast::Expression{ast::BinaryExpr{op, box(std::move(left)), box(std::move(right))}};
    }
    return left;
}

ast::Expression Parser::parseUnary() {
    switch (current().kind) {
        case TokenKind::Minus: {
            advance();
            ast::Expression operand = parseUnary();
            return ast::Expression{ast::UnaryExpr{ast::UnaryOp::Neg, box(std::move(operand))}};
        }
        case TokenKind::Not: {
            advance();
            ast::Expression operand = parseUnary();
            return ast::Expression{ast::UnaryExpr{ast::UnaryOp::Not, box(std::move(operand))}};
        }
        case TokenKind::Ampersand: {
            advance();
            ast::Expression operand = parseUnary();
            return ast::Expression{ast::AddressOfExpr{box(std::move(operand))}};
        }
        case TokenKind::Star: {
            advance();
            ast::Expression operand = parseUnary();
            return ast::Expression{ast::DerefExpr{box(std::move(operand))}};
        }
        default:
            return parsePrimary();
    }
}

ast::Expression Parser::parseTemplateString(const std::string& s) {
    std::vector<ast::TemplateStringPart> parts;
    std::string literal;

    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '(') {
            i += 2;
            if (!literal.empty()) {
                parts.push_back(ast::TemplateLiteralPart{literal});
                literal.clear();
            }
            std::string exprStr;
            int depth = 1;
            while (i < s.size()) {
                if (s[i] == '(') { depth++; exprStr.push_back(s[i]); }
                else if (s[i] == ')') {
                    depth--;
                    if (depth == 0) { i++; break; }
                    exprStr.push_back(s[i]);
                } else {
                    exprStr.push_back(s[i]);
                }
                i++;
            }

            auto [innerExpr, format] = parseFormatSpec(exprStr);

            lexer::Lexer subLexer(innerExpr, file_);
            auto subTokens = subLexer.tokenize();
            Parser subParser(subTokens, file_, innerExpr);
            ast::Expression expr = subParser.parseExpression();

            parts.push_back(ast::TemplateExprPart{box(std::move(expr)), format});
        } else {
            literal.push_back(s[i]);
            i++;
        }
    }

    if (!literal.empty()) parts.push_back(ast::TemplateLiteralPart{literal});
    return ast::Expression{ast::TemplateStringExpr{std::move(parts)}};
}

std::pair<std::string, std::optional<ast::FormatSpec>> Parser::parseFormatSpec(const std::string& exprStr) {
    size_t colonPos = exprStr.rfind(':');
    if (colonPos == std::string::npos) return {exprStr, std::nullopt};

    std::string exprPart = trim(exprStr.substr(0, colonPos));
    std::string formatPart = trim(exprStr.substr(colonPos + 1));
    if (formatPart.empty()) return {exprStr, std::nullopt};

    ast::FormatSpec spec;
    size_t i = 0;
    if (i < formatPart.size() && formatPart[i] == '0') {
        spec.padding = '0';
        i++;
    }
    std::string widthStr;
    while (i < formatPart.size() && std::isdigit(static_cast<unsigned char>(formatPart[i]))) {
        widthStr.push_back(formatPart[i]);
        i++;
    }
    if (!widthStr.empty()) spec.width = static_cast<size_t>(std::stoul(widthStr));

    if (i < formatPart.size()) {
        switch (formatPart[i]) {
            case 'd': spec.formatType = ast::FormatType::Decimal; break;
            case 'x': spec.formatType = ast::FormatType::Hex; break;
            case 'X': spec.formatType = ast::FormatType::HexUpper; break;
            case 's': spec.formatType = ast::FormatType::String; break;
            default: spec.formatType = ast::FormatType::Auto; break;
        }
    }

    return {exprPart, spec};
}

ast::Expression Parser::parsePrimary() {
    switch (current().kind) {
        case TokenKind::Number: {
            int64_t v = current().number;
            advance();
            return ast::Expression{ast::NumberExpr{v}};
        }
        case TokenKind::String: {
            std::string s = current().text;
            advance();
            if (current().kind == TokenKind::LBracket) {
                advance();
                ast::Expression index = parseExpression();
                expect(TokenKind::RBracket);
                return ast::Expression{ast::StringIndexExpr{
                    box(ast::Expression{ast::StringExpr{s}}), box(std::move(index))}};
            }
            if (s.find("$(") != std::string::npos) return parseTemplateString(s);
            return ast::Expression{ast::StringExpr{s}};
        }
        case TokenKind::Func: {
            advance();
            expect(TokenKind::LeftParen);
            auto params = parseParamList();
            expect(TokenKind::RightParen);
            std::string returnType = parseOptionalReturnType();
            skipNewlines();
            expect(TokenKind::LeftBrace);
            auto body = parseBlock();
            return ast::Expression{ast::FunctionLiteralExpr{params, returnType, std::move(body), {}}};
        }
        case TokenKind::Identifier: {
            std::string name = current().text;
            advance();

            if (current().kind == TokenKind::Less && looksLikeGenericArgsHead()) {
                name += parseGenericArgsString();
            }

            if (current().kind == TokenKind::Dot) {
                advance();
                if (current().kind != TokenKind::Identifier) error("expected member name after '.'");
                std::string member = current().text;
                advance();

                if (current().kind == TokenKind::LeftParen) {
                    advance();
                    std::vector<ast::Expression> args;
                    while (current().kind != TokenKind::RightParen) {
                        args.push_back(parseExpression());
                        if (current().kind == TokenKind::Comma) advance();
                    }
                    expect(TokenKind::RightParen);
                    ast::MethodCallExpr call;
                    call.object = name;
                    call.member = member;
                    call.args = std::move(args);
                    return ast::Expression{std::move(call)};
                }
                return ast::Expression{ast::FieldAccessExpr{
                    box(ast::Expression{ast::IdentifierExpr{name}}), member}};
            }

            if (current().kind == TokenKind::LeftParen) {
                advance();
                std::vector<ast::Expression> args;
                while (current().kind != TokenKind::RightParen) {
                    args.push_back(parseExpression());
                    if (current().kind == TokenKind::Comma) advance();
                }
                expect(TokenKind::RightParen);

                if (name == "eval" && args.size() == 1) {
                    return ast::Expression{ast::EvalExpr{box(std::move(args[0]))}};
                }
                return ast::Expression{ast::CallExpr{name, std::move(args)}};
            }

            if (current().kind == TokenKind::LBracket) {
                advance();
                ast::Expression index = parseExpression();
                expect(TokenKind::RBracket);
                return ast::Expression{ast::ArrayAccessExpr{name, box(std::move(index))}};
            }

            if (current().kind == TokenKind::LeftBrace && !noStructLiteral_) {
                advance();
                skipNewlines();
                std::vector<std::pair<std::string, ast::Expression>> fields;
                while (current().kind != TokenKind::RightBrace) {
                    if (current().kind != TokenKind::Identifier) error("expected field name in struct literal");
                    std::string fieldName = current().text;
                    advance();
                    expect(TokenKind::Colon);
                    ast::Expression fieldValue = parseExpression();
                    fields.emplace_back(fieldName, std::move(fieldValue));
                    if (current().kind == TokenKind::Comma) advance();
                    skipNewlines();
                }
                expect(TokenKind::RightBrace);
                return ast::Expression{ast::StructLiteralExpr{name, std::move(fields)}};
            }

            return ast::Expression{ast::IdentifierExpr{name}};
        }
        case TokenKind::LeftParen: {
            advance();
            ast::Expression expr = parseExpression();
            expect(TokenKind::RightParen);
            return expr;
        }
        default:
            error(std::string("unexpected token: ") + lexer::tokenKindName(current().kind));
    }
}

} // namespace agn::parser
