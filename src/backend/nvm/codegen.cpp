// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#include "backend/nvm/codegen.hpp"
#include "backend/nvm/opcodes.hpp"

#include <cctype>
#include <cstdlib>

namespace agn::backend::nvm {

using agn::parser::TypeKind;

namespace {

bool asmLineIsSyscallExit(const std::string& line) {
    std::string code = line;
    size_t semi = code.find(';');
    if (semi != std::string::npos) code = code.substr(0, semi);
    size_t start = code.find_first_not_of(" \t");
    if (start == std::string::npos) return false;
    size_t end = code.find_first_of(" \t", start);
    std::string word1 = code.substr(start, end == std::string::npos ? std::string::npos : end - start);
    for (auto& c : word1) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (word1 != "syscall") return false;
    if (end == std::string::npos) return false;
    size_t argStart = code.find_first_not_of(" \t", end);
    if (argStart == std::string::npos) return false;
    size_t argEnd = code.find_first_of(" \t", argStart);
    std::string word2 = code.substr(argStart, argEnd == std::string::npos ? std::string::npos : argEnd - argStart);
    for (auto& c : word2) c = char(std::tolower(static_cast<unsigned char>(c)));
    return word2 == "exit";
}

bool asmBlockIsSyscallExit(const ast::InlineAsmStmt& asmStmt) {
    std::string text;
    for (auto& part : asmStmt.parts) {
        if (part.kind == ast::AsmPart::Kind::Literal) text += part.text;
    }
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        if (asmLineIsSyscallExit(text.substr(pos, nl - pos))) return true;
        pos = nl + 1;
    }
    return false;
}

} // namespace

bool NVMCodeGen::hasReturnOrExit(const std::vector<ast::Statement>& stmts) {
    for (auto& stmt : stmts) {
        if (std::get_if<ast::ReturnStmt>(&stmt.node)) return true;
        if (auto* n = std::get_if<ast::InlineAsmStmt>(&stmt.node)) {
            if (asmBlockIsSyscallExit(*n)) return true;
        }
        if (auto* n = std::get_if<ast::IfStmt>(&stmt.node)) {
            if (hasReturnOrExit(n->thenBody)) return true;
            if (n->elseBody && hasReturnOrExit(*n->elseBody)) return true;
        }
        if (auto* n = std::get_if<ast::ForStmt>(&stmt.node)) {
            if (hasReturnOrExit(n->body)) return true;
        }
    }
    return false;
}

void NVMCodeGen::emitByte(uint8_t b) { bytecode_.push_back(b); }

void NVMCodeGen::emitPush(int32_t value) {
    emitByte(PUSH);
    uint32_t u = static_cast<uint32_t>(value);
    bytecode_.push_back(uint8_t(u >> 24));
    bytecode_.push_back(uint8_t(u >> 16));
    bytecode_.push_back(uint8_t(u >> 8));
    bytecode_.push_back(uint8_t(u));
}

void NVMCodeGen::emitJumpRef(const std::string& label) {
    jumpPatches_.push_back({uint32_t(bytecode_.size()), label});
    bytecode_.insert(bytecode_.end(), {0, 0, 0, 0});
}

void NVMCodeGen::addLabel(const std::string& label) { labels_[label] = uint32_t(bytecode_.size()); }

std::string NVMCodeGen::generateLabel(const std::string& prefix) {
    return prefix + "_" + currentFunction_ + "_" + std::to_string(labelCounter_++);
}

void NVMCodeGen::patchJumps() {
    for (auto& [pos, label] : jumpPatches_) {
        auto it = labels_.find(label);
        if (it == labels_.end()) continue;
        uint32_t target = it->second;
        bytecode_[pos] = uint8_t(target >> 24);
        bytecode_[pos + 1] = uint8_t(target >> 16);
        bytecode_[pos + 2] = uint8_t(target >> 8);
        bytecode_[pos + 3] = uint8_t(target);
    }
}

int NVMCodeGen::fieldOffset(const std::string& structName, const std::string& field) {
    auto it = checker_.structs().find(structName);
    if (it == checker_.structs().end()) return -1;
    for (size_t i = 0; i < it->second.size(); i++) {
        if (it->second[i].first == field) return int(i) * 4;
    }
    return -1;
}

std::vector<uint8_t> NVMCodeGen::generate(ast::Program& program) {
    bytecode_.insert(bytecode_.end(), {'N', 'V', 'M', '0'});

    ast::Function* mainFn = nullptr;
    for (auto& f : program.functions) if (f.name == "main") mainFn = &f;
    if (mainFn) generateFunction(*mainFn, "main", true);

    for (auto& f : program.functions) {
        if (&f == mainFn) continue;
        std::string name = f.receiver ? (f.receiver->type + "_" + f.name) : f.name;
        generateFunction(f, name, false);
    }

    for (auto& [modName, mod] : program.modules) {
        if (modName == "stdio" || modName == "os") continue;
        for (auto& f : mod.functions) {
            if (!f.isExported) continue;
            currentModulePrefix_ = modName;
            generateFunction(f, modName + "_" + f.name, false);
            currentModulePrefix_.clear();
        }
    }

    if (needsHeapAlloc_) emitHeapAllocHelper();
    if (needsPrintInt_) emitPrintIntHelper();

    patchJumps();
    return bytecode_;
}

void NVMCodeGen::generateFunction(ast::Function& func, const std::string& fullName, bool isMain) {
    currentFunction_ = fullName;
    vars_.clear();
    compileTimeStrings_.clear();
    nextLocal_ = 0;
    inMain_ = isMain;

    if (!isMain) addLabel("func_" + fullName);

    uint8_t argIndex = 0;
    if (func.receiver) {
        vars_[func.receiver->name] = VarInfo{true, argIndex++, func.receiver->type};
    }
    for (auto& p : func.params) {
        std::string structType;
        auto pType = checker_.resolveTypeString(p.type);
        if (pType.kind == TypeKind::Struct) structType = pType.structName;
        vars_[p.name] = VarInfo{true, argIndex++, structType};
    }

    emitByte(ENTER);
    enterOperandPos_ = bytecode_.size();
    emitByte(0);

    for (auto& stmt : func.body) generateStatement(stmt);

    if (isMain) {
        if (!hasReturnOrExit(func.body)) {
            emitPush(0);
            emitByte(SYSCALL);
            emitByte(SYS_EXIT);
        }
    } else {
        emitByte(LEAVE);
        emitByte(RET);
    }

    bytecode_[enterOperandPos_] = uint8_t(nextLocal_);
}

