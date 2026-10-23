/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A valid database image with byte edits taken from the input, mounted,
 * read entirely, checked, then written to. Built with SQNANO_FUZZING, which
 * skips the page CRC check so that corrupted pages reach the B+tree code:
 * whatever the content, the engine must report an error, never crash,
 * read out of bounds or loop forever.
 */

#include "kv.h"

#define MEDIA (128 * 1024)

static uint8_t mem[MEDIA], image[MEDIA];
static struct sqnano_ram_media ram;
static struct sqnano_db db;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	struct sqnano_db_config cfg = {.page_size = 512, .create = true};
	struct sqnano_txn t;
	uint32_t seed = 9;

	(void)argc;
	(void)argv;
	sqnano_ram_media_init(&ram, mem, MEDIA, SQNANO_RAM_NOR, 4096);
	sqnano_ram_media_blank(&ram);
	CHECK_RC(sqnano_db_open(&db, &ram.media, &cfg, NULL), SQNANO_OK);
	for (int round = 0; round < 4; round++) {
		CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
		if (round == 0) {
			db_lock(&db);
			CHECK_RC(bt_tree_create(&t, KV_TREE), SQNANO_OK);
			db_unlock(&db);
		}
		for (int i = 0; i < 60; i++) {
			uint32_t k = test_rand(&seed) % 300;

			if (round && i % 3 == 0) {
				bool found;

				CHECK_RC(kv_del(&t, k, &found), SQNANO_OK);
			} else {
				/* Some values overflow a 512-byte page. */
				CHECK_RC(kv_put(&t, k, test_rand(&seed), i % 7 ? 60 : 480), SQNANO_OK);
			}
		}
		CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
	}
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	memcpy(image, mem, MEDIA);
	return 0;
}

static void read_all(void)
{
	static uint8_t val[2048];
	struct sqnano_txn r;
	struct sqnano_cursor c;
	uint16_t vl;
	int rc;

	if (sqnano_txn_begin(&db, &r, SQNANO_TXN_READ, 0)) {
		return;
	}
	db_lock(&db);
	for (uint16_t tree = 0; tree <= KV_TREE; tree += KV_TREE) {
		rc = bt_open(&r, &c, tree);
		if (rc == SQNANO_OK) {
			rc = bt_seek(&c, NULL, 0, SQNANO_SEEK_FIRST, NULL);
		}
		while (rc == SQNANO_OK && c.state == CUR_VALID) {
			(void)bt_value(&c, val, sizeof(val), &vl);
			rc = bt_next(&c);
		}
	}
	db_unlock(&db);
	(void)sqnano_txn_commit(&r);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct sqnano_db_config cfg = {.page_size = 512, .create = false};
	struct sqnano_txn t;

	memcpy(mem, image, MEDIA);
	/* Each 4 bytes: an offset (24 bits) and a new byte. */
	for (size_t i = 0; i + 4 <= size; i += 4) {
		uint32_t off = (uint32_t)(data[i] | data[i + 1] << 8 | data[i + 2] << 16);

		mem[off % MEDIA] = data[i + 3];
	}
	sqnano_ram_media_init(&ram, mem, MEDIA, SQNANO_RAM_NOR, 4096);
	if (sqnano_db_open(&db, &ram.media, &cfg, NULL) != SQNANO_OK) {
		return 0;
	}
	(void)sqnano_db_check(&db);
	read_all();
	if (sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1) == SQNANO_OK) {
		int rc = SQNANO_OK;

		for (uint32_t k = 0; k < 40 && rc == SQNANO_OK; k++) {
			bool found;

			rc = k % 2 ? kv_put(&t, k * 7 % 300, k, 200) : kv_del(&t, k * 11 % 300, &found);
		}
		rc = rc ? (sqnano_txn_rollback(&t), rc) : sqnano_txn_commit(&t);
		if (rc == SQNANO_OK) {
			(void)sqnano_db_check(&db);
			read_all();
		}
	}
	(void)sqnano_db_close(&db);
	return 0;
}
