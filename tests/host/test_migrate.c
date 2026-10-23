/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * OTA migration scenario. This file is compiled three times, with the code
 * that sqnanoc generated for schema versions 1, 2 and 3 (FW = 1, 2, 3).
 * Each run plays the role of one firmware image on a database image kept
 * in a file:
 *
 *   fw1 init        create a v1 database with 20 devices
 *   fw2 upgrade     migrate to v2 (pinned), add data
 *   fw1 revert      MCUboot reverted: back to the pinned v1 state
 *   fw2 cuts        migration with a power cut at every write
 *   fw2 confirm     migrate again and confirm the image
 *   fw1 refuse      v2 is not usable by firmware v1
 *   fw3 upgrade     expand-only migration to v3, set a v3 column, confirm
 *   fw2 compat      firmware v2 opens the v3 database and rewrites a row
 *   fw3 verify      the v3 column survived the v2 rewrite
 *   fw4 lazy        lazy migration to v4: no row rewritten, rows converted
 *                   when read, partly converted in the background, with a
 *                   power cut at every write of a conversion step
 *   fw3 revert      MCUboot reverted: v3 again, nothing left to convert
 *   fw4 lazy        the same lazy migration again
 *   fw4 confirm     the image is confirmed
 *   fw3 refuse      v4 is not usable by firmware v3
 *   fw5 upgrade     expand-only v5 while the v4 conversion is still pending
 *   fw5b refuse     fw5 built to migrate from v5 only: no v4 conversion code
 *   fw5 finish      the background conversion completes
 *   fw5b verify     now fw5b opens it, and the rows read the same
 */

#include <stdio.h>
#include <stdlib.h>

#include "sqnano_gen.h"
#include "testutil.h"

#define MEDIA_SIZE (64 * 1024)

static uint8_t mem[MEDIA_SIZE];
#if FW == 2 || FW >= 4
static uint8_t image[MEDIA_SIZE];
#endif
static struct sqnano_ram_media ram;
static struct sqnano_db db;
static const struct sqnano_db_config cfg = {.page_size = 1024, .create = true};

static void load(const char *path)
{
	FILE *f = fopen(path, "rb");

	CHECK(f != NULL);
	CHECK(fread(mem, 1, sizeof(mem), f) == sizeof(mem));
	fclose(f);
}

static void save(const char *path)
{
	FILE *f = fopen(path, "wb");

	CHECK(f != NULL);
	CHECK(fwrite(mem, 1, sizeof(mem), f) == sizeof(mem));
	fclose(f);
}

#if FW <= 2 || FW >= 4
static int64_t count(void)
{
	struct q_dev_count q;

	CHECK_RC(q_dev_count_open(&q, &db, NULL), SQNANO_OK);
	CHECK_RC(q_dev_count_step(&q), SQNANO_ROW);
	CHECK_RC(q_dev_count_step(&q), SQNANO_DONE);
	return q.count;
}

static void add(int64_t id, const char *name, int64_t fw)
{
	struct q_dev_add q;

	CHECK_RC(q_dev_add_open(&q, &db, NULL), SQNANO_OK);
	q_dev_add_bind(&q, id, name, fw);
	CHECK_RC(q_dev_add_run(&q), SQNANO_OK);
}
#endif

static struct sqnano_stats stats(void)
{
	struct sqnano_stats st;

	CHECK_RC(sqnano_db_stats(&db, &st), SQNANO_OK);
	return st;
}

#if FW == 2
static void check_v2_content(int64_t devices, int64_t want_migrated)
{
	struct q_status_count s;
	struct q_ev_count e;
	struct q_dev_by_name n;
	int64_t migrated = 0, fresh = 0;
	int rc;

	CHECK_RC(q_status_count_open(&s, &db, NULL), SQNANO_OK);
	while ((rc = q_status_count_step(&s)) == SQNANO_ROW) {
		if (strcmp(s.status, "migrated") == 0) {
			migrated = s.count;
		} else if (strcmp(s.status, "new") == 0) {
			fresh = s.count;
		}
	}
	CHECK_RC(rc, SQNANO_DONE);
	/* Devices with fw 1 and 2 were marked by v2_up.sql. */
	CHECK(migrated == want_migrated);
	CHECK(fresh == devices - want_migrated);
	CHECK_RC(q_ev_count_open(&e, &db, NULL), SQNANO_OK);
	CHECK_RC(q_ev_count_step(&e), SQNANO_ROW);
	CHECK(e.count == 1);
	(void)q_ev_count_reset(&e);
	CHECK_RC(q_dev_by_name_open(&n, &db, NULL), SQNANO_OK);
	q_dev_by_name_bind(&n, "dev7");
	CHECK_RC(q_dev_by_name_step(&n), SQNANO_ROW);
	CHECK(n.id == 7);
	CHECK_RC(q_dev_by_name_step(&n), SQNANO_DONE);
}
#endif

