/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * SQNano on Zephyr: devicetree databases on the flash simulator (shadow
 * paging) and the EEPROM simulator (undo journal), through sqlite3.h and
 * through the typed API, with reopen and snapshot isolation.
 */

#include <zephyr/ztest.h>

#include <sqlite3.h>
#include <sqnano/zephyr.h>

#include "sqnano_gen.h"

static void fill(sqlite3 *c, int n)
{
	sqlite3_stmt *st;

	zassert_ok(sqlite3_exec(c, "CREATE TABLE IF NOT EXISTS sensor (rid INTEGER PRIMARY KEY, "
				   "id INTEGER NOT NULL, ts INTEGER NOT NULL, value REAL, unit "
				   "TEXT DEFAULT 'C')",
				NULL, NULL, NULL));
	zassert_ok(sqlite3_exec(c, "BEGIN", NULL, NULL, NULL));
	zassert_ok(sqlite3_prepare_v2(c, "INSERT INTO sensor (id, ts, value) VALUES (:id, :ts, :value)",
				      -1, &st, NULL));
	for (int i = 0; i < n; i++) {
		sqlite3_bind_int(st, 1, i % 4);
		sqlite3_bind_int64(st, 2, 1760780000 + i);
		sqlite3_bind_double(st, 3, 20.0 + (i % 10) / 4.0);
		int rc = sqlite3_step(st);

		zassert_equal(rc, SQLITE_DONE, "row %d: %d %s", i, rc, sqlite3_errmsg(c));
		sqlite3_reset(st);
	}
	sqlite3_finalize(st);
	zassert_ok(sqlite3_exec(c, "COMMIT", NULL, NULL, NULL), "%s", sqlite3_errmsg(c));
}

static int last_two(sqlite3 *c, int id, int64_t *ts)
{
	sqlite3_stmt *st;
	int rows = 0;

	zassert_ok(sqlite3_prepare_v2(c, "SELECT ts, value FROM sensor WHERE id = :id ORDER BY ts "
					 "DESC LIMIT :n",
				      -1, &st, NULL));
	sqlite3_bind_int(st, 1, id);
	sqlite3_bind_int(st, 2, 2);
	while (sqlite3_step(st) == SQLITE_ROW) {
		ts[rows++] = sqlite3_column_int64(st, 0);
	}
	sqlite3_finalize(st);
	return rows;
}

static void run_on(const char *path, int n)
{
	sqlite3 *c = NULL;
	int64_t ts[2];
	struct sqnano_db *db;
	struct sqnano_stats st;

	zassert_ok(sqlite3_open(path, &c), "%s", sqlite3_errmsg(c));
	fill(c, n);
	zassert_equal(last_two(c, 3, ts), 2);
	/* n is a multiple of 4: the last row (i = n - 1) has id 3. */
	zassert_equal(ts[0], 1760780000 + n - 1);
	zassert_true(ts[0] > ts[1]);
	db = sqnano_sqlite3_db(c);
	zassert_ok(sqnano_db_check(db));
	zassert_ok(sqnano_db_stats(db, &st));
	TC_PRINT("%s: %s, %u-byte pages, %u/%u used, %u writes, %u erases\n", path,
		 st.mode == SQNANO_MODE_SHADOW ? "shadow paging" : "undo journal", st.page_size,
		 st.pages_used, st.pages_total, st.writes, st.erases);
	zassert_ok(sqlite3_close(c));
}

ZTEST(sqnano, test_flash_partition)
{
	run_on("/data/app.db", 300);
}

ZTEST(sqnano, test_fram_eeprom)
{
	run_on("fram.db", 120);
}

ZTEST(sqnano, test_typed_api_and_reopen)
{
	const struct sqnano_dt_db *d = sqnano_dt_find("app.db");
	struct sqnano_db *db;
	struct q_sensor_stats q;
	int groups = 0;
	int rc;

	zassert_not_null(d);
	zassert_ok(sqnano_dt_open(d, &sqnano_app_schema, &db));
	/* Remount from the flash: the data must still be there. */
	zassert_ok(sqnano_db_close(db));
	zassert_ok(sqnano_dt_open(d, &sqnano_app_schema, &db));
	zassert_ok(q_sensor_stats_open(&q, db, NULL));
	while ((rc = q_sensor_stats_step(&q)) == SQNANO_ROW) {
		zassert_equal(q.id, groups);
		zassert_true(q.count >= 75);
		zassert_true(q.max_value >= q.min_value);
		groups++;
	}
	zassert_equal(rc, SQNANO_DONE);
	zassert_equal(groups, 4);
}

static int64_t one_int(sqlite3 *c, const char *sql)
{
	sqlite3_stmt *st;
	int64_t v;

	zassert_ok(sqlite3_prepare_v2(c, sql, -1, &st, NULL), "%s", sqlite3_errmsg(c));
	zassert_equal(sqlite3_step(st), SQLITE_ROW);
	v = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return v;
}

