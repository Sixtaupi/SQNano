# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(SQNANO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
list(APPEND EXTRA_ZEPHYR_MODULES ${SQNANO_ROOT})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sqnano_tiny)

target_sources(app PRIVATE src/main.c)
zephyr_sqnano_queries(queries.sql SCHEMA schema)
