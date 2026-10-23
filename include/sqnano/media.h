/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Storage media drivers.
 *
 * The engine only sees numbered pages. A media driver describes what the
 * physical medium can do (struct sqnano_media_caps) and the pager derives
 * its commit strategy from it: media that need an erase use shadow paging,
 * byte-writable media use in-place updates protected by an undo journal.
 */

#ifndef SQNANO_MEDIA_H_
#define SQNANO_MEDIA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capabilities of a storage medium. */
struct sqnano_media_caps {
	/** Program granularity: 1 byte (FRAM), 4..256 bytes (NOR), 2 KiB (NAND). */
	uint32_t write_granularity;
	/** Erase unit, or 0 when the medium needs no erase (FRAM, EEPROM). */
	uint32_t erase_size;
	/** Rated erase or write cycles per cell. */
	uint32_t endurance_cycles;
	/** Size of a write guaranteed atomic on power loss; usually 0. */
	uint32_t atomic_write_size;
	/** The medium needs error correction (NAND). */
	bool needs_ecc;
	/** Pages of a block must be programmed in order (NAND). */
	bool sequential_in_block;
	/**
	 * Rewriting a page elsewhere costs no more than in place (a file
	 * system): copy-on-write is chosen over the undo journal, which writes
	 * everything twice.
	 */
	bool prefer_shadow;
	/** Page size to use when the configuration leaves it to the engine (0: none). */
	uint32_t page_hint;
};

struct sqnano_media;

/** Driver operations. Every operation returns 0 or a negative errno value. */
struct sqnano_media_api {
	/** Prepare the medium and fill in its size and capabilities. */
	int (*init)(struct sqnano_media *m);
	int (*read)(const struct sqnano_media *m, uint32_t off, void *buf, size_t len);
	/** Program @p len bytes. Erase-based media require an erased range. */
	int (*write)(const struct sqnano_media *m, uint32_t off, const void *buf, size_t len);
	/** Erase a range aligned on caps.erase_size. Unused when erase_size is 0. */
	int (*erase)(const struct sqnano_media *m, uint32_t off, size_t len);
	/**
	 * Optional barrier: every write before it is durable when it returns.
	 * A driver that buffers writes must provide it; the engine calls it
	 * before and after each commit record.
	 */
	int (*sync)(const struct sqnano_media *m);
	/**
	 * Optional hint: the content of the range no longer matters (free
	 * pages). It may later read back as any earlier content of the range.
	 */
	int (*discard)(const struct sqnano_media *m, uint32_t off, size_t len);
};

/** A storage medium instance. */
struct sqnano_media {
	const struct sqnano_media_api *api;
	/** Driver-specific context. */
	void *ctx;
	/** Usable size in bytes, filled by init(). */
	uint32_t size;
	/** Capabilities, filled by init(). */
	struct sqnano_media_caps caps;
};

/**
 * @brief Operations of a file system for the file media.
 *
 * Chunk @p index is a file of the database directory. Every operation
 * returns 0 or a negative errno value.
 */
struct sqnano_file_ops {
	/**
	 * Read @p len bytes at @p off of the chunk. Bytes beyond the end of the
	 * file, or of a missing file, read as 0.
	 */
	int (*read)(void *ctx, uint32_t index, uint32_t off, void *buf, size_t len);
	/** Replace the whole chunk, atomically on power loss (LittleFS files are). */
	int (*write)(void *ctx, uint32_t index, const void *buf, size_t len);
};

/**
 * @brief Media made of files on a power-safe file system (LittleFS).
 *
 * The media is cut into chunks of @c chunk bytes, one file each, created
 * when first written. One chunk is kept in RAM and written back when
 * another one is needed or at a commit barrier, so the pages a transaction
 * writes next to each other cost one file update. Each file update is
 * atomic, so the engine uses copy-on-write pages and a superblock ring as
 * on flash, without any erase.
 */
struct sqnano_file_media {
	struct sqnano_media media;
	const struct sqnano_file_ops *ops;
	void *ctx;
	uint32_t size;
	uint32_t chunk;
	/** @c chunk bytes of RAM owned by the caller. */
	uint8_t *cache;
	uint32_t cached;
	bool dirty;
	/** File updates, for statistics and tests. */
	uint32_t writes;
};

/**
 * @brief Initialize a file media.
 *
 * @param size Size of the media in bytes (a multiple of @p chunk).
 * @param chunk Chunk size: a multiple of the page size, typically the file
 *        system block size.
 * @param cache Buffer of @p chunk bytes.
 */
