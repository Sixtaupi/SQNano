/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Hybrid mode on the flash and EEPROM simulators: one row per commit, then
 * the database read back after closing it; the flash erases stay rare.
 */

#include <zephyr/ztest.h>

#include <sqlite3.h>
#include <sqnano/zephyr.h>

static int count(sqlite3 *c, const char *sql)
{
	sqlite3_stmt *st;
	int n = -1;

	zassert_ok(sqlite3_prepare_v2(c, sql, -1, &st, NULL), "%s", sqlite3_errmsg(c));
	if (sqlite3_step(st) == SQLITE_ROW) {
		n = sqlite3_column_int(st, 0);
	}
	sqlite3_finalize(st);
	return n;
}

ZTEST(sqnano_hybrid, test_commits_and_reopen)
{
	const struct sqnano_dt_db *d = sqnano_dt_find("app.db");
	struct sqnano_hybrid_zmedia *zm;
	sqlite3 *c = NULL;
	sqlite3_stmt *st;

	zassert_not_null(d);
	zm = CONTAINER_OF(d->media, struct sqnano_hybrid_zmedia, hy.media);
	zassert_ok(sqlite3_open("app.db", &c), "%s", sqlite3_errmsg(c));
	zassert_ok(sqlite3_prepare_v2(c, "INSERT INTO sensor (id, ts, value) VALUES (:id, :ts, :value)",
				      -1, &st, NULL));
	for (int i = 0; i < 400; i++) {
		/* Autocommit: one commit per row. */
		sqlite3_bind_int(st, 1, i % 4);
		sqlite3_bind_int64(st, 2, 9000 + i);
		sqlite3_bind_double(st, 3, i * 0.25);
		zassert_equal(sqlite3_step(st), SQLITE_DONE, "%s", sqlite3_errmsg(c));
		sqlite3_reset(st);
	}
	sqlite3_finalize(st);
	zassert_ok(sqlite3_exec(c, "DELETE FROM sensor WHERE ts < 9100", NULL, NULL, NULL));
	zassert_equal(count(c, "SELECT count(*) FROM sensor"), 300);
	TC_PRINT("hybrid: 400 commits, %u flash erases, %u flash programs, %u evictions, "
		 "%u erases kept in FRAM\n",
		 zm->hy.flash_erases, zm->hy.flash_programs, zm->hy.evictions,
		 zm->hy.virtual_erases);
	/* The table grows, so its pages reach the flash; the hot ones do not. */
	zassert_true(zm->hy.flash_erases < 400 && zm->hy.virtual_erases > zm->hy.flash_erases,
		     "the FRAM should absorb most erases");
	zassert_ok(sqlite3_close(c));

	/* Closed, then mounted again from the flash and the FRAM. */
	zassert_ok(sqnano_db_close(d->db));
	zassert_ok(sqlite3_open("app.db", &c));
	zassert_equal(count(c, "SELECT count(*) FROM sensor WHERE id = 2"), 75);
	zassert_ok(sqnano_db_check(sqnano_sqlite3_db(c)));
	zassert_ok(sqlite3_close(c));
}

ZTEST_SUITE(sqnano_hybrid, NULL, NULL, NULL, NULL, NULL);
