/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * sqlite3.h facade and typed API on the sample application schema
 * (tests/host/app): prepare by hash, binding, column conversions,
 * transactions, constraints, UPSERT, pragmas, errors and persistence.
 */

#include <sqlite3.h>

#include "sqnano_gen.h"
#include "testutil.h"

/* Not passed literally to sqlite3_*(): never extracted, so never precompiled. */
static const char *const not_compiled[] = {
	"SELECT * FROM sensor WHERE value > 3",
	"SELECT ts, value FROM sensor WHERE id = 0 ORDER BY ts DESC LIMIT 100",
};

static uint8_t mem[256 * 1024];
static struct sqnano_ram_media ram;
static struct sqnano_db db;

static void exec_ok(sqlite3 *c, const char *sql)
{
	char *err = NULL;
	int rc = sqlite3_exec(c, sql, NULL, NULL, &err);

	if (rc != SQLITE_OK) {
		fprintf(stderr, "exec '%s': %d %s\n", sql, rc, err ? err : "");
		exit(1);
	}
}

static int count_cb(void *arg, int n, char **argv, char **names)
{
	(void)n;
	(void)argv;
	(void)names;
	(*(int *)arg)++;
	return 0;
}

static void test_basic(sqlite3 *c)
{
	sqlite3_stmt *st;
	const char *tail;
	int rc;

	/* Insert through the facade, with different spellings of the same SQL. */
	CHECK_RC(sqlite3_prepare_v2(c, "insert into SENSOR (id, ts, value)\n VALUES (:id,:ts,:value)",
				    -1, &st, &tail),
		 SQLITE_OK);
	CHECK(sqlite3_bind_parameter_count(st) == 3);
	CHECK(sqlite3_bind_parameter_index(st, ":ts") == 2);
	for (int i = 0; i < 50; i++) {
		CHECK_RC(sqlite3_bind_int(st, 1, i % 5), SQLITE_OK);
		CHECK_RC(sqlite3_bind_int64(st, 2, 1000 + i), SQLITE_OK);
		CHECK_RC(sqlite3_bind_double(st, 3, 20.0 + i / 10.0), SQLITE_OK);
		CHECK_RC(sqlite3_step(st), SQLITE_DONE);
		CHECK(sqlite3_changes(c) == 1);
		CHECK(sqlite3_last_insert_rowid(c) == i + 1);
		CHECK_RC(sqlite3_reset(st), SQLITE_OK);
	}
	CHECK_RC(sqlite3_finalize(st), SQLITE_OK);

	/* The query of the architecture document. */
	CHECK_RC(sqlite3_prepare_v2(c, "SELECT ts, value FROM sensor WHERE id = :id ORDER BY ts DESC "
				       "LIMIT :n;",
				    -1, &st, NULL),
		 SQLITE_OK);
	CHECK(sqlite3_column_count(st) == 2);
	CHECK(strcmp(sqlite3_column_name(st, 0), "ts") == 0);
	CHECK(strcmp(sqlite3_column_decltype(st, 1), "REAL") == 0);
	sqlite3_bind_int(st, 1, 3);
	sqlite3_bind_text(st, 2, "2", -1, SQLITE_TRANSIENT);
	CHECK_RC(sqlite3_step(st), SQLITE_ROW);
	CHECK(sqlite3_column_int64(st, 0) == 1048);
	CHECK(sqlite3_column_type(st, 1) == SQLITE_FLOAT);
	CHECK(strcmp((const char *)sqlite3_column_text(st, 0), "1048") == 0);
	CHECK(strcmp((const char *)sqlite3_column_text(st, 1), "24.8") == 0);
	CHECK_RC(sqlite3_step(st), SQLITE_ROW);
	CHECK(sqlite3_column_int64(st, 0) == 1043);
	CHECK_RC(sqlite3_step(st), SQLITE_DONE);
	CHECK_RC(sqlite3_finalize(st), SQLITE_OK);

	/* Unknown SQL is rejected with its hash. */
	rc = sqlite3_prepare_v2(c, not_compiled[0], -1, &st, NULL);
	CHECK(rc == SQLITE_ERROR && st == NULL);
	CHECK(strncmp(sqlite3_errmsg(c), "query not precompiled: 0x", 25) == 0);

	/* Aggregates with GROUP BY. */
	{
		int rows = 0;

		CHECK_RC(sqlite3_prepare_v2(c, "SELECT id, count(*), min(value), max(value), "
					       "avg(value) FROM sensor GROUP BY id ORDER BY id",
					    -1, &st, NULL),
			 SQLITE_OK);
		while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
			CHECK(sqlite3_column_int(st, 0) == rows);
			CHECK(sqlite3_column_int(st, 1) == 10);
			rows++;
		}
		CHECK_RC(rc, SQLITE_DONE);
		CHECK(rows == 5);
		sqlite3_finalize(st);
	}
}