void sqnano_file_media_init(struct sqnano_file_media *m, const struct sqnano_file_ops *ops,
			    void *ctx, uint32_t size, uint32_t chunk, uint8_t *cache);

#ifndef SQNANO_HYBRID_MAX_SLOTS
#if defined(CONFIG_SQNANO_HYBRID_MAX_SLOTS)
#define SQNANO_HYBRID_MAX_SLOTS CONFIG_SQNANO_HYBRID_MAX_SLOTS
#else
#define SQNANO_HYBRID_MAX_SLOTS 64
#endif
#endif
#ifndef SQNANO_HYBRID_MAX_ERASES
#if defined(CONFIG_SQNANO_HYBRID_MAX_ERASES)
#define SQNANO_HYBRID_MAX_ERASES CONFIG_SQNANO_HYBRID_MAX_ERASES
#else
#define SQNANO_HYBRID_MAX_ERASES 64
#endif
#endif
#ifndef SQNANO_HYBRID_MAX_BLOCK
#if defined(CONFIG_SQNANO_MAX_PAGE_SIZE)
#define SQNANO_HYBRID_MAX_BLOCK CONFIG_SQNANO_MAX_PAGE_SIZE
#else
#define SQNANO_HYBRID_MAX_BLOCK 4096
#endif
#endif

/**
 * @brief NOR flash behind a FRAM: the FRAM keeps the recent blocks.
 *
 * Behaves as the flash (same size, erase unit and program rules), but a
 * block write goes to a FRAM slot (header and CRC last) and an erase is
 * only recorded in the FRAM. A block reaches the flash when the FRAM is
 * full, oldest first; the flash unit is physically erased at that point
 * if it was erased since. Versions that die quickly (the superblock ring,
 * the root and hot leaves of the trees, pages freed by the next commit)
 * never wear the flash. Every step is ordered so that a power cut leaves
 * the state before or after the interrupted operation.
 */
struct sqnano_hybrid_media {
	struct sqnano_media media;
	struct sqnano_media *flash;
	struct sqnano_media *fram;
	uint32_t blk;
	uint32_t esize;
	uint32_t nslots;
	uint32_t nerase;
	uint32_t slot_off;
	uint32_t gen;
	/* RAM image of the FRAM tables; seq 0 marks a free entry. */
	struct {
		uint32_t block;
		uint32_t seq;
	} slot[SQNANO_HYBRID_MAX_SLOTS];
	struct {
		uint32_t unit;
		uint32_t seq;
	} er[SQNANO_HYBRID_MAX_ERASES];
	uint8_t buf[SQNANO_HYBRID_MAX_BLOCK];
	uint8_t tmp[SQNANO_HYBRID_MAX_BLOCK];
	/** Statistics. */
	uint32_t evictions;
	uint32_t flash_programs;
	uint32_t flash_erases;
	uint32_t virtual_erases;
	uint32_t refreshes;
	uint32_t discards;
};

/**
 * @brief Initialize a hybrid media.
 *
 * @param block Block size: the database page size, a divisor of the flash
 *        erase unit; 0 selects the default page size of the engine.
 */
void sqnano_hybrid_media_init(struct sqnano_hybrid_media *h, struct sqnano_media *flash,
			      struct sqnano_media *fram, uint32_t block);

#ifndef SQNANO_NAND_MAX_BLOCKS
#if defined(CONFIG_SQNANO_NAND_MAX_BLOCKS)
#define SQNANO_NAND_MAX_BLOCKS CONFIG_SQNANO_NAND_MAX_BLOCKS
#else
#define SQNANO_NAND_MAX_BLOCKS 256
#endif
#endif
#ifndef SQNANO_NAND_MAX_SLOTS
#if defined(CONFIG_SQNANO_NAND_MAX_SLOTS)
#define SQNANO_NAND_MAX_SLOTS CONFIG_SQNANO_NAND_MAX_SLOTS
#else
#define SQNANO_NAND_MAX_SLOTS 4096
#endif
#endif
#ifndef SQNANO_NAND_MAX_PAGE
#if defined(CONFIG_SQNANO_NAND_MAX_PAGE)
#define SQNANO_NAND_MAX_PAGE CONFIG_SQNANO_NAND_MAX_PAGE
#else
#define SQNANO_NAND_MAX_PAGE 4096
#endif
#endif

/**
 * @brief Raw NAND operations (a chip, or a partition of one).
 *
 * Pages and blocks are numbered from 0 within the partition. Every
 * operation returns 0 or a negative errno value; -EIO is reserved for a
 * failure reported by the chip (program or erase status), which makes the
 * block bad. Any other error, a power loss for instance, is passed up.
 */
