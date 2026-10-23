/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Engine tests: randomized B+tree operations against a model on every
 * media kind and several page sizes, rollbacks, remounts, snapshot
 * isolation and space reclamation.
 */

#include "kv.h"

static uint8_t mem[1024 * 1024];
static struct sqnano_db db;

struct media_case {
	const char *name;
	enum sqnano_ram_kind kind;
	uint32_t erase;
	uint16_t page;
	uint16_t maxlen;
	uint32_t size;
};

static const struct media_case cases[] = {
	{"nor-512", SQNANO_RAM_NOR, 512, 512, 120, 512 * 1024},
	{"nor-4096", SQNANO_RAM_NOR, 4096, 4096, 400, 1024 * 1024},
	{"fram-128", SQNANO_RAM_FRAM, 0, 128, 20, 160 * 1024},
	{"fram-256", SQNANO_RAM_FRAM, 0, 256, 60, 256 * 1024},
	{"fram-1024", SQNANO_RAM_FRAM, 0, 1024, 250, 512 * 1024},
};

static void open_db(struct sqnano_ram_media *r, const struct media_case *mc)
{
	struct sqnano_db_config cfg = {.page_size = mc->page, .create = true, .journal_slots = 8};

	CHECK_RC(sqnano_db_open(&db, &r->media, &cfg, NULL), SQNANO_OK);
}

static void create_tree(void)
{
	struct sqnano_txn t;

	CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
	db_lock(&db);
	CHECK_RC(bt_tree_create(&t, KV_TREE), SQNANO_OK);
	db_unlock(&db);
	CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
}

static void verify_committed(const struct kv_model *m, uint16_t maxlen)
{
	struct sqnano_txn r;

	CHECK_RC(sqnano_txn_begin(&db, &r, SQNANO_TXN_READ, 0), SQNANO_OK);
	CHECK_RC(kv_verify(&r, m, maxlen), SQNANO_OK);
	CHECK_RC(sqnano_txn_commit(&r), SQNANO_OK);
}

static void random_case(const struct media_case *mc)
{
	static struct kv_model model, work;
	struct sqnano_ram_media r;
	uint32_t seed = 0x9e3779b9u ^ mc->page;

	sqnano_ram_media_init(&r, mem, mc->size, mc->kind, mc->erase);
	sqnano_ram_media_blank(&r);
	open_db(&r, mc);
	create_tree();
	memset(&model, 0, sizeof(model));

	for (int round = 0; round < 300; round++) {
		struct sqnano_txn t;
		int ops = 1 + (int)(test_rand(&seed) % 40);

		work = model;
		CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
		for (int i = 0; i < ops; i++) {
			uint32_t key = test_rand(&seed) % KV_MAX;

			if (test_rand(&seed) % 3 == 0) {
				bool found;

				CHECK_RC(kv_del(&t, key, &found), SQNANO_OK);
				CHECK(found == (bool)work.present[key]);
				work.present[key] = 0;
			} else {
				uint32_t v = test_rand(&seed);

				CHECK_RC(kv_put(&t, key, v, mc->maxlen), SQNANO_OK);
				work.present[key] = 1;
				work.val[key] = v;
			}
		}
		if (round % 7 == 0) {
			CHECK_RC(kv_verify(&t, &work, mc->maxlen), SQNANO_OK);
		}
		if (test_rand(&seed) % 6 == 0) {
			CHECK_RC(sqnano_txn_rollback(&t), SQNANO_OK);
		} else {
			CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
			model = work;
		}
		verify_committed(&model, mc->maxlen);
		if (round % 50 == 49) {
			CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
			CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
			open_db(&r, mc);
			verify_committed(&model, mc->maxlen);
		}
	}
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);

	/* Delete everything: the space must come back. */
	{
		struct sqnano_txn t;
		struct sqnano_stats st;

		CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
		for (uint32_t key = 0; key < KV_MAX; key++) {
			bool found;

			CHECK_RC(kv_del(&t, key, &found), SQNANO_OK);
			model.present[key] = 0;
		}
		CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
		verify_committed(&model, mc->maxlen);
		CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
		CHECK_RC(sqnano_db_stats(&db, &st), SQNANO_OK);
		/* Master tree root + empty KV tree root. */
		CHECK(st.pages_used == 2);
	}
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	printf("random %-10s ok\n", mc->name);
}

