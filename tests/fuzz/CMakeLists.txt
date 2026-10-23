# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
#
# Fuzz targets. With clang and -DSQNANO_LIBFUZZER=ON they are libFuzzer
# binaries (CI); otherwise driver.c runs them on given inputs plus random
# mutations, and ctest runs a short smoke pass of each.

option(SQNANO_LIBFUZZER "Build the fuzz targets with libFuzzer (clang)" OFF)

# The engine without the page CRC check, so that corrupt pages reach the tree code.
add_library(sqnano_fuzz STATIC ${SQNANO_ENGINE_SOURCES_ABS})
target_include_directories(sqnano_fuzz PUBLIC ${PROJECT_SOURCE_DIR}/include
                           PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(sqnano_fuzz PUBLIC SQNANO_FUZZING=1 _POSIX_C_SOURCE=200809L)
target_link_libraries(sqnano_fuzz PUBLIC Threads::Threads m)

set(fuzz_flags -g -fno-omit-frame-pointer -fno-sanitize-recover=all)
if(SQNANO_LIBFUZZER)
  list(APPEND fuzz_flags -fsanitize=fuzzer-no-link,address,undefined)
  set(fuzz_link -fsanitize=fuzzer,address,undefined)
elseif(NOT SQNANO_SANITIZE)
  list(APPEND fuzz_flags -fsanitize=address,undefined)
  set(fuzz_link -fsanitize=address,undefined)
endif()
target_compile_options(sqnano_fuzz PUBLIC ${fuzz_flags})
target_link_options(sqnano_fuzz PUBLIC ${fuzz_link})

function(sqnano_fuzz_target name)
  if(SQNANO_LIBFUZZER)
    add_executable(${name} ${name}.c)
  else()
    add_executable(${name} ${name}.c driver.c)
  endif()
  target_link_libraries(${name} PRIVATE sqnano_fuzz)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src
                             ${PROJECT_SOURCE_DIR}/tests/host)
  if(NOT SQNANO_LIBFUZZER)
    add_test(NAME fuzz_${name} COMMAND ${name} -runs=3000 -seed=1
             ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name})
  endif()
endfunction()

sqnano_fuzz_target(fuzz_mount)
sqnano_fuzz_target(fuzz_keys)
sqnano_fuzz_target(fuzz_remote)
sqnano_queries(TARGET fuzz_remote SCHEMA ../host/app/schema QUERIES ../host/app/queries.sql)
sqnano_fuzz_target(fuzz_sql)
sqnano_queries(TARGET fuzz_sql SCHEMA ../host/app/schema QUERIES ../host/app/queries.sql
  EXTRACT fuzz_sql.c)