struct sqnano_nand_ops {
	/**
	 * Read the data area of a page. Returns 0, the number of bit flips
	 * the chip's ECC corrected (> 0), or -EBADMSG when it could not
	 * correct the page.
	 */
	int (*read)(void *ctx, uint32_t page, void *buf);
	/** Program an erased page, once (pages of a block in increasing order). */
	int (*program)(void *ctx, uint32_t page, const void *buf);
	int (*erase)(void *ctx, uint32_t block);
	/** 1 when the block is marked bad, 0 otherwise. Optional. */
	int (*is_bad)(void *ctx, uint32_t block);
	/** Mark a block bad (after a program or erase failure). Optional. */
	int (*mark_bad)(void *ctx, uint32_t block);
};

/** Geometry and policy of a NAND media. */
struct sqnano_nand_geometry {
	/** Data area of a page, at most SQNANO_NAND_MAX_PAGE bytes. */
	uint32_t page_size;
	/** Pages per erase block, at most 255. */
	uint32_t pages_per_block;
	/** Blocks of the partition, at most SQNANO_NAND_MAX_BLOCKS. */
	uint32_t blocks;
	/** Pages per block kept to absorb interrupted programs (0: 2). */
	uint8_t spare_pages;
	/** Blocks kept to replace bad ones (0: 2 % of the blocks plus 2). */
	uint16_t reserved_blocks;
	/** Software ECC, for chips without on-die ECC: corrects 1 bit per 512 bytes. */
	bool soft_ecc;
	/** Bit flips corrected in a page that trigger the refresh of its block (0: 1). */
	uint8_t refresh_threshold;
};

/**
 * @brief NAND flash seen as a NOR flash by the engine.
 *
 * The engine programs each page once between erases, in any order; NAND
 * requires the pages of a block to be programmed in order. Each logical
 * erase unit is backed by a physical block whose pages are written one
 * after the other, each tagged in its last bytes with the logical page it
 * holds (the Zephyr NAND API has no access to the spare area); the mapping
 * is rebuilt from the tags at init. Logical pages are therefore a little
 * smaller than the NAND pages.
 *
 * A block that fails a program or an erase is marked bad and its live pages
 * are copied to a reserved block; a block whose pages need ECC corrections
 * is refreshed the same way. A copy is tagged with a generation and closed
 * by a marker page, so that a power cut during it leaves the old block in
 * use.
 */
struct sqnano_nand_media {
	struct sqnano_media media;
	const struct sqnano_nand_ops *ops;
	void *ctx;
	struct sqnano_nand_geometry geo;
	/** Logical page size and erase unit (filled by init). */
	uint32_t slot;
	uint32_t unit_size;
	uint16_t spb;
	uint16_t units;
	uint16_t cursor;
	/* Per logical unit: physical block, next physical page, generation, flags. */
	uint16_t phys[SQNANO_NAND_MAX_BLOCKS];
	uint8_t fill[SQNANO_NAND_MAX_BLOCKS];
	uint16_t gen[SQNANO_NAND_MAX_BLOCKS];
	uint8_t uflags[SQNANO_NAND_MAX_BLOCKS];
	/* Per physical block. */
	uint8_t bstate[SQNANO_NAND_MAX_BLOCKS];
	/* Per logical page: physical page within the block, 0xff when not written. */
	uint8_t map[SQNANO_NAND_MAX_SLOTS];
	uint8_t buf[SQNANO_NAND_MAX_PAGE];
	/** Statistics. */
	uint32_t programs;
	uint32_t erases;
	uint32_t corrected;
	uint32_t uncorrectable;
	uint32_t relocations;
	uint32_t refreshes;
	uint32_t bad_blocks;
	uint32_t factory_bad;
};

/** Initialize a NAND media; the geometry is copied. */
void sqnano_nand_media_init(struct sqnano_nand_media *n, const struct sqnano_nand_ops *ops,
			    void *ctx, const struct sqnano_nand_geometry *geo);

/**
 * @brief RAM NAND with power cuts, bad blocks and bit flips (tests).
 *
 * Checks the NAND rules: a page is programmed once after an erase, and the
 * pages of a block in increasing order (violations counts the breaches).
 * A cut interrupts a program (the page holds part of the data and noise)
 * or an erase (some pages erased, the others left); every later operation
 * fails with -ENXIO until power on. With chip_ecc, the chip corrects up to
 * 4 bit flips per page and reports a torn page as uncorrectable; without,
 * flips and noise reach the reader.
 */
