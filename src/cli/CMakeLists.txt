# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
add_executable(agnostic main.cpp)
if(AGNOSTIC_EMBED_SHIM)
  target_sources(agnostic PRIVATE ${AGNOSTIC_EMBED_SHIM})
endif()
target_include_directories(agnostic PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(agnostic PRIVATE agn_parser agn_backend_llvm agn_backend_nvm agn_backend_gcc)
target_compile_definitions(agnostic PRIVATE AGNOSTIC_VERSION="${AGNOSTIC_VERSION}")