void NVMCodeGen::storeHeapI32() {
    uint8_t valLocal = nextLocal_++;
    uint8_t addrLocal = nextLocal_++;
    emitByte(STORE_REL);
    emitByte(valLocal);
    emitByte(STORE_REL);
    emitByte(addrLocal);
    for (int i = 0; i < 4; i++) {
        int shift = 24 - i * 8;
        emitByte(LOAD_REL);
        emitByte(addrLocal);
        if (i != 0) {
            emitPush(i);
            emitByte(ADD);
        }
        emitByte(LOAD_REL);
        emitByte(valLocal);
        if (shift != 0) {
            emitPush(shift);
            emitByte(SHR);
        }
        emitPush(0xFF);
        emitByte(AND);
        emitByte(STORE_HEAP);
    }
}

void NVMCodeGen::materializeString(const std::string& text) {
    needsHeapAlloc_ = true;
    size_t len = text.size() + 1 + 3; // +3: room for LOAD_HEAP's 4-byte reads past the last char

    emitPush(int32_t(len));
    emitByte(CALL);
    emitJumpRef("func___heap_alloc");
    emitByte(POP);
    emitByte(LOAD);
    emitByte(255);

    uint8_t baseLocal = nextLocal_++;
    emitByte(STORE_REL);
    emitByte(baseLocal);

    for (size_t i = 0; i < text.size(); i++) {
        emitByte(LOAD_REL);
        emitByte(baseLocal);
        if (i != 0) {
            emitPush(int32_t(i));
            emitByte(ADD);
        }
        emitPush(int32_t(uint8_t(text[i])));
        emitByte(STORE_HEAP);
    }

    emitByte(LOAD_REL);
    emitByte(baseLocal);
    emitPush(int32_t(text.size()));
    emitByte(ADD);
    emitPush(0);
    emitByte(STORE_HEAP);

    emitByte(LOAD_REL);
    emitByte(baseLocal);
}

void NVMCodeGen::writeHeapString(const std::string& text) {
    materializeString(text);
    uint8_t tmp = nextLocal_++;
    emitByte(STORE_REL);
    emitByte(tmp);
    emitPush(1);
    emitByte(LOAD_REL);
    emitByte(tmp);
    emitByte(SYSCALL);
    emitByte(SYS_WRITE);
    emitByte(POP);
}

void NVMCodeGen::writeExprAsString(ast::Expression& expr) {
    generateExpression(expr);
    uint8_t tmp = nextLocal_++;
    emitByte(STORE_REL);
    emitByte(tmp);
    emitPush(1);
    emitByte(LOAD_REL);
    emitByte(tmp);
    emitByte(SYSCALL);
    emitByte(SYS_WRITE);
    emitByte(POP);
}

void NVMCodeGen::generateStructLiteral(ast::StructLiteralExpr& lit) {
    needsHeapAlloc_ = true;
    auto structIt = checker_.structs().find(lit.structName);
    size_t numFields = structIt != checker_.structs().end() ? structIt->second.size() : lit.fields.size();

    emitPush(int32_t(numFields * 4));
    emitByte(CALL);
    emitJumpRef("func___heap_alloc");
    emitByte(POP);
    emitByte(LOAD);
    emitByte(255);

    uint8_t baseLocal = nextLocal_++;
    emitByte(STORE_REL);
    emitByte(baseLocal);

    for (auto& [fieldName, fieldExpr] : lit.fields) {
        int offset = fieldOffset(lit.structName, fieldName);
        if (offset < 0) continue;
        emitByte(LOAD_REL);
        emitByte(baseLocal);
        if (offset != 0) {
            emitPush(offset);
            emitByte(ADD);
        }
        generateExpression(fieldExpr);
        storeHeapI32();
    }

    emitByte(LOAD_REL);
    emitByte(baseLocal);
}

void NVMCodeGen::generateStdioCall(const std::string& member, std::vector<ast::Expression>& args) {
    if (member == "Print" || member == "Println") {
        if (!args.empty()) {
            if (std::get_if<ast::StringExpr>(&args[0].node)) {
                writeExprAsString(args[0]);
            } else {
                needsPrintInt_ = true;
                generateExpression(args[0]);
                emitByte(CALL);
                emitJumpRef("func___print_int");
                emitByte(POP);
            }
            if (member == "Println") writeHeapString("\n");
        }
    } else if (member == "PrintStr") {
        if (!args.empty()) writeExprAsString(args[0]);
    } else if (member == "PrintlnStr") {
        if (!args.empty()) {
            writeExprAsString(args[0]);
            writeHeapString("\n");
        }
    } else if (member == "PrintChar") {
        if (!args.empty()) {
            generateExpression(args[0]);
            uint8_t chLocal = nextLocal_++;
            emitByte(STORE_REL);
            emitByte(chLocal);

            emitPush(1);
            emitByte(CALL);
            emitJumpRef("func___heap_alloc");
            emitByte(POP);
            emitByte(LOAD);
            emitByte(255);
            uint8_t bufLocal = nextLocal_++;
            emitByte(STORE_REL);
            emitByte(bufLocal);

            emitByte(LOAD_REL);
            emitByte(bufLocal);
            emitByte(LOAD_REL);
            emitByte(chLocal);
            emitByte(STORE_HEAP);

            emitPush(1);
            emitByte(LOAD_REL);
            emitByte(bufLocal);
            emitByte(SYSCALL);
            emitByte(SYS_WRITE);
            emitByte(POP);
        }
    } else if (member == "ReadInt" || member == "ReadChar" || member == "ReadLine") {
        std::fprintf(stderr, "error: stdio.%s is not available in the nvm backend (stdin is not readable in the kernel)\n",
                     member.c_str());
        std::exit(1);
    } else if (member == "Flush") {
        // no-op: the runtime does not buffer writes
    }
    emitPush(0);
}

