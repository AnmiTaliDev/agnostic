// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#include "lexer/lexer.hpp"
#include "misc/diagnostic.hpp"

#include <cctype>
#include <cstdlib>
#include <limits>
#include <unordered_map>

namespace agn::lexer {

using agn::misc::CompileError;
using agn::misc::ErrorKind;

Lexer::Lexer(std::string input, std::string file)
    : input_(std::move(input)), file_(std::move(file)) {
    currentChar_ = input_.empty() ? std::nullopt : std::optional<char>(input_[0]);
}

void Lexer::advance() {
    if (currentChar_ && *currentChar_ == '\n') {
        line_++;
        column_ = 1;
    } else if (currentChar_) {
        column_++;
    }
    position_++;
    currentChar_ = position_ < input_.size() ? std::optional<char>(input_[position_]) : std::nullopt;
}

std::optional<char> Lexer::peek(size_t offset) const {
    size_t pos = position_ + offset;
    return pos < input_.size() ? std::optional<char>(input_[pos]) : std::nullopt;
}

void Lexer::skipWhitespace() {
    while (currentChar_ && (*currentChar_ == ' ' || *currentChar_ == '\t' || *currentChar_ == '\r')) {
        advance();
    }
}

void Lexer::skipComment() {
    if (currentChar_ == '/' && peek(1) == '/') {
        while (currentChar_ && *currentChar_ != '\n') advance();
    }
}

Token Lexer::readNumber() {
    std::string digits;
    while (currentChar_ && std::isdigit(static_cast<unsigned char>(*currentChar_))) {
        digits.push_back(*currentChar_);
        advance();
    }

    if (currentChar_ == '.' && peek(1) && std::isdigit(static_cast<unsigned char>(*peek(1)))) {
        digits.push_back('.');
        advance();
        while (currentChar_ && std::isdigit(static_cast<unsigned char>(*currentChar_))) {
            digits.push_back(*currentChar_);
            advance();
        }
        Token t{TokenKind::Float, "", 0};
        t.numberF = std::strtod(digits.c_str(), nullptr);
        return t;
    }

    errno = 0;
    char* end = nullptr;
    long long value = std::strtoll(digits.c_str(), &end, 10);
    if (errno == ERANGE) {
        std::fprintf(stderr, "Warning: Number '%s' is too large, using i64::MAX\n", digits.c_str());
        value = std::numeric_limits<int64_t>::max();
    }
    return Token{TokenKind::Number, "", value};
}

Token Lexer::readIdentifier() {
    static const std::unordered_map<std::string, TokenKind> keywords = {
        {"package", TokenKind::Package}, {"import", TokenKind::Import},
        {"use", TokenKind::Import},      {"func", TokenKind::Func},
        {"fn", TokenKind::Func},         {"var", TokenKind::Var},
        {"let", TokenKind::Var},         {"if", TokenKind::If},
        {"else", TokenKind::Else},       {"for", TokenKind::For},
        {"while", TokenKind::For},       {"loop", TokenKind::For},
        {"return", TokenKind::Return},   {"asm", TokenKind::Asm},
        {"struct", TokenKind::Struct},   {"comptime", TokenKind::Comptime},
        {"break", TokenKind::Break},     {"continue", TokenKind::Continue},
    };

    std::string id;
    while (currentChar_ && (std::isalnum(static_cast<unsigned char>(*currentChar_)) || *currentChar_ == '_')) {
        id.push_back(*currentChar_);
        advance();
    }

    auto it = keywords.find(id);
    if (it != keywords.end()) return Token{it->second, "", 0};
    return Token{TokenKind::Identifier, id, 0};
}

Token Lexer::readString() {
    advance();
    std::string value;
    while (currentChar_ && *currentChar_ != '"') {
        if (*currentChar_ == '\\') {
            advance();
            if (currentChar_) {
                switch (*currentChar_) {
                    case 'n': value.push_back('\n'); break;
                    case 't': value.push_back('\t'); break;
                    case 'r': value.push_back('\r'); break;
                    case '\\': value.push_back('\\'); break;
                    case '"': value.push_back('"'); break;
                    default: value.push_back(*currentChar_); break;
                }
                advance();
            }
        } else {
            value.push_back(*currentChar_);
            advance();
        }
    }
    if (currentChar_ == '"') advance();
    return Token{TokenKind::String, value, 0};
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

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    for (;;) {
        skipWhitespace();
        skipComment();

        size_t startLine = line_;
        size_t startColumn = column_;

        if (!currentChar_) {
            tokens.push_back(Token{TokenKind::Eof, "", 0});
            tokens.back().line = startLine;
            tokens.back().column = startColumn;
            break;
        }

        size_t sizeBefore = tokens.size();
        char ch = *currentChar_;
        switch (ch) {
            case '\n':
                tokens.push_back(Token{TokenKind::Newline, "", 0});
                advance();
                break;
            case '+':
                advance();
                if (currentChar_ == '+') { tokens.push_back(Token{TokenKind::DoublePlus, "", 0}); advance(); }
                else tokens.push_back(Token{TokenKind::Plus, "", 0});
                break;
            case '-':
                advance();
                if (currentChar_ == '>') { tokens.push_back(Token{TokenKind::Arrow, "", 0}); advance(); }
                else tokens.push_back(Token{TokenKind::Minus, "", 0});
                break;
            case '*': tokens.push_back(Token{TokenKind::Star, "", 0}); advance(); break;
            case '/': tokens.push_back(Token{TokenKind::Slash, "", 0}); advance(); break;
            case '%': tokens.push_back(Token{TokenKind::Percent, "", 0}); advance(); break;
            case '=':
                advance();
                if (currentChar_ == '=') { tokens.push_back(Token{TokenKind::Equal, "", 0}); advance(); }
                else tokens.push_back(Token{TokenKind::Assign, "", 0});
                break;
            case '!':
                advance();
                if (currentChar_ == '=') { tokens.push_back(Token{TokenKind::NotEqual, "", 0}); advance(); }
                else tokens.push_back(Token{TokenKind::Not, "", 0});
                break;
            case '<':
                advance();
                if (currentChar_ == '<') { tokens.push_back(Token{TokenKind::LShift, "", 0}); advance(); }
                else if (currentChar_ == '=') { tokens.push_back(Token{TokenKind::LessEqual, "", 0}); advance(); }
                else tokens.push_back(Token{TokenKind::Less, "", 0});
                break;
            case '>':
                advance();
                if (currentChar_ == '>') { tokens.push_back(Token{TokenKind::RShift, "", 0}); advance(); }
                else if (currentChar_ == '=') { tokens.push_back(Token{TokenKind::GreaterEqual, "", 0}); advance(); }
                else tokens.push_back(Token{TokenKind::Greater, "", 0});
                break;
            case '&':
                advance();
                if (currentChar_ == '&') { tokens.push_back(Token{TokenKind::And, "", 0}); advance(); }
                else tokens.push_back(Token{TokenKind::Ampersand, "", 0});
                break;
            case '|':
                advance();
                if (currentChar_ == '|') { tokens.push_back(Token{TokenKind::Or, "", 0}); advance(); }
                else tokens.push_back(Token{TokenKind::Pipe, "", 0});
                break;
            case '^': tokens.push_back(Token{TokenKind::Caret, "", 0}); advance(); break;
            case '(': tokens.push_back(Token{TokenKind::LeftParen, "", 0}); advance(); break;
            case ')': tokens.push_back(Token{TokenKind::RightParen, "", 0}); advance(); break;
            case '{': tokens.push_back(Token{TokenKind::LeftBrace, "", 0}); advance(); break;
            case '}': tokens.push_back(Token{TokenKind::RightBrace, "", 0}); advance(); break;
            case '[': tokens.push_back(Token{TokenKind::LBracket, "", 0}); advance(); break;
            case ']': tokens.push_back(Token{TokenKind::RBracket, "", 0}); advance(); break;
            case ',': tokens.push_back(Token{TokenKind::Comma, "", 0}); advance(); break;
            case ';': tokens.push_back(Token{TokenKind::Semicolon, "", 0}); advance(); break;
            case ':': tokens.push_back(Token{TokenKind::Colon, "", 0}); advance(); break;
            case '.': tokens.push_back(Token{TokenKind::Dot, "", 0}); advance(); break;
            case '$': tokens.push_back(Token{TokenKind::Dollar, "", 0}); advance(); break;
            case '#':
                advance();
                while (currentChar_ && *currentChar_ != '\n') advance();
                break;
            case '"':
                tokens.push_back(readString());
                break;
            default:
                if (std::isdigit(static_cast<unsigned char>(ch))) {
                    tokens.push_back(readNumber());
                } else if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
                    tokens.push_back(readIdentifier());
                } else {
                    CompileError err(ErrorKind::Lexer,
                                      std::string("unexpected character: '") + ch + "'",
                                      file_, line_, column_);
                    err.withSourceLine(agn::misc::extractSourceLine(input_, line_));
                    err.display();
                    std::exit(1);
                }
                break;
        }

        if (tokens.size() > sizeBefore) {
            tokens.back().line = startLine;
            tokens.back().column = startColumn;
        }
    }

    return tokens;
}

} // namespace agn::lexer
