/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/* Zephyr shell command "db" (see src/shell/minisql.c for the interpreter). */

#include <stdarg.h>
#include <string.h>

#include <zephyr/shell/shell.h>

#include <sqnano/shell.h>
#include <sqnano/zephyr.h>

static struct sqnano_shell state;
static bool ready;
static char line[CONFIG_SQNANO_SHELL_LINE_SIZE];

static void out(void *arg, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	shell_vfprintf((const struct shell *)arg, SHELL_NORMAL, fmt, ap);
	va_end(ap);
}

static int select_db(const struct shell *sh, const struct sqnano_dt_db *d)
{
	struct sqnano_db *db;
	int rc = sqnano_dt_open(d, NULL, &db);

	if (rc) {
		shell_error(sh, "cannot open %s (%d)", d->path, rc);
		return -EIO;
	}
	sqnano_shell_init(&state, db, out, (void *)sh);
	state.allow_write = IS_ENABLED(CONFIG_SQNANO_SHELL_WRITE);
	ready = true;
	return 0;
}

/* Run "<first> <rest>"; arguments arrive raw so that SQL quotes are kept. */
static int run_line(const struct shell *sh, const char *first, const char *rest)
{
	size_t n = 0;

	if (strcmp(first, ".databases") == 0) {
		for (int i = 0; i < sqnano_dt_count(); i++) {
			const struct sqnano_dt_db *d = sqnano_dt_get(i);

			shell_print(sh, "%s%s", d->path,
				    ready && state.db == d->db ? "  (current)" : "");
		}
		return 0;
	}
	if (strcmp(first, ".open") == 0) {
		const struct sqnano_dt_db *d = rest ? sqnano_dt_find(rest) : NULL;

		if (!d) {
			shell_error(sh, "usage: db .open <path> (see db .databases)");
			return -EINVAL;
		}
		return select_db(sh, d);
	}
	if (!ready) {
		if (sqnano_dt_count() == 0) {
			shell_error(sh, "no database in the devicetree (compatible zephyr,sqnano)");
			return -ENODEV;
		}
		if (select_db(sh, sqnano_dt_get(0))) {
			return -EIO;
		}
	}
	state.arg = (void *)sh;
	n = strlen(first);
	if (n + (rest ? strlen(rest) + 1 : 0) + 1 > sizeof(line)) {
		shell_error(sh, "command too long (CONFIG_SQNANO_SHELL_LINE_SIZE)");
		return -E2BIG;
	}
	memcpy(line, first, n + 1);
	if (rest) {
		line[n] = ' ';
		strcpy(line + n + 1, rest);
	}
	return sqnano_shell_exec(&state, line) == SQNANO_OK ? 0 : -EINVAL;
}

static int cmd_db(const struct shell *sh, size_t argc, char **argv)
{
	static char joined[CONFIG_SQNANO_SHELL_LINE_SIZE];
	size_t n = 0;

	if (argc < 2) {
		shell_help(sh);
		return SHELL_CMD_HELP_PRINTED;
	}
	/*
	 * Words without a completion entry arrive split, with their quotes
	 * removed by the shell: rebuild the line as well as possible.
	 */
	for (size_t i = 1; i < argc; i++) {
		size_t len = strlen(argv[i]);

		if (n + len + 2 > sizeof(joined)) {
			shell_error(sh, "command too long (CONFIG_SQNANO_SHELL_LINE_SIZE)");
			return -E2BIG;
		}
		if (n) {
			joined[n++] = ' ';
		}
		memcpy(joined + n, argv[i], len);
		n += len;
	}
	joined[n] = 0;
	return run_line(sh, joined, NULL);
}

static int cmd_word(const struct shell *sh, size_t argc, char **argv)
{
	return run_line(sh, argv[0], argc > 1 ? argv[1] : NULL);
}

/*
 * Completion entries, sorted as the shell requires. They are final commands
 * taking a raw argument, so SQL reaches the interpreter with its quotes.
 */
static const char *const words[] = {
	".check", ".databases", ".help",   ".indexes", ".limit",  ".mode",
	".open",  ".queries",   ".readonly", ".run",   ".schema", ".stats",
	".tables", ".timeout",  "EXPLAIN", "SELECT",   "explain", "select",
};

static void complete(size_t idx, struct shell_static_entry *entry)
{
	entry->syntax = idx < ARRAY_SIZE(words) ? words[idx] : NULL;
	entry->handler = cmd_word;
	entry->subcmd = NULL;
	entry->help = NULL;
	entry->args.mandatory = 1;
	entry->args.optional = SHELL_OPT_ARG_RAW;
}

SHELL_DYNAMIC_CMD_CREATE(db_words, complete);

SHELL_CMD_ARG_REGISTER(db, &db_words,
		       "SQNano: restricted SQL on the target and dot commands (db .help)", cmd_db, 1,
		       SHELL_OPT_ARG_CHECK_SKIP);