static void snapshot_case(const struct media_case *mc)
{
	static struct kv_model a, b, c;
	struct sqnano_ram_media r;
	struct sqnano_txn w, r1, r2;
	struct sqnano_stats st;
	uint32_t seed = 1234;

	sqnano_ram_media_init(&r, mem, mc->size, mc->kind, mc->erase);
	sqnano_ram_media_blank(&r);
	open_db(&r, mc);
	create_tree();
	memset(&a, 0, sizeof(a));

	CHECK_RC(sqnano_txn_begin(&db, &w, SQNANO_TXN_WRITE, -1), SQNANO_OK);
	for (uint32_t k = 0; k < 200; k++) {
		a.present[k] = 1;
		a.val[k] = test_rand(&seed);
		CHECK_RC(kv_put(&w, k, a.val[k], mc->maxlen), SQNANO_OK);
	}
	CHECK_RC(sqnano_txn_commit(&w), SQNANO_OK);

	/* r1 sees state A for its whole life. */
	CHECK_RC(sqnano_txn_begin(&db, &r1, SQNANO_TXN_READ, 0), SQNANO_OK);
	b = a;
	CHECK_RC(sqnano_txn_begin(&db, &w, SQNANO_TXN_WRITE, -1), SQNANO_OK);
	for (uint32_t k = 0; k < 200; k += 2) {
		b.val[k] = test_rand(&seed);
		CHECK_RC(kv_put(&w, k, b.val[k], mc->maxlen), SQNANO_OK);
	}
	/* While the writer runs, r1 still reads A. */
	CHECK_RC(kv_verify(&r1, &a, mc->maxlen), SQNANO_OK);
	CHECK_RC(sqnano_txn_commit(&w), SQNANO_OK);
	CHECK_RC(kv_verify(&r1, &a, mc->maxlen), SQNANO_OK);

	/* A second commit while r1 is open (journal mode: copy-on-write fallback). */
	c = b;
	CHECK_RC(sqnano_txn_begin(&db, &w, SQNANO_TXN_WRITE, -1), SQNANO_OK);
	for (uint32_t k = 1; k < 200; k += 3) {
		bool found;

		c.present[k] = 0;
		CHECK_RC(kv_del(&w, k, &found), SQNANO_OK);
	}
	CHECK_RC(sqnano_txn_commit(&w), SQNANO_OK);
	CHECK_RC(kv_verify(&r1, &a, mc->maxlen), SQNANO_OK);
	CHECK_RC(sqnano_txn_begin(&db, &r2, SQNANO_TXN_READ, 0), SQNANO_OK);
	CHECK_RC(kv_verify(&r2, &c, mc->maxlen), SQNANO_OK);
	CHECK_RC(sqnano_db_stats(&db, &st), SQNANO_OK);
	CHECK(st.pages_limbo > 0);
	CHECK_RC(sqnano_txn_commit(&r1), SQNANO_OK);
	CHECK_RC(sqnano_txn_commit(&r2), SQNANO_OK);
	CHECK_RC(sqnano_db_stats(&db, &st), SQNANO_OK);
	CHECK(st.pages_limbo == 0);
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);

	/* A write transaction rolled back leaves no trace. */
	CHECK_RC(sqnano_txn_begin(&db, &w, SQNANO_TXN_WRITE, -1), SQNANO_OK);
	for (uint32_t k = 0; k < KV_MAX; k++) {
		CHECK_RC(kv_put(&w, k, k, mc->maxlen), SQNANO_OK);
	}
	CHECK_RC(sqnano_txn_rollback(&w), SQNANO_OK);
	verify_committed(&c, mc->maxlen);
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	open_db(&r, mc);
	verify_committed(&c, mc->maxlen);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	printf("snapshot %-10s ok\n", mc->name);
}

static void busy_case(void)
{
	const struct media_case *mc = &cases[3];
	struct sqnano_ram_media r;
	struct sqnano_txn w1, w2;

	sqnano_ram_media_init(&r, mem, mc->size, mc->kind, mc->erase);
	sqnano_ram_media_blank(&r);
	open_db(&r, mc);
	CHECK_RC(sqnano_txn_begin(&db, &w1, SQNANO_TXN_WRITE, -1), SQNANO_OK);
	CHECK_RC(sqnano_txn_begin(&db, &w2, SQNANO_TXN_WRITE, 10), SQNANO_BUSY);
	CHECK_RC(sqnano_txn_commit(&w1), SQNANO_OK);
	CHECK_RC(sqnano_txn_begin(&db, &w2, SQNANO_TXN_WRITE, 10), SQNANO_OK);
	CHECK_RC(sqnano_txn_commit(&w2), SQNANO_OK);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	printf("busy ok\n");
}

