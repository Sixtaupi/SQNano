/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Runner for the differential test (tests/host/difftest.py).
 *
 * Reads commands on stdin:
 *   Q <query name> <param>...   run a compiled query
 *   C                           integrity check
 *   X                           close and reopen the database
 * Values are encoded as n (NULL), i:<int>, r:<hex float>, t:<hex utf-8>,
 * b:<hex bytes>. Output: one "R <value>..." line per row, then
 * "D <changes>" or "E <primary result code> <message>".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqnano_gen.h"

static uint8_t mem[4 * 1024 * 1024];
static struct sqnano_ram_media ram;
static struct sqnano_db db;
static union {
	struct sqnano_exec x;
	uint64_t align;
	uint8_t raw[65536];
} state;
static uint8_t textbuf[64][512];

static int hexval(char c)
{
	return c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
}

static void decode(char *tok, sqnano_value *v, int slot)
{
	if (tok[0] == 'n') {
		sqnano_v_null(v);
	} else if (tok[0] == 'i') {
		sqnano_v_int(v, strtoll(tok + 2, NULL, 10));
	} else if (tok[0] == 'r') {
		sqnano_v_real(v, strtod(tok + 2, NULL));
	} else {
		size_t n = strlen(tok + 2) / 2;
		uint8_t *b = textbuf[slot];

		for (size_t i = 0; i < n && i < sizeof(textbuf[0]) - 1; i++) {
			b[i] = (uint8_t)(hexval(tok[2 + 2 * i]) * 16 + hexval(tok[3 + 2 * i]));
		}
		b[n] = 0;
		if (tok[0] == 't') {
			sqnano_v_text(v, (const char *)b, (uint32_t)n);
		} else {
			sqnano_v_blob(v, b, (uint32_t)n);
		}
	}
}

static void encode(const sqnano_value *v)
{
	switch (v->type) {
	case SQNANO_INTEGER:
		printf(" i:%lld", (long long)v->u.i);
		break;
	case SQNANO_FLOAT:
		printf(" r:%a", v->u.r);
		break;
	case SQNANO_TEXT:
	case SQNANO_BLOB:
		printf(" %c:", v->type == SQNANO_TEXT ? 't' : 'b');
		for (uint32_t i = 0; i < v->n; i++) {
			printf("%02x", v->u.p[i]);
		}
		break;
	default:
		printf(" n");
		break;
	}
}

static void open_db(void)
{
	struct sqnano_db_config cfg = {.page_size = 512, .create = true};
	int rc = sqnano_db_open(&db, &ram.media, &cfg, &sqnano_app_schema);

	if (rc) {
		fprintf(stderr, "open failed: %d\n", rc);
		exit(1);
	}
	/* The reference runs with PRAGMA foreign_keys = ON. */
	(void)sqnano_db_foreign_keys(&db, 1);
}

static struct sqnano_txn txn;
static bool in_txn;
static char sp_names[16][16];
static uint32_t sp_ids[16];
static int nsp;

static int find_sp(const char *name)
{
	for (int i = nsp - 1; i >= 0; i--) {
		if (name && strcmp(sp_names[i], name) == 0) {
			return i;
		}
	}
	return -1;
}

/* Transaction commands; prints "D <rc>" (0 on success). */
static int txn_cmd(char cmd, const char *name)
{
	int i, rc;

	switch (cmd) {
	case 'B':
		rc = sqnano_txn_begin(&db, &txn, SQNANO_TXN_WRITE, -1);
		in_txn = rc == SQNANO_OK;
		nsp = 0;
		return rc;
	case 'M':
	case 'K':
		rc = cmd == 'M' ? sqnano_txn_commit(&txn) : sqnano_txn_rollback(&txn);
		if (rc == SQNANO_CONSTRAINT_FOREIGNKEY) {
			return rc & 0xff; /* deferred violations: still open */
		}
		in_txn = false;
		nsp = 0;
		return rc;
	case 'S':
		if (nsp == 16) {
			return SQNANO_FULL;
		}
		rc = sqnano_txn_savepoint(&txn, &sp_ids[nsp]);
		if (rc == SQNANO_OK) {
			snprintf(sp_names[nsp++], sizeof(sp_names[0]), "%s", name);
		}
		return rc;
	default:
		i = find_sp(name);
		if (i < 0) {
			return SQNANO_ERROR;
		}
		if (cmd == 'T') {
			rc = sqnano_txn_rollback_to(&txn, sp_ids[i]);
			nsp = i + 1;
		} else {
			rc = sqnano_txn_release(&txn, sp_ids[i]);
			nsp = i;
		}
		return rc;
	}
}

