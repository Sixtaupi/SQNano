# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
#
# Host build: engine library, host compiler integration and tests. Zephyr
# builds use zephyr/CMakeLists.txt instead.

cmake_minimum_required(VERSION 3.20)
project(sqnano VERSION 0.1.0 LANGUAGES C)

option(SQNANO_BUILD_TESTS "Build the SQNano host test suite" ON)
option(SQNANO_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

include(cmake/sqnano.cmake)

set(SQNANO_ENGINE_SOURCES
  src/core/util.c
  src/core/value.c
  src/core/exec.c
  src/core/db.c
  src/core/pool.c
  src/btree/btree.c
  src/btree/record.c
  src/pager/pager.c
  src/media/media_ram.c
  src/media/media_file.c
  src/media/media_hybrid.c
  src/media/media_nand.c
  src/media/nand_ram.c
  src/compat/sqlite3.c
  src/compat/normalize.c
  src/shell/minisql.c
  src/remote/remote.c)
list(TRANSFORM SQNANO_ENGINE_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/ OUTPUT_VARIABLE SQNANO_ENGINE_SOURCES_ABS)

find_package(Threads REQUIRED)

add_library(sqnano STATIC ${SQNANO_ENGINE_SOURCES})
target_include_directories(sqnano PUBLIC include)
target_include_directories(sqnano PRIVATE src)
target_link_libraries(sqnano PUBLIC Threads::Threads m)
set_target_properties(sqnano PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(sqnano PUBLIC _POSIX_C_SOURCE=200809L)
target_compile_options(sqnano PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)
if(SQNANO_SANITIZE)
  target_compile_options(sqnano PUBLIC -fsanitize=address,undefined -fno-sanitize-recover=all
                         -fno-omit-frame-pointer)
  target_link_options(sqnano PUBLIC -fsanitize=address,undefined)
endif()

if(SQNANO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/host)
  add_subdirectory(tests/fuzz)
endif()
