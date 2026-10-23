/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The remote cursor server is an entry point from outside the device (an
 * SMP group): arbitrary CBOR requests, with response buffers of any size,
 * on the test application database (rowid and WITHOUT ROWID tables).
 * Byte 0 of the input picks the buffer size and whether writes are
 * allowed; the rest is the request.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqnano/exec.h>
#include <sqnano/media.h>
#include <sqnano/remote.h>

#include "sqnano_gen.h"

#define MEDIA (128 * 1024)

static uint8_t mem[MEDIA], image[MEDIA];
static struct sqnano_ram_media ram;
static struct sqnano_db db;
static const struct sqnano_db_config cfg = {.page_size = 512, .create = true};
static const struct sqnano_remote_db dbs[] = {{"app.db", &db}};
static union {
	struct sqnano_exec x;
	uint64_t align;
	uint8_t raw[16384];
} st;

static void run(const char *name, const sqnano_value *p, int np)
{
	const struct sqnano_query *q = sqnano_query_find(&sqnano_app_schema, name);

	if (!q || sqnano_exec_open(&st.x, q, &db, NULL)) {
		abort();
	}
	for (int i = 0; i < np; i++) {
		st.x.params[i] = p[i];
	}
	if (sqnano_exec_run(&st.x)) {
		abort();
	}
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	sqnano_value p[3];

	(void)argc;
	(void)argv;
	sqnano_ram_media_init(&ram, mem, MEDIA, SQNANO_RAM_NOR, 4096);
	sqnano_ram_media_blank(&ram);
	if (sqnano_db_open(&db, &ram.media, &cfg, &sqnano_app_schema)) {
		abort();
	}
	for (int i = 0; i < 120; i++) {
		sqnano_v_int(&p[0], i % 5);
		sqnano_v_int(&p[1], 1760780000 + i);
		sqnano_v_real(&p[2], 20.0 + (i % 13) / 4.0);
		run("sensor_add", p, 3);
	}
	sqnano_v_text(&p[0], "name", 4);
	sqnano_v_text(&p[1], "probe", 5);
	run("config_set", p, 2);
	for (int i = 0; i < 30; i++) {
		sqnano_v_text(&p[0], i % 2 ? "pump" : "fan", i % 2 ? 4 : 3);
		sqnano_v_int(&p[1], i);
		sqnano_v_text(&p[2], "on", 2);
		run("event_add", p, 3);
	}
	(void)sqnano_db_close(&db);
	memcpy(image, mem, MEDIA);
	return sqnano_db_open(&db, &ram.media, &cfg, &sqnano_app_schema) ? -1 : 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static uint8_t out[4096];
	bool write;
	size_t cap, olen = 0;
	int rc;

	if (size < 1) {
		return 0;
	}
	write = data[0] & 0x80;
	cap = 8 + (size_t)(data[0] & 0x7f) * 32;
	rc = sqnano_remote_handle(dbs, 1, data + 1, size - 1, out, cap, &olen, write);
	if ((rc == SQNANO_OK && olen > cap) || (rc != SQNANO_OK && rc != SQNANO_TOOBIG)) {
		abort();
	}
	if (write) {
		/* Back to the initial content, so that every input runs the same way. */
		(void)sqnano_db_close(&db);
		memcpy(mem, image, MEDIA);
		if (sqnano_db_open(&db, &ram.media, &cfg, &sqnano_app_schema)) {
			abort();
		}
	}
	return 0;
}
