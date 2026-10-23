/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Databases declared in the devicetree (compatible "zephyr,sqnano").
 *
 * The storage phandle selects the driver: a flash partition uses the
 * flash_area API (shadow paging), through the NAND layer with the nand
 * property; any other node is an EEPROM API device (FRAM, MRAM, EEPROM:
 * undo journal).
 */

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/toolchain.h>

#include <sqnano/exec.h>
#include <sqnano/zephyr.h>

#ifdef CONFIG_SQNANO_SQLITE_COMPAT
#include <sqlite3.h>
#endif

#define DT_DRV_COMPAT zephyr_sqnano

/* Provided by the code that sqnanoc generates for the application. */
extern const struct sqnano_schema sqnano_app_schema __weak;

#define STORAGE(inst)      DT_INST_PHANDLE(inst, storage)
#define IS_FS(inst)        DT_INST_NODE_HAS_PROP(inst, fs_dir)
#define IS_HYBRID(inst)    DT_INST_NODE_HAS_PROP(inst, journal)
#define IS_PARTITION(inst) DT_PARTITION_EXISTS(STORAGE(inst))
#define IS_NAND(inst)      DT_INST_PROP(inst, nand)

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define MEDIA_DEFINE(inst)                                                                         \
	COND_CODE_1(IS_FS(inst), (FS_MEDIA_DEFINE(inst)), (DEV_MEDIA_DEFINE(inst)))               \
	static struct sqnano_db db_##inst;

#define FS_MEDIA_DEFINE(inst)                                                                      \
	BUILD_ASSERT(IS_ENABLED(CONFIG_SQNANO_MEDIA_FS),                                           \
		     "enable CONFIG_SQNANO_MEDIA_FS for fs-dir storage");                         \
	BUILD_ASSERT(DT_INST_NODE_HAS_PROP(inst, fs_size), "fs-dir needs fs-size");                 \
	static struct sqnano_fs_media media_##inst;                                                 \
	static uint8_t cache_##inst[DT_INST_PROP(inst, fs_chunk)] __aligned(4);

#define DEV_MEDIA_DEFINE(inst)                                                                     \
	COND_CODE_1(IS_HYBRID(inst), (HYBRID_MEDIA_DEFINE(inst)),                                  \
		    (COND_CODE_1(IS_NAND(inst), (NAND_MEDIA_DEFINE(inst)), (ONE_MEDIA_DEFINE(inst)))))

#define NAND_MEDIA_DEFINE(inst)                                                                    \
	BUILD_ASSERT(IS_ENABLED(CONFIG_SQNANO_MEDIA_NAND),                                         \
		     "enable CONFIG_SQNANO_MEDIA_NAND for nand storage");                         \
	BUILD_ASSERT(IS_PARTITION(inst), "nand storage must be a flash partition");                \
	static struct sqnano_nand_zmedia media_##inst;

#define HYBRID_MEDIA_DEFINE(inst)                                                                  \
	BUILD_ASSERT(IS_ENABLED(CONFIG_SQNANO_MEDIA_HYBRID),                                       \
		     "enable CONFIG_SQNANO_MEDIA_HYBRID for a journal device");                   \
	BUILD_ASSERT(IS_PARTITION(inst), "hybrid mode needs a flash partition as storage");        \
	static struct sqnano_hybrid_zmedia media_##inst;