static void test_txn_and_constraints(sqlite3 *c)
{
	sqlite3_stmt *st;
	int n = 0;
	int rc;

	exec_ok(c, "BEGIN");
	CHECK(!sqlite3_get_autocommit(c));
	exec_ok(c, "DELETE FROM sensor WHERE ts < 1010");
	CHECK(sqlite3_changes(c) == 10);
	exec_ok(c, "ROLLBACK");
	CHECK(sqlite3_get_autocommit(c));
	/* Literal values differ from the precompiled text. */
	CHECK_RC(sqlite3_exec(c, not_compiled[1], count_cb, &n, NULL), SQLITE_ERROR);
	exec_ok(c, "BEGIN IMMEDIATE TRANSACTION; DELETE FROM sensor WHERE ts < :before; COMMIT");
	/* Unbound parameter: NULL, so nothing deleted. */
	CHECK(sqlite3_changes(c) == 0);

	/* UPSERT and the NOCASE primary key. */
	CHECK_RC(sqlite3_prepare_v2(c, "INSERT INTO config (key, val) VALUES (:key, :val) ON CONFLICT "
				       "(key) DO UPDATE SET val = excluded.val",
				    -1, &st, NULL),
		 SQLITE_OK);
	sqlite3_bind_text(st, 1, "Mode", -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, "eco", 3, SQLITE_TRANSIENT);
	CHECK_RC(sqlite3_step(st), SQLITE_DONE);
	sqlite3_reset(st);
	sqlite3_bind_text(st, 1, "MODE", -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, "boost", -1, SQLITE_TRANSIENT);
	CHECK_RC(sqlite3_step(st), SQLITE_DONE);
	sqlite3_reset(st);
	/* CHECK (length(key) > 0) */
	sqlite3_bind_text(st, 1, "", -1, SQLITE_STATIC);
	rc = sqlite3_step(st);
	CHECK(rc == SQLITE_CONSTRAINT);
	CHECK(sqlite3_extended_errcode(c) == SQLITE_CONSTRAINT_CHECK);
	CHECK(strcmp(sqlite3_errmsg(c), "CHECK constraint failed: length(key) > 0") == 0);
	sqlite3_finalize(st);

	CHECK_RC(sqlite3_prepare_v2(c, "SELECT val FROM config WHERE key = :key", -1, &st, NULL),
		 SQLITE_OK);
	sqlite3_bind_text(st, 1, "mode", -1, SQLITE_STATIC);
	CHECK_RC(sqlite3_step(st), SQLITE_ROW);
	CHECK(strcmp((const char *)sqlite3_column_text(st, 0), "boost") == 0);
	CHECK_RC(sqlite3_step(st), SQLITE_DONE);
	sqlite3_finalize(st);

	/* NOT NULL */
	CHECK_RC(sqlite3_prepare_v2(c, "INSERT INTO sensor (id, ts, value) VALUES (:id, :ts, :value)",
				    -1, &st, NULL),
		 SQLITE_OK);
	sqlite3_bind_int(st, 1, 1);
	rc = sqlite3_step(st);
	CHECK(rc == SQLITE_CONSTRAINT);
	CHECK(strcmp(sqlite3_errmsg(c), "NOT NULL constraint failed: sensor.ts") == 0);
	sqlite3_finalize(st);
}