static void full_case(void)
{
	struct sqnano_ram_media r;
	struct sqnano_db_config cfg = {.page_size = 256, .create = true};
	struct sqnano_txn t;
	uint32_t k;
	int rc = SQNANO_OK;

	/* A tiny FRAM: filling it must report SQNANO_FULL, never overflow. */
	sqnano_ram_media_init(&r, mem, 16 * 1024, SQNANO_RAM_FRAM, 0);
	sqnano_ram_media_blank(&r);
	CHECK_RC(sqnano_db_open(&db, &r.media, &cfg, NULL), SQNANO_OK);
	create_tree();
	CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
	for (k = 0; k < 100000 && rc == SQNANO_OK; k++) {
		rc = kv_put(&t, k, 50, 60);
	}
	CHECK_RC(rc, SQNANO_FULL);
	CHECK_RC(sqnano_txn_rollback(&t), SQNANO_OK);
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	printf("full ok (%u keys before SQNANO_FULL)\n", k);
}

/* Erase counts per sector, through a wrapper around the RAM driver. */
static uint32_t wear_erases[64];
static uint32_t wear_esize;
static int (*wear_orig)(const struct sqnano_media *m, uint32_t off, size_t len);

static int wear_erase(const struct sqnano_media *m, uint32_t off, size_t len)
{
	for (uint32_t o = off; o < off + len; o += wear_esize) {
		wear_erases[o / wear_esize]++;
	}
	return wear_orig(m, off, len);
}

/* A rolling log, one row per commit: data sectors must wear evenly. */
static void wear_case(uint16_t page)
{
	static struct sqnano_media_api api;
	struct sqnano_ram_media r;
	struct sqnano_db_config cfg = {.page_size = page, .create = true};
	uint32_t n = 256 * 1024 / 4096, total = 0, max = 0;

	sqnano_ram_media_init(&r, mem, 256 * 1024, SQNANO_RAM_NOR, 4096);
	sqnano_ram_media_blank(&r);
	api = *r.media.api;
	wear_orig = api.erase;
	api.erase = wear_erase;
	r.media.api = &api;
	wear_esize = 4096;
	CHECK_RC(sqnano_db_open(&db, &r.media, &cfg, NULL), SQNANO_OK);
	create_tree();
	for (uint32_t k = 0; k < 3500; k++) {
		struct sqnano_txn t;
		bool found;

		if (k == 500) {
			memset(wear_erases, 0, sizeof(wear_erases));
		}
		CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
		if (k >= 500) {
			CHECK_RC(kv_del(&t, k - 500, &found), SQNANO_OK);
		}
		CHECK_RC(kv_put(&t, k, k, 32), SQNANO_OK);
		CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
	}
	/* The first sectors hold the superblock log. */
	for (uint32_t s = db.sb_units; s < n; s++) {
		total += wear_erases[s];
		max = wear_erases[s] > max ? wear_erases[s] : max;
	}
	n -= db.sb_units;
	CHECK(max * n <= total * 3 / 2 + 4 * n);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	printf("wear %u/4096 ok (sector erases: max %u, mean %u)\n", page, max, total / n);
}

/* Deleting most keys must give the pages back: underfull leaves merge. */
static void merge_case(void)
{
	static struct kv_model m;
	struct sqnano_ram_media r;
	struct sqnano_db_config cfg = {.page_size = 256, .create = true};
	struct sqnano_stats st;
	struct sqnano_txn t;
	uint32_t full;

	sqnano_ram_media_init(&r, mem, 512 * 1024, SQNANO_RAM_FRAM, 0);
	sqnano_ram_media_blank(&r);
	CHECK_RC(sqnano_db_open(&db, &r.media, &cfg, NULL), SQNANO_OK);
	create_tree();
	memset(&m, 0, sizeof(m));
	CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
	for (uint32_t k = 0; k < KV_MAX; k++) {
		m.present[k] = 1;
		m.val[k] = k * 7;
		CHECK_RC(kv_put(&t, k, m.val[k], 30), SQNANO_OK);
	}
	CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
	CHECK_RC(sqnano_db_stats(&db, &st), SQNANO_OK);
	full = st.pages_used;
	/* One key in ten survives, deleted in a scattered order. */
	for (uint32_t i = 0; i < KV_MAX; i++) {
		uint32_t k = (i * 263) % KV_MAX;
		bool found;

		if (k % 10 == 0) {
			continue;
		}
		CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
		CHECK_RC(kv_del(&t, k, &found), SQNANO_OK);
		CHECK(found);
		m.present[k] = 0;
		CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
	}
	verify_committed(&m, 30);
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
	CHECK_RC(sqnano_db_stats(&db, &st), SQNANO_OK);
	CHECK(st.pages_used * 4 < full);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	printf("merge ok (%u pages full, %u after deleting 90%%)\n", full, st.pages_used);
}

int main(void)
{
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		random_case(&cases[i]);
		snapshot_case(&cases[i]);
	}
	busy_case();
	full_case();
	merge_case();
	wear_case(4096);
	wear_case(1024);
	return 0;
}
