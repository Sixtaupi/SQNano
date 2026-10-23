/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Power-cut fuzzing.
 *
 * A deterministic workload of transactions runs on a simulated medium. It is
 * replayed once for every write or erase it performs, with a power cut
 * injected during that operation (torn write, partially erased sector,
 * undefined byte at the cut point). Half of the runs also cut power again
 * during the recovery that follows. After the final remount the database
 * must pass the structural check and hold exactly the state of the last
 * commit or of the interrupted one, and must accept new transactions.
 *
 * Usage: test_powercut [stride]  (stride > 1 samples the cut points)
 */

#include "kv.h"

#define TXNS 10

static uint8_t mem[512 * 1024];
static uint8_t image[512 * 1024];
static struct sqnano_db db;
static struct kv_model states[TXNS + 1];

struct media_case {
	const char *name;
	enum sqnano_ram_kind kind;
	uint32_t erase;
	uint16_t page;
	uint16_t maxlen;
	uint32_t size;
	uint16_t jslots;
	enum sqnano_mode mode;
	uint8_t sb_units;
	/* Extra commits before the workload, to bring the ring near its wrap. */
	uint8_t warm;
};

static const struct media_case cases[] = {
	{"nor-512", SQNANO_RAM_NOR, 512, 512, 100, 256 * 1024, 0},
	{"nor-1024", SQNANO_RAM_NOR, 1024, 1024, 200, 512 * 1024, 0},
	{"fram-256", SQNANO_RAM_FRAM, 0, 256, 60, 192 * 1024, 8},
	{"fram-128", SQNANO_RAM_FRAM, 0, 128, 20, 128 * 1024, 32},
	/* Values longer than a cell: overflow pages. */
	{"nor-512-big", SQNANO_RAM_NOR, 512, 512, 480, 512 * 1024, 0},
	{"fram-256-big", SQNANO_RAM_FRAM, 0, 256, 400, 384 * 1024, 16},
	/* Pages smaller than the erase unit; the small one needs compaction. */
	{"nor-256/4k", SQNANO_RAM_NOR, 4096, 256, 100, 256 * 1024, 0},
	{"nor-512/4k-gc", SQNANO_RAM_NOR, 4096, 512, 120, 72 * 1024, 0},
	/* Superblock rings that wrap during the workload. */
	{"nor-512-ring3", SQNANO_RAM_NOR, 512, 512, 100, 256 * 1024, 0, SQNANO_MODE_AUTO, 3, 18},
	{"eeprom-cow", SQNANO_RAM_FRAM, 0, 256, 60, 128 * 1024, 0, SQNANO_MODE_SHADOW, 2},
};

static int open_db(struct sqnano_ram_media *r, const struct media_case *mc)
{
	struct sqnano_db_config cfg = {
		.page_size = mc->page,
		.create = true,
		.journal_slots = mc->jslots,
		.mode = mc->mode,
		.sb_units = mc->sb_units};

	return sqnano_db_open(&db, &r->media, &cfg, NULL);
}

/* Run the workload; returns the number of transactions committed. */
static int workload(const struct media_case *mc, bool record)
{
	uint32_t seed = 0xc0ffee;
	struct kv_model m;
	int committed = 0;

	m = states[0];
	for (int i = 0; i < TXNS; i++) {
		struct sqnano_txn t;
		int ops = 5 + (int)(test_rand(&seed) % 25);
		int rc = sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1);
		static struct kv_model saved[3];
		uint32_t sp[3];
		int nsp = 0;

		for (int k = 0; k < ops && rc == SQNANO_OK; k++) {
			uint32_t key = test_rand(&seed) % 300;
			uint32_t r = test_rand(&seed) % 16;

			if (SQNANO_MAX_SAVEPOINTS > 0 && r == 0 && nsp < 3) {
				rc = sqnano_txn_savepoint(&t, &sp[nsp]);
				saved[nsp++] = m;
				continue;
			}
			if (SQNANO_MAX_SAVEPOINTS > 0 && r == 1 && nsp > 0) {
				/* Undo back to a random savepoint and check the content. */
				int j = (int)(test_rand(&seed) % (uint32_t)nsp);

				rc = sqnano_txn_rollback_to(&t, sp[j]);
				m = saved[j];
				nsp = j + 1;
				if (rc == SQNANO_OK) {
					rc = kv_verify(&t, &m, mc->maxlen);
					CHECK(rc == SQNANO_OK || !record);
				}
				continue;
			}
			if (SQNANO_MAX_SAVEPOINTS > 0 && r == 2 && nsp > 0) {
				int j = (int)(test_rand(&seed) % (uint32_t)nsp);

				rc = sqnano_txn_release(&t, sp[j]);
				nsp = j;
				continue;
			}
			if (test_rand(&seed) % 4 == 0) {
				bool found;

				rc = kv_del(&t, key, &found);
				m.present[key] = 0;
			} else {
				uint32_t v = test_rand(&seed);

				rc = kv_put(&t, key, v, mc->maxlen);
				m.present[key] = 1;
				m.val[key] = v;
			}
		}
		if (rc == SQNANO_OK) {
			rc = sqnano_txn_commit(&t);
		} else {
			(void)sqnano_txn_rollback(&t);
		}
		if (rc != SQNANO_OK) {
			if (record) {
				fprintf(stderr, "%s: transaction %d failed (%d)\n", mc->name, i, rc);
			}
			CHECK(!record);
			return committed;
		}
		committed++;
		if (record) {
			states[committed] = m;
		}
	}
	return committed;
}