void NVMCodeGen::emitStrlenInto(uint8_t strLocal, uint8_t outLenLocal) {
    emitPush(0);
    emitByte(STORE_REL);
    emitByte(outLenLocal);

    std::string loopLbl = generateLabel("strlen_loop");
    std::string endLbl = generateLabel("strlen_end");
    addLabel(loopLbl);

    emitByte(LOAD_REL);
    emitByte(strLocal);
    emitByte(LOAD_REL);
    emitByte(outLenLocal);
    emitByte(ADD);
    emitByte(LOAD_HEAP);
    emitPush(24);
    emitByte(SHR);
    emitPush(0xFF);
    emitByte(AND);
    emitByte(JZ);
    emitJumpRef(endLbl);

    emitByte(LOAD_REL);
    emitByte(outLenLocal);
    emitPush(1);
    emitByte(ADD);
    emitByte(STORE_REL);
    emitByte(outLenLocal);
    emitByte(JMP);
    emitJumpRef(loopLbl);

    addLabel(endLbl);
}

void NVMCodeGen::emitCopyBytes(uint8_t srcLocal, uint8_t destLocal, uint8_t destOffsetLocal, uint8_t lenLocal) {
    uint8_t iLocal = nextLocal_++;
    emitPush(0);
    emitByte(STORE_REL);
    emitByte(iLocal);

    std::string loopLbl = generateLabel("copy_loop");
    std::string endLbl = generateLabel("copy_end");
    addLabel(loopLbl);

    emitByte(LOAD_REL);
    emitByte(iLocal);
    emitByte(LOAD_REL);
    emitByte(lenLocal);
    emitByte(LT);
    emitByte(JZ);
    emitJumpRef(endLbl);

    emitByte(LOAD_REL);
    emitByte(destLocal);
    emitByte(LOAD_REL);
    emitByte(destOffsetLocal);
    emitByte(ADD);
    emitByte(LOAD_REL);
    emitByte(iLocal);
    emitByte(ADD);

    emitByte(LOAD_REL);
    emitByte(srcLocal);
    emitByte(LOAD_REL);
    emitByte(iLocal);
    emitByte(ADD);
    emitByte(LOAD_HEAP);
    emitPush(24);
    emitByte(SHR);
    emitPush(0xFF);
    emitByte(AND);

    emitByte(STORE_HEAP);

    emitByte(LOAD_REL);
    emitByte(iLocal);
    emitPush(1);
    emitByte(ADD);
    emitByte(STORE_REL);
    emitByte(iLocal);
    emitByte(JMP);
    emitJumpRef(loopLbl);

    addLabel(endLbl);
}

void NVMCodeGen::generateStringCall(const std::string& member, std::vector<ast::Expression>& args) {
    if (member == "len") {
        generateExpression(args[0]);
        uint8_t sLocal = nextLocal_++;
        emitByte(STORE_REL);
        emitByte(sLocal);

        uint8_t lenLocal = nextLocal_++;
        emitStrlenInto(sLocal, lenLocal);

        emitByte(LOAD_REL);
        emitByte(lenLocal);
        return;
    }
    if (member == "compare") {
        generateExpression(args[0]);
        uint8_t aLocal = nextLocal_++;
        emitByte(STORE_REL);
        emitByte(aLocal);
        generateExpression(args[1]);
        uint8_t bLocal = nextLocal_++;
        emitByte(STORE_REL);
        emitByte(bLocal);

        uint8_t iLocal = nextLocal_++;
        emitPush(0);
        emitByte(STORE_REL);
        emitByte(iLocal);
        uint8_t chALocal = nextLocal_++;
        uint8_t chBLocal = nextLocal_++;

        std::string loopLbl = generateLabel("strcmp_loop");
        std::string diffLbl = generateLabel("strcmp_diff");
        std::string eqLbl = generateLabel("strcmp_eq");
        std::string doneLbl = generateLabel("strcmp_done");
        addLabel(loopLbl);

        emitByte(LOAD_REL);
        emitByte(aLocal);
        emitByte(LOAD_REL);
        emitByte(iLocal);
        emitByte(ADD);
        emitByte(LOAD_HEAP);
        emitPush(24);
        emitByte(SHR);
        emitPush(0xFF);
        emitByte(AND);
        emitByte(STORE_REL);
        emitByte(chALocal);

        emitByte(LOAD_REL);
        emitByte(bLocal);
        emitByte(LOAD_REL);
        emitByte(iLocal);
        emitByte(ADD);
        emitByte(LOAD_HEAP);
        emitPush(24);
        emitByte(SHR);
        emitPush(0xFF);
        emitByte(AND);
        emitByte(STORE_REL);
        emitByte(chBLocal);

        emitByte(LOAD_REL);
        emitByte(chALocal);
        emitByte(LOAD_REL);
        emitByte(chBLocal);
        emitByte(NEQ);
        emitByte(JNZ);
        emitJumpRef(diffLbl);

        emitByte(LOAD_REL);
        emitByte(chALocal);
        emitByte(JZ);
        emitJumpRef(eqLbl);

        emitByte(LOAD_REL);
        emitByte(iLocal);
        emitPush(1);
        emitByte(ADD);
        emitByte(STORE_REL);
        emitByte(iLocal);
        emitByte(JMP);
        emitJumpRef(loopLbl);

        addLabel(diffLbl);
        emitByte(LOAD_REL);
        emitByte(chALocal);
        emitByte(LOAD_REL);
        emitByte(chBLocal);
        emitByte(CMP);
        emitByte(JMP);
        emitJumpRef(doneLbl);

        addLabel(eqLbl);
        emitPush(0);
        emitByte(JMP);
        emitJumpRef(doneLbl);

        addLabel(doneLbl);
        return;
    }
    if (member == "concat") {
        needsHeapAlloc_ = true;
        generateExpression(args[0]);
        uint8_t s1Local = nextLocal_++;
        emitByte(STORE_REL);
        emitByte(s1Local);
        generateExpression(args[1]);
        uint8_t s2Local = nextLocal_++;
        emitByte(STORE_REL);
        emitByte(s2Local);

        uint8_t len1Local = nextLocal_++;
        emitStrlenInto(s1Local, len1Local);
        uint8_t len2Local = nextLocal_++;
        emitStrlenInto(s2Local, len2Local);

        emitByte(LOAD_REL);
        emitByte(len1Local);
        emitByte(LOAD_REL);
        emitByte(len2Local);
        emitByte(ADD);
        emitPush(1 + 3);
        emitByte(ADD);
        emitByte(CALL);
        emitJumpRef("func___heap_alloc");
        emitByte(POP);
        emitByte(LOAD);
        emitByte(255);

        uint8_t bufLocal = nextLocal_++;
        emitByte(STORE_REL);
        emitByte(bufLocal);

        uint8_t zeroLocal = nextLocal_++;
        emitPush(0);
        emitByte(STORE_REL);
        emitByte(zeroLocal);
        emitCopyBytes(s1Local, bufLocal, zeroLocal, len1Local);
        emitCopyBytes(s2Local, bufLocal, len1Local, len2Local);

        emitByte(LOAD_REL);
        emitByte(bufLocal);
        emitByte(LOAD_REL);
        emitByte(len1Local);
        emitByte(ADD);
        emitByte(LOAD_REL);
        emitByte(len2Local);
        emitByte(ADD);
        emitPush(0);
        emitByte(STORE_HEAP);

        emitByte(LOAD_REL);
        emitByte(bufLocal);
        return;
    }
    emitPush(0);
}

