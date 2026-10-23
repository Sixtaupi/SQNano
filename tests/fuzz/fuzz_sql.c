/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The sqlite3.h facade on SQL text from the input: normalization, lookup
 * of the compiled statement, binding and stepping. The input is the SQL
 * text, then after a NUL byte the bytes the parameters are made of.
 */

#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>
#include <sqnano/media.h>

#include "sqnano_gen.h"

#define MEDIA (128 * 1024)

static uint8_t mem[MEDIA], image[MEDIA];
static struct sqnano_ram_media ram;
static struct sqnano_db db;
static const struct sqnano_db_config cfg = {.page_size = 512, .create = true};

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	sqlite3 *c;

	(void)argc;
	(void)argv;
	sqnano_ram_media_init(&ram, mem, MEDIA, SQNANO_RAM_NOR, 4096);
	sqnano_ram_media_blank(&ram);
	if (sqnano_sqlite3_register("app.db", &db, &ram.media, &cfg, &sqnano_app_schema) ||
	    sqlite3_open("app.db", &c)) {
		abort();
	}
	for (int i = 0; i < 50; i++) {
		sqlite3_stmt *st;

		if (sqlite3_prepare_v2(c, "INSERT INTO sensor (id, ts, value) VALUES (:id, :ts, :value)",
				       -1, &st, NULL)) {
			abort();
		}
		sqlite3_bind_int(st, 1, i % 4);
		sqlite3_bind_int64(st, 2, 1000 + i);
		sqlite3_bind_double(st, 3, i * 0.5);
		if (sqlite3_step(st) != SQLITE_DONE) {
			abort();
		}
		sqlite3_finalize(st);
	}
	sqlite3_close(c);
	(void)sqnano_db_close(&db);
	memcpy(image, mem, MEDIA);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static char sql[4097];
	const uint8_t *p, *end = data + size;
	const char *tail;
	sqlite3_stmt *st;
	sqlite3 *c;
	size_t n = 0;

	while (n < size && data[n] && n < sizeof(sql) - 1) {
		sql[n] = (char)data[n];
		n++;
	}
	sql[n] = 0;
	p = data + n + (n < size);
	if (sqlite3_open("app.db", &c)) {
		abort();
	}
	tail = sql;
	for (int k = 0; k < 4 && tail && *tail; k++) {
		const char *next = NULL;

		if (sqlite3_prepare_v2(c, tail, -1, &st, &next) != SQLITE_OK || !st) {
			break;
		}
		for (int i = 1; i <= sqlite3_bind_parameter_count(st) && p < end; i++) {
			uint8_t kind = *p++;
			size_t len = kind >> 3 < (size_t)(end - p) ? kind >> 3 : (size_t)(end - p);

			switch (kind & 7) {
			case 0:
				sqlite3_bind_null(st, i);
				break;
			case 1:
				sqlite3_bind_int64(st, i, (int64_t)(kind * 0x0101010101010101ull - 7u));
				break;
			case 2:
				sqlite3_bind_double(st, i, (double)(int8_t)kind / 3.0);
				break;
			case 3:
				sqlite3_bind_text(st, i, (const char *)p, (int)len, SQLITE_TRANSIENT);
				p += len;
				break;
			default:
				sqlite3_bind_blob(st, i, p, (int)len, SQLITE_TRANSIENT);
				p += len;
			}
		}
		for (int r = 0; r < 200 && sqlite3_step(st) == SQLITE_ROW; r++) {
			for (int col = 0; col < sqlite3_column_count(st); col++) {
				(void)sqlite3_column_text(st, col);
			}
		}
		sqlite3_finalize(st);
		tail = next;
	}
	(void)sqlite3_exec(c, sql, NULL, NULL, NULL);
	sqlite3_close(c);
	/* Writes are undone for the next input. */
	(void)sqnano_db_close(&db);
	memcpy(mem, image, MEDIA);
	return 0;
}
