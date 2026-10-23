/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/* Diagnostic shell interpreter: restricted SQL, plans, dot commands, modes. */

#include <stdarg.h>

#include <sqnano/shell.h>

#include "sqnano_gen.h"
#include "testutil.h"

static uint8_t mem[256 * 1024];
static struct sqnano_ram_media ram;
static struct sqnano_db db;
static char out[16384];
static size_t used;

static void capture(void *arg, const char *fmt, ...)
{
	va_list ap;

	(void)arg;
	va_start(ap, fmt);
	used += (size_t)vsnprintf(out + used, sizeof(out) - used, fmt, ap);
	va_end(ap);
}

static const char *run(struct sqnano_shell *sh, const char *cmd, int want)
{
	int rc;

	used = 0;
	out[0] = 0;
	rc = sqnano_shell_exec(sh, cmd);
	if (rc != want) {
		fprintf(stderr, "'%s' returned %d, expected %d:\n%s", cmd, rc, want, out);
		exit(1);
	}
	return out;
}

static void expect(struct sqnano_shell *sh, const char *cmd, const char *needle)
{
	const char *o = run(sh, cmd, SQNANO_OK);

	if (!strstr(o, needle)) {
		fprintf(stderr, "'%s': '%s' not found in:\n%s", cmd, needle, o);
		exit(1);
	}
}

static void expect_error(struct sqnano_shell *sh, const char *cmd, const char *needle)
{
	used = 0;
	out[0] = 0;
	CHECK(sqnano_shell_exec(sh, cmd) != SQNANO_OK);
	if (!strstr(out, needle)) {
		fprintf(stderr, "'%s': '%s' not found in:\n%s", cmd, needle, out);
		exit(1);
	}
}

int main(void)
{
	struct sqnano_db_config cfg = {.page_size = 512, .create = true};
	struct sqnano_shell sh;

	sqnano_ram_media_init(&ram, mem, sizeof(mem), SQNANO_RAM_NOR, 512);
	sqnano_ram_media_blank(&ram);
	CHECK_RC(sqnano_db_open(&db, &ram.media, &cfg, &sqnano_app_schema), SQNANO_OK);
	for (int i = 0; i < 40; i++) {
		struct q_sensor_add q;

		CHECK_RC(q_sensor_add_open(&q, &db, NULL), SQNANO_OK);
		q_sensor_add_bind(&q, i % 4, 1760782000 + 60 * i, 18.0 + (i % 7) * 0.7);
		CHECK_RC(q_sensor_add_run(&q), SQNANO_OK);
	}
	for (int i = 0; i < 10; i++) {
		static const char *const devs[] = {"pump", "valve", "fan"};
		struct q_event_add q;

		CHECK_RC(q_event_add_open(&q, &db, NULL), SQNANO_OK);
		q_event_add_bind(&q, devs[i % 3], i, i % 2 ? "on" : "off");
		CHECK_RC(q_event_add_run(&q), SQNANO_OK);
	}
	sqnano_shell_init(&sh, &db, capture, NULL);

	expect(&sh, ".tables", "sensor  config");
	expect(&sh, ".schema sensor", "CREATE INDEX sensor_pk ON sensor(id, ts)");
	expect(&sh, ".indexes", "sensor_pk");
	expect(&sh, ".stats", "shadow paging");
	expect(&sh, ".queries", "sensor_last");
	expect(&sh, "SELECT ts, value FROM sensor WHERE id = 3 AND value > 20.5 ORDER BY ts DESC LIMIT 5;",
	       "idx sensor_pk");
	expect(&sh, "SELECT ts, value FROM sensor WHERE id = 3 AND value > 20.5 ORDER BY ts DESC LIMIT 5;",
	       "1760783140  21.5");
	expect(&sh, "EXPLAIN SELECT * FROM sensor WHERE value > 25", "FULL SCAN sensor - no index on value");
	expect(&sh, "EXPLAIN SELECT * FROM sensor WHERE id = 2", "USING INDEX sensor_pk (1 equality)");
	/* WITHOUT ROWID: the table is its PRIMARY KEY (dev, seq DESC). */
	expect(&sh, "EXPLAIN SELECT * FROM event WHERE dev = 'fan'",
	       "SEARCH event USING PRIMARY KEY (1 equality)");
	expect(&sh, "select seq, kind from event where dev = 'fan' limit 1", "\n8           off");
	expect(&sh, "select seq, kind from event where dev = 'fan' limit 1", "(1 row, primary key");
	expect(&sh, "select count(*) from event where dev = 'pump' and seq < 5", "2");
	expect_error(&sh, "select rowid from event", "no such column: rowid");
	expect(&sh, "select count(*), min(value), max(value), avg(ts) from sensor where id = 1",
	       "count(*)");
	expect(&sh, "select count(*) from sensor where value between 18 and 19", "(1 row");
	expect(&sh, "select rid from sensor where rid >= 38", "(3 rows, rowid");
	expect(&sh, "select rid, unit from sensor where unit like 'c' and rid < 3", "(2 rows");
	expect(&sh, "select * from sensor where value is null", "(0 rows");
	expect(&sh, ".mode json", "");
	expect(&sh, "select rid, unit from sensor where rid = 1", "{\"rid\":1,\"unit\":\"C\"}");
	expect(&sh, ".mode csv", "");
	expect(&sh, "select rid, id from sensor where rid = 2", "rid,id\n2,1\n");
	expect(&sh, ".mode table", "");
	expect(&sh, ".limit 3", "limit 3 rows");
	expect(&sh, "select rid from sensor", "(stopped at .limit 3)");
	expect(&sh, ".run sensor_last id=3 n=2", "(2 rows");
	expect_error(&sh, ".run sensor_add id=9 ts=1 value=2", "CONFIG_SQNANO_SHELL_WRITE");
	sh.allow_write = true;
	expect_error(&sh, ".run sensor_add id=9 ts=1 value=2", ".readonly off");
	expect(&sh, ".readonly off", "readonly off");
	expect(&sh, ".run sensor_add id=9 ts=1 value=2", "1 changes");
	expect(&sh, ".check", "ok");
	expect_error(&sh, "select * from sensor where value > 1 order by value", "needs an index");
	expect_error(&sh, "select id, count(*) from sensor", "GROUP BY");
	expect_error(&sh, "select * from sensor s, config c", "sqnano-cli");
	expect_error(&sh, "select * from sensor where id = 1 or id = 2", "OR not supported");
	expect_error(&sh, "delete from sensor", "only SELECT");
	expect_error(&sh, "select nope from sensor", "no such column: nope");
	printf("shell ok\n");
	return 0;
}
