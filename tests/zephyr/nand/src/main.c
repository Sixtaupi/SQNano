/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * NAND media over a flash partition: the flash simulator rejects any page
 * programmed twice without an erase. One row per commit, then the database
 * read back after closing it.
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

ZTEST(sqnano_nand, test_commits_and_reopen)
{
	const struct sqnano_dt_db *d = sqnano_dt_find("app.db");
	struct sqnano_nand_zmedia *zm;
	struct sqnano_stats st;
	sqlite3 *c = NULL;
	sqlite3_stmt *ins;

	zassert_not_null(d);
	zm = CONTAINER_OF(d->media, struct sqnano_nand_zmedia, nm.media);
	zassert_ok(sqlite3_open("app.db", &c), "%s", sqlite3_errmsg(c));
	zassert_equal(zm->geo.page_size, 512);
	zassert_equal(zm->geo.pages_per_block, 16);
	zassert_ok(sqnano_db_stats(sqnano_sqlite3_db(c), &st));
	/* 512 - 16 bytes of tag - 2 bytes of ECC, rounded down to 16. */
	zassert_equal(st.page_size, 480);
	zassert_ok(sqlite3_prepare_v2(c, "INSERT INTO sensor (id, ts, value) VALUES (:id, :ts, :value)",
				      -1, &ins, NULL));
	for (int i = 0; i < 400; i++) {
		sqlite3_bind_int(ins, 1, i % 4);
		sqlite3_bind_int64(ins, 2, 9000 + i);
		sqlite3_bind_double(ins, 3, i * 0.25);
		zassert_equal(sqlite3_step(ins), SQLITE_DONE, "%s", sqlite3_errmsg(c));
		sqlite3_reset(ins);
	}
	sqlite3_finalize(ins);
	zassert_ok(sqlite3_exec(c, "DELETE FROM sensor WHERE ts < 9100", NULL, NULL, NULL));
	zassert_equal(count(c, "SELECT count(*) FROM sensor"), 300);
	TC_PRINT("nand: %u units of %u pages, 400 commits: %u page programs, %u block erases, "
		 "%u units moved\n",
		 zm->nm.units, zm->nm.spb, zm->nm.programs, zm->nm.erases, zm->nm.relocations);
	zassert_ok(sqlite3_close(c));

	/* Mounted again: the mapping comes back from the page tags. */
	zassert_ok(sqnano_db_close(d->db));
	zassert_ok(sqlite3_open("app.db", &c));
	zassert_equal(count(c, "SELECT count(*) FROM sensor WHERE id = 2"), 75);
	zassert_ok(sqnano_db_check(sqnano_sqlite3_db(c)));
	zassert_ok(sqlite3_close(c));
}

ZTEST_SUITE(sqnano_nand, NULL, NULL, NULL, NULL, NULL);