#if FW >= 4
static int fw3_set = 0; /* device 3 rewritten by fw4 (its fw changed, not its label) */

static void set_fw(int64_t id, int64_t fw)
{
	struct q_dev_set_fw u;

	CHECK_RC(q_dev_set_fw_open(&u, &db, NULL), SQNANO_OK);
	q_dev_set_fw_bind(&u, fw, id);
	CHECK_RC(q_dev_set_fw_run(&u), SQNANO_OK);
	fw3_set = 1;
}

/* What an eager v4 migration would have stored, for every device. */
static void check_lazy(void)
{
	CHECK(count() == 21);
	for (int i = 1; i <= 21; i++) {
		struct q_dev_lazy q;
		int64_t fw0 = i == 21 ? 1 : i % 4 + 1;
		int64_t fw4 = i == 1 ? 42 : fw0;
		char label[24], status[24], note[24];

		snprintf(label, sizeof(label), "DEV%d-%lld", i, (long long)fw4);
		if (fw0 < 3) {
			snprintf(status, sizeof(status), "migrated");
		} else {
			snprintf(status, sizeof(status), "fw%lld", (long long)fw4);
		}
		snprintf(note, sizeof(note), "%s%s", i == 1 ? "keep me" : "", fw4 > 3 ? "!" : "");
		CHECK_RC(q_dev_lazy_open(&q, &db, NULL), SQNANO_OK);
		q_dev_lazy_bind(&q, i);
		CHECK_RC(q_dev_lazy_step(&q), SQNANO_ROW);
		if (strcmp(q.label, label) || strcmp(q.status, status) ||
		    (note[0] ? !q.note || strcmp(q.note, note) : q.note != NULL)) {
			fprintf(stderr, "device %d: %s %s %s, expected %s %s %s\n", i, q.label, q.status,
				q.note ? q.note : "NULL", label, status, note[0] ? note : "NULL");
			exit(1);
		}
		(void)q_dev_lazy_reset(&q);
	}
}
#endif

static int open_db(void)
{
	return sqnano_db_open(&db, &ram.media, &cfg, &sqnano_app_schema);
}

