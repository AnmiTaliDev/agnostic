# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
find_path(GCCJIT_INCLUDE_DIR NAMES libgccjit.h)
find_library(GCCJIT_LIBRARY NAMES gccjit)

if(NOT GCCJIT_INCLUDE_DIR)
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=libgccjit.h
    OUTPUT_VARIABLE GCCJIT_HEADER_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(GCCJIT_HEADER_PATH AND NOT GCCJIT_HEADER_PATH STREQUAL "libgccjit.h" AND EXISTS "${GCCJIT_HEADER_PATH}")
    get_filename_component(GCCJIT_INCLUDE_DIR "${GCCJIT_HEADER_PATH}" DIRECTORY)
  endif()
endif()

if(NOT GCCJIT_LIBRARY)
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=libgccjit.so
    OUTPUT_VARIABLE GCCJIT_LIB_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(GCCJIT_LIB_PATH AND NOT GCCJIT_LIB_PATH STREQUAL "libgccjit.so" AND EXISTS "${GCCJIT_LIB_PATH}")
    set(GCCJIT_LIBRARY "${GCCJIT_LIB_PATH}")
  endif()
endif()

if(NOT GCCJIT_INCLUDE_DIR)
  file(GLOB GCCJIT_HEADER_CANDIDATES /usr/lib/gcc/*/*/include/libgccjit.h)
  if(GCCJIT_HEADER_CANDIDATES)
    list(GET GCCJIT_HEADER_CANDIDATES 0 GCCJIT_HEADER_CANDIDATE)
    get_filename_component(GCCJIT_INCLUDE_DIR "${GCCJIT_HEADER_CANDIDATE}" DIRECTORY)
  endif()
endif()
if(NOT GCCJIT_LIBRARY)
  file(GLOB GCCJIT_LIB_CANDIDATES /usr/lib/gcc/*/*/libgccjit.so /usr/lib/*/libgccjit.so.*)
  if(GCCJIT_LIB_CANDIDATES)
    list(GET GCCJIT_LIB_CANDIDATES 0 GCCJIT_LIBRARY)
  endif()
endif()

if(NOT GCCJIT_INCLUDE_DIR OR NOT GCCJIT_LIBRARY)
  message(FATAL_ERROR "libgccjit not found (need libgccjit.h and libgccjit.so)")
endif()

add_library(agn_backend_gcc STATIC backend.cpp)
target_include_directories(agn_backend_gcc PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_include_directories(agn_backend_gcc PRIVATE ${GCCJIT_INCLUDE_DIR})
target_link_libraries(agn_backend_gcc PUBLIC agn_ast agn_parser PRIVATE ${GCCJIT_LIBRARY})