ZTEST(sqnano, test_savepoint_join)
{
	sqlite3 *c = NULL;
	sqlite3_stmt *st;
	int groups = 0;

	zassert_ok(sqlite3_open("/data/app.db", &c), "%s", sqlite3_errmsg(c));
	zassert_equal(one_int(c, "SELECT count(*) FROM sensor WHERE id = 1"), 75);
	zassert_ok(sqlite3_exec(c, "SAVEPOINT s1", NULL, NULL, NULL));
	zassert_ok(sqlite3_exec(c, "DELETE FROM sensor WHERE id = 1", NULL, NULL, NULL));
	zassert_equal(one_int(c, "SELECT count(*) FROM sensor WHERE id = 1"), 0);
	zassert_ok(sqlite3_exec(c, "ROLLBACK TO s1", NULL, NULL, NULL));
	zassert_ok(sqlite3_exec(c, "RELEASE s1", NULL, NULL, NULL));
	zassert_equal(one_int(c, "SELECT count(*) FROM sensor WHERE id = 1"), 75);

	/* Self-join on the (id, ts) index, and a correlated subquery. */
	zassert_ok(sqlite3_prepare_v2(c, "SELECT a.id, count(*) FROM sensor AS a JOIN sensor AS b "
					 "ON b.id = a.id AND b.ts = a.ts GROUP BY a.id",
				      -1, &st, NULL));
	while (sqlite3_step(st) == SQLITE_ROW) {
		zassert_equal(sqlite3_column_int(st, 1), 75);
		groups++;
	}
	sqlite3_finalize(st);
	zassert_equal(groups, 4);
	zassert_equal(one_int(c, "SELECT count(*) FROM sensor AS s WHERE ts = (SELECT max(ts) "
				 "FROM sensor WHERE id = s.id)"),
		      4);
	zassert_ok(sqlite3_close(c));
}

/* An ORDER BY larger than the sort buffer: runs in free pages, then a merge. */
ZTEST(sqnano, test_external_sort)
{
	sqlite3 *c = NULL;
	sqlite3_stmt *st;
	struct sqnano_stats stats;
	double prev_v = 1e9;
	int64_t prev_ts = 0;
	int rows = 0, total;

	zassert_ok(sqlite3_open("/data/app.db", &c), "%s", sqlite3_errmsg(c));
	fill(c, 400);
	zassert_ok(sqlite3_prepare_v2(c, "SELECT count(*) FROM sensor", -1, &st, NULL));
	zassert_equal(sqlite3_step(st), SQLITE_ROW);
	total = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	zassert_ok(sqlite3_prepare_v2(c, "SELECT value, ts, unit FROM sensor ORDER BY value DESC, ts",
				      -1, &st, NULL));
	while (sqlite3_step(st) == SQLITE_ROW) {
		double v = sqlite3_column_double(st, 0);
		int64_t ts = sqlite3_column_int64(st, 1);

		zassert_true(v < prev_v || (v == prev_v && ts > prev_ts), "row %d out of order", rows);
		prev_v = v;
		prev_ts = ts;
		rows++;
	}
	zassert_equal(sqlite3_finalize(st), SQLITE_OK, "%s", sqlite3_errmsg(c));
	zassert_equal(rows, total);
	zassert_ok(sqnano_db_stats(sqnano_sqlite3_db(c), &stats));
	zassert_true(stats.sort_runs > 0, "the sort fitted in RAM");
	zassert_ok(sqnano_db_check(sqnano_sqlite3_db(c)));
	TC_PRINT("external sort: %d rows, %u runs\n", rows, stats.sort_runs);
	/* The other tests count the rows of their own fill(). */
	zassert_ok(sqlite3_exec(c, "DELETE FROM sensor", NULL, NULL, NULL), "%s", sqlite3_errmsg(c));
	zassert_ok(sqlite3_close(c));
}

ZTEST(sqnano, test_snapshot_isolation)
{
	const struct sqnano_dt_db *d = sqnano_dt_find("fram.db");
	struct sqnano_db *db;
	struct sqnano_txn snap;
	struct q_sensor_purge purge;
	struct q_sensor_stats q;
	int64_t before = 0, after = 0, seen = 0;

	zassert_ok(sqnano_dt_open(d, &sqnano_app_schema, &db));
	zassert_ok(sqnano_txn_begin(db, &snap, SQNANO_TXN_READ, 0));
	zassert_ok(q_sensor_stats_open(&q, db, NULL));
	while (q_sensor_stats_step(&q) == SQNANO_ROW) {
		before += q.count;
	}
	/* Delete everything in another transaction... */
	zassert_ok(q_sensor_purge_open(&purge, db, NULL));
	q_sensor_purge_bind(&purge, INT64_MAX);
	zassert_ok(q_sensor_purge_run(&purge));
	zassert_ok(q_sensor_stats_open(&q, db, NULL));
	while (q_sensor_stats_step(&q) == SQNANO_ROW) {
		after += q.count;
	}
	/* ...the snapshot still sees the old content. */
	zassert_ok(q_sensor_stats_open(&q, db, &snap));
	while (q_sensor_stats_step(&q) == SQNANO_ROW) {
		seen += q.count;
	}
	zassert_ok(sqnano_txn_commit(&snap));
	zassert_equal(before, 120);
	zassert_equal(after, 0);
	zassert_equal(seen, 120);
	zassert_ok(sqnano_db_check(db));
}

ZTEST_SUITE(sqnano, NULL, NULL, NULL, NULL, NULL);