int main(int argc, char **argv)
{
	static char line[16384];
	enum sqnano_ram_kind kind = SQNANO_RAM_NOR;

	uint32_t erase = 512;

	if (argc > 1 && strcmp(argv[1], "fram") == 0) {
		kind = SQNANO_RAM_FRAM;
	}
	if (argc > 1 && strcmp(argv[1], "nor4k") == 0) {
		/* 512-byte pages in 4 KiB erase units: compaction. */
		erase = 4096;
	}
	sqnano_ram_media_init(&ram, mem, erase == 4096 ? 256 * 1024 : sizeof(mem), kind, erase);
	sqnano_ram_media_blank(&ram);
	open_db();
	while (fgets(line, sizeof(line), stdin)) {
		char *save = NULL;
		char *cmd = strtok_r(line, " \n", &save);
		const struct sqnano_query *q;
		struct sqnano_exec *x = &state.x;
		int rc;
		int np = 0;

		if (!cmd) {
			continue;
		}
		if (strcmp(cmd, "C") == 0 && getenv("DIFF_STATS")) {
			struct sqnano_stats st;

			(void)sqnano_db_stats(&db, &st);
			fprintf(stderr, "stats: used %u/%u compactions %u erases %u sort runs %u\n",
				st.pages_used, st.pages_total, st.compactions, st.erases, st.sort_runs);
		}
		if (strcmp(cmd, "C") == 0) {
			int crc = sqnano_db_check(&db);

			/* No statement is running: every sort run must have been freed. */
			for (uint32_t w = 0; w < SQNANO_BM_WORDS && crc == SQNANO_OK; w++) {
				crc = db.bm_temp[w] ? -100 : crc;
			}
			printf("D %d\n", crc);
			fflush(stdout);
			continue;
		}
		if (strchr("BMKSTL", cmd[0]) && cmd[1] == 0) {
			printf("D %d\n", txn_cmd(cmd[0], strtok_r(NULL, " \n", &save)));
			fflush(stdout);
			continue;
		}
		if (strcmp(cmd, "X") == 0) {
			(void)sqnano_db_close(&db);
			open_db();
			printf("D 0\n");
			fflush(stdout);
			continue;
		}
		q = sqnano_query_find(&sqnano_app_schema, strtok_r(NULL, " \n", &save));
		/* state holds 64 KiB: any state_size (16 bits) fits. */
		if (!q) {
			printf("E 1 no such query\n");
			fflush(stdout);
			continue;
		}
		sqnano_exec_open(x, q, &db, in_txn ? &txn : NULL);
		for (char *tok; (tok = strtok_r(NULL, " \n", &save)) != NULL && np < q->nparams; np++) {
			decode(tok, &x->params[np], np);
		}
		while ((rc = sqnano_exec_step(x)) == SQNANO_ROW) {
			printf("R");
			for (int i = 0; i < q->ncols; i++) {
				encode(&x->out[i]);
			}
			printf("\n");
		}
		if (in_txn && !txn.active) {
			/* OR ROLLBACK ended the transaction. */
			in_txn = false;
			nsp = 0;
		}
		if (rc == SQNANO_DONE) {
			printf("D %lld\n", (long long)x->changes);
		} else {
			if ((rc & 0xff) == SQNANO_FULL && getenv("DIFF_FULL")) {
				struct sqnano_stats st;

				(void)sqnano_db_stats(&db, &st);
				fprintf(stderr, "FULL: used %u/%u limbo %u compactions %u in_txn %d\n",
					st.pages_used, st.pages_total, st.pages_limbo, st.compactions,
					in_txn);
			}
			printf("E %d %s\n", rc & 0xff, x->errmsg);
		}
		fflush(stdout);
	}
	return 0;
}
