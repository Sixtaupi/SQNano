/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Interactive diagnostic interpreter (the shell "db" command).
 *
 * Runs a restricted SQL directly on the target, without any compiled code:
 * one table, AND of column/literal comparisons, ORDER BY on an index, simple
 * aggregates, LIMIT. The statement is translated into a declarative filter
 * structure (table, index, key range, filters, columns, limit, aggregate)
 * whose cost is known before execution. Precompiled queries are available
 * through ".run". Everything else is refused with a suggestion.
 */

#ifndef SQNANO_SHELL_H_
#define SQNANO_SHELL_H_

#include <stdbool.h>
#include <stdint.h>

#include <sqnano/sqnano.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sqnano_shell_mode {
	SQNANO_SHELL_TABLE,
	SQNANO_SHELL_CSV,
	SQNANO_SHELL_JSON,
	SQNANO_SHELL_LINE,
};

/** Interpreter state, kept between commands. */
struct sqnano_shell {
	struct sqnano_db *db;
	/** printf-like output function. */
	void (*print)(void *arg, const char *fmt, ...);
	void *arg;
	enum sqnano_shell_mode mode;
	/** Row cap applied when the statement has no LIMIT. */
	uint32_t limit;
	/** Maximum duration of a statement in milliseconds (0: none). */
	uint32_t timeout_ms;
	bool read_only;
	/** Writes allowed at all (CONFIG_SQNANO_SHELL_WRITE). */
	bool allow_write;
};

/** Initialize @p sh with the default settings (table mode, 100 rows, read-only). */
void sqnano_shell_init(struct sqnano_shell *sh, struct sqnano_db *db,
		       void (*print)(void *arg, const char *fmt, ...), void *arg);

/**
 * @brief Run one command line: a dot command or a restricted SQL statement.
 *
 * @return SQNANO_OK or an error code; the error was printed.
 */
int sqnano_shell_exec(struct sqnano_shell *sh, const char *line);

#ifdef __cplusplus
}
#endif

#endif /* SQNANO_SHELL_H_ */
