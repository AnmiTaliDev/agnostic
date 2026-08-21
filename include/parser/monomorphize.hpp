// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#pragma once

#include "ast/ast.hpp"

namespace agn::parser {

void monomorphizeGenerics(ast::Program& program);

} // namespace agn::parser