void NVMCodeGen::generateNovariaRemove(ast::Expression& filenameExpr) {
    generateExpression(filenameExpr);
    uint8_t baseLocal = nextLocal_++;
    emitByte(STORE_REL);
    emitByte(baseLocal);

    emitPush(0);

    uint8_t iLocal = nextLocal_++;
    emitPush(0);
    emitByte(STORE_REL);
    emitByte(iLocal);

    std::string loopLbl = generateLabel("remove_loop");
    std::string endLbl = generateLabel("remove_end");
    addLabel(loopLbl);

    emitByte(LOAD_REL);
    emitByte(baseLocal);
    emitByte(LOAD_REL);
    emitByte(iLocal);
    emitByte(ADD);
    emitByte(LOAD_HEAP);
    emitPush(24);
    emitByte(SHR);
    emitPush(0xFF);
    emitByte(AND);

    emitByte(DUP);
    emitByte(JZ);
    emitJumpRef(endLbl);

    emitByte(LOAD_REL);
    emitByte(iLocal);
    emitPush(1);
    emitByte(ADD);
    emitByte(STORE_REL);
    emitByte(iLocal);
    emitByte(JMP);
    emitJumpRef(loopLbl);

    addLabel(endLbl);
    emitByte(POP); // discard the duplicated terminator byte (0)

    emitByte(SYSCALL);
    emitByte(SYS_REMOVE);
}

void NVMCodeGen::generateNovariaCall(const std::string& member, std::vector<ast::Expression>& args) {
    if (member == "Exit") {
        if (!args.empty()) generateExpression(args[0]);
        else emitPush(0);
        emitByte(SYSCALL);
        emitByte(SYS_EXIT);
        emitPush(0);
        return;
    }
    if (member == "Open") {
        generateExpression(args[0]);
        emitByte(SYSCALL);
        emitByte(SYS_OPEN);
        return;
    }
    if (member == "Read") {
        generateExpression(args[0]);
        generateExpression(args[1]);
        emitByte(SYSCALL);
        emitByte(SYS_READ);
        return;
    }
    if (member == "Write") {
        generateExpression(args[0]);
        generateExpression(args[1]);
        emitByte(SYSCALL);
        emitByte(SYS_WRITE);
        return;
    }
    if (member == "MemAlloc") {
        needsHeapAlloc_ = true;
        generateExpression(args[0]);
        emitByte(CALL);
        emitJumpRef("func___heap_alloc");
        emitByte(POP);
        emitByte(LOAD);
        emitByte(255);
        return;
    }
    if (member == "Remove") {
        generateNovariaRemove(args[0]);
        emitPush(0);
        return;
    }
    if (member == "Exec") {
        generateExpression(args[0]);
        emitByte(SYSCALL);
        emitByte(SYS_OPEN);
        uint8_t fdLocal = nextLocal_++;
        emitByte(STORE_REL);
        emitByte(fdLocal);
        emitPush(0);
        emitByte(LOAD_REL);
        emitByte(fdLocal);
        emitByte(SYSCALL);
        emitByte(SYS_SPAWN);
        emitPush(0);
        return;
    }
    emitPush(0);
}

void NVMCodeGen::generateCall(const std::string& name, std::vector<ast::Expression>& args) {
    if (currentModulePrefix_ == "string" && (name == "len" || name == "compare" || name == "concat")) {
        generateStringCall(name, args);
        return;
    }

    std::string label = name;
    std::string sigKey = name;
    if (!currentModulePrefix_.empty() && vars_.find(name) == vars_.end()) {
        std::string qualified = currentModulePrefix_ + "." + name;
        if (checker_.functions().count(qualified)) {
            label = currentModulePrefix_ + "_" + name;
            sigKey = qualified;
        }
    }

    for (auto it = args.rbegin(); it != args.rend(); ++it) generateExpression(*it);
    emitByte(CALL);
    emitJumpRef("func_" + label);
    for (size_t i = 0; i < args.size(); i++) emitByte(POP);

    auto sigIt = checker_.functions().find(sigKey);
    bool isVoid = sigIt == checker_.functions().end() || sigIt->second.returnType.kind == TypeKind::Void;
    if (!isVoid) {
        emitByte(LOAD);
        emitByte(255);
    } else {
        emitPush(0);
    }
}

