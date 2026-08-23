# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
add_library(agn_parser STATIC parser.cpp typechecker.cpp monomorphize.cpp comptime_eval.cpp)
target_include_directories(agn_parser PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(agn_parser PUBLIC agn_ast agn_lexer agn_misc)
