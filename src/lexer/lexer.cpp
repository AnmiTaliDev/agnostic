// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>

#include "lexer/lexer.hpp"
#include "misc/diagnostic.hpp"

namespace agn::lexer {

using agn::misc::CompileError;
using agn::misc::ErrorKind;

extern "C" {
long LexSkip(const char* source, long pos);
long LexKind(const char* source, long pos);
long LexEnd(const char* source, long pos);
const char* LexText(const char* source, long pos);
long LexNumberValue(const char* source, long pos);
double LexFloatValue(const char* source, long pos);
}

Lexer::Lexer(std::string input, std::string file)
    : input_(std::move(input)), file_(std::move(file)) {
    currentChar_ = input_.empty() ? std::nullopt : std::optional<char>(input_[0]);
}

void Lexer::advance() {}
std::optional<char> Lexer::peek(size_t) const { return std::nullopt; }
void Lexer::skipWhitespace() {}
void Lexer::skipComment() {}
Token Lexer::readNumber() { return Token{}; }
Token Lexer::readIdentifier() { return Token{}; }
Token Lexer::readString() { return Token{}; }

std::vector<Token> Lexer::tokenize() {
    const char* src = input_.c_str();
    std::vector<Token> tokens;

    size_t pos = 0;
    size_t line = 1;
    size_t column = 1;
    auto advanceTo = [&](size_t target) {
        while (pos < target && pos < input_.size()) {
            if (input_[pos] == '\n') {
                line++;
                column = 1;
            } else {
                column++;
            }
            pos++;
        }
    };

    for (;;) {
        advanceTo(static_cast<size_t>(LexSkip(src, static_cast<long>(pos))));

        size_t startLine = line;
        size_t startColumn = column;
        long kind = LexKind(src, static_cast<long>(pos));
        long end = LexEnd(src, static_cast<long>(pos));

        if (kind < 0) {
            char ch = pos < input_.size() ? input_[pos] : '\0';
            CompileError err(ErrorKind::Lexer, std::string("unexpected character: '") + ch + "'", file_, line, column);
            err.withSourceLine(agn::misc::extractSourceLine(input_, line));
            err.display();
            std::exit(1);
        }

        Token t;
        t.kind = static_cast<TokenKind>(kind);
        t.line = startLine;
        t.column = startColumn;
        if (t.kind == TokenKind::Identifier || t.kind == TokenKind::String) {
            t.text = LexText(src, static_cast<long>(pos));
        } else if (t.kind == TokenKind::Number || t.kind == TokenKind::BoolLiteral) {
            t.number = LexNumberValue(src, static_cast<long>(pos));
        } else if (t.kind == TokenKind::Float) {
            t.numberF = LexFloatValue(src, static_cast<long>(pos));
        }
        tokens.push_back(t);

        bool isEof = t.kind == TokenKind::Eof;
        advanceTo(static_cast<size_t>(end));
        if (isEof) break;
    }

    return tokens;
}

const char* tokenKindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::Package: return "package";
        case TokenKind::Import: return "import";
        case TokenKind::Func: return "func";
        case TokenKind::Var: return "var";
        case TokenKind::If: return "if";
        case TokenKind::Else: return "else";
        case TokenKind::For: return "for";
        case TokenKind::Return: return "return";
        case TokenKind::Asm: return "asm";
        case TokenKind::Struct: return "struct";
        case TokenKind::Comptime: return "comptime";
        case TokenKind::Break: return "break";
        case TokenKind::Continue: return "continue";
        case TokenKind::Identifier: return "identifier";
        case TokenKind::Number: return "number";
        case TokenKind::Float: return "float";
        case TokenKind::BoolLiteral: return "bool literal";
        case TokenKind::String: return "string";
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%";
        case TokenKind::Assign: return "=";
        case TokenKind::Equal: return "==";
        case TokenKind::NotEqual: return "!=";
        case TokenKind::Less: return "<";
        case TokenKind::LessEqual: return "<=";
        case TokenKind::Greater: return ">";
        case TokenKind::GreaterEqual: return ">=";
        case TokenKind::And: return "&&";
        case TokenKind::Or: return "||";
        case TokenKind::Not: return "!";
        case TokenKind::Pipe: return "|";
        case TokenKind::Caret: return "^";
        case TokenKind::LShift: return "<<";
        case TokenKind::RShift: return ">>";
        case TokenKind::LeftParen: return "(";
        case TokenKind::RightParen: return ")";
        case TokenKind::LeftBrace: return "{";
        case TokenKind::RightBrace: return "}";
        case TokenKind::LBracket: return "[";
        case TokenKind::RBracket: return "]";
        case TokenKind::Comma: return ",";
        case TokenKind::Semicolon: return ";";
        case TokenKind::Colon: return ":";
        case TokenKind::Dot: return ".";
        case TokenKind::Arrow: return "->";
        case TokenKind::Ampersand: return "&";
        case TokenKind::DoublePlus: return "++";
        case TokenKind::Dollar: return "$";
        case TokenKind::Newline: return "newline";
        case TokenKind::Eof: return "eof";
    }
    return "?";
}

} // namespace agn::lexer
