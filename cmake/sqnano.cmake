# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

include(CMakeParseArguments)

get_filename_component(SQNANO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SQNANOC_DIR "${SQNANO_ROOT}/tools/sqnanoc" CACHE INTERNAL "sqnanoc location")

#[=======================================================================[.rst:
sqnano_queries
--------------

Compile the schema and the SQL statements of an application with the host
compiler ``sqnanoc`` and add the generated C code to a target::

  sqnano_queries(
    TARGET target
    SCHEMA schema-dir-or-file
    [QUERIES file.sql ...]        named queries ("-- name: x")
    [MANIFEST file.sql ...]       statements captured at run time
    [EXTRACT file.c ...]          sources scanned for sqlite3_prepare/exec literals
    [WRAPPERS name:arg ...]       application helpers taking SQL
    [VERSION n]                   build for schema version n (default: latest)
    [MIGRATE_FROM n]              oldest version the firmware migrates from
    [OUTPUT_DIR dir]
    [PREFIX name])                generated file names (default sqnano_gen)

The generated header (``<PREFIX>.h``) declares ``sqnano_app_schema`` and the
typed API of every named query.
#]=======================================================================]
function(sqnano_queries)
  set(one TARGET SCHEMA VERSION MIGRATE_FROM OUTPUT_DIR PREFIX)
  set(multi QUERIES MANIFEST EXTRACT WRAPPERS)
  cmake_parse_arguments(SQ "" "${one}" "${multi}" ${ARGN})
  if(SQ_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "sqnano_queries: unknown arguments: ${SQ_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT SQ_TARGET OR NOT TARGET ${SQ_TARGET})
    message(FATAL_ERROR "sqnano_queries: TARGET must name an existing target")
  endif()
  if(NOT SQ_SCHEMA)
    message(FATAL_ERROR "sqnano_queries: SCHEMA is required")
  endif()
  if(NOT SQ_PREFIX)
    set(SQ_PREFIX sqnano_gen)
  endif()
  if(NOT SQ_OUTPUT_DIR)
    set(SQ_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/sqnano/${SQ_TARGET})
  endif()
  if(NOT SQ_MIGRATE_FROM)
    set(SQ_MIGRATE_FROM 1)
  endif()
  if(NOT Python3_EXECUTABLE)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  endif()

  get_filename_component(schema ${SQ_SCHEMA} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  set(args compile --schema ${schema} --out ${SQ_OUTPUT_DIR} --prefix ${SQ_PREFIX}
      --migrate-from ${SQ_MIGRATE_FROM})
  set(deps)
  if(IS_DIRECTORY ${schema})
    file(GLOB schema_files ${schema}/*.sql)
    list(APPEND deps ${schema_files})
    # Re-run CMake when a schema version is added.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${schema})
  else()
    list(APPEND deps ${schema})
  endif()
  if(SQ_VERSION)
    list(APPEND args --version ${SQ_VERSION})
  endif()
  foreach(kind QUERIES MANIFEST EXTRACT)
    string(TOLOWER ${kind} opt)
    foreach(f ${SQ_${kind}})
      get_filename_component(abs ${f} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
      list(APPEND args --${opt} ${abs})
      list(APPEND deps ${abs})
    endforeach()
  endforeach()
  foreach(w ${SQ_WRAPPERS})
    list(APPEND args --wrapper ${w})
  endforeach()
  file(GLOB compiler_sources ${SQNANOC_DIR}/*.py)

  set(outputs ${SQ_OUTPUT_DIR}/${SQ_PREFIX}.c ${SQ_OUTPUT_DIR}/${SQ_PREFIX}.h)
  add_custom_command(
    OUTPUT ${outputs}
    COMMAND ${Python3_EXECUTABLE} ${SQNANOC_DIR} ${args}
    DEPENDS ${deps} ${compiler_sources}
    COMMENT "sqnanoc: compiling SQL for ${SQ_TARGET}"
    VERBATIM)
  target_sources(${SQ_TARGET} PRIVATE ${outputs})
  target_include_directories(${SQ_TARGET} PRIVATE ${SQ_OUTPUT_DIR})
endfunction()

#[=======================================================================[.rst:
zephyr_sqnano_queries
---------------------

Zephyr flavor, adding the generated code to the ``app`` target::

  zephyr_sqnano_queries(queries.sql [more.sql ...] SCHEMA schema/
                        [EXTRACT src/main.c ...] [WRAPPERS name:arg ...])

The migration window comes from CONFIG_SQNANO_MIGRATE_FROM_MIN.
#]=======================================================================]
function(zephyr_sqnano_queries)
  cmake_parse_arguments(ZQ "" "SCHEMA;VERSION;TARGET" "EXTRACT;MANIFEST;WRAPPERS" ${ARGN})
  if(NOT ZQ_TARGET)
    set(ZQ_TARGET app)
  endif()
  set(extra)
  if(ZQ_VERSION)
    list(APPEND extra VERSION ${ZQ_VERSION})
  endif()
  if(DEFINED CONFIG_SQNANO_MIGRATE_FROM_MIN)
    list(APPEND extra MIGRATE_FROM ${CONFIG_SQNANO_MIGRATE_FROM_MIN})
  endif()
  sqnano_queries(
    TARGET ${ZQ_TARGET}
    SCHEMA ${ZQ_SCHEMA}
    QUERIES ${ZQ_UNPARSED_ARGUMENTS}
    EXTRACT ${ZQ_EXTRACT}
    MANIFEST ${ZQ_MANIFEST}
    WRAPPERS ${ZQ_WRAPPERS}
    ${extra})
endfunction()
