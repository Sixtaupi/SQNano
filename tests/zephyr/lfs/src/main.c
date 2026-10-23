/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A database in a LittleFS directory: written through sqlite3.h, then read
 * back after closing the database and after remounting the file system.
 */

#include <zephyr/fs/fs.h>
#include <zephyr/ztest.h>

#include <sqlite3.h>
#include <sqnano/zephyr.h>

FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(lfs));

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

static void reopen_db(void)
{
	const struct sqnano_dt_db *d = sqnano_dt_find("app.db");

	zassert_not_null(d);
	if (d->db->is_open) {
		zassert_ok(sqnano_db_close(d->db));
	}
}

ZTEST(sqnano_lfs, test_write_and_read_back)
{
	sqlite3 *c = NULL;
	sqlite3_stmt *st;
	struct sqnano_stats stats;
	struct fs_dirent ent;

	zassert_ok(sqlite3_open("app.db", &c), "%s", sqlite3_errmsg(c));
	zassert_ok(sqlite3_exec(c, "BEGIN", NULL, NULL, NULL));
	zassert_ok(sqlite3_prepare_v2(c, "INSERT INTO sensor (id, ts, value) VALUES (:id, :ts, :value)",
				      -1, &st, NULL));
	for (int i = 0; i < 300; i++) {
		sqlite3_bind_int(st, 1, i % 3);
		sqlite3_bind_int64(st, 2, 5000 + i);
		sqlite3_bind_double(st, 3, i * 0.5);
		zassert_equal(sqlite3_step(st), SQLITE_DONE, "%s", sqlite3_errmsg(c));
		sqlite3_reset(st);
	}
	sqlite3_finalize(st);
	zassert_ok(sqlite3_exec(c, "COMMIT", NULL, NULL, NULL), "%s", sqlite3_errmsg(c));
	zassert_equal(count(c, "SELECT count(*) FROM sensor"), 300);
	zassert_ok(sqnano_db_stats(sqnano_sqlite3_db(c), &stats));
	zassert_equal(stats.mode, SQNANO_MODE_SHADOW, "a file system uses copy-on-write");
	TC_PRINT("littlefs: %u/%u pages used\n", stats.pages_used, stats.pages_total);
	zassert_ok(sqlite3_close(c));
	zassert_ok(fs_stat("/lfs/app.db/0", &ent), "chunk files are in the directory");

	/* Closed and reopened from the files. */
	reopen_db();
	zassert_ok(sqlite3_open("app.db", &c));
	zassert_equal(count(c, "SELECT count(*) FROM sensor WHERE id = 1"), 100);
	zassert_ok(sqlite3_exec(c, "DELETE FROM sensor WHERE ts < 5100", NULL, NULL, NULL));
	zassert_ok(sqlite3_close(c));

	/* The file system unmounted and mounted again. */
	reopen_db();
	zassert_ok(fs_unmount(&FS_FSTAB_ENTRY(DT_NODELABEL(lfs))));
	zassert_ok(fs_mount(&FS_FSTAB_ENTRY(DT_NODELABEL(lfs))));
	zassert_ok(sqlite3_open("app.db", &c));
	zassert_equal(count(c, "SELECT count(*) FROM sensor"), 200);
	zassert_ok(sqnano_db_check(sqnano_sqlite3_db(c)));
	zassert_ok(sqlite3_close(c));
}

ZTEST_SUITE(sqnano_lfs, NULL, NULL, NULL, NULL, NULL);
