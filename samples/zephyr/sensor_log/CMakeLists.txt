# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# When SQNano is not listed in the west manifest, point Zephyr at it.
if(NOT DEFINED ZEPHYR_SQNANO_MODULE_DIR)
  list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../..)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sqnano_sensor_log)

target_sources(app PRIVATE src/main.c)

# Compile the schema and the SQL of the application on the host. Literal SQL
# passed to sqlite3_prepare_v2()/sqlite3_exec() in main.c is extracted too.
zephyr_sqnano_queries(queries.sql SCHEMA schema EXTRACT src/main.c)
