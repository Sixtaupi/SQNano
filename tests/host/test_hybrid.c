/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Hybrid media (NOR flash behind a FRAM): power cuts during every write and
 * erase of either memory, including during recovery, with a FRAM small
 * enough to evict all the time; then the flash wear with and without the
 * FRAM.
 */

#include "kv.h"

#define TXNS 10

struct hcase {
	const char *name;
	uint32_t erase;
	uint16_t page;
	uint32_t nor_size;
	uint32_t fram_size;
	uint16_t maxlen;
};

static const struct hcase cases[] = {
	{"nor-4k/512+fram-16k", 4096, 512, 128 * 1024, 16 * 1024, 100},
	{"nor-512/512+fram-8k", 512, 512, 128 * 1024, 8 * 1024, 100},
	{"nor-4k/1k+fram-24k", 4096, 1024, 192 * 1024, 24 * 1024, 300},
};

static uint8_t nor_mem[256 * 1024], fram_mem[32 * 1024];
static uint8_t nor_image[256 * 1024], fram_image[32 * 1024];
static struct sqnano_ram_media nor, fram;
static struct sqnano_hybrid_media hy;
static struct sqnano_db db;
static struct kv_model states[TXNS + 1];

static int open_db(const struct hcase *hc)
{
	struct sqnano_db_config cfg = {.page_size = hc->page, .create = true};

	sqnano_hybrid_media_init(&hy, &nor.media, &fram.media, hc->page);
	return sqnano_db_open(&db, &hy.media, &cfg, NULL);
}

static void power_on(void)
{
	sqnano_ram_media_power_on(&nor);
	fram.powered_off = false;
}

