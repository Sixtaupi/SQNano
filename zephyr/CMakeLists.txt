# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_SQNANO)
  set(SQNANO_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

  # Makes zephyr_sqnano_queries() available to the application.
  include(${SQNANO_DIR}/cmake/sqnano.cmake)

  zephyr_library_named(sqnano)
  zephyr_include_directories(${SQNANO_DIR}/include)
  zephyr_library_include_directories(${SQNANO_DIR}/src)
  zephyr_library_sources(
    ${SQNANO_DIR}/src/core/util.c
    ${SQNANO_DIR}/src/core/value.c
    ${SQNANO_DIR}/src/core/exec.c
    ${SQNANO_DIR}/src/core/db.c
    ${SQNANO_DIR}/src/core/pool.c
    ${SQNANO_DIR}/src/btree/btree.c
    ${SQNANO_DIR}/src/btree/record.c
    ${SQNANO_DIR}/src/pager/pager.c
    ${SQNANO_DIR}/src/media/media_ram.c
    ${SQNANO_DIR}/src/zephyr/sqnano_dt.c)
  zephyr_library_sources_ifdef(CONFIG_SQNANO_MEDIA_FLASH ${SQNANO_DIR}/src/media/media_flash.c)
  zephyr_library_sources_ifdef(CONFIG_SQNANO_MEDIA_EEPROM ${SQNANO_DIR}/src/media/media_eeprom.c)
  zephyr_library_sources_ifdef(CONFIG_SQNANO_MEDIA_HYBRID ${SQNANO_DIR}/src/media/media_hybrid.c)
  zephyr_library_sources_ifdef(CONFIG_SQNANO_MEDIA_NAND
    ${SQNANO_DIR}/src/media/media_nand.c
    ${SQNANO_DIR}/src/media/nand_ram.c
    ${SQNANO_DIR}/src/zephyr/media_nand_flash.c)
  zephyr_library_sources_ifdef(CONFIG_SQNANO_MEDIA_FS
    ${SQNANO_DIR}/src/media/media_file.c
    ${SQNANO_DIR}/src/zephyr/media_fs.c)
  zephyr_library_sources_ifdef(CONFIG_SQNANO_SQLITE_COMPAT
    ${SQNANO_DIR}/src/compat/sqlite3.c
    ${SQNANO_DIR}/src/compat/normalize.c)
  zephyr_library_sources_ifdef(CONFIG_SQNANO_SHELL
    ${SQNANO_DIR}/src/shell/minisql.c
    ${SQNANO_DIR}/src/shell/shell_cmd.c)
  zephyr_library_sources_ifdef(CONFIG_SQNANO_REMOTE
    ${SQNANO_DIR}/src/remote/remote.c
    ${SQNANO_DIR}/src/zephyr/sqnano_smp.c)
endif()