#define ONE_MEDIA_DEFINE(inst)                                                                     \
	COND_CODE_1(IS_PARTITION(inst),                                                            \
		    (BUILD_ASSERT(IS_ENABLED(CONFIG_SQNANO_MEDIA_FLASH),                          \
				  "enable CONFIG_SQNANO_MEDIA_FLASH for partition storage");      \
		     static struct sqnano_flash_media media_##inst;),                              \
		    (BUILD_ASSERT(IS_ENABLED(CONFIG_SQNANO_MEDIA_EEPROM),                         \
				  "enable CONFIG_SQNANO_MEDIA_EEPROM for EEPROM/FRAM storage");   \
		     static struct sqnano_eeprom_media media_##inst;))

DT_INST_FOREACH_STATUS_OKAY(MEDIA_DEFINE)

#define ENTRY(inst)                                                                                \
	{                                                                                          \
		.path = DT_INST_PROP(inst, path),                                                  \
		.db = &db_##inst,                                                                  \
		.media = COND_CODE_1(IS_FS(inst), (&media_##inst.fm.media),                        \
				     (COND_CODE_1(IS_HYBRID(inst), (&media_##inst.hy.media),        \
						  (COND_CODE_1(IS_NAND(inst), (&media_##inst.nm.media),   \
							       (&media_##inst.media)))))),          \
		.cfg =                                                                             \
			{                                                                          \
				.page_size = DT_INST_PROP_OR(inst, page_size, 0),                  \
				.journal_slots = DT_INST_PROP_OR(inst, journal_slots, 0),          \
				.mode = (enum sqnano_mode)DT_INST_ENUM_IDX_OR(inst, mode, 0),      \
				.sb_units = DT_INST_PROP_OR(inst, superblock_units, 0),            \
				.create = true,                                                    \
			},                                                                         \
	},

static struct sqnano_dt_db dbs[] = {DT_INST_FOREACH_STATUS_OKAY(ENTRY)};

#define MEDIA_INIT(inst)                                                                           \
	COND_CODE_1(IS_FS(inst),                                                                   \
		    (sqnano_fs_media_init(&media_##inst, DT_INST_PROP(inst, fs_dir),              \
					  DT_INST_PROP_OR(inst, fs_size, 0),                       \
					  DT_INST_PROP(inst, fs_chunk), cache_##inst);),           \
		    (DEV_MEDIA_INIT(inst)))

#define DEV_MEDIA_INIT(inst)                                                                       \
	COND_CODE_1(IS_HYBRID(inst), (HYBRID_MEDIA_INIT(inst)),                                    \
		    (COND_CODE_1(IS_NAND(inst), (NAND_MEDIA_INIT(inst)), (ONE_MEDIA_INIT(inst)))))

#define NAND_MEDIA_INIT(inst)                                                                      \
	(void)sqnano_nand_zmedia_init(&media_##inst, PARTITION_BY_NODE(STORAGE(inst)),             \
				      DT_INST_PROP(inst, nand_soft_ecc),                           \
				      DT_INST_PROP_OR(inst, nand_reserved_blocks, 0));

/* The block size is the page size: 0 picks the flash erase unit, as the pager does. */
#define HYBRID_MEDIA_INIT(inst)                                                                    \
	sqnano_flash_media_init(&media_##inst.flash, PARTITION_BY_NODE(STORAGE(inst)));            \
	sqnano_eeprom_media_init(&media_##inst.fram,                                               \
				 DEVICE_DT_GET(DT_INST_PHANDLE(inst, journal)));                   \
	sqnano_hybrid_media_init(&media_##inst.hy, &media_##inst.flash.media,                      \
				 &media_##inst.fram.media, DT_INST_PROP_OR(inst, page_size, 0));

#define ONE_MEDIA_INIT(inst)                                                                       \
	COND_CODE_1(IS_PARTITION(inst),                                                            \
		    (sqnano_flash_media_init(&media_##inst, PARTITION_BY_NODE(STORAGE(inst)));),   \
		    (sqnano_eeprom_media_init(&media_##inst, DEVICE_DT_GET(STORAGE(inst)));))

static int sqnano_dt_init(void)
{
	DT_INST_FOREACH_STATUS_OKAY(MEDIA_INIT)
#ifdef CONFIG_SQNANO_SQLITE_COMPAT
	for (size_t i = 0; i < ARRAY_SIZE(dbs); i++) {
		(void)sqnano_sqlite3_register(dbs[i].path, dbs[i].db, dbs[i].media, &dbs[i].cfg,
					      &sqnano_app_schema);
	}
#endif
	return 0;
}

SYS_INIT(sqnano_dt_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

int sqnano_dt_count(void)
{
	return ARRAY_SIZE(dbs);
}

const struct sqnano_dt_db *sqnano_dt_get(int i)
{
	return (i >= 0 && i < (int)ARRAY_SIZE(dbs)) ? &dbs[i] : NULL;
}

#else

int sqnano_dt_count(void)
{
	return 0;
}

const struct sqnano_dt_db *sqnano_dt_get(int i)
{
	ARG_UNUSED(i);
	return NULL;
}

#endif

const struct sqnano_dt_db *sqnano_dt_find(const char *path)
{
	for (int i = 0; i < sqnano_dt_count(); i++) {
		if (strcmp(sqnano_dt_get(i)->path, path) == 0) {
			return sqnano_dt_get(i);
		}
	}
	return NULL;
}

int sqnano_dt_open(const struct sqnano_dt_db *d, const struct sqnano_schema *schema,
		   struct sqnano_db **out)
{
	static K_MUTEX_DEFINE(open_lock);
	int rc = SQNANO_OK;

	if (!d) {
		return SQNANO_MISUSE;
	}
	k_mutex_lock(&open_lock, K_FOREVER);
	if (!d->db->is_open) {
		rc = sqnano_db_open(d->db, d->media, &d->cfg,
				    schema ? schema : (&sqnano_app_schema ? &sqnano_app_schema : NULL));
	}
	k_mutex_unlock(&open_lock);
	if (out) {
		*out = rc == SQNANO_OK ? d->db : NULL;
	}
	return rc;
}