static int64_t count_sensor(sqlite3 *c)
{
	sqlite3_stmt *st;
	int64_t n;

	CHECK_RC(sqlite3_prepare_v2(c, "SELECT count(*) FROM sensor", -1, &st, NULL), SQLITE_OK);
	CHECK_RC(sqlite3_step(st), SQLITE_ROW);
	n = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return n;
}

static void test_savepoints(sqlite3 *c)
{
	int64_t n0 = count_sensor(c);
	sqlite3_stmt *st;

	/* SAVEPOINT outside a transaction starts one; releasing it commits. */
	exec_ok(c, "SAVEPOINT a");
	CHECK(!sqlite3_get_autocommit(c));
	exec_ok(c, "INSERT INTO sensor (id, ts, value) VALUES (90, 1, 1.0)");
	exec_ok(c, "SAVEPOINT b");
	exec_ok(c, "INSERT INTO sensor (id, ts, value) VALUES (90, 2, 2.0)");
	CHECK(count_sensor(c) == n0 + 2);
	exec_ok(c, "ROLLBACK TO b");
	CHECK(count_sensor(c) == n0 + 1);
	exec_ok(c, "INSERT INTO sensor (id, ts, value) VALUES (90, 3, 3.0)");
	exec_ok(c, "RELEASE b");
	CHECK(!sqlite3_get_autocommit(c));
	CHECK_RC(sqlite3_exec(c, "RELEASE b", NULL, NULL, NULL), SQLITE_ERROR);
	CHECK(strcmp(sqlite3_errmsg(c), "no such savepoint: b") == 0);
	exec_ok(c, "SAVEPOINT \"c\"; DELETE FROM sensor WHERE id = 90; "
		   "ROLLBACK TRANSACTION TO SAVEPOINT c");
	CHECK(count_sensor(c) == n0 + 2);
	exec_ok(c, "RELEASE SAVEPOINT a");
	CHECK(sqlite3_get_autocommit(c));
	CHECK(count_sensor(c) == n0 + 2);

	/* A statement that fails inside BEGIN only undoes itself. */
	exec_ok(c, "BEGIN");
	exec_ok(c, "INSERT INTO sensor (id, ts, value) VALUES (91, 1, 1.0)");
	CHECK_RC(sqlite3_exec(c, "INSERT INTO config (key, val) VALUES ('ok1', 'x'), ('', 'y')",
			      NULL, NULL, NULL),
		 SQLITE_CONSTRAINT);
	exec_ok(c, "COMMIT");
	CHECK(count_sensor(c) == n0 + 3);
	CHECK_RC(sqlite3_prepare_v2(c, "SELECT val FROM config WHERE key = :key", -1, &st, NULL),
		 SQLITE_OK);
	sqlite3_bind_text(st, 1, "ok1", -1, SQLITE_STATIC);
	CHECK_RC(sqlite3_step(st), SQLITE_DONE);
	sqlite3_finalize(st);
	exec_ok(c, "DELETE FROM sensor WHERE id >= 90");
}

