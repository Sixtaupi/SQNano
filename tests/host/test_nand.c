/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * NAND media on a simulated chip:
 *  - a power cut during every program, erase and bad block marking, and a
 *    second one during the recovery, with factory bad blocks;
 *  - blocks wearing out (program and erase failures) during a workload;
 *  - stored bit flips, corrected by the software ECC or by the chip, and
 *    the refresh of the blocks they hit; uncorrectable pages are reported.
 * The simulator checks that each page is programmed once per erase and the
 * pages of a block in order.
 */

#include "kv.h"

#define TXNS 10

struct ncase {
	const char *name;
	uint32_t page;
	uint32_t ppb;
	uint32_t blocks;
	bool soft_ecc;
	uint16_t maxlen;
	uint8_t bad[3]; /* factory bad blocks (0: none) */
};

static const struct ncase cases[] = {
	{"512x16 soft ECC", 512, 16, 40, true, 100, {3, 17, 0}},
	{"2048x32 chip ECC", 2048, 32, 16, false, 300, {6, 0, 0}},
};

static uint8_t mem[2048 * 32 * 40], image[2048 * 32 * 40];
static uint8_t meta[32 * 40], meta_image[32 * 40];
static struct sqnano_ram_nand chip, chip_image;
static struct sqnano_nand_media nm;
static struct sqnano_db db;
static struct kv_model states[TXNS + 1];

static size_t mem_size(const struct ncase *nc)
{
	return (size_t)nc->page * nc->ppb * nc->blocks;
}

static int open_db(const struct ncase *nc)
{
	struct sqnano_db_config cfg = {.create = true};
	struct sqnano_nand_geometry geo = {
		.page_size = nc->page,
		.pages_per_block = nc->ppb,
		.blocks = nc->blocks,
		.soft_ecc = nc->soft_ecc,
		.reserved_blocks = 4,
	};

	sqnano_nand_media_init(&nm, &chip.ops, &chip, &geo);
	return sqnano_db_open(&db, &nm.media, &cfg, NULL);
}

static void save(const struct ncase *nc)
{
	memcpy(image, mem, mem_size(nc));
	memcpy(meta_image, meta, nc->ppb * nc->blocks);
	chip_image = chip;
}

static void restore(const struct ncase *nc)
{
	memcpy(mem, image, mem_size(nc));
	memcpy(meta, meta_image, nc->ppb * nc->blocks);
	chip = chip_image;
}

