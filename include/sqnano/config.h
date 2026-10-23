/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Compile-time resource limits.
 *
 * Under Zephyr every limit comes from Kconfig. Host builds use the defaults
 * below, which can be overridden with compiler definitions. Every structure
 * that depends on a limit is sized statically: the worst case is known at
 * build time.
 */

#ifndef SQNANO_CONFIG_H_
#define SQNANO_CONFIG_H_

#if defined(__ZEPHYR__)
#define SQNANO_MAX_PAGE_SIZE      CONFIG_SQNANO_MAX_PAGE_SIZE
#define SQNANO_PAGE_CACHE_COUNT   CONFIG_SQNANO_PAGE_CACHE_COUNT
#define SQNANO_MAX_PAGES          CONFIG_SQNANO_MAX_PAGES
#define SQNANO_MAX_READERS        CONFIG_SQNANO_MAX_READERS
#define SQNANO_MAX_DEPTH          CONFIG_SQNANO_MAX_DEPTH
#define SQNANO_MAX_KEY_SIZE       CONFIG_SQNANO_MAX_KEY_SIZE
#define SQNANO_MAX_RECORD_SIZE    CONFIG_SQNANO_MAX_RECORD_SIZE
#define SQNANO_JOURNAL_MAX_SLOTS  CONFIG_SQNANO_JOURNAL_MAX_SLOTS
#define SQNANO_SORT_RAM_SIZE      CONFIG_SQNANO_SORT_RAM_SIZE
#define SQNANO_SORT_BUFFERS       CONFIG_SQNANO_SORT_BUFFERS
#define SQNANO_SCRATCH_SIZE       CONFIG_SQNANO_SCRATCH_SIZE
#define SQNANO_AGG_TEXT_SIZE      CONFIG_SQNANO_AGG_TEXT_SIZE
#define SQNANO_MAX_CONNECTIONS    CONFIG_SQNANO_MAX_CONNECTIONS
#define SQNANO_MAX_STMTS          CONFIG_SQNANO_MAX_STMTS
#define SQNANO_STMT_POOL_SIZE     CONFIG_SQNANO_STMT_POOL_SIZE
#ifdef CONFIG_SQNANO_SQLITE_COMPAT
#define SQNANO_POOL_COMPAT CONFIG_SQNANO_MAX_STMTS
#else
#define SQNANO_POOL_COMPAT 0
#endif
#ifdef CONFIG_SQNANO_SHELL
#define SQNANO_POOL_SHELL 1
#else
#define SQNANO_POOL_SHELL 0
#endif
#ifdef CONFIG_SQNANO_REMOTE
#define SQNANO_POOL_REMOTE 1
#else
#define SQNANO_POOL_REMOTE 0
#endif
/* Statements alive at once: sqlite3.h handles, one for the shell, one for the remote server. */
#define SQNANO_POOL_SLOTS (SQNANO_POOL_COMPAT + SQNANO_POOL_SHELL + SQNANO_POOL_REMOTE)
#define SQNANO_PARAM_BUF_SIZE     CONFIG_SQNANO_PARAM_BUF_SIZE
#define SQNANO_MAX_DATABASES      CONFIG_SQNANO_MAX_DATABASES
#define SQNANO_YIELD_ROWS         CONFIG_SQNANO_YIELD_ROWS
#define SQNANO_MAX_COLUMNS        CONFIG_SQNANO_MAX_COLUMNS
#define SQNANO_MAX_SAVEPOINTS     CONFIG_SQNANO_MAX_SAVEPOINTS
#ifdef CONFIG_SQNANO_SORT_EXTERNAL
#define SQNANO_SORT_MAX_RUNS CONFIG_SQNANO_SORT_MAX_RUNS
#else
#define SQNANO_SORT_MAX_RUNS 0
#endif
#ifdef CONFIG_SQNANO_FOREIGN_KEYS
#define SQNANO_FOREIGN_KEYS 1
#define SQNANO_FK_MAX_DEPTH CONFIG_SQNANO_FK_MAX_DEPTH
#else
#define SQNANO_FOREIGN_KEYS 0
#define SQNANO_FK_MAX_DEPTH 0
#endif
#else
#ifndef SQNANO_MAX_SAVEPOINTS
#define SQNANO_MAX_SAVEPOINTS 8
#endif
#ifndef SQNANO_FOREIGN_KEYS
#define SQNANO_FOREIGN_KEYS 1
#endif
#ifndef SQNANO_FK_MAX_DEPTH
#define SQNANO_FK_MAX_DEPTH 4
#endif

