// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#include "parser/comptime_eval.hpp"

namespace agn::parser {

ComptimeValue ComptimeValue::makeI64(int64_t v) {
    ComptimeValue val; val.kind = Kind::I64; val.i = v; return val;
}
ComptimeValue ComptimeValue::makeF64(double v) {
    ComptimeValue val; val.kind = Kind::F64; val.f = v; return val;
}
ComptimeValue ComptimeValue::makeBool(bool v) {
    ComptimeValue val; val.kind = Kind::Bool; val.b = v; return val;
}
ComptimeValue ComptimeValue::makeString(std::string v) {
    ComptimeValue val; val.kind = Kind::String; val.s = std::move(v); return val;
}

bool ComptimeValue::truthy() const {
    switch (kind) {
        case Kind::Bool: return b;
        case Kind::I64: return i != 0;
        case Kind::F64: return f != 0.0;
        case Kind::String: return !s.empty();
        case Kind::Void: return false;
    }
    return false;
}

ComptimeEvaluator::ComptimeEvaluator(ast::Program& program, std::string targetOs, std::string targetArch, std::string memMode)
    : program_(program), targetOs_(std::move(targetOs)), targetArch_(std::move(targetArch)), memMode_(std::move(memMode)) {
    scopes_.emplace_back();
}

bool ComptimeEvaluator::consumeStep() {
    if (++steps_ > kStepBudget) {
        fail("comptime evaluation exceeded the step budget (possible infinite loop)");
        return false;
    }
    return true;
}

void ComptimeEvaluator::fail(const std::string& message) {
    if (lastError_.empty()) lastError_ = message;
}

std::optional<ComptimeValue> ComptimeEvaluator::lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return std::nullopt;
}

void ComptimeEvaluator::bind(const std::string& name, ComptimeValue value) {
    scopes_.back()[name] = std::move(value);
}

bool ComptimeEvaluator::assign(const std::string& name, ComptimeValue value) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) { found->second = std::move(value); return true; }
    }
    return false;
}

std::optional<ComptimeValue> ComptimeEvaluator::lookupGlobal(const std::string& name) const {
    auto found = scopes_.front().find(name);
    if (found != scopes_.front().end()) return found->second;
    return std::nullopt;
}

void ComptimeEvaluator::bindGlobal(const std::string& name, ComptimeValue value) {
    scopes_.front()[name] = std::move(value);
}

bool ComptimeEvaluator::hasGlobal(const std::string& name) const {
    return scopes_.front().count(name) != 0;
}

std::optional<ComptimeValue> ComptimeEvaluator::callFunction(const std::string& name, std::vector<ComptimeValue> args) {
    const ast::Function* target = nullptr;
    for (auto& f : program_.functions) {
        if (f.name == name && !f.receiver) { target = &f; break; }
    }
    if (!target) { fail("cannot evaluate at compile time: function '" + name + "' not found"); return std::nullopt; }
    if (target->params.size() != args.size()) {
        fail("cannot evaluate at compile time: '" + name + "' called with the wrong number of arguments");
        return std::nullopt;
    }

    scopes_.emplace_back();
    for (size_t i = 0; i < args.size(); i++) scopes_.back()[target->params[i].name] = args[i];

    std::optional<ComptimeValue> result;
    auto outcome = execBlock(target->body);
    if (outcome.flow == Flow::Return) result = outcome.returnValue;
    else result = ComptimeValue{};

    scopes_.pop_back();
    return result;
}