static void test_pragmas(sqlite3 *c)
{
	sqlite3_stmt *st;

	CHECK_RC(sqlite3_prepare_v2(c, "PRAGMA user_version", -1, &st, NULL), SQLITE_OK);
	CHECK_RC(sqlite3_step(st), SQLITE_ROW);
	CHECK(sqlite3_column_int(st, 0) == SQNANO_SCHEMA_VERSION);
	CHECK_RC(sqlite3_step(st), SQLITE_DONE);
	sqlite3_finalize(st);
	exec_ok(c, "PRAGMA user_version = 7; PRAGMA journal_mode=WAL; PRAGMA synchronous = NORMAL");
	CHECK_RC(sqlite3_prepare_v2(c, "pragma USER_VERSION;", -1, &st, NULL), SQLITE_OK);
	CHECK_RC(sqlite3_step(st), SQLITE_ROW);
	CHECK(sqlite3_column_int(st, 0) == 7);
	sqlite3_finalize(st);
	CHECK_RC(sqlite3_prepare_v2(c, "PRAGMA integrity_check", -1, &st, NULL), SQLITE_OK);
	CHECK_RC(sqlite3_step(st), SQLITE_ROW);
	CHECK(strcmp((const char *)sqlite3_column_text(st, 0), "ok") == 0);
	sqlite3_finalize(st);
	exec_ok(c, "PRAGMA foreign_keys = ON");
	CHECK_RC(sqlite3_prepare_v2(c, "PRAGMA foreign_keys", -1, &st, NULL), SQLITE_OK);
	CHECK_RC(sqlite3_step(st), SQLITE_ROW);
	CHECK(sqlite3_column_int(st, 0) == 1);
	sqlite3_finalize(st);
	exec_ok(c, "PRAGMA foreign_keys = OFF");
}

static void test_typed(void)
{
	struct q_sensor_last q;
	int rows = 0;
	int rc;

	CHECK_RC(q_sensor_last_open(&q, &db, NULL), SQNANO_OK);
	q_sensor_last_bind(&q, 4, 3);
	while ((rc = q_sensor_last_step(&q)) == SQNANO_ROW) {
		CHECK(q.ts == 1049 - 5 * rows);
		CHECK(q.value > 20.0);
		rows++;
	}
	CHECK_RC(rc, SQNANO_DONE);
	CHECK(rows == 3);
}

int main(void)
{
	struct sqnano_db_config cfg = {.page_size = 256, .journal_slots = 16};
	sqlite3 *c;

	sqnano_ram_media_init(&ram, mem, sizeof(mem), SQNANO_RAM_FRAM, 0);
	sqnano_ram_media_blank(&ram);
	CHECK_RC(sqnano_sqlite3_register("app.db", &db, &ram.media, &cfg, &sqnano_app_schema),
		 SQNANO_OK);
	CHECK(sqlite3_open("/data/missing.db", &c) == SQLITE_CANTOPEN);
	sqlite3_close(c);
	CHECK_RC(sqlite3_open("/data/app.db", &c), SQLITE_OK);
	exec_ok(c, "CREATE TABLE IF NOT EXISTS sensor (rid INTEGER PRIMARY KEY, id INTEGER NOT "
		   "NULL, ts INTEGER NOT NULL, value REAL, unit TEXT DEFAULT 'C')");
	test_basic(c);
	test_txn_and_constraints(c);
	test_savepoints(c);
	test_pragmas(c);
	test_typed();
	{
		/* Every state went back to the pool; some were in use. */
		struct sqnano_pool_stats ps;

		CHECK_RC(sqnano_pool_stats(&sqnano_app_schema, &ps), SQNANO_OK);
		CHECK(ps.used == 0 && ps.statements == 0 && ps.peak > 0 && ps.fails == 0);
		CHECK(ps.size == (SQNANO_STMT_POOL_SIZE ? SQNANO_STMT_POOL_SIZE
							 : ps.largest * SQNANO_POOL_SLOTS));
		printf("statement pool: peak %u of %u bytes, largest state %u\n", ps.peak, ps.size,
		       ps.largest);
	}
	CHECK_RC(sqlite3_close(c), SQLITE_OK);

	/* Reopen from the media. */
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	CHECK_RC(sqnano_db_open(&db, &ram.media, &cfg, &sqnano_app_schema), SQNANO_OK);
	test_typed();
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
	printf("sqlite3 facade ok\n");
	return 0;
}