static bool matches(const struct kv_model *m, uint16_t maxlen)
{
	struct sqnano_txn r;
	int rc;

	CHECK_RC(sqnano_txn_begin(&db, &r, SQNANO_TXN_READ, 0), SQNANO_OK);
	kv_verbose = 0;
	rc = kv_verify(&r, m, maxlen);
	kv_verbose = 1;
	CHECK_RC(sqnano_txn_commit(&r), SQNANO_OK);
	return rc == SQNANO_OK;
}

static void run_case(const struct media_case *mc, uint32_t stride)
{
	struct sqnano_ram_media r;
	uint32_t total;
	uint32_t runs = 0, after = 0, recut = 0;

	sqnano_ram_media_init(&r, mem, mc->size, mc->kind, mc->erase);
	sqnano_ram_media_blank(&r);
	CHECK_RC(open_db(&r, mc), SQNANO_OK);
	{
		struct sqnano_txn t;
		uint32_t seed = 77;

		memset(&states[0], 0, sizeof(states[0]));
		CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
		db_lock(&db);
		CHECK_RC(bt_tree_create(&t, KV_TREE), SQNANO_OK);
		db_unlock(&db);
		for (uint32_t k = 0; k < 300; k += 2) {
			states[0].present[k] = 1;
			states[0].val[k] = test_rand(&seed);
			CHECK_RC(kv_put(&t, k, states[0].val[k], mc->maxlen), SQNANO_OK);
		}
		CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
		for (uint8_t i = 0; i < mc->warm; i++) {
			CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
			CHECK_RC(kv_put(&t, 0, states[0].val[0], mc->maxlen), SQNANO_OK);
			CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
		}
	}
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	memcpy(image, mem, mc->size);

	/* Reference run. */
	CHECK_RC(open_db(&r, mc), SQNANO_OK);
	r.ops = 0;
	CHECK(workload(mc, true) == TXNS);
	total = r.ops;
	{
		struct sqnano_stats st;

		CHECK_RC(sqnano_db_stats(&db, &st), SQNANO_OK);
		if (st.compactions) {
			printf("powercut %-8s %u compactions in the reference run\n", mc->name,
			       st.compactions);
		}
	}
	CHECK(matches(&states[TXNS], mc->maxlen));
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);

	for (uint32_t cut = 0; cut < total; cut += stride) {
		int done;
		int rc;

		memcpy(mem, image, mc->size);
		sqnano_ram_media_power_on(&r);
		CHECK_RC(open_db(&r, mc), SQNANO_OK);
		sqnano_ram_media_cut_after(&r, cut, cut * 2654435761u + 1);
		done = workload(mc, false);
		CHECK(r.powered_off);
		sqnano_ram_media_power_on(&r);

		if (cut & 1) {
			/* Second cut during recovery. */
			sqnano_ram_media_cut_after(&r, cut % 3, cut + 7);
			rc = open_db(&r, mc);
			if (rc != SQNANO_OK) {
				recut++;
			}
			sqnano_ram_media_power_on(&r);
		}
		rc = open_db(&r, mc);
		if (rc != SQNANO_OK) {
			fprintf(stderr, "%s: cut %u: remount failed (%d)\n", mc->name, cut, rc);
			exit(1);
		}
		if (sqnano_db_check(&db) != SQNANO_OK) {
			fprintf(stderr, "%s: cut %u: integrity check failed\n", mc->name, cut);
			exit(1);
		}
		if (matches(&states[done], mc->maxlen)) {
			/* Last committed state. */
		} else if (done < TXNS && matches(&states[done + 1], mc->maxlen)) {
			after++;
		} else {
			fprintf(stderr, "%s: cut %u after %d commits: unexpected content\n", mc->name,
				cut, done);
			exit(1);
		}
		/* The database must stay writable after recovery. */
		{
			struct sqnano_txn t;

			CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
			CHECK_RC(kv_put(&t, 299, 5, mc->maxlen), SQNANO_OK);
			CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
			CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
		}
		CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
		runs++;
	}
	/* Flash: nothing was ever programmed without an erase. */
	CHECK(r.overwrites == 0);
	printf("powercut %-9s %u cut points (%u ops), %u ended on the new state, %u recovery "
	       "cuts: ok\n",
	       mc->name, runs, total, after, recut);
}

int main(int argc, char **argv)
{
	uint32_t stride = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 1;

	if (stride == 0) {
		stride = 1;
	}
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		run_case(&cases[i], stride);
	}
	return 0;
}