int main(int argc, char **argv)
{
	const char *action;
	const char *path;

	CHECK(argc == 3);
	action = argv[1];
	path = argv[2];
	sqnano_ram_media_init(&ram, mem, sizeof(mem), SQNANO_RAM_NOR, 1024);
	if (strcmp(action, "init") == 0) {
		sqnano_ram_media_blank(&ram);
	} else {
		load(path);
	}

#if FW == 1
	if (strcmp(action, "init") == 0) {
		char name[16];

		CHECK_RC(open_db(), SQNANO_OK);
		for (int i = 1; i <= 20; i++) {
			snprintf(name, sizeof(name), "dev%d", i);
			add(i, name, i % 4 + 1);
		}
		CHECK(count() == 20);
		CHECK(stats().schema_version == 1);
	} else if (strcmp(action, "revert") == 0) {
		struct q_dev_get g;

		CHECK_RC(open_db(), SQNANO_OK);
		/* Writes made by firmware v2 after the migration are gone (documented). */
		CHECK(count() == 20);
		CHECK(stats().schema_version == 1);
		CHECK(!stats().pinned);
		CHECK_RC(q_dev_get_open(&g, &db, NULL), SQNANO_OK);
		q_dev_get_bind(&g, 5);
		CHECK_RC(q_dev_get_step(&g), SQNANO_ROW);
		CHECK(strcmp(g.name, "dev5") == 0 && g.fw == 2);
		(void)q_dev_get_reset(&g);
		add(21, "dev21", 1);
	} else if (strcmp(action, "refuse") == 0) {
		CHECK_RC(open_db(), SQNANO_CANTOPEN_SCHEMA);
		printf("fw1 %s ok\n", action);
		return 0;
	}
#elif FW == 2
	if (strcmp(action, "upgrade") == 0) {
		CHECK_RC(open_db(), SQNANO_OK);
		CHECK(stats().schema_version == 2);
		CHECK(stats().pinned);
		check_v2_content(20, 10);
		add(100, "dev100", 9);
		CHECK(count() == 21);
	} else if (strcmp(action, "cuts") == 0) {
		uint32_t total, cut;

		memcpy(image, mem, sizeof(mem));
		ram.ops = 0;
		CHECK_RC(open_db(), SQNANO_OK);
		total = ram.ops;
		CHECK(total > 0);
		for (cut = 0; cut < total; cut++) {
			int rc;

			memcpy(mem, image, sizeof(mem));
			sqnano_ram_media_cut_after(&ram, cut, cut + 1);
			rc = open_db();
			CHECK(rc != SQNANO_OK);
			sqnano_ram_media_power_on(&ram);
			/* Whatever the cut point, the next boot ends migrated. */
			CHECK_RC(open_db(), SQNANO_OK);
			CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
			CHECK(stats().schema_version == 2);
			check_v2_content(21, 11);
			CHECK(count() == 21);
		}
		printf("fw2 migration survived %u power cuts\n", total);
		memcpy(mem, image, sizeof(mem));
		return 0;
	} else if (strcmp(action, "confirm") == 0) {
		CHECK_RC(open_db(), SQNANO_OK);
		CHECK(stats().pinned);
		check_v2_content(21, 11);
		CHECK_RC(sqnano_migration_confirm(&db), SQNANO_OK);
		CHECK(!stats().pinned);
		CHECK(stats().pages_pinned == 0);
	} else if (strcmp(action, "compat") == 0) {
		struct q_dev_set_fw u;

		CHECK_RC(open_db(), SQNANO_OK);
		/* Database v3, firmware v2: compatible thanks to expand/contract. */
		CHECK(stats().schema_version == 3);
		CHECK_RC(q_dev_set_fw_open(&u, &db, NULL), SQNANO_OK);
		q_dev_set_fw_bind(&u, 42, 1);
		CHECK_RC(q_dev_set_fw_run(&u), SQNANO_OK);
	}
#elif FW == 3
	if (strcmp(action, "refuse") == 0) {
		CHECK_RC(open_db(), SQNANO_CANTOPEN_SCHEMA);
		printf("fw3 %s ok\n", action);
		return 0;
	}
	if (strcmp(action, "revert") == 0) {
		struct q_dev_get_note n;

		CHECK_RC(open_db(), SQNANO_OK);
		CHECK(stats().schema_version == 3);
		CHECK(stats().lazy_pending == 0);
		CHECK_RC(q_dev_get_note_open(&n, &db, NULL), SQNANO_OK);
		q_dev_get_note_bind(&n, 1);
		CHECK_RC(q_dev_get_note_step(&n), SQNANO_ROW);
		CHECK(n.note && strcmp(n.note, "keep me") == 0);
		(void)q_dev_get_note_reset(&n);
	}
	if (strcmp(action, "upgrade") == 0) {
		struct q_dev_note n;

		CHECK_RC(open_db(), SQNANO_OK);
		CHECK(stats().schema_version == 3);
		CHECK(stats().compat_version == 2);
		CHECK_RC(q_dev_note_open(&n, &db, NULL), SQNANO_OK);
		q_dev_note_bind(&n, "keep me", 1);
		CHECK_RC(q_dev_note_run(&n), SQNANO_OK);
		CHECK_RC(sqnano_migration_confirm(&db), SQNANO_OK);
	} else if (strcmp(action, "verify") == 0) {
		struct q_dev_get_note n;
		struct q_dev_get g;

		CHECK_RC(open_db(), SQNANO_OK);
		CHECK_RC(q_dev_get_note_open(&n, &db, NULL), SQNANO_OK);
		q_dev_get_note_bind(&n, 1);
		CHECK_RC(q_dev_get_note_step(&n), SQNANO_ROW);
		CHECK(n.note && strcmp(n.note, "keep me") == 0);
		(void)q_dev_get_note_reset(&n);
		CHECK_RC(q_dev_get_open(&g, &db, NULL), SQNANO_OK);
		q_dev_get_bind(&g, 1);
		CHECK_RC(q_dev_get_step(&g), SQNANO_ROW);
		CHECK(g.fw == 42);
		(void)q_dev_get_reset(&g);
	}
#else
	if (strcmp(action, "lazy") == 0) {
		uint32_t ops, total;
		int rc;

		ram.ops = 0;
		CHECK_RC(open_db(), SQNANO_OK);
		ops = ram.ops;
		CHECK(stats().schema_version == 4);
		CHECK(stats().lazy_pending == 4);
		/* A superblock and a few pages: the 21 rows were not rewritten. */
		CHECK(ops < 12);
		check_lazy();
		/* A row written now is stored converted: it is not converted again. */
		set_fw(3, 9);
		check_lazy();
		CHECK_RC(sqnano_migration_step(&db, 5), SQNANO_OK);
		check_lazy();
		CHECK(stats().lazy_pending == 4);
		/* Power cuts during a conversion step: rows read the same afterwards. */
		CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
		memcpy(image, mem, sizeof(mem));
		CHECK_RC(open_db(), SQNANO_OK);
		ram.ops = 0;
		CHECK_RC(sqnano_migration_step(&db, 6), SQNANO_OK);
		total = ram.ops;
		for (uint32_t cut = 0; cut < total; cut++) {
			memcpy(mem, image, sizeof(mem));
			CHECK_RC(open_db(), SQNANO_OK);
			sqnano_ram_media_cut_after(&ram, cut, cut + 7);
			rc = sqnano_migration_step(&db, 6);
			CHECK(rc != SQNANO_OK && rc != SQNANO_DONE);
			sqnano_ram_media_power_on(&ram);
			CHECK_RC(open_db(), SQNANO_OK);
			CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
			check_lazy();
			CHECK(sqnano_migration_step(&db, 6) == SQNANO_OK);
			check_lazy();
		}
		printf("fw4 lazy conversion survived %u power cuts\n", total);
		memcpy(mem, image, sizeof(mem));
		CHECK_RC(open_db(), SQNANO_OK);
	} else if (strcmp(action, "confirm") == 0) {
		CHECK_RC(open_db(), SQNANO_OK);
		CHECK(stats().pinned);
		check_lazy();
		CHECK_RC(sqnano_migration_confirm(&db), SQNANO_OK);
	} else if (strcmp(action, "upgrade") == 0) {
		CHECK_RC(open_db(), SQNANO_OK);
		CHECK(stats().schema_version == 5);
		CHECK(stats().lazy_pending == 4);
		check_lazy();
		CHECK_RC(sqnano_migration_confirm(&db), SQNANO_OK);
	} else if (strcmp(action, "refuse") == 0) {
		/* Rows may still need the v4 conversion, which this build lacks. */
		CHECK_RC(open_db(), SQNANO_CANTOPEN_SCHEMA);
		printf("fw5b %s ok\n", action);
		return 0;
	} else if (strcmp(action, "finish") == 0) {
		int steps = 0, rc;

		CHECK_RC(open_db(), SQNANO_OK);
		while ((rc = sqnano_migration_step(&db, 3)) == SQNANO_OK) {
			steps++;
			check_lazy();
		}
		CHECK_RC(rc, SQNANO_DONE);
		CHECK(steps > 1);
		CHECK(stats().lazy_pending == 0);
		check_lazy();
		printf("fw5 conversion finished in %d steps\n", steps + 1);
	} else if (strcmp(action, "verify") == 0) {
		CHECK_RC(open_db(), SQNANO_OK);
		CHECK(stats().lazy_pending == 0);
		check_lazy();
	}
#endif
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	save(path);
	printf("fw%d %s ok\n", FW, action);
	return 0;
}
