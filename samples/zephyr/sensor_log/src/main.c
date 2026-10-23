/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Sensor log: plain SQLite code (sqlite3.h) on a flash partition, plus the
 * typed API generated for a named query. The "db" shell command stays
 * available afterwards:
 *
 *   uart:~$ db .tables
 *   uart:~$ db SELECT ts, value FROM sensor WHERE id = 3 ORDER BY ts DESC LIMIT 5
 *   uart:~$ db .run sensor_last id=3 n=2
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif

#include <sqlite3.h>

#include "sqnano_gen.h"

static int check(sqlite3 *db, int rc, const char *what)
{
	if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
		printk("%s failed: %s\n", what, sqlite3_errmsg(db));
	}
	return rc;
}

static void log_readings(sqlite3 *db)
{
	sqlite3_stmt *st;

	check(db, sqlite3_exec(db, "BEGIN", NULL, NULL, NULL), "BEGIN");
	check(db,
	      sqlite3_prepare_v2(db, "INSERT INTO sensor (id, ts, value) VALUES (?, ?, ?)", -1, &st,
				 NULL),
	      "prepare");
	for (int i = 0; i < 64; i++) {
		sqlite3_bind_int(st, 1, i % 4);
		sqlite3_bind_int64(st, 2, 1760782000 + 15 * i);
		sqlite3_bind_double(st, 3, 19.5 + (i * 7 % 23) / 10.0);
		check(db, sqlite3_step(st), "insert");
		sqlite3_reset(st);
	}
	sqlite3_finalize(st);
	check(db, sqlite3_exec(db, "COMMIT", NULL, NULL, NULL), "COMMIT");
}

static void show_last(sqlite3 *db, int id)
{
	sqlite3_stmt *st;

	printk("sensor %d: last readings\n", id);
	check(db,
	      sqlite3_prepare_v2(db, "SELECT ts, value FROM sensor WHERE id = :id ORDER BY ts DESC "
				     "LIMIT :n",
				 -1, &st, NULL),
	      "prepare");
	sqlite3_bind_int(st, sqlite3_bind_parameter_index(st, ":id"), id);
	sqlite3_bind_int(st, sqlite3_bind_parameter_index(st, ":n"), 3);
	while (sqlite3_step(st) == SQLITE_ROW) {
		printk("  ts=%lld value=%s\n", (long long)sqlite3_column_int64(st, 0),
		       (const char *)sqlite3_column_text(st, 1));
	}
	sqlite3_finalize(st);
}

static void show_stats(sqlite3 *db)
{
	struct q_sensor_stats q;

	/* Typed API: fields named after the result columns. */
	if (q_sensor_stats_open(&q, sqnano_sqlite3_db(db), NULL)) {
		return;
	}
	while (q_sensor_stats_step(&q) == SQNANO_ROW) {
		printk("sensor %lld: %lld readings, min %d.%02d max %d.%02d\n", (long long)q.id,
		       (long long)q.count, (int)q.min_value, (int)(q.min_value * 100) % 100,
		       (int)q.max_value, (int)(q.max_value * 100) % 100);
	}
}

int main(void)
{
	sqlite3 *db;

	if (sqlite3_open("/data/app.db", &db) != SQLITE_OK) {
		printk("open failed: %s\n", sqlite3_errmsg(db));
		return 0;
	}
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
	/*
	 * Confirm the image and the schema migration together: until then an
	 * MCUboot revert finds the pre-migration database intact.
	 */
	if (boot_write_img_confirmed() == 0) {
		(void)sqnano_migration_confirm(sqnano_sqlite3_db(db));
	}
#endif
	log_readings(db);
	show_last(db, 3);
	show_stats(db);
	check(db, sqlite3_exec(db, "DELETE FROM sensor WHERE ts < 1760782300", NULL, NULL, NULL),
	      "purge");
	printk("purged %d rows\n", sqlite3_changes(db));
	sqlite3_close(db);
	{
		/* What the statements needed: CONFIG_SQNANO_STMT_POOL_SIZE can be set from it. */
		struct sqnano_pool_stats ps;

		if (sqnano_pool_stats(&sqnano_app_schema, &ps) == SQNANO_OK) {
			printk("statement pool: peak %u of %u bytes\n", ps.peak, ps.size);
		}
	}
	printk("sensor_log done; try the shell: db .help\n");
	return 0;
}
