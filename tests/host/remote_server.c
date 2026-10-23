/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The remote cursor server on the host, for the sqnano-cli tests: SMP
 * packets on stdin, answers on stdout, as the Zephyr SMP group does them.
 * The database (the test application schema) is filled with sample rows.
 *
 * Usage: remote_server [response capacity in bytes]
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sqnano/exec.h>
#include <sqnano/media.h>
#include <sqnano/remote.h>

#include "sqnano_gen.h"

static uint8_t mem[512 * 1024];
static struct sqnano_ram_media ram;
static struct sqnano_db db;
static union {
	struct sqnano_exec x;
	uint64_t align;
	uint8_t raw[16384];
} st;

static void run(const char *name, const sqnano_value *p, int np)
{
	const struct sqnano_query *q = sqnano_query_find(&sqnano_app_schema, name);
	int rc;

	if (!q || sqnano_exec_open(&st.x, q, &db, NULL)) {
		fprintf(stderr, "cannot open %s\n", name);
		exit(1);
	}
	for (int i = 0; i < np; i++) {
		st.x.params[i] = p[i];
	}
	rc = sqnano_exec_run(&st.x);
	if (rc) {
		fprintf(stderr, "%s: %s\n", name, st.x.errmsg);
		exit(1);
	}
}

static void fill(void)
{
	static const uint8_t blob[] = {0, 1, 2, 0xff};
	sqnano_value p[3];

	for (int i = 0; i < 300; i++) {
		sqnano_v_int(&p[0], i % 5);
		sqnano_v_int(&p[1], 1760780000 + i);
		if (i % 17 == 0) {
			sqnano_v_null(&p[2]);
		} else {
			sqnano_v_real(&p[2], 20.0 + (i % 13) / 4.0);
		}
		run("sensor_add", p, 3);
	}
	sqnano_v_text(&p[0], "name", 4);
	sqnano_v_text(&p[1], "probe \"A\" été", 15);
	run("config_set", p, 2);
	sqnano_v_text(&p[0], "raw", 3);
	sqnano_v_blob(&p[1], blob, sizeof(blob));
	run("config_set", p, 2);
	sqnano_v_text(&p[0], "gain", 4);
	sqnano_v_real(&p[1], -1.5e-3);
	run("config_set", p, 2);
	sqnano_v_text(&p[0], "big", 3);
	sqnano_v_int(&p[1], INT64_MIN);
	run("config_set", p, 2);
	for (int i = 0; i < 40; i++) {
		static const char *const devs[] = {"pump", "valve", "fan"};
		const char *d = devs[i % 3];

		sqnano_v_text(&p[0], d, (uint32_t)strlen(d));
		sqnano_v_int(&p[1], i);
		sqnano_v_text(&p[2], i % 2 ? "on" : "off", i % 2 ? 2 : 3);
		run("event_add", p, 3);
	}
}

static int read_all(uint8_t *b, size_t n)
{
	while (n) {
		ssize_t r = read(0, b, n);

		if (r <= 0) {
			return -1;
		}
		b += r;
		n -= (size_t)r;
	}
	return 0;
}

int main(int argc, char **argv)
{
	static uint8_t req[4096], resp[65536], pkt[65536 + 16];
	struct sqnano_db_config cfg = {.page_size = 512, .create = true};
	struct sqnano_remote_db dbs[1] = {{"app.db", &db}};
	size_t cap = argc > 1 ? (size_t)atoi(argv[1]) : 2048;
	uint8_t hdr[8];

	sqnano_ram_media_init(&ram, mem, sizeof(mem), SQNANO_RAM_NOR, 512);
	sqnano_ram_media_blank(&ram);
	if (sqnano_db_open(&db, &ram.media, &cfg, &sqnano_app_schema)) {
		return 1;
	}
	fill();
	/* SMP header: op, flags, length, group, sequence, command. */
	while (read_all(hdr, 8) == 0) {
		uint16_t len = (uint16_t)(hdr[2] << 8 | hdr[3]);
		size_t olen = 0, n = 0;
		int rc;

		if (len > sizeof(req) || read_all(req, len)) {
			return 1;
		}
		rc = sqnano_remote_handle(dbs, 1, req, len, resp, cap, &olen, true);
		if (rc) {
			return 1;
		}
		/* Payload {"d": bytes} as the Zephyr SMP group writes it. */
		pkt[8 + n++] = 0xa1;
		pkt[8 + n++] = 0x61;
		pkt[8 + n++] = 'd';
		pkt[8 + n++] = 0x59;
		pkt[8 + n++] = (uint8_t)(olen >> 8);
		pkt[8 + n++] = (uint8_t)olen;
		memcpy(pkt + 8 + n, resp, olen);
		n += olen;
		memcpy(pkt, hdr, 8);
		pkt[0] = (uint8_t)(hdr[0] + 1);
		pkt[2] = (uint8_t)(n >> 8);
		pkt[3] = (uint8_t)n;
		if (write(1, pkt, 8 + n) != (ssize_t)(8 + n)) {
			return 1;
		}
	}
	return 0;
}
