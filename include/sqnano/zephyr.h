/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Zephyr integration: media drivers and devicetree databases.
 *
 * Databases are declared in the devicetree:
 *
 * @code{.dts}
 * appdb: sqnano {
 *     compatible = "zephyr,sqnano";
 *     path = "app.db";
 *     storage = <&storage_partition>;   // or <&fram0> (EEPROM API)
 * };
 *
 * bigdb: sqnano-nand {
 *     compatible = "zephyr,sqnano";
 *     path = "big.db";
 *     storage = <&nand_partition>;      // a partition of a spi-nand device
 *     nand;
 * };
 *
 * logdb: sqnano-lfs {
 *     compatible = "zephyr,sqnano";
 *     path = "log.db";
 *     fs-dir = "/lfs/log.db";           // a directory on a mounted LittleFS
 *     fs-size = <DT_SIZE_K(128)>;
 * };
 * @endcode
 *
 * Each instance gets a struct sqnano_db and a media driver, and is
 * registered for sqlite3_open() when CONFIG_SQNANO_SQLITE_COMPAT is set.
 */

#ifndef SQNANO_ZEPHYR_H_
#define SQNANO_ZEPHYR_H_

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <sqnano/media.h>
#include <sqnano/sqnano.h>

#ifdef __cplusplus
extern "C" {
#endif

struct flash_area;

/** Flash partition media (NOR, internal flash, QSPI). */
struct sqnano_flash_media {
	struct sqnano_media media;
	const struct flash_area *fa;
};

/** EEPROM API media (FRAM, MRAM, EEPROM). */
struct sqnano_eeprom_media {
	struct sqnano_media media;
	const struct device *dev;
};

/** Flash partition behind a FRAM (hybrid mode). */
struct sqnano_hybrid_zmedia {
	struct sqnano_hybrid_media hy;
	struct sqnano_flash_media flash;
	struct sqnano_eeprom_media fram;
};

/** NAND flash partition (spi_nand, flexspi_nand drivers). */
struct sqnano_nand_zmedia {
	struct sqnano_nand_media nm;
	const struct flash_area *fa;
	struct sqnano_nand_geometry geo;
};

/** Files in a directory of a mounted file system (LittleFS). */
struct sqnano_fs_media {
	struct sqnano_file_media fm;
	const char *dir;
};

void sqnano_flash_media_init(struct sqnano_flash_media *m, const struct flash_area *fa);
/**
 * @brief Initialize a file system media.
 *
 * @param dir Directory of the database files, created on first open.
 * @param cache Buffer of @p chunk bytes.
 */
void sqnano_fs_media_init(struct sqnano_fs_media *m, const char *dir, uint32_t size,
			  uint32_t chunk, uint8_t *cache);
void sqnano_eeprom_media_init(struct sqnano_eeprom_media *m, const struct device *dev);
/**
 * @brief Initialize a NAND partition media.
 *
 * The geometry comes from the flash driver: its write block is the NAND
 * page, its erase page the block.
 *
 * @param soft_ecc Software ECC, for chips without on-die ECC.
 * @param reserved_blocks Blocks kept to replace bad ones (0: default).
 */
int sqnano_nand_zmedia_init(struct sqnano_nand_zmedia *z, const struct flash_area *fa,
			    bool soft_ecc, uint16_t reserved_blocks);

/** A database declared in the devicetree. */
struct sqnano_dt_db {
	const char *path;
	struct sqnano_db *db;
	struct sqnano_media *media;
	struct sqnano_db_config cfg;
};

/** Find a devicetree database by its path property. */
const struct sqnano_dt_db *sqnano_dt_find(const char *path);

/** Number of devicetree databases and access by index. */
int sqnano_dt_count(void);
const struct sqnano_dt_db *sqnano_dt_get(int i);

/**
 * @brief Open a devicetree database with the application schema.
 *
 * Opening is idempotent: later calls return the already open database.
 */
int sqnano_dt_open(const struct sqnano_dt_db *d, const struct sqnano_schema *schema,
		   struct sqnano_db **out);

#ifdef __cplusplus
}
#endif

#endif /* SQNANO_ZEPHYR_H_ */