void NVMCodeGen::generateMethodCall(ast::MethodCallExpr& expr) {
    const std::string& object = expr.object;
    const std::string& member = expr.member;
    std::vector<ast::Expression>& args = expr.args;

    if (object == "stdio") {
        generateStdioCall(member, args);
        return;
    }
    if (object == "string" && (member == "len" || member == "compare" || member == "concat")) {
        generateStringCall(member, args);
        return;
    }
    if (object == "novaria") {
        generateNovariaCall(member, args);
        return;
    }
    if (object == "os") {
        std::fprintf(stderr, "error: the 'os' module (file I/O, command-line arguments) is not supported by the nvm backend, use 'novaria' instead\n");
        std::exit(1);
    }
    if (object == "string" &&
        (member == "indexOf" || member == "contains" || member == "startsWith" || member == "endsWith" ||
         member == "charAt" || member == "substr" || member == "toUpper" || member == "toLower")) {
        std::fprintf(stderr, "error: 'string.%s' is not supported by the nvm backend\n", member.c_str());
        std::exit(1);
    }

    if (expr.kind == ast::MethodCallKind::StructField) {
        std::fprintf(stderr, "error: calling a function-valued struct field ('%s.%s') is not supported by the nvm backend\n",
                     expr.resolvedStructName.c_str(), member.c_str());
        std::exit(1);
    }

    bool isMethod = expr.kind == ast::MethodCallKind::Method;
    auto it = vars_.find(object);

    for (auto argIt = args.rbegin(); argIt != args.rend(); ++argIt) generateExpression(*argIt);

    std::string key;
    if (isMethod) {
        emitByte(it->second.isArg ? LOAD_ARG : LOAD_REL);
        emitByte(it->second.index);
        key = expr.resolvedStructName + "_" + member;
    } else {
        key = object + "_" + member;
    }
    emitByte(CALL);
    emitJumpRef("func_" + key);

    size_t argCount = args.size() + (isMethod ? 1 : 0);
    for (size_t i = 0; i < argCount; i++) emitByte(POP);

    std::string sigKey = isMethod ? (expr.resolvedStructName + "." + member) : (object + "." + member);
    auto sigIt = checker_.functions().find(sigKey);
    bool isVoid = sigIt == checker_.functions().end() || sigIt->second.returnType.kind == TypeKind::Void;
    if (!isVoid) {
        emitByte(LOAD);
        emitByte(255);
    } else {
        emitPush(0);
    }
}

void NVMCodeGen::generateStatement(ast::Statement& stmt) {
    if (auto* n = std::get_if<ast::VarDeclStmt>(&stmt.node)) {
        std::string structType;
        if (!n->varType.empty()) {
            auto t = checker_.resolveTypeString(n->varType);
            if (t.kind == TypeKind::Struct) structType = t.structName;
            if (t.kind == TypeKind::F64) {
                std::fprintf(stderr, "error: floating-point values are not supported by the nvm backend\n");
                std::exit(1);
            }
        }
        if (n->value) {
            if (auto* s = std::get_if<ast::StringExpr>(&n->value->node)) compileTimeStrings_[n->name] = s->value;
            if (auto* sl = std::get_if<ast::StructLiteralExpr>(&n->value->node)) structType = sl->structName;
            generateExpression(*n->value);
        } else {
            emitPush(0);
        }
        uint8_t slot = nextLocal_++;
        vars_[n->name] = VarInfo{false, slot, structType};
        emitByte(STORE_REL);
        emitByte(slot);
        return;
    }
    if (auto* n = std::get_if<ast::ArrayDeclStmt>(&stmt.node)) {
        needsHeapAlloc_ = true;
        emitPush(int32_t(n->size * 4));
        emitByte(CALL);
        emitJumpRef("func___heap_alloc");
        emitByte(POP);
        emitByte(LOAD);
        emitByte(255);

        uint8_t slot = nextLocal_++;
        VarInfo info{false, slot, ""};
        info.isArray = true;
        vars_[n->name] = info;
        emitByte(STORE_REL);
        emitByte(slot);
        return;
    }
    if (auto* n = std::get_if<ast::AssignmentStmt>(&stmt.node)) {
        generateExpression(n->value);
        auto it = vars_.find(n->name);
        if (it != vars_.end()) {
            emitByte(it->second.isArg ? STORE_ARG : STORE_REL);
            emitByte(it->second.index);
        }
        return;
    }
    if (auto* n = std::get_if<ast::ArrayAssignmentStmt>(&stmt.node)) {
        auto it = vars_.find(n->name);
        if (it == vars_.end() || !it->second.isArray) return;
        emitByte(it->second.isArg ? LOAD_ARG : LOAD_REL);
        emitByte(it->second.index);
        generateExpression(n->index);
        emitPush(4);
        emitByte(MUL);
        emitByte(ADD);
        generateExpression(n->value);
        storeHeapI32();
        return;
    }
    if (auto* n = std::get_if<ast::FieldAssignmentStmt>(&stmt.node)) {
        auto* id = std::get_if<ast::IdentifierExpr>(&n->object.node);
        if (!id) return;
        auto it = vars_.find(id->name);
        if (it == vars_.end() || it->second.structType.empty()) return;
        int offset = fieldOffset(it->second.structType, n->field);
        if (offset < 0) return;

        emitByte(it->second.isArg ? LOAD_ARG : LOAD_REL);
        emitByte(it->second.index);
        if (offset != 0) {
            emitPush(offset);
            emitByte(ADD);
        }
        generateExpression(n->value);
        storeHeapI32();
        return;
    }
    if (auto* n = std::get_if<ast::IfStmt>(&stmt.node)) {
        generateExpression(n->condition);
        std::string elseLbl = generateLabel("else");
        std::string endLbl = generateLabel("endif");
        emitByte(JZ);
        emitJumpRef(elseLbl);
        for (auto& s : n->thenBody) generateStatement(s);
        emitByte(JMP);
        emitJumpRef(endLbl);
        addLabel(elseLbl);
        if (n->elseBody) for (auto& s : *n->elseBody) generateStatement(s);
        addLabel(endLbl);
        return;
    }
    if (auto* n = std::get_if<ast::ForStmt>(&stmt.node)) {
        std::string startLbl = generateLabel("for_start");
        std::string endLbl = generateLabel("for_end");
        addLabel(startLbl);
        if (n->condition) {
            generateExpression(*n->condition);
            emitByte(JZ);
            emitJumpRef(endLbl);
        }
        loopLabelStack_.push_back({startLbl, endLbl});
        for (auto& s : n->body) generateStatement(s);
        loopLabelStack_.pop_back();
        emitByte(JMP);
        emitJumpRef(startLbl);
        addLabel(endLbl);
        return;
    }
    if (std::get_if<ast::BreakStmt>(&stmt.node)) {
        emitByte(JMP);
        emitJumpRef(loopLabelStack_.back().second);
        return;
    }
    if (std::get_if<ast::ContinueStmt>(&stmt.node)) {
        emitByte(JMP);
        emitJumpRef(loopLabelStack_.back().first);
        return;
    }
    if (auto* n = std::get_if<ast::ReturnStmt>(&stmt.node)) {
        if (inMain_) {
            if (n->value) generateExpression(*n->value);
            else emitPush(0);
            emitByte(SYSCALL);
            emitByte(SYS_EXIT);
        } else {
            if (n->value) {
                generateExpression(*n->value);
                emitByte(STORE);
                emitByte(255);
            }
            emitByte(LEAVE);
            emitByte(RET);
        }
        return;
    }
    if (auto* n = std::get_if<ast::ExpressionStmt>(&stmt.node)) {
        generateExpression(n->expr);
        emitByte(POP);
        return;
    }
    if (auto* n = std::get_if<ast::PointerAssignmentStmt>(&stmt.node)) {
        generateExpression(n->target);
        generateExpression(n->value);
        emitByte(STORE_ABS);
        return;
    }
    if (auto* n = std::get_if<ast::InlineAsmStmt>(&stmt.node)) {
        std::string asmText;
        for (auto& part : n->parts) {
            if (part.kind == ast::AsmPart::Kind::Literal) {
                asmText += part.text;
            } else {
                auto it = compileTimeStrings_.find(part.text);
                if (it != compileTimeStrings_.end()) {
                    asmText += it->second;
                    asmText += '\n';
                } else {
                    auto vit = vars_.find(part.text);
                    if (vit != vars_.end()) {
                        asmText += vit->second.isArg ? "load_arg " : "load_rel ";
                        asmText += std::to_string(vit->second.index) + "\n";
                    }
                }
            }
        }
        size_t pos = 0;
        while (pos < asmText.size()) {
            size_t nl = asmText.find('\n', pos);
            if (nl == std::string::npos) nl = asmText.size();
            std::string line = asmText.substr(pos, nl - pos);
            size_t start = line.find_first_not_of(" \t");
            if (start != std::string::npos) {
                std::string trimmed = line.substr(start);
                if (!trimmed.empty() && trimmed[0] != ';') {
                    size_t comment = trimmed.find(';');
                    if (comment != std::string::npos) trimmed = trimmed.substr(0, comment);
                    if (!trimmed.empty()) emitAsmInstruction(trimmed);
                }
            }
            pos = nl + 1;
        }
        return;
    }
    if (auto* n = std::get_if<ast::ComptimeStmt>(&stmt.node)) {
        for (auto& s : n->body) generateStatement(s);
        return;
    }
}

