# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

function(sqnano_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sqnano)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(${name} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
endfunction()

# Engine: B+tree against a model, snapshots, space reclamation.
sqnano_test(test_engine test_engine.c)
add_test(NAME engine COMMAND test_engine)

# Power-cut fuzzing on NOR and FRAM: a cut during every write and erase.
sqnano_test(test_powercut test_powercut.c)
add_test(NAME powercut COMMAND test_powercut)

# sqlite3.h facade and typed API on the sample application.
sqnano_test(test_sqlite test_sqlite.c)
sqnano_queries(TARGET test_sqlite
  SCHEMA app/schema
  QUERIES app/queries.sql
  EXTRACT test_sqlite.c
  WRAPPERS exec_ok:2)
add_test(NAME sqlite3_facade COMMAND test_sqlite)
# The same with a first-fit arena just above the largest state.
sqnano_test(test_sqlite_arena test_sqlite.c)
target_compile_definitions(test_sqlite_arena PRIVATE SQNANO_STMT_POOL_SIZE=4096)
sqnano_queries(TARGET test_sqlite_arena
  SCHEMA app/schema
  QUERIES app/queries.sql
  EXTRACT test_sqlite.c
  WRAPPERS exec_ok:2)
add_test(NAME sqlite3_facade_arena COMMAND test_sqlite_arena)

# File media (LittleFS model): power cuts during every file update, POSIX files.
sqnano_test(test_file_media test_file_media.c)
add_test(NAME file_media COMMAND test_file_media)

# Hybrid media (NOR behind a FRAM): power cuts on both memories, flash wear.
sqnano_test(test_hybrid test_hybrid.c)
add_test(NAME hybrid COMMAND test_hybrid)

# NAND media: power cuts, worn blocks, bit flips, NAND program rules.
sqnano_test(test_nand test_nand.c)
add_test(NAME nand COMMAND test_nand)

# Statement state pool.
sqnano_test(test_pool test_pool.c)
add_test(NAME pool COMMAND test_pool)

# Diagnostic shell.
sqnano_test(test_shell test_shell.c)
sqnano_queries(TARGET test_shell SCHEMA app/schema QUERIES app/queries.sql)
add_test(NAME shell COMMAND test_shell)

# Differential test against SQLite. The runner gets a large sort buffer.
add_library(sqnano_big STATIC ${SQNANO_ENGINE_SOURCES_ABS})
target_include_directories(sqnano_big PUBLIC ${PROJECT_SOURCE_DIR}/include PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(sqnano_big PUBLIC SQNANO_SORT_RAM_SIZE=65000 _POSIX_C_SOURCE=200809L)
target_link_libraries(sqnano_big PUBLIC Threads::Threads m)
set_target_properties(sqnano_big PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
add_executable(diff_runner diff_runner.c)
target_link_libraries(diff_runner PRIVATE sqnano_big)
sqnano_queries(TARGET diff_runner SCHEMA diff/schema QUERIES diff/queries.sql)
# Generated code must stay warning-free: Zephyr builds it with -Werror.
target_compile_options(diff_runner PRIVATE -Wall -Wextra -Wshadow -Wno-unused-parameter -Werror)
foreach(media nor fram nor4k)
  set(extra)
  if(media STREQUAL "nor4k")
    # Small flash: a statement may run out of erased space (see difftest.py).
    set(extra --tolerate-full)
  endif()
  foreach(seed 1 2)
    add_test(NAME difftest_${media}_${seed}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/difftest.py $<TARGET_FILE:diff_runner>
              --seed ${seed} --rounds 3000 --media ${media} ${extra})
  endforeach()
endforeach()

# The same with a small sort buffer: large sorts go through the external sort,
# with merges in several passes.
add_library(sqnano_smallsort STATIC ${SQNANO_ENGINE_SOURCES_ABS})
target_include_directories(sqnano_smallsort PUBLIC ${PROJECT_SOURCE_DIR}/include PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(sqnano_smallsort PUBLIC SQNANO_SORT_RAM_SIZE=4096 SQNANO_SORT_MAX_RUNS=4
  _POSIX_C_SOURCE=200809L)
target_link_libraries(sqnano_smallsort PUBLIC Threads::Threads m)
set_target_properties(sqnano_smallsort PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
add_executable(diff_runner_smallsort diff_runner.c)
target_link_libraries(diff_runner_smallsort PRIVATE sqnano_smallsort)
sqnano_queries(TARGET diff_runner_smallsort SCHEMA diff/schema QUERIES diff/queries.sql)
foreach(media nor fram)
  add_test(NAME difftest_smallsort_${media}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/difftest.py
            $<TARGET_FILE:diff_runner_smallsort> --seed 3 --rounds 3000 --media ${media})
endforeach()

# Remote cursor server and sqnano-cli, through SMP packets on a pipe.
sqnano_test(remote_server remote_server.c)
sqnano_queries(TARGET remote_server SCHEMA app/schema QUERIES app/queries.sql)
add_test(NAME remote_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_remote.py $<TARGET_FILE:remote_server>)

# OTA migrations: five firmware generations on the same database image; fw5b
# is fw5 built to migrate from v5 only (it lacks the lazy conversion of v4).
foreach(v 1 2 3 4 5 5b)
  string(SUBSTRING ${v} 0 1 ver)
  sqnano_test(test_migrate_fw${v} test_migrate.c)
  target_compile_definitions(test_migrate_fw${v} PRIVATE FW=${ver})
  set(q QUERIES migrate/q1.sql)
  if(ver GREATER_EQUAL 2)
    list(APPEND q migrate/q2.sql)
  endif()
  if(ver GREATER_EQUAL 3)
    list(APPEND q migrate/q3.sql)
  endif()
  if(ver GREATER_EQUAL 4)
    list(APPEND q migrate/q4.sql)
  endif()
  set(from 1)
  if(v STREQUAL "5b")
    set(from 5)
    target_compile_definitions(test_migrate_fw${v} PRIVATE SHORT_WINDOW=1)
  endif()
  sqnano_queries(TARGET test_migrate_fw${v} SCHEMA migrate/schema VERSION ${ver} MIGRATE_FROM ${from} ${q})
endforeach()
add_test(NAME migrations
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/migrate_scenario.py
          $<TARGET_FILE:test_migrate_fw1> $<TARGET_FILE:test_migrate_fw2>
          $<TARGET_FILE:test_migrate_fw3> $<TARGET_FILE:test_migrate_fw4>
          $<TARGET_FILE:test_migrate_fw5> $<TARGET_FILE:test_migrate_fw5b>
          ${CMAKE_CURRENT_BINARY_DIR}/migrate.img)

# Host compiler: parser, catalog, normalization (Python and C), determinism.
add_executable(normalize_tool normalize_tool.c)
target_link_libraries(normalize_tool PRIVATE sqnano)
target_include_directories(normalize_tool PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME compiler
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_compiler.py
          $<TARGET_FILE:normalize_tool>)

# The differential runners use their own engine builds: sanitize them too.
if(SQNANO_SANITIZE)
  foreach(lib sqnano_big sqnano_smallsort)
    target_compile_options(${lib} PUBLIC -fsanitize=address,undefined -fno-sanitize-recover=all
                           -fno-omit-frame-pointer)
    target_link_options(${lib} PUBLIC -fsanitize=address,undefined)
  endforeach()
endif()