#ifndef SQNANO_MAX_COLUMNS
#define SQNANO_MAX_COLUMNS 32
#endif
#ifndef SQNANO_MAX_PAGE_SIZE
#define SQNANO_MAX_PAGE_SIZE 4096
#endif
#ifndef SQNANO_PAGE_CACHE_COUNT
#define SQNANO_PAGE_CACHE_COUNT 12
#endif
#ifndef SQNANO_MAX_PAGES
#define SQNANO_MAX_PAGES 2048
#endif
#ifndef SQNANO_MAX_READERS
#define SQNANO_MAX_READERS 4
#endif
#ifndef SQNANO_MAX_DEPTH
#define SQNANO_MAX_DEPTH 12
#endif
#ifndef SQNANO_MAX_KEY_SIZE
#define SQNANO_MAX_KEY_SIZE 128
#endif
#ifndef SQNANO_MAX_RECORD_SIZE
#define SQNANO_MAX_RECORD_SIZE 512
#endif
#ifndef SQNANO_JOURNAL_MAX_SLOTS
#define SQNANO_JOURNAL_MAX_SLOTS 32
#endif
#ifndef SQNANO_SORT_RAM_SIZE
#define SQNANO_SORT_RAM_SIZE 8192
#endif
#ifndef SQNANO_SORT_BUFFERS
#define SQNANO_SORT_BUFFERS 2
#endif
#ifndef SQNANO_SORT_MAX_RUNS
#define SQNANO_SORT_MAX_RUNS 16
#endif
#ifndef SQNANO_SCRATCH_SIZE
#define SQNANO_SCRATCH_SIZE 512
#endif
#ifndef SQNANO_AGG_TEXT_SIZE
#define SQNANO_AGG_TEXT_SIZE 64
#endif
#ifndef SQNANO_MAX_CONNECTIONS
#define SQNANO_MAX_CONNECTIONS 4
#endif
#ifndef SQNANO_MAX_STMTS
#define SQNANO_MAX_STMTS 8
#endif
#ifndef SQNANO_STMT_POOL_SIZE
#define SQNANO_STMT_POOL_SIZE 0
#endif
#ifndef SQNANO_POOL_SLOTS
#define SQNANO_POOL_SLOTS (SQNANO_MAX_STMTS + 2)
#endif
#ifndef SQNANO_PARAM_BUF_SIZE
#define SQNANO_PARAM_BUF_SIZE 256
#endif
#ifndef SQNANO_MAX_DATABASES
#define SQNANO_MAX_DATABASES 4
#endif
#ifndef SQNANO_YIELD_ROWS
#define SQNANO_YIELD_ROWS 32
#endif
#endif

/** Size of a page header in bytes. */
#define SQNANO_PAGE_HDR 20
/** Size of a journal slot header in bytes. */
#define SQNANO_JRNL_HDR 16
/** Smallest supported page size. */
#define SQNANO_MIN_PAGE_SIZE 128
/** Version of the on-media format, stored in each superblock. */
#define SQNANO_FORMAT_VERSION 2

#if SQNANO_MAX_PAGE_SIZE < SQNANO_MIN_PAGE_SIZE || SQNANO_MAX_PAGE_SIZE > 32768
#error "SQNANO_MAX_PAGE_SIZE must be between 128 and 32768"
#endif
#if SQNANO_PAGE_CACHE_COUNT < 6
#error "SQNANO_PAGE_CACHE_COUNT must be at least 6"
#endif
#if SQNANO_MAX_SAVEPOINTS > 15
#error "SQNANO_MAX_SAVEPOINTS must be at most 15"
#endif
#if SQNANO_SORT_RAM_SIZE > 65535
#error "SQNANO_SORT_RAM_SIZE must fit in 16 bits"
#endif
#if SQNANO_SORT_MAX_RUNS == 1 || SQNANO_SORT_MAX_RUNS > 64
#error "SQNANO_SORT_MAX_RUNS must be 0 (no external sort) or between 2 and 64"
#endif

#endif /* SQNANO_CONFIG_H_ */
