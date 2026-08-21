// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#include "parser/monomorphize.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace agn::parser {

namespace {

struct MonoState {
    std::unordered_map<std::string, ast::StructDecl> templates;
    std::unordered_set<std::string> registered;
    std::unordered_set<std::string> inProgress;
    std::vector<ast::StructDecl> newStructs;
};

std::string rewriteTypeString(const std::string& s, MonoState& st,
                               const std::unordered_map<std::string, std::string>* subst);

std::string mangle(const std::string& name, const std::vector<std::string>& args) {
    std::string m = "__gen_" + name;
    for (auto& a : args) m += "$" + a;
    return m;
}

std::string instantiate(const std::string& name, const std::vector<std::string>& args, MonoState& st) {
    std::string mangled = mangle(name, args);
    if (st.registered.count(mangled)) return mangled;
    if (st.inProgress.count(mangled)) {
        std::fprintf(stderr, "error: recursive generic instantiation of '%s'\n", name.c_str());
        std::exit(1);
    }
    auto tmplIt = st.templates.find(name);
    if (tmplIt == st.templates.end()) {
        std::fprintf(stderr, "error: '%s' is not a declared generic struct\n", name.c_str());
        std::exit(1);
    }
    const ast::StructDecl& tmpl = tmplIt->second;
    if (tmpl.typeParams.size() != args.size()) {
        std::fprintf(stderr, "error: '%s' expects %zu type argument(s), got %zu\n", name.c_str(),
                     tmpl.typeParams.size(), args.size());
        std::exit(1);
    }

    std::unordered_map<std::string, std::string> subst;
    for (size_t i = 0; i < tmpl.typeParams.size(); i++) subst[tmpl.typeParams[i]] = args[i];

    st.inProgress.insert(mangled);
    std::vector<ast::Parameter> concreteFields;
    for (auto& f : tmpl.fields) {
        concreteFields.push_back(ast::Parameter{f.name, rewriteTypeString(f.type, st, &subst)});
    }
    st.inProgress.erase(mangled);
    st.registered.insert(mangled);

    st.newStructs.push_back(ast::StructDecl{mangled, {}, std::move(concreteFields)});
    return mangled;
}

bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string rewriteOne(const std::string& s, size_t& pos, MonoState& st,
                        const std::unordered_map<std::string, std::string>* subst) {
    if (s.compare(pos, 5, "func(") == 0) {
        pos += 5;
        std::string out = "func(";
        bool first = true;
        while (pos < s.size() && s[pos] != ')') {
            if (!first) out += ",";
            first = false;
            out += rewriteOne(s, pos, st, subst);
            if (pos < s.size() && s[pos] == ',') pos++;
        }
        if (pos < s.size()) pos++; // ')'
        out += ")";
        if (s.compare(pos, 2, "->") == 0) {
            pos += 2;
            out += "->" + rewriteOne(s, pos, st, subst);
        }
        return out;
    }

    size_t start = pos;
    while (pos < s.size() && isIdentChar(s[pos])) pos++;
    std::string name = s.substr(start, pos - start);

    if (pos < s.size() && s[pos] == '<') {
        pos++;
        std::vector<std::string> args;
        while (pos < s.size() && s[pos] != '>') {
            args.push_back(rewriteOne(s, pos, st, subst));
            if (pos < s.size() && s[pos] == ',') pos++;
        }
        if (pos < s.size()) pos++; // '>'
        return instantiate(name, args, st);
    }

    if (subst) {
        auto it = subst->find(name);
        if (it != subst->end()) return it->second;
    }
    return name;
}

std::string rewriteTypeString(const std::string& s, MonoState& st,
                               const std::unordered_map<std::string, std::string>* subst) {
    if (s.empty()) return s;
    size_t pos = 0;
    return rewriteOne(s, pos, st, subst);
}

void rewriteExpr(ast::Expression& e, MonoState& st);

void rewriteStmt(ast::Statement& s, MonoState& st) {
    std::visit(
        [&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ast::VarDeclStmt>) {
                if (!node.varType.empty()) node.varType = rewriteTypeString(node.varType, st, nullptr);
                if (node.value) rewriteExpr(*node.value, st);
            } else if constexpr (std::is_same_v<T, ast::ArrayDeclStmt>) {
                node.elementType = rewriteTypeString(node.elementType, st, nullptr);
            } else if constexpr (std::is_same_v<T, ast::AssignmentStmt>) {
                rewriteExpr(node.value, st);
            } else if constexpr (std::is_same_v<T, ast::ArrayAssignmentStmt>) {
                rewriteExpr(node.index, st);
                rewriteExpr(node.value, st);
            } else if constexpr (std::is_same_v<T, ast::PointerAssignmentStmt>) {
                rewriteExpr(node.target, st);
                rewriteExpr(node.value, st);
            } else if constexpr (std::is_same_v<T, ast::FieldAssignmentStmt>) {
                rewriteExpr(node.object, st);
                rewriteExpr(node.value, st);
            } else if constexpr (std::is_same_v<T, ast::IfStmt>) {
                rewriteExpr(node.condition, st);
                for (auto& s2 : node.thenBody) rewriteStmt(s2, st);
                if (node.elseBody)
                    for (auto& s2 : *node.elseBody) rewriteStmt(s2, st);
            } else if constexpr (std::is_same_v<T, ast::ForStmt>) {
                if (node.condition) rewriteExpr(*node.condition, st);
                for (auto& s2 : node.body) rewriteStmt(s2, st);
            } else if constexpr (std::is_same_v<T, ast::ReturnStmt>) {
                if (node.value) rewriteExpr(*node.value, st);
            } else if constexpr (std::is_same_v<T, ast::ExpressionStmt>) {
                rewriteExpr(node.expr, st);
            } else if constexpr (std::is_same_v<T, ast::ComptimeStmt>) {
                for (auto& s2 : node.body) rewriteStmt(s2, st);
            }
        },
        s.node);
}