struct sqnano_ram_nand {
	struct sqnano_nand_ops ops;
	uint8_t *mem;
	/* Per page: bit 7 programmed, bit 6 torn, bits 0-5 bit flips (chip_ecc). */
	uint8_t *meta;
	uint32_t page_size;
	uint32_t ppb;
	uint32_t blocks;
	bool chip_ecc;
	uint8_t bad[SQNANO_NAND_MAX_BLOCKS];
	/* Worn blocks fail every program and erase. */
	uint8_t worn[SQNANO_NAND_MAX_BLOCKS];
	uint8_t next[SQNANO_NAND_MAX_BLOCKS];
	int32_t cut_countdown;
	bool powered_off;
	uint32_t rng;
	/** Program, erase and mark-bad operations. */
	uint32_t ops_count;
	uint32_t programs;
	uint32_t erases;
	uint32_t violations;
	/** Wear out the block of the Nth program or erase from now (-1: never). */
	int32_t fail_program_after;
	int32_t fail_erase_after;
};

/**
 * @brief Initialize a RAM NAND, erased, without bad blocks.
 *
 * @param mem Data, @p blocks * @p ppb * @p page_size bytes.
 * @param meta One byte per page.
 */
void sqnano_ram_nand_init(struct sqnano_ram_nand *r, uint8_t *mem, uint8_t *meta,
			  uint32_t page_size, uint32_t ppb, uint32_t blocks, bool chip_ecc);
/** Arm a power cut during the (@p ops + 1)th program, erase or mark-bad. */
void sqnano_ram_nand_cut_after(struct sqnano_ram_nand *r, uint32_t ops, uint32_t seed);
void sqnano_ram_nand_power_on(struct sqnano_ram_nand *r);
/** Flip bit @p bit of page @p page (a stored error, as charge loss causes). */
void sqnano_ram_nand_flip(struct sqnano_ram_nand *r, uint32_t page, uint32_t bit);

/** Kind of memory simulated by the RAM driver. */
enum sqnano_ram_kind {
	/** Byte-writable memory (FRAM, MRAM, EEPROM). */
	SQNANO_RAM_FRAM,
	/** NOR flash: programming can only clear bits, erase sets 0xFF. */
	SQNANO_RAM_NOR,
};

/**
 * @brief RAM media with power-cut injection.
 *
 * Simulates FRAM or NOR flash in a caller-provided buffer. A power cut can be
 * armed to occur during the Nth write or erase: the operation is applied
 * partially (a random prefix, possibly with a corrupted boundary byte) and
 * every later operation fails until sqnano_ram_media_power_on() is called.
 */
struct sqnano_ram_media {
	struct sqnano_media media;
	uint8_t *mem;
	uint32_t mem_size;
	enum sqnano_ram_kind kind;
	uint32_t write_granularity;
	uint32_t erase_size;
	/** Remaining write/erase operations before the cut, or -1 when disarmed. */
	int32_t cut_countdown;
	bool powered_off;
	uint32_t rng;
	/** Number of write and erase operations performed since init. */
	uint32_t ops;
	/** Number of erase operations performed since init. */
	uint32_t erases;
	/** NOR: bytes programmed without having been erased (must stay 0). */
	uint32_t overwrites;
	/**
	 * Media cut together (the NOR and the FRAM of one board): point it at
	 * the leader, whose power state and cut countdown the group shares.
	 */
	struct sqnano_ram_media *group;
	/** Leader: write and erase operations of the whole group. */
	uint32_t gops;
};

/**
 * @brief Initialize a RAM medium.
 *
 * @param r Instance.
 * @param mem Backing memory, kept by the caller.
 * @param size Size of @p mem in bytes.
 * @param kind Simulated technology.
 * @param erase_size Erase unit for SQNANO_RAM_NOR, ignored for FRAM.
 */
void sqnano_ram_media_init(struct sqnano_ram_media *r, uint8_t *mem, uint32_t size,
			   enum sqnano_ram_kind kind, uint32_t erase_size);

/** Erase the whole medium to its blank state (0xFF for NOR, 0x00 for FRAM). */
void sqnano_ram_media_blank(struct sqnano_ram_media *r);

/** Arm a power cut during the (@p ops + 1)th write or erase. */
void sqnano_ram_media_cut_after(struct sqnano_ram_media *r, uint32_t ops, uint32_t seed);

/** Restore power and disarm any pending cut. */
void sqnano_ram_media_power_on(struct sqnano_ram_media *r);

#ifdef __cplusplus
}
#endif

#endif /* SQNANO_MEDIA_H_ */