static int workload(uint32_t seed, int txns, uint16_t maxlen, struct kv_model *m, bool record)
{
	int committed = 0;

	for (int i = 0; i < txns; i++) {
		struct kv_model next = *m;
		struct sqnano_txn t;
		int rc = sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1);

		for (int k = 0; k < 15 && rc == SQNANO_OK; k++) {
			uint32_t key = test_rand(&seed) % 300;

			if (test_rand(&seed) % 4 == 0) {
				bool found;

				rc = kv_del(&t, key, &found);
				next.present[key] = 0;
			} else {
				uint32_t v = test_rand(&seed);

				rc = kv_put(&t, key, v, maxlen);
				next.present[key] = 1;
				next.val[key] = v;
			}
		}
		rc = rc == SQNANO_OK ? sqnano_txn_commit(&t) : (sqnano_txn_rollback(&t), rc);
		if (rc) {
			if (record) {
				fprintf(stderr, "transaction %d failed (%d)\n", i, rc);
			}
			return committed;
		}
		*m = next;
		committed++;
		if (record && committed <= TXNS) {
			states[committed] = next;
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

static void setup(const struct ncase *nc, struct kv_model *m)
{
	struct sqnano_txn t;
	uint32_t seed = 77;

	sqnano_ram_nand_init(&chip, mem, meta, nc->page, nc->ppb, nc->blocks, !nc->soft_ecc);
	for (int i = 0; i < 3 && nc->bad[i]; i++) {
		chip.bad[nc->bad[i]] = 1;
	}
	CHECK_RC(open_db(nc), SQNANO_OK);
	CHECK(nm.factory_bad == (uint32_t)(nc->bad[0] != 0) + (nc->bad[1] != 0) + (nc->bad[2] != 0));
	memset(m, 0, sizeof(*m));
	CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
	db_lock(&db);
	CHECK_RC(bt_tree_create(&t, KV_TREE), SQNANO_OK);
	db_unlock(&db);
	for (uint32_t k = 0; k < 300; k += 2) {
		m->present[k] = 1;
		m->val[k] = test_rand(&seed);
		CHECK_RC(kv_put(&t, k, m->val[k], nc->maxlen), SQNANO_OK);
	}
	CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
}

static uint32_t programmed_page(const struct ncase *nc, uint32_t *seed);

/*
 * With @p moves, bit flips make blocks be refreshed and a block wears out
 * during the workload: the cuts then also hit the copies.
 */
static void powercut(const struct ncase *nc, bool moves)
{
	uint32_t total, after = 0, recut = 0;
	struct kv_model m;

	setup(nc, &states[0]);
	if (moves) {
		uint32_t seed = 31;

		static uint8_t hit[32 * 40];

		/* One flip per page: within what both ECCs correct. */
		memset(hit, 0, sizeof(hit));
		for (int i = 0; i < 30; i++) {
			uint32_t p = programmed_page(nc, &seed);

			if (!hit[p]) {
				hit[p] = 1;
				sqnano_ram_nand_flip(&chip, p, test_rand(&seed) % 2048);
			}
		}
		chip.fail_program_after = 20;
	}
	save(nc);
	CHECK_RC(open_db(nc), SQNANO_OK);
	if (!moves) {
		printf("nand %-18s logical page %u bytes, %u units of %u pages, %u KB\n", nc->name,
		       nm.slot, nm.units, nm.spb, nm.media.size / 1024);
	}
	chip.ops_count = 0;
	m = states[0];
	CHECK(workload(0xabcd, TXNS, nc->maxlen, &m, true) == TXNS);
	total = chip.ops_count;
	CHECK(matches(&states[TXNS], nc->maxlen));
	CHECK(!moves || (nm.refreshes > 0 && nm.bad_blocks > 0));
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	CHECK(chip.violations == 0);

	for (uint32_t cut = 0; cut < total; cut++) {
		int done, rc;

		restore(nc);
		CHECK_RC(open_db(nc), SQNANO_OK);
		sqnano_ram_nand_cut_after(&chip, cut, cut * 2654435761u + 1);
		m = states[0];
		done = workload(0xabcd, TXNS, nc->maxlen, &m, false);
		CHECK(chip.powered_off);
		sqnano_ram_nand_power_on(&chip);
		if (cut & 1) {
			/* A second cut while the mount repairs (moves, erases, marks). */
			sqnano_ram_nand_cut_after(&chip, cut % 3, cut + 3);
			if (open_db(nc) != SQNANO_OK) {
				recut++;
			}
			sqnano_ram_nand_power_on(&chip);
		}
		rc = open_db(nc);
		if (rc != SQNANO_OK) {
			fprintf(stderr, "%s: cut %u: remount failed (%d)\n", nc->name, cut, rc);
			exit(1);
		}
		CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
		if (!matches(&states[done], nc->maxlen)) {
			if (done >= TXNS || !matches(&states[done + 1], nc->maxlen)) {
				fprintf(stderr, "%s: cut %u after %d commits: unexpected content\n",
					nc->name, cut, done);
				exit(1);
			}
			after++;
		}
		m = states[0];
		CHECK(workload(0x1234, TXNS, nc->maxlen, &m, false) == TXNS);
		CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
		CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
		if (chip.violations) {
			fprintf(stderr, "%s: cut %u: %u NAND program rule violations\n", nc->name, cut,
				chip.violations);
			exit(1);
		}
	}
	printf("nand %-18s %u cut points%s, %u ended on the new state, %u recovery cuts: ok\n",
	       nc->name, total, moves ? " (refreshes, a block wearing out)" : "", after, recut);
}

/* Blocks wear out during a long workload: every transaction must still commit. */
static void wearout(const struct ncase *nc)
{
	struct kv_model m;
	uint32_t seed = 99;
	uint32_t grown, moved;

	setup(nc, &m);
	CHECK_RC(open_db(nc), SQNANO_OK);
	for (int round = 0; round < 42; round++) {
		/* About six blocks wear out: more than the reserved ones. */
		if (round % 14 == 0) {
			chip.fail_program_after = (int32_t)(test_rand(&seed) % 40);
		} else if (round % 14 == 7) {
			chip.fail_erase_after = (int32_t)(test_rand(&seed) % 6);
		}
		if (workload(round * 7919u + 1, 3, nc->maxlen, &m, true) != 3) {
			int st[4] = {0};

			for (uint32_t b = 0; b < nc->blocks; b++) {
				st[nm.bstate[b]]++;
			}
			fprintf(stderr, "round %d: bad %u reloc %u free %d dirty %d used %d bad %d\n",
				round, nm.bad_blocks, nm.relocations, st[0], st[1], st[2], st[3]);
			exit(1);
		}
		CHECK(matches(&m, nc->maxlen));
	}
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
	grown = nm.bad_blocks;
	moved = nm.relocations;
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	CHECK_RC(open_db(nc), SQNANO_OK);
	CHECK(matches(&m, nc->maxlen));
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
	CHECK(chip.violations == 0);
	CHECK(grown > 4 && nm.factory_bad >= grown);
	printf("nand %-18s wear-out: %u blocks went bad, %u units moved: ok\n", nc->name, grown,
	       moved);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
}

/* Programmed pages of the chip, in a random order. */
static uint32_t programmed_page(const struct ncase *nc, uint32_t *seed)
{
	for (;;) {
		uint32_t p = test_rand(seed) % (nc->ppb * nc->blocks);

		if (meta[p] & 0x80) {
			return p;
		}
	}
}

static void bitflips(const struct ncase *nc)
{
	struct kv_model m;
	uint32_t seed = 4242;
	uint32_t pages = 0, corrected, refreshed;
	static uint8_t hit[32 * 40];
	int rc;

	setup(nc, &m);
	CHECK_RC(open_db(nc), SQNANO_OK);
	CHECK(workload(5, 6, nc->maxlen, &m, false) == 6);
	/* One flip in many pages: all corrected, the blocks refreshed. */
	memset(hit, 0, sizeof(hit));
	for (uint32_t i = 0; i < 40; i++) {
		uint32_t p = programmed_page(nc, &seed);

		if (!hit[p]) {
			hit[p] = 1;
			sqnano_ram_nand_flip(&chip, p, test_rand(&seed) % (nm.slot * 8));
			pages++;
		}
	}
	CHECK(matches(&m, nc->maxlen));
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
	CHECK(nm.corrected > 0);
	CHECK(workload(6, 2, nc->maxlen, &m, false) == 2);
	CHECK(nm.refreshes > 0);
	corrected = nm.corrected;
	refreshed = nm.refreshes;
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	CHECK_RC(open_db(nc), SQNANO_OK);
	CHECK(matches(&m, nc->maxlen));
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
	CHECK(chip.violations == 0);
	printf("nand %-18s %u flipped pages: %u bits corrected, %u blocks refreshed: ok\n", nc->name,
	       pages, corrected, refreshed);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);

	/* Too many flips in one page of every block: reported, never returned. */
	for (uint32_t p = 0; p < nc->ppb * nc->blocks; p++) {
		if ((meta[p] & 0x80) && p % nc->ppb == 0) {
			for (int k = 0; k < 6; k++) {
				sqnano_ram_nand_flip(&chip, p, 8u * (uint32_t)k + 1u);
			}
		}
	}
	rc = open_db(nc);
	if (rc == SQNANO_OK) {
		rc = sqnano_db_check(&db);
		if (rc == SQNANO_OK && !matches(&m, nc->maxlen)) {
			rc = SQNANO_CORRUPT;
		}
	}
	CHECK(rc != SQNANO_OK);
	printf("nand %-18s uncorrectable pages detected (%d): ok\n", nc->name, rc);
}

int main(void)
{
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		powercut(&cases[i], false);
		powercut(&cases[i], true);
		wearout(&cases[i]);
		bitflips(&cases[i]);
	}
	return 0;
}