void NVMCodeGen::generateExpression(ast::Expression& expr) {
    if (auto* n = std::get_if<ast::NumberExpr>(&expr.node)) {
        emitPush(int32_t(n->value));
        return;
    }
    if (std::get_if<ast::FloatExpr>(&expr.node)) {
        std::fprintf(stderr, "error: floating-point values are not supported by the nvm backend\n");
        std::exit(1);
    }
    if (auto* n = std::get_if<ast::StringExpr>(&expr.node)) {
        materializeString(n->value);
        return;
    }
    if (std::get_if<ast::TemplateStringExpr>(&expr.node)) {
        std::fprintf(stderr, "error: template string interpolation ('$(...)') is not supported by the nvm backend "
                              "(use string.concat)\n");
        std::exit(1);
    }
    if (auto* n = std::get_if<ast::IdentifierExpr>(&expr.node)) {
        auto it = vars_.find(n->name);
        if (it != vars_.end()) {
            emitByte(it->second.isArg ? LOAD_ARG : LOAD_REL);
            emitByte(it->second.index);
        } else if (checker_.functions().count(n->name)) {
            std::fprintf(stderr, "error: referencing function '%s' as a value is not supported by the nvm backend\n",
                         n->name.c_str());
            std::exit(1);
        } else {
            emitPush(0);
        }
        return;
    }
    if (auto* n = std::get_if<ast::BinaryExpr>(&expr.node)) {
        if (n->op == ast::BinaryOp::Concat) {
            std::fprintf(stderr, "error: the '++' string concatenation operator is not supported by the nvm backend "
                                  "(use string.concat instead)\n");
            std::exit(1);
        }
        generateExpression(*n->left);
        generateExpression(*n->right);
        switch (n->op) {
            case ast::BinaryOp::Add: emitByte(ADD); break;
            case ast::BinaryOp::Sub: emitByte(SUB); break;
            case ast::BinaryOp::Mul: emitByte(MUL); break;
            case ast::BinaryOp::Div: emitByte(DIV); break;
            case ast::BinaryOp::Mod: emitByte(MOD); break;
            case ast::BinaryOp::Equal: emitByte(EQ); break;
            case ast::BinaryOp::NotEqual: emitByte(NEQ); break;
            case ast::BinaryOp::Less: emitByte(LT); break;
            case ast::BinaryOp::Greater: emitByte(GT); break;
            case ast::BinaryOp::LessEqual: emitByte(GT); emitPush(0); emitByte(EQ); break;
            case ast::BinaryOp::GreaterEqual: emitByte(LT); emitPush(0); emitByte(EQ); break;
            case ast::BinaryOp::BitAnd: emitByte(AND); break;
            case ast::BinaryOp::BitOr: emitByte(OR); break;
            case ast::BinaryOp::BitXor: emitByte(XOR); break;
            case ast::BinaryOp::Shl: emitByte(SHL); break;
            case ast::BinaryOp::Shr: emitByte(SAR); break;
            default: break;
        }
        return;
    }
    if (auto* n = std::get_if<ast::UnaryExpr>(&expr.node)) {
        generateExpression(*n->operand);
        if (n->op == ast::UnaryOp::Neg) {
            emitPush(0);
            emitByte(SWAP);
            emitByte(SUB);
        } else {
            emitPush(0);
            emitByte(EQ);
        }
        return;
    }
    if (auto* n = std::get_if<ast::CallExpr>(&expr.node)) {
        generateCall(n->function, n->args);
        return;
    }
    if (auto* n = std::get_if<ast::MethodCallExpr>(&expr.node)) {
        generateMethodCall(*n);
        return;
    }
    if (auto* n = std::get_if<ast::DerefExpr>(&expr.node)) {
        generateExpression(*n->operand);
        emitByte(LOAD_ABS);
        return;
    }
    if (std::get_if<ast::AddressOfExpr>(&expr.node)) {
        std::fprintf(stderr, "error: '&' has no valid address to take in the nvm backend (in %s)\n",
                     currentFunction_.c_str());
        std::exit(1);
    }
    if (std::get_if<ast::FunctionLiteralExpr>(&expr.node)) {
        std::fprintf(stderr, "error: function values/closures are not supported by the nvm backend (in %s)\n",
                     currentFunction_.c_str());
        std::exit(1);
    }
    if (auto* n = std::get_if<ast::EvalExpr>(&expr.node)) {
        auto* s = std::get_if<ast::StringExpr>(&n->instruction->node);
        if (!s) {
            std::fprintf(stderr, "error: 'eval' argument must be a string literal (in %s)\n",
                         currentFunction_.c_str());
            std::exit(1);
        }
        emitAsmInstruction(s->value);
        emitPush(0);
        return;
    }
    if (auto* n = std::get_if<ast::FieldAccessExpr>(&expr.node)) {
        auto* id = std::get_if<ast::IdentifierExpr>(&n->object->node);
        if (id) {
            auto it = vars_.find(id->name);
            if (it != vars_.end() && !it->second.structType.empty()) {
                int offset = fieldOffset(it->second.structType, n->field);
                if (offset >= 0) {
                    emitByte(it->second.isArg ? LOAD_ARG : LOAD_REL);
                    emitByte(it->second.index);
                    if (offset != 0) {
                        emitPush(offset);
                        emitByte(ADD);
                    }
                    emitByte(LOAD_HEAP);
                    return;
                }
            }
        }
        emitPush(0);
        return;
    }
    if (auto* n = std::get_if<ast::StructLiteralExpr>(&expr.node)) {
        generateStructLiteral(*n);
        return;
    }
    if (auto* n = std::get_if<ast::ArrayAccessExpr>(&expr.node)) {
        auto it = vars_.find(n->name);
        if (it != vars_.end() && it->second.isArray) {
            emitByte(it->second.isArg ? LOAD_ARG : LOAD_REL);
            emitByte(it->second.index);
            generateExpression(*n->index);
            emitPush(4);
            emitByte(MUL);
            emitByte(ADD);
            emitByte(LOAD_HEAP);
            return;
        }
        emitPush(0);
        return;
    }
    if (auto* n = std::get_if<ast::StringIndexExpr>(&expr.node)) {
        generateExpression(*n->str);
        generateExpression(*n->index);
        emitByte(ADD);
        emitByte(LOAD_HEAP);
        emitPush(24);
        emitByte(SHR);
        emitPush(0xFF);
        emitByte(AND);
        return;
    }
    emitPush(0);
}