static int workload(const struct hcase *hc, bool record)
{
	uint32_t seed = 0xabcd;
	struct kv_model m = states[0];
	int committed = 0;

	for (int i = 0; i < TXNS; i++) {
		struct sqnano_txn t;
		int rc = sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1);

		for (int k = 0; k < 15 && rc == SQNANO_OK; k++) {
			uint32_t key = test_rand(&seed) % 300;

			if (test_rand(&seed) % 4 == 0) {
				bool found;

				rc = kv_del(&t, key, &found);
				m.present[key] = 0;
			} else {
				uint32_t v = test_rand(&seed);

				rc = kv_put(&t, key, v, hc->maxlen);
				m.present[key] = 1;
				m.val[key] = v;
			}
		}
		rc = rc == SQNANO_OK ? sqnano_txn_commit(&t) : (sqnano_txn_rollback(&t), rc);
		if (rc) {
			if (record) {
				fprintf(stderr, "%s: transaction %d failed (%d)\n", hc->name, i, rc);
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

static void setup(const struct hcase *hc)
{
	struct sqnano_txn t;
	uint32_t seed = 77;

	sqnano_ram_media_init(&nor, nor_mem, hc->nor_size, SQNANO_RAM_NOR, hc->erase);
	sqnano_ram_media_init(&fram, fram_mem, hc->fram_size, SQNANO_RAM_FRAM, 0);
	sqnano_ram_media_blank(&nor);
	sqnano_ram_media_blank(&fram);
	fram.group = &nor;
	CHECK_RC(open_db(hc), SQNANO_OK);
	memset(&states[0], 0, sizeof(states[0]));
	CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
	db_lock(&db);
	CHECK_RC(bt_tree_create(&t, KV_TREE), SQNANO_OK);
	db_unlock(&db);
	for (uint32_t k = 0; k < 300; k += 2) {
		states[0].present[k] = 1;
		states[0].val[k] = test_rand(&seed);
		CHECK_RC(kv_put(&t, k, states[0].val[k], hc->maxlen), SQNANO_OK);
	}
	CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	memcpy(nor_image, nor_mem, hc->nor_size);
	memcpy(fram_image, fram_mem, hc->fram_size);
}

static void run_case(const struct hcase *hc)
{
	uint32_t total, after = 0, recut = 0;

	setup(hc);
	CHECK_RC(open_db(hc), SQNANO_OK);
	nor.gops = 0;
	CHECK(workload(hc, true) == TXNS);
	total = nor.gops;
	CHECK(matches(&states[TXNS], hc->maxlen));
	printf("hybrid %-22s reference: %u ops, %u evictions, %u flash programs, %u flash "
	       "erases, %u erases kept in FRAM, %u refreshes\n",
	       hc->name, total, hy.evictions, hy.flash_programs, hy.flash_erases, hy.virtual_erases,
	       hy.refreshes);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);

	for (uint32_t cut = 0; cut < total; cut++) {
		int done, rc;

		memcpy(nor_mem, nor_image, hc->nor_size);
		memcpy(fram_mem, fram_image, hc->fram_size);
		power_on();
		CHECK_RC(open_db(hc), SQNANO_OK);
		sqnano_ram_media_cut_after(&nor, cut, cut * 2654435761u + 1);
		done = workload(hc, false);
		CHECK(nor.powered_off);
		power_on();
		if (cut & 1) {
			/* A second cut during the recovery of both memories. */
			sqnano_ram_media_cut_after(&nor, cut % 5, cut + 3);
			if (open_db(hc) != SQNANO_OK) {
				recut++;
			}
			power_on();
		}
		rc = open_db(hc);
		if (rc != SQNANO_OK) {
			fprintf(stderr, "%s: cut %u: remount failed (%d)\n", hc->name, cut, rc);
			exit(1);
		}
		CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
		if (!matches(&states[done], hc->maxlen)) {
			if (done >= TXNS || !matches(&states[done + 1], hc->maxlen)) {
				fprintf(stderr, "%s: cut %u after %d commits: unexpected content\n",
					hc->name, cut, done);
				exit(1);
			}
			after++;
		}
		CHECK(workload(hc, false) == TXNS);
		CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
		CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
		/* The flash is only programmed where it was erased. */
		CHECK(nor.overwrites == 0);
	}
	printf("hybrid %-22s %u cut points, %u ended on the new state, %u recovery cuts: ok\n",
	       hc->name, total, after, recut);
}

/* A rolling log, one row per commit: flash erases per commit with and without FRAM. */
static void wear(void)
{
	static const uint32_t fram_sizes[] = {0, 16 * 1024, 32 * 1024};
	const uint32_t commits = 3000;

	for (size_t f = 0; f < sizeof(fram_sizes) / sizeof(fram_sizes[0]); f++) {
		struct sqnano_db_config cfg = {.page_size = 512, .create = true};
		struct sqnano_media *m;
		struct sqnano_txn t;
		uint32_t erases0;

		sqnano_ram_media_init(&nor, nor_mem, 256 * 1024, SQNANO_RAM_NOR, 4096);
		sqnano_ram_media_init(&fram, fram_mem, fram_sizes[f] ? fram_sizes[f] : 1024,
				      SQNANO_RAM_FRAM, 0);
		sqnano_ram_media_blank(&nor);
		sqnano_ram_media_blank(&fram);
		sqnano_hybrid_media_init(&hy, &nor.media, &fram.media, 512);
		m = fram_sizes[f] ? &hy.media : &nor.media;
		CHECK_RC(sqnano_db_open(&db, m, &cfg, NULL), SQNANO_OK);
		CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
		db_lock(&db);
		CHECK_RC(bt_tree_create(&t, KV_TREE), SQNANO_OK);
		db_unlock(&db);
		CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
		erases0 = nor.erases;
		for (uint32_t k = 0; k < commits + 200; k++) {
			bool found;

			if (k == 200) {
				erases0 = nor.erases;
			}
			CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
			if (k >= 200) {
				CHECK_RC(kv_del(&t, k - 200, &found), SQNANO_OK);
			}
			CHECK_RC(kv_put(&t, k, k, 32), SQNANO_OK);
			CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
		}
		CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
		CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
		printf("wear: %2u KB FRAM: %.3f flash erases per commit\n", fram_sizes[f] / 1024,
		       (double)(nor.erases - erases0) / commits);
	}
}

int main(void)
{
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		run_case(&cases[i]);
	}
	wear();
	return 0;
}