void rewriteExpr(ast::Expression& e, MonoState& st) {
    std::visit(
        [&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ast::BinaryExpr>) {
                rewriteExpr(*node.left, st);
                rewriteExpr(*node.right, st);
            } else if constexpr (std::is_same_v<T, ast::UnaryExpr>) {
                rewriteExpr(*node.operand, st);
            } else if constexpr (std::is_same_v<T, ast::CallExpr>) {
                for (auto& a : node.args) rewriteExpr(a, st);
            } else if constexpr (std::is_same_v<T, ast::MethodCallExpr>) {
                for (auto& a : node.args) rewriteExpr(a, st);
            } else if constexpr (std::is_same_v<T, ast::ArrayAccessExpr>) {
                rewriteExpr(*node.index, st);
            } else if constexpr (std::is_same_v<T, ast::StringIndexExpr>) {
                rewriteExpr(*node.str, st);
                rewriteExpr(*node.index, st);
            } else if constexpr (std::is_same_v<T, ast::AddressOfExpr>) {
                rewriteExpr(*node.operand, st);
            } else if constexpr (std::is_same_v<T, ast::DerefExpr>) {
                rewriteExpr(*node.operand, st);
            } else if constexpr (std::is_same_v<T, ast::EvalExpr>) {
                rewriteExpr(*node.instruction, st);
            } else if constexpr (std::is_same_v<T, ast::FieldAccessExpr>) {
                rewriteExpr(*node.object, st);
            } else if constexpr (std::is_same_v<T, ast::FunctionLiteralExpr>) {
                for (auto& p : node.params) p.type = rewriteTypeString(p.type, st, nullptr);
                node.returnType = rewriteTypeString(node.returnType, st, nullptr);
                for (auto& s2 : node.body) rewriteStmt(s2, st);
            } else if constexpr (std::is_same_v<T, ast::StructLiteralExpr>) {
                node.structName = rewriteTypeString(node.structName, st, nullptr);
                for (auto& [fname, fexpr] : node.fields) rewriteExpr(fexpr, st);
            } else if constexpr (std::is_same_v<T, ast::TemplateStringExpr>) {
                for (auto& part : node.parts) {
                    if (auto* e2 = std::get_if<ast::TemplateExprPart>(&part)) rewriteExpr(*e2->expr, st);
                }
            }
        },
        e.node);
}

void rewriteFunction(ast::Function& f, MonoState& st) {
    if (f.receiver) f.receiver->type = rewriteTypeString(f.receiver->type, st, nullptr);
    for (auto& p : f.params) p.type = rewriteTypeString(p.type, st, nullptr);
    f.returnType = rewriteTypeString(f.returnType, st, nullptr);
    for (auto& s : f.body) rewriteStmt(s, st);
}

} // namespace

void monomorphizeGenerics(ast::Program& program) {
    MonoState st;

    std::vector<ast::StructDecl> concrete;
    for (auto& s : program.structs) {
        if (!s.typeParams.empty()) {
            st.templates[s.name] = s;
        } else {
            concrete.push_back(std::move(s));
        }
    }
    program.structs = std::move(concrete);

    for (auto& s : program.structs) {
        for (auto& f : s.fields) f.type = rewriteTypeString(f.type, st, nullptr);
    }

    for (auto& f : program.functions) rewriteFunction(f, st);
    for (auto& [modName, mod] : program.modules) {
        for (auto& f : mod.functions) rewriteFunction(f, st);
    }

    for (auto& s : st.newStructs) program.structs.push_back(std::move(s));
}

} // namespace agn::parser