namespace {

bool isValidInt(const std::string& s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); i++) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

} // namespace

void NVMCodeGen::emitAsmInstruction(const std::string& line) {
    size_t start = line.find_first_not_of(' ');
    if (start == std::string::npos) return;
    size_t end = line.find(' ', start);
    std::string instr = line.substr(start, end == std::string::npos ? std::string::npos : end - start);
    for (auto& c : instr) c = char(std::tolower(static_cast<unsigned char>(c)));

    std::string rest = end == std::string::npos ? "" : line.substr(end + 1);
    size_t restStart = rest.find_first_not_of(' ');
    if (restStart != std::string::npos) rest = rest.substr(restStart);
    else rest.clear();

    auto requireInt = [&](const std::string& op) -> int32_t {
        if (!isValidInt(rest)) {
            std::fprintf(stderr, "error: '%s' needs an integer operand, got '%s' (in %s)\n", op.c_str(), rest.c_str(),
                         currentFunction_.c_str());
            std::exit(1);
        }
        return std::atoi(rest.c_str());
    };

    if (instr == "push") emitPush(requireInt("push"));
    else if (instr == "pop") emitByte(POP);
    else if (instr == "dup") emitByte(DUP);
    else if (instr == "swap") emitByte(SWAP);
    else if (instr == "add") emitByte(ADD);
    else if (instr == "sub") emitByte(SUB);
    else if (instr == "mul") emitByte(MUL);
    else if (instr == "div") emitByte(DIV);
    else if (instr == "mod") emitByte(MOD);
    else if (instr == "ret") emitByte(RET);
    else if (instr == "leave") emitByte(LEAVE);
    else if (instr == "halt") emitByte(HALT);
    else if (instr == "nop") emitByte(NOP);
    else if (instr == "and") emitByte(AND);
    else if (instr == "or") emitByte(OR);
    else if (instr == "xor") emitByte(XOR);
    else if (instr == "not") emitByte(NOT);
    else if (instr == "shl") emitByte(SHL);
    else if (instr == "shr") emitByte(SHR);
    else if (instr == "sar") emitByte(SAR);
    else if (instr == "load_arg") { emitByte(LOAD_ARG); emitByte(uint8_t(requireInt("load_arg"))); }
    else if (instr == "store_arg") { emitByte(STORE_ARG); emitByte(uint8_t(requireInt("store_arg"))); }
    else if (instr == "load_rel") { emitByte(LOAD_REL); emitByte(uint8_t(requireInt("load_rel"))); }
    else if (instr == "store_rel") { emitByte(STORE_REL); emitByte(uint8_t(requireInt("store_rel"))); }
    else if (instr == "load") { emitByte(LOAD); emitByte(uint8_t(requireInt("load"))); }
    else if (instr == "store") { emitByte(STORE); emitByte(uint8_t(requireInt("store"))); }
    else if (instr == "syscall") {
        emitByte(SYSCALL);
        std::string lower = rest;
        for (auto& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        uint8_t num;
        if (lower == "exit") num = SYS_EXIT;
        else if (lower == "spawn") num = SYS_SPAWN;
        else if (lower == "open") num = SYS_OPEN;
        else if (lower == "read") num = SYS_READ;
        else if (lower == "write") num = SYS_WRITE;
        else if (lower == "remove") num = SYS_REMOVE;
        else if (lower == "sbrk") num = SYS_SBRK;
        else if (isValidInt(rest)) num = uint8_t(std::atoi(rest.c_str()));
        else {
            std::fprintf(stderr, "error: unknown syscall '%s' in inline asm (in %s)\n", rest.c_str(),
                         currentFunction_.c_str());
            std::exit(1);
        }
        emitByte(num);
    } else {
        std::fprintf(stderr, "error: unknown inline asm instruction '%s' (in %s)\n", instr.c_str(),
                     currentFunction_.c_str());
        std::exit(1);
    }
}

void NVMCodeGen::emitHeapAllocHelper() {
    addLabel("func___heap_alloc");
    currentFunction_ = "__heap_alloc";
    emitByte(ENTER);
    size_t enterPos = bytecode_.size();
    emitByte(0);

    nextLocal_ = 0;
    uint8_t offsetLocal = nextLocal_++;

    emitByte(LOAD);
    emitByte(253);
    emitByte(STORE_REL);
    emitByte(offsetLocal);

    emitByte(LOAD);
    emitByte(253);
    emitByte(LOAD_ARG);
    emitByte(0);
    emitByte(ADD);
    emitByte(STORE);
    emitByte(253);

    emitByte(LOAD_ARG);
    emitByte(0);
    emitByte(SYSCALL);
    emitByte(SYS_SBRK);

    emitByte(LOAD_REL);
    emitByte(offsetLocal);
    emitByte(STORE);
    emitByte(255);

    bytecode_[enterPos] = uint8_t(nextLocal_);
    emitByte(LEAVE);
    emitByte(RET);
}

void NVMCodeGen::emitPrintIntHelper() {
    needsHeapAlloc_ = true;
    addLabel("func___print_int");
    currentFunction_ = "__print_int";
    emitByte(ENTER);
    size_t enterPos = bytecode_.size();
    emitByte(0);
    nextLocal_ = 0;

    uint8_t valueLocal = nextLocal_++;
    uint8_t bufLocal = nextLocal_++;
    uint8_t posLocal = nextLocal_++;
    uint8_t negLocal = nextLocal_++;

    emitByte(LOAD_ARG);
    emitByte(0);
    emitPush(0);
    emitByte(LT);
    emitByte(STORE_REL);
    emitByte(negLocal);

    emitByte(LOAD_ARG);
    emitByte(0);
    emitByte(STORE_REL);
    emitByte(valueLocal);

    std::string skipNegate = generateLabel("skip_negate");
    emitByte(LOAD_REL);
    emitByte(negLocal);
    emitByte(JZ);
    emitJumpRef(skipNegate);
    emitPush(0);
    emitByte(LOAD_REL);
    emitByte(valueLocal);
    emitByte(SUB);
    emitByte(STORE_REL);
    emitByte(valueLocal);
    addLabel(skipNegate);

    emitPush(17);
    emitByte(CALL);
    emitJumpRef("func___heap_alloc");
    emitByte(POP);
    emitByte(LOAD);
    emitByte(255);
    emitByte(STORE_REL);
    emitByte(bufLocal);

    emitByte(LOAD_REL);
    emitByte(bufLocal);
    emitPush(16);
    emitByte(ADD);
    emitPush(0);
    emitByte(STORE_HEAP);

    emitPush(16);
    emitByte(STORE_REL);
    emitByte(posLocal);

    std::string digitLoop = generateLabel("digit_loop");
    std::string digitDone = generateLabel("digit_done");
    std::string afterZero = generateLabel("after_zero");
    std::string skipSign = generateLabel("skip_sign");

    emitByte(LOAD_REL);
    emitByte(valueLocal);
    emitPush(0);
    emitByte(EQ);
    emitByte(JZ);
    emitJumpRef(digitLoop);

    emitByte(LOAD_REL);
    emitByte(posLocal);
    emitPush(1);
    emitByte(SUB);
    emitByte(STORE_REL);
    emitByte(posLocal);
    emitByte(LOAD_REL);
    emitByte(bufLocal);
    emitByte(LOAD_REL);
    emitByte(posLocal);
    emitByte(ADD);
    emitPush('0');
    emitByte(STORE_HEAP);
    emitByte(JMP);
    emitJumpRef(afterZero);

    addLabel(digitLoop);
    emitByte(LOAD_REL);
    emitByte(valueLocal);
    emitPush(0);
    emitByte(EQ);
    emitByte(JNZ);
    emitJumpRef(digitDone);

    emitByte(LOAD_REL);
    emitByte(posLocal);
    emitPush(1);
    emitByte(SUB);
    emitByte(STORE_REL);
    emitByte(posLocal);

    emitByte(LOAD_REL);
    emitByte(bufLocal);
    emitByte(LOAD_REL);
    emitByte(posLocal);
    emitByte(ADD);
    emitByte(LOAD_REL);
    emitByte(valueLocal);
    emitPush(10);
    emitByte(MOD);
    emitPush('0');
    emitByte(ADD);
    emitByte(STORE_HEAP);

    emitByte(LOAD_REL);
    emitByte(valueLocal);
    emitPush(10);
    emitByte(DIV);
    emitByte(STORE_REL);
    emitByte(valueLocal);

    emitByte(JMP);
    emitJumpRef(digitLoop);
    addLabel(digitDone);
    addLabel(afterZero);

    emitByte(LOAD_REL);
    emitByte(negLocal);
    emitPush(0);
    emitByte(EQ);
    emitByte(JNZ);
    emitJumpRef(skipSign);
    emitByte(LOAD_REL);
    emitByte(posLocal);
    emitPush(1);
    emitByte(SUB);
    emitByte(STORE_REL);
    emitByte(posLocal);
    emitByte(LOAD_REL);
    emitByte(bufLocal);
    emitByte(LOAD_REL);
    emitByte(posLocal);
    emitByte(ADD);
    emitPush('-');
    emitByte(STORE_HEAP);
    addLabel(skipSign);

    emitPush(1);
    emitByte(LOAD_REL);
    emitByte(bufLocal);
    emitByte(LOAD_REL);
    emitByte(posLocal);
    emitByte(ADD);
    emitByte(SYSCALL);
    emitByte(SYS_WRITE);
    emitByte(POP);

    bytecode_[enterPos] = uint8_t(nextLocal_);
    emitByte(LEAVE);
    emitByte(RET);
}

} // namespace agn::backend::nvm
