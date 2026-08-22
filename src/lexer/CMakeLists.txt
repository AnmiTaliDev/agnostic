# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
option(AGNOSTIC_BOOTSTRAP
       "Build with the plain C++ lexer (stage 0) instead of the self-hosted lexer.agn" OFF)

if(AGNOSTIC_BOOTSTRAP)
  add_library(agn_lexer STATIC ${PROJECT_SOURCE_DIR}/old/lexer/lexer.cpp)
  target_include_directories(agn_lexer PUBLIC ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(agn_lexer PUBLIC agn_misc)
else()
  if(NOT AGNOSTIC_BOOTSTRAP_COMPILER)
    message(FATAL_ERROR
      "Building the self-hosted lexer needs a bootstrap compiler:\n"
      "  cmake -S . -B build-bootstrap -DAGNOSTIC_BOOTSTRAP=ON && cmake --build build-bootstrap\n"
      "  cmake -S . -B build -DAGNOSTIC_BOOTSTRAP_COMPILER=$PWD/build-bootstrap/src/cli/agnostic")
  endif()

  set(LEXER_AGN_OBJ ${CMAKE_CURRENT_BINARY_DIR}/lexer_agn.o)
  add_custom_command(
    OUTPUT ${LEXER_AGN_OBJ}
    COMMAND ${AGNOSTIC_BOOTSTRAP_COMPILER} ${CMAKE_CURRENT_SOURCE_DIR}/lexer.agn
            --backend=llvm --mem=manual -c --output=${CMAKE_CURRENT_BINARY_DIR}/lexer_agn
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lexer.agn
    COMMENT "Self-hosting: compiling lexer.agn with the bootstrap compiler"
    VERBATIM
  )
  add_custom_target(agn_lexer_agn_obj DEPENDS ${LEXER_AGN_OBJ})
  set_source_files_properties(${LEXER_AGN_OBJ} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

  add_library(agn_lexer STATIC lexer.cpp ${LEXER_AGN_OBJ})
  add_dependencies(agn_lexer agn_lexer_agn_obj)
  target_include_directories(agn_lexer PUBLIC ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(agn_lexer PUBLIC agn_misc agn_llvm_runtime agn_memory_manual agn_platform_linux)

  set(AGNOSTIC_EMBED_SHIM ${CMAKE_CURRENT_SOURCE_DIR}/agn_embed_shim.cpp PARENT_SCOPE)
endif()