std::optional<ComptimeValue> ComptimeEvaluator::eval(const ast::Expression& expr) {
    if (auto* n = std::get_if<ast::NumberExpr>(&expr.node)) return ComptimeValue::makeI64(n->value);
    if (auto* n = std::get_if<ast::FloatExpr>(&expr.node)) return ComptimeValue::makeF64(n->value);
    if (auto* n = std::get_if<ast::BoolExpr>(&expr.node)) return ComptimeValue::makeBool(n->value);
    if (auto* n = std::get_if<ast::StringExpr>(&expr.node)) return ComptimeValue::makeString(n->value);

    if (auto* n = std::get_if<ast::IdentifierExpr>(&expr.node)) {
        if (n->name == "TARGET_OS") return ComptimeValue::makeString(targetOs_);
        if (n->name == "TARGET_ARCH") return ComptimeValue::makeString(targetArch_);
        if (n->name == "MEM_MODE") return ComptimeValue::makeString(memMode_);
        if (auto v = lookup(n->name)) return v;
        fail("cannot evaluate at compile time: '" + n->name + "' is not a compile-time constant");
        return std::nullopt;
    }

    if (auto* n = std::get_if<ast::UnaryExpr>(&expr.node)) {
        auto v = eval(*n->operand);
        if (!v) return std::nullopt;
        if (n->op == ast::UnaryOp::Not) return ComptimeValue::makeBool(!v->truthy());
        if (n->op == ast::UnaryOp::Neg) {
            if (v->kind == ComptimeValue::Kind::F64) return ComptimeValue::makeF64(-v->f);
            if (v->kind == ComptimeValue::Kind::I64) return ComptimeValue::makeI64(-v->i);
            fail("cannot evaluate at compile time: negation requires a numeric value");
            return std::nullopt;
        }
        return std::nullopt;
    }

    if (auto* n = std::get_if<ast::BinaryExpr>(&expr.node)) {
        if (n->op == ast::BinaryOp::And) {
            auto l = eval(*n->left);
            if (!l) return std::nullopt;
            if (!l->truthy()) return ComptimeValue::makeBool(false);
            auto r = eval(*n->right);
            if (!r) return std::nullopt;
            return ComptimeValue::makeBool(r->truthy());
        }
        if (n->op == ast::BinaryOp::Or) {
            auto l = eval(*n->left);
            if (!l) return std::nullopt;
            if (l->truthy()) return ComptimeValue::makeBool(true);
            auto r = eval(*n->right);
            if (!r) return std::nullopt;
            return ComptimeValue::makeBool(r->truthy());
        }

        auto l = eval(*n->left);
        auto r = eval(*n->right);
        if (!l || !r) return std::nullopt;

        if (n->op == ast::BinaryOp::Concat) {
            if (l->kind != ComptimeValue::Kind::String || r->kind != ComptimeValue::Kind::String) {
                fail("cannot evaluate at compile time: string concatenation requires string operands");
                return std::nullopt;
            }
            return ComptimeValue::makeString(l->s + r->s);
        }

        if (n->op == ast::BinaryOp::Equal || n->op == ast::BinaryOp::NotEqual) {
            bool eq;
            if (l->kind == ComptimeValue::Kind::String || r->kind == ComptimeValue::Kind::String) eq = l->s == r->s;
            else if (l->kind == ComptimeValue::Kind::Bool || r->kind == ComptimeValue::Kind::Bool) eq = l->truthy() == r->truthy();
            else eq = l->asDouble() == r->asDouble();
            return ComptimeValue::makeBool(n->op == ast::BinaryOp::Equal ? eq : !eq);
        }

        if (!l->isNumeric() || !r->isNumeric()) {
            fail("cannot evaluate at compile time: operator requires numeric operands");
            return std::nullopt;
        }
        bool isFloat = l->kind == ComptimeValue::Kind::F64 || r->kind == ComptimeValue::Kind::F64;

        switch (n->op) {
            case ast::BinaryOp::Add:
                return isFloat ? ComptimeValue::makeF64(l->asDouble() + r->asDouble()) : ComptimeValue::makeI64(l->i + r->i);
            case ast::BinaryOp::Sub:
                return isFloat ? ComptimeValue::makeF64(l->asDouble() - r->asDouble()) : ComptimeValue::makeI64(l->i - r->i);
            case ast::BinaryOp::Mul:
                return isFloat ? ComptimeValue::makeF64(l->asDouble() * r->asDouble()) : ComptimeValue::makeI64(l->i * r->i);
            case ast::BinaryOp::Div:
                return isFloat ? ComptimeValue::makeF64(l->asDouble() / r->asDouble()) : ComptimeValue::makeI64(r->i != 0 ? l->i / r->i : 0);
            case ast::BinaryOp::Mod:
                if (isFloat) { fail("cannot evaluate at compile time: '%' does not support float operands"); return std::nullopt; }
                return ComptimeValue::makeI64(r->i != 0 ? l->i % r->i : 0);
            case ast::BinaryOp::BitAnd:
                if (isFloat) { fail("cannot evaluate at compile time: bitwise ops require integers"); return std::nullopt; }
                return ComptimeValue::makeI64(l->i & r->i);
            case ast::BinaryOp::BitOr:
                if (isFloat) { fail("cannot evaluate at compile time: bitwise ops require integers"); return std::nullopt; }
                return ComptimeValue::makeI64(l->i | r->i);
            case ast::BinaryOp::BitXor:
                if (isFloat) { fail("cannot evaluate at compile time: bitwise ops require integers"); return std::nullopt; }
                return ComptimeValue::makeI64(l->i ^ r->i);
            case ast::BinaryOp::Shl:
                if (isFloat) { fail("cannot evaluate at compile time: shifts require integers"); return std::nullopt; }
                return ComptimeValue::makeI64(l->i << r->i);
            case ast::BinaryOp::Shr:
                if (isFloat) { fail("cannot evaluate at compile time: shifts require integers"); return std::nullopt; }
                return ComptimeValue::makeI64(l->i >> r->i);
            case ast::BinaryOp::Less:
                return ComptimeValue::makeBool(l->asDouble() < r->asDouble());
            case ast::BinaryOp::LessEqual:
                return ComptimeValue::makeBool(l->asDouble() <= r->asDouble());
            case ast::BinaryOp::Greater:
                return ComptimeValue::makeBool(l->asDouble() > r->asDouble());
            case ast::BinaryOp::GreaterEqual:
                return ComptimeValue::makeBool(l->asDouble() >= r->asDouble());
            default:
                break;
        }
        return std::nullopt;
    }

    if (auto* n = std::get_if<ast::CallExpr>(&expr.node)) {
        std::vector<ComptimeValue> args;
        for (auto& a : n->args) {
            auto v = eval(a);
            if (!v) return std::nullopt;
            args.push_back(*v);
        }
        if (!lastError_.empty()) return std::nullopt;
        return callFunction(n->function, std::move(args));
    }

    fail("cannot evaluate at compile time: unsupported expression");
    return std::nullopt;
}

