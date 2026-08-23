// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#pragma once

#include "ast/ast.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agn::parser {

struct ComptimeValue {
    enum class Kind { I64, F64, Bool, String, Void } kind = Kind::Void;
    int64_t i = 0;
    double f = 0.0;
    bool b = false;
    std::string s;

    static ComptimeValue makeI64(int64_t v);
    static ComptimeValue makeF64(double v);
    static ComptimeValue makeBool(bool v);
    static ComptimeValue makeString(std::string v);

    bool isNumeric() const { return kind == Kind::I64 || kind == Kind::F64; }
    double asDouble() const { return kind == Kind::F64 ? f : static_cast<double>(i); }
    bool truthy() const;
};

class ComptimeEvaluator {
public:
    ComptimeEvaluator(ast::Program& program, std::string targetOs, std::string targetArch, std::string memMode);

    enum class Flow { Normal, Return, Break, Continue };
    struct ExecOutcome {
        Flow flow = Flow::Normal;
        std::optional<ComptimeValue> returnValue;
    };

    std::optional<ComptimeValue> eval(const ast::Expression& expr);
    ExecOutcome execStatement(const ast::Statement& stmt);
    ExecOutcome execBlock(const std::vector<ast::Statement>& body);

    std::optional<ComptimeValue> lookupGlobal(const std::string& name) const;
    void bindGlobal(const std::string& name, ComptimeValue value);
    bool hasGlobal(const std::string& name) const;

    const std::string& lastError() const { return lastError_; }
    void clearError() { lastError_.clear(); }

private:
    std::optional<ComptimeValue> lookup(const std::string& name) const;
    void bind(const std::string& name, ComptimeValue value);
    bool assign(const std::string& name, ComptimeValue value);
    std::optional<ComptimeValue> callFunction(const std::string& name, std::vector<ComptimeValue> args);
    bool consumeStep();
    void fail(const std::string& message);

    ast::Program& program_;
    std::string targetOs_;
    std::string targetArch_;
    std::string memMode_;
    std::vector<std::unordered_map<std::string, ComptimeValue>> scopes_;
    std::string lastError_;
    size_t steps_ = 0;

    static constexpr size_t kStepBudget = 10'000'000;
};

} // namespace agn::parser