ComptimeEvaluator::ExecOutcome ComptimeEvaluator::execBlock(const std::vector<ast::Statement>& body) {
    for (auto& stmt : body) {
        auto outcome = execStatement(stmt);
        if (outcome.flow != Flow::Normal) return outcome;
    }
    return ExecOutcome{};
}

ComptimeEvaluator::ExecOutcome ComptimeEvaluator::execStatement(const ast::Statement& stmt) {
    if (!consumeStep()) return ExecOutcome{};

    if (auto* n = std::get_if<ast::VarDeclStmt>(&stmt.node)) {
        if (!n->value) { fail("cannot evaluate at compile time: variable '" + n->name + "' has no initializer"); return ExecOutcome{}; }
        auto v = eval(*n->value);
        if (!v) return ExecOutcome{};
        bind(n->name, *v);
        return ExecOutcome{};
    }
    if (auto* n = std::get_if<ast::AssignmentStmt>(&stmt.node)) {
        auto v = eval(n->value);
        if (!v) return ExecOutcome{};
        if (!assign(n->name, *v)) fail("cannot evaluate at compile time: '" + n->name + "' is not a compile-time variable");
        return ExecOutcome{};
    }
    if (auto* n = std::get_if<ast::IfStmt>(&stmt.node)) {
        auto cond = eval(n->condition);
        if (!cond) return ExecOutcome{};
        if (cond->truthy()) return execBlock(n->thenBody);
        if (n->elseBody) return execBlock(*n->elseBody);
        return ExecOutcome{};
    }
    if (auto* n = std::get_if<ast::ForStmt>(&stmt.node)) {
        while (true) {
            if (n->condition) {
                auto cond = eval(*n->condition);
                if (!cond) return ExecOutcome{};
                if (!cond->truthy()) break;
            }
            if (!consumeStep()) return ExecOutcome{};
            auto outcome = execBlock(n->body);
            if (outcome.flow == Flow::Return) return outcome;
            if (outcome.flow == Flow::Break) break;
        }
        return ExecOutcome{};
    }
    if (std::get_if<ast::BreakStmt>(&stmt.node)) return ExecOutcome{Flow::Break, std::nullopt};
    if (std::get_if<ast::ContinueStmt>(&stmt.node)) return ExecOutcome{Flow::Continue, std::nullopt};
    if (auto* n = std::get_if<ast::ReturnStmt>(&stmt.node)) {
        if (!n->value) return ExecOutcome{Flow::Return, ComptimeValue{}};
        auto v = eval(*n->value);
        if (!v) return ExecOutcome{};
        return ExecOutcome{Flow::Return, v};
    }
    if (auto* n = std::get_if<ast::ExpressionStmt>(&stmt.node)) {
        eval(n->expr);
        return ExecOutcome{};
    }
    if (auto* n = std::get_if<ast::ComptimeStmt>(&stmt.node)) {
        return execBlock(n->body);
    }

    fail("cannot evaluate at compile time: unsupported statement");
    return ExecOutcome{};
}

} // namespace agn::parser
