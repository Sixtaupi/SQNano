/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Page manager: page cache, allocation, commit strategies.
 *
 * Media layout without erase (FRAM, EEPROM; P = page size, K superblock
 * units, chosen at format time and recorded in every superblock):
 *
 *   [0, K.P)         superblock ring: the record of commit seq goes to slot
 *                    seq % (K.P / record size), so no byte is hot
 *   [K.P, K.P + J)   journal slots (journal mode only), each P + 16 bytes;
 *                    each transaction starts one slot further
 *   [data_off, ...)  data pages, numbered from 1
 *
 * Flash (E = erase unit, U = max(E, P), P divides U):
 *
 *   [0, K.E)         superblock log: records appended one after the other,
 *                    the next unit of the ring is erased when one is full
 *   [K.E, ...)       data pages, U / P per erase unit
 *
 * A commit always ends by writing a superblock with seq + 1, so a power cut
 * leaves either the previous or the new state. Pages reachable from the
 * committed state are never overwritten, except on journal media where a
 * page is first copied to the undo journal and then updated in place. A
 * transaction falls back to copy-on-write when an old reader still needs
 * the journal, when the journal is full, or for pages pinned by an
 * unconfirmed migration.
 *
 * On flash a page is only programmed when erased. An erase unit is erased
 * once all its pages are free; compaction moves the live pages out of a
 * unit to make it so (see pg_gc).
 *
 * Freed pages wait in a "limbo" set until no snapshot older than the
 * freeing commit is open.
 */

#include <errno.h>

#include "../sqnano_int.h"

#define SB_MAGIC 0x414e5153u /* "SQNA" */
#define SB_SIZE  64
#define SB_UNITS_MAX 16
#define JR_MAGIC 0x4a53u

static int io_err(int err)
{
	return err == -ENOSPC ? SQNANO_FULL : SQNANO_IOERR;
}

uint16_t pg_usable(const struct sqnano_db *db)
{
	return (uint16_t)(db->page_size - SQNANO_PAGE_HDR);
}

static uint32_t page_off(const struct sqnano_db *db, uint32_t pgno)
{
	return db->data_off + (pgno - 1) * (uint32_t)db->page_size;
}

static uint32_t page_crc(uint32_t pgno, const uint8_t *data, uint16_t ps)
{
	uint8_t no[4];

	put_u32(no, pgno);
	return sqnano_crc32(sqnano_crc32(0, no, 4), data + 4, ps - 4u);
}

static bool erase_based(const struct sqnano_db *db)
{
	return db->media->caps.erase_size != 0;
}

/* Write a full region, erasing it first on flash. */
static int media_put(struct sqnano_db *db, uint32_t off, const uint8_t *buf, uint32_t len,
		     uint32_t region)
{
	int err;

	if (db->read_only) {
		return SQNANO_READONLY;
	}
	if (erase_based(db)) {
		err = db->media->api->erase(db->media, off, region);
		db->stat_erases++;
		if (err) {
			return io_err(err);
		}
	}
	err = db->media->api->write(db->media, off, buf, len);
	db->stat_writes++;
	return err ? io_err(err) : SQNANO_OK;
}

/* Program an erased range (flash). */
static int media_program(struct sqnano_db *db, uint32_t off, const uint8_t *buf, uint32_t len)
{
	int err;

	if (db->read_only) {
		return SQNANO_READONLY;
	}
	err = db->media->api->write(db->media, off, buf, len);
	db->stat_writes++;
	return err ? io_err(err) : SQNANO_OK;
}

static int media_erase(struct sqnano_db *db, uint32_t off, uint32_t len)
{
	int err;

	if (db->read_only) {
		return SQNANO_READONLY;
	}
	err = db->media->api->erase(db->media, off, len);
	db->stat_erases++;
	return err ? io_err(err) : SQNANO_OK;
}

/* Barrier for media that buffer writes (file systems). */
static int media_sync(struct sqnano_db *db)
{
	int err = db->media->api->sync ? db->media->api->sync(db->media) : 0;

	return err ? io_err(err) : SQNANO_OK;
}

/* Free for good: media that keep copies (FRAM in front of a flash) drop them. */
static void media_discard(struct sqnano_db *db, uint32_t pgno)
{
	if (db->media->api->discard && !db->read_only) {
		(void)db->media->api->discard(db->media, page_off(db, pgno), db->page_size);
	}
}

static int media_get(struct sqnano_db *db, uint32_t off, uint8_t *buf, uint32_t len)
{
	int err = db->media->api->read(db->media, off, buf, len);

	db->stat_reads++;
	return err ? SQNANO_IOERR : SQNANO_OK;
}

static int write_page(struct sqnano_db *db, uint32_t pgno, uint8_t *data)
{
	put_u32(data + PG_SEQ, db->sb.seq + 1);
	put_u32(data + PG_CRC, page_crc(pgno, data, db->page_size));
	if (erase_based(db)) {
		/*
		 * The allocator only hands out erased pages, and a page programmed
		 * once is copied before being modified again (pg_write).
		 */
		bm_set(db->bm_dirty, pgno);
		return media_program(db, page_off(db, pgno), data, db->page_size);
	}
	return media_put(db, page_off(db, pgno), data, db->page_size, db->page_size);
}

static int read_page(struct sqnano_db *db, uint32_t pgno, uint8_t *data)
{
	int rc = media_get(db, page_off(db, pgno), data, db->page_size);

	if (rc) {
		return rc;
	}
#ifndef SQNANO_FUZZING
	/* (Fuzzing skips it, so that corrupt pages reach the tree code.) */
	if (get_u32(data + PG_CRC) != page_crc(pgno, data, db->page_size)) {
		return SQNANO_CORRUPT;
	}
#endif
	return SQNANO_OK;
}

/* ---- Superblocks ---- */

static void sb_encode(const struct sqnano_db *db, const struct sqnano_sb *sb, uint8_t *b)
{
	memset(b, 0, SB_SIZE);
	put_u32(b + 0, SB_MAGIC);
	put_u16(b + 4, SQNANO_FORMAT_VERSION);
	put_u16(b + 6, sb->page_size);
	put_u32(b + 8, sb->seq);
	put_u32(b + 12, sb->master_root);
	put_u32(b + 16, sb->npages);
	put_u16(b + 20, sb->schema_version);
	put_u16(b + 22, sb->compat_version);
	put_u32(b + 24, sb->schema_hash);
	put_u32(b + 28, (uint32_t)sb->user_version);
	put_u32(b + 32, sb->pinned_root);
	put_u16(b + 36, sb->pinned_version);
	put_u16(b + 38, sb->pinned_compat);
	put_u32(b + 40, sb->pinned_hash);
	put_u32(b + 44, sb->pinned_seq);
	b[48] = db->mode;
	b[49] = db->sb_units;
	put_u16(b + 50, sb->lazy_pending);
	put_u16(b + 52, sb->pinned_lazy);
	put_u32(b + 60, sqnano_crc32(0, b, 60));
}

static bool sb_decode(const uint8_t *b, struct sqnano_sb *sb, uint8_t *mode, uint8_t *units)
{
	if (get_u32(b) != SB_MAGIC || get_u32(b + 60) != sqnano_crc32(0, b, 60)) {
		return false;
	}
	sb->format = get_u16(b + 4);
	sb->page_size = get_u16(b + 6);
	sb->seq = get_u32(b + 8);
	sb->master_root = get_u32(b + 12);
	sb->npages = get_u32(b + 16);
	sb->schema_version = get_u16(b + 20);
	sb->compat_version = get_u16(b + 22);
	sb->schema_hash = get_u32(b + 24);
	sb->user_version = (int32_t)get_u32(b + 28);
	sb->pinned_root = get_u32(b + 32);
	sb->pinned_version = get_u16(b + 36);
	sb->pinned_compat = get_u16(b + 38);
	sb->pinned_hash = get_u32(b + 40);
	sb->pinned_seq = get_u32(b + 44);
	*mode = b[48];
	*units = b[49];
	sb->lazy_pending = get_u16(b + 50);
	sb->pinned_lazy = get_u16(b + 52);
	return true;
}

static uint32_t sb_rec_size(const struct sqnano_db *db)
{
	uint32_t gran = db->media->caps.write_granularity ? db->media->caps.write_granularity : 1;

	return (SB_SIZE + gran - 1) / gran * gran;
}

/*
 * Flash: append the record after the newest one. When its erase unit is
 * full, the next unit of the ring (holding only older records) is erased.
 */
static int sb_append(struct sqnano_db *db, const struct sqnano_sb *sb)
{
	uint32_t rec = sb_rec_size(db);
	uint32_t e = db->media->caps.erase_size;
	uint8_t sec = db->sb_sector;
	uint32_t off = db->sb_next;
	uint8_t *b = db->scratch;
	int rc;

	if (off + rec > e) {
		sec = (uint8_t)((sec + 1u) % db->sb_units);
		off = 0;
		rc = media_erase(db, sec * e, e);
		if (rc) {
			return rc;
		}
	}
	memset(b, 0xff, rec);
	sb_encode(db, sb, b);
	rc = media_program(db, sec * e + off, b, rec);
	if (rc == SQNANO_OK) {
		db->sb_sector = sec;
		db->sb_next = off + rec;
	}
	return rc;
}

static int sb_write(struct sqnano_db *db, const struct sqnano_sb *sb)
{
	uint32_t rec = sb_rec_size(db);
	uint32_t slots = db->sb_units * db->sb_unit / rec;
	uint8_t *b = db->scratch;

	if (erase_based(db)) {
		return sb_append(db, sb);
	}
	/* A torn record only loses itself: the previous one is in another slot. */
	memset(b, 0x00, rec);
	sb_encode(db, sb, b);
	return media_put(db, sb->seq % slots * rec, b, rec, rec);
}

/*
 * Find the newest valid superblock record in units [0, units). On flash,
 * also find where the next record goes. With @p units 0, only report the
 * ring size recorded in the first valid record of the first two units.
 */
static int sb_scan(struct sqnano_db *db, uint8_t units, struct sqnano_sb *best, uint8_t *mode,
		   bool *found)
{
	uint32_t rec = sb_rec_size(db);
	uint32_t u = db->sb_unit;
	uint32_t next[SB_UNITS_MAX] = {0};
	uint8_t *b = db->scratch;
	bool probe = units == 0;
	int rc;

	*found = false;
	for (uint8_t sec = 0; sec < (probe ? 2 : units); sec++) {
		for (uint32_t off = 0; off + rec <= u; off += rec) {
			struct sqnano_sb sb;
			uint8_t md;
			uint8_t k;
			bool blank = true;

			rc = media_get(db, sec * u + off, b, rec);
			if (rc) {
				return rc;
			}
			for (uint32_t i = 0; i < rec && blank && erase_based(db); i++) {
				blank = b[i] == 0xff;
			}
			if (erase_based(db) && blank) {
				continue;
			}
			/* A torn record is skipped: the next one goes after it. */
			next[sec] = off + rec;
			if (!sb_decode(b, &sb, &md, &k)) {
				continue;
			}
			if (probe) {
				db->sb_units = k;
				*found = true;
				return SQNANO_OK;
			}
			if (!*found || sb.seq > best->seq) {
				*best = sb;
				*mode = md;
				*found = true;
				db->sb_sector = sec;
			}
		}
	}
	db->sb_next = next[db->sb_sector];
	return SQNANO_OK;
}

/* ---- Frames ---- */

static struct sqnano_frame *fr_find(struct sqnano_db *db, uint32_t pgno, bool priv)
{
	for (int i = 0; i < SQNANO_PAGE_CACHE_COUNT; i++) {
		struct sqnano_frame *f = &db->frames[i];

		if ((f->flags & FR_VALID) && f->pgno == pgno && !(f->flags & FR_TEMP) &&
		    !!(f->flags & FR_PRIVATE) == priv) {
			return f;
		}
	}
	return NULL;
}

static void fr_drop(struct sqnano_frame *f)
{
	f->flags = 0;
	f->pgno = 0;
}

static int fr_flush(struct sqnano_db *db, struct sqnano_frame *f)
{
	int rc;

	if (!(f->flags & FR_DIRTY)) {
		return SQNANO_OK;
	}
	rc = write_page(db, f->pgno, f->data);
	if (rc == SQNANO_OK) {
		f->flags &= (uint8_t)~FR_DIRTY;
	}
	return rc;
}

/* Find a frame to reuse; dirty frames are written out first. */
static int fr_alloc(struct sqnano_db *db, struct sqnano_frame **out)
{
	struct sqnano_frame *best = NULL;
	struct sqnano_frame *dirty = NULL;
	int rc;

	for (int i = 0; i < SQNANO_PAGE_CACHE_COUNT; i++) {
		struct sqnano_frame *f = &db->frames[i];

		if (f->refs) {
			continue;
		}
		if (!(f->flags & FR_VALID)) {
			best = f;
			break;
		}
		if (f->flags & FR_DIRTY) {
			if (!dirty || f->lru < dirty->lru) {
				dirty = f;
			}
		} else if (!best || f->lru < best->lru) {
			best = f;
		}
	}
	if (!best) {
		if (!dirty) {
			return SQNANO_NOMEM;
		}
		rc = fr_flush(db, dirty);
		if (rc) {
			return rc;
		}
		best = dirty;
	}
	fr_drop(best);
	*out = best;
	return SQNANO_OK;
}

static void fr_hold(struct sqnano_db *db, struct sqnano_frame *f)
{
	f->refs++;
	f->lru = ++db->lru_clock;
}

void pg_put(struct sqnano_db *db, struct sqnano_frame *f)
{
	(void)db;
	if (!f) {
		return;
	}
	if (f->refs) {
		f->refs--;
	}
	if (!f->refs && (f->flags & FR_TEMP)) {
		fr_drop(f);
	}
}

static int jslot_find(const struct sqnano_db *db, uint32_t pgno)
{
	for (int i = 0; i < db->jcount; i++) {
		if (db->jmap[i] == pgno) {
			return i;
		}
	}
	return -1;
}

static uint32_t jslot_phys(const struct sqnano_db *db, int slot)
{
	return db->jrnl_off + (uint32_t)slot * (db->page_size + SQNANO_JRNL_HDR);
}

/* Slot of the @p i th page journaled by the transaction. */
static uint32_t jslot_off(const struct sqnano_db *db, int i)
{
	return jslot_phys(db, (db->jstart + i) % db->jslots);
}

static int jread(struct sqnano_db *db, int slot, uint8_t *data)
{
	return media_get(db, jslot_off(db, slot) + SQNANO_JRNL_HDR, data, db->page_size);
}

static bool valid_pgno(const struct sqnano_db *db, uint32_t pgno)
{
	return pgno >= 1 && pgno <= db->npages;
}

int pg_get(struct sqnano_txn *t, uint32_t pgno, struct sqnano_frame **out)
{
	struct sqnano_db *db = t->db;
	struct sqnano_frame *f;
	int rc;

	if (!valid_pgno(db, pgno)) {
		return SQNANO_CORRUPT;
	}
	if (t->write &&
	    (bm_test(db->bm_alloc, pgno) || (t->inplace && bm_test(db->bm_jrnl, pgno)))) {
		/* Page owned by the writer. */
		f = fr_find(db, pgno, true);
		if (!f) {
			rc = fr_alloc(db, &f);
			if (rc) {
				return rc;
			}
			rc = read_page(db, pgno, f->data);
			if (rc) {
				return rc;
			}
			f->pgno = pgno;
			f->flags = FR_VALID | FR_PRIVATE;
		} else {
			db->stat_hits++;
		}
		fr_hold(db, f);
		*out = f;
		return SQNANO_OK;
	}
	if (!t->write && db->jtxn == t->seq + 1 && bm_test(db->bm_jrnl, pgno)) {
		/* The snapshot predates the in-place update: use the undo image. */
		int slot = jslot_find(db, pgno);
		bool latest = db->sb.seq == t->seq;

		if (slot < 0) {
			return SQNANO_INTERNAL;
		}
		f = latest ? fr_find(db, pgno, false) : NULL;
		if (!f) {
			rc = fr_alloc(db, &f);
			if (rc) {
				return rc;
			}
			rc = jread(db, slot, f->data);
			if (rc) {
				return rc;
			}
			f->pgno = pgno;
			f->flags = latest ? FR_VALID : (FR_VALID | FR_TEMP);
		}
		fr_hold(db, f);
		*out = f;
		return SQNANO_OK;
	}
	f = fr_find(db, pgno, false);
	if (!f) {
		rc = fr_alloc(db, &f);
		if (rc) {
			return rc;
		}
		rc = read_page(db, pgno, f->data);
		if (rc) {
			return rc;
		}
		f->pgno = pgno;
		f->flags = FR_VALID;
	} else {
		db->stat_hits++;
	}
	fr_hold(db, f);
	*out = f;
	return SQNANO_OK;
}

/* ---- Flash erase units ---- */

static uint32_t unit_first(const struct sqnano_db *db, uint32_t pgno)
{
	return (pgno - 1) / db->ppe * db->ppe + 1;
}

/* Is every page of the erase unit holding @p pgno free for good? */
static bool unit_free(const struct sqnano_db *db, uint32_t pgno)
{
	uint32_t first = unit_first(db, pgno);

	for (uint32_t p = first; p < first + db->ppe; p++) {
		if (bm_test(db->bm_used, p) || bm_test(db->bm_limbo, p) ||
		    bm_test(db->bm_pinned, p) || bm_test(db->bm_freed, p) ||
		    bm_test(db->bm_temp, p)) {
			return false;
		}
	}
	return true;
}

static int unit_erase(struct sqnano_db *db, uint32_t pgno)
{
	uint32_t first = unit_first(db, pgno);
	int rc = media_erase(db, page_off(db, first), db->esize);

	if (rc == SQNANO_OK) {
		for (uint32_t p = first; p < first + db->ppe; p++) {
			bm_clr(db->bm_dirty, p);
		}
	}
	return rc;
}

/*
 * Which free pages are still erased? Pages of a unit without live data are
 * erased on demand anyway; the others are read.
 */
static int dirty_rebuild(struct sqnano_db *db)
{
	uint8_t *b = db->scratch;
	int rc;

	memset(db->bm_dirty, 0, sizeof(db->bm_dirty));
	for (uint32_t p = 1; p <= db->npages; p++) {
		bool blank = true;

		if (bm_test(db->bm_used, p) || bm_test(db->bm_pinned, p) || unit_free(db, p)) {
			bm_set(db->bm_dirty, p);
			continue;
		}
		rc = media_get(db, page_off(db, p), b, db->page_size);
		if (rc) {
			return rc;
		}
		for (uint32_t i = 0; i < db->page_size && blank; i++) {
			blank = b[i] == 0xff;
		}
		if (!blank) {
			bm_set(db->bm_dirty, p);
		}
	}
	return SQNANO_OK;
}

/* Pages a transaction can still get: erased ones, and those of units free for good. */
static uint32_t avail_pages(const struct sqnano_db *db)
{
	uint32_t n = 0;

	for (uint32_t p = 1; p <= db->npages; p += db->ppe) {
		if (unit_free(db, p)) {
			n += db->ppe;
			continue;
		}
		for (uint32_t q = p; q < p + db->ppe; q++) {
			if (!bm_test(db->bm_used, q) && !bm_test(db->bm_limbo, q) &&
			    !bm_test(db->bm_pinned, q) && !bm_test(db->bm_freed, q) &&
			    !bm_test(db->bm_temp, q) && !bm_test(db->bm_dirty, q)) {
				n++;
			}
		}
	}
	return n;
}

/* Pages kept for compaction: a write transaction cannot use them. */
static uint32_t gc_reserve(const struct sqnano_db *db)
{
	return db->ppe > 1 ? 2u * db->ppe + 8u : 0;
}

/* ---- Savepoints ---- */

/*
 * Each page touched while a savepoint is open records the savepoint depth at
 * which the writer acquired it (allocated or journaled, low nibble) and at
 * which it freed it (high nibble). A page the writer owned before the newest
 * savepoint opened is frozen: later writes copy it instead of modifying it,
 * and freeing it keeps it reserved. ROLLBACK TO undoes every page whose level
 * is above the savepoint and restores the root it saved.
 */
#if SQNANO_MAX_SAVEPOINTS > 0
#define LVL_ACQ(v)  ((v) & 0x0fu)
#define LVL_FREE(v) ((uint8_t)(v) >> 4)

static bool frozen(const struct sqnano_db *db, uint32_t pgno)
{
	return db->sp_depth && LVL_ACQ(db->sp_lvl[pgno]) < db->sp_depth;
}

static void lvl_acquired(struct sqnano_db *db, uint32_t pgno)
{
	if (db->sp_depth) {
		db->sp_lvl[pgno] = db->sp_depth;
	}
}

static void lvl_freed(struct sqnano_db *db, uint32_t pgno)
{
	if (db->sp_depth) {
		db->sp_lvl[pgno] = (uint8_t)(LVL_ACQ(db->sp_lvl[pgno]) | db->sp_depth << 4);
	}
}

/*
 * The savepoints above depth @p d are gone: their changes now belong to
 * level @p d, and pages freed at that level that nobody froze become free.
 */
static void sp_merge(struct sqnano_db *db, uint8_t d)
{
	for (uint32_t p = 1; p <= db->npages; p++) {
		uint8_t v = db->sp_lvl[p];
		uint8_t a = LVL_ACQ(v) > d ? d : LVL_ACQ(v);
		uint8_t f = LVL_FREE(v) > d ? d : LVL_FREE(v);

		if (bm_test(db->bm_alloc, p) && bm_test(db->bm_freed, p) && a == d) {
			bm_clr(db->bm_alloc, p);
			bm_clr(db->bm_freed, p);
			a = f = 0;
		}
		db->sp_lvl[p] = (uint8_t)(a | f << 4);
	}
	db->sp_depth = d;
}

static int sp_find(const struct sqnano_db *db, uint32_t id)
{
	for (int i = 0; i < db->sp_depth; i++) {
		if (db->sp[i].serial == id) {
			return i;
		}
	}
	return -1;
}
#else
#define frozen(db, pgno)       false
#define lvl_acquired(db, pgno) ((void)0)
#define lvl_freed(db, pgno)    ((void)0)
#define sp_merge(db, d)        ((void)0)
#endif

static void release_limbo(struct sqnano_db *db)
{
	if (!db->limbo_any) {
		return;
	}
	for (int i = 0; i < SQNANO_MAX_READERS; i++) {
		if (db->reader_used[i] && db->readers[i] < db->limbo_seq) {
			return;
		}
	}
	for (uint32_t w = 0; db->media->api->discard && w < SQNANO_BM_WORDS; w++) {
		for (uint32_t b = 0; db->bm_limbo[w] && b < 32; b++) {
			if (db->bm_limbo[w] & (1u << b)) {
				media_discard(db, w * 32 + b);
			}
		}
	}
	memset(db->bm_limbo, 0, sizeof(db->bm_limbo));
	db->limbo_any = 0;
}

/*
 * Next-fit spreads writes over the medium. Each lap around it starts at a
 * pseudo-random page, so that page lifetimes cannot stay in phase with the
 * laps and keep wearing the same erase units.
 */
static void next_hint(struct sqnano_db *db, uint32_t next)
{
	uint32_t n = db->npages;
	uint32_t to_lap = (db->alloc_lap + n - db->alloc_hint) % n;
	uint32_t to_next = (next + n - db->alloc_hint) % n;

	if (to_next == 0) {
		to_next = n;
	}
	if (to_lap != 0 && to_lap <= to_next) {
		uint32_t x = db->alloc_rng;

		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		db->alloc_rng = x;
		db->alloc_lap = 1 + x % n;
		next = db->alloc_lap;
	}
	db->alloc_hint = next;
}

static int find_free(struct sqnano_db *db, uint32_t *pgno)
{
	uint32_t words = (db->npages + 32) / 32;
	uint32_t start = db->alloc_hint / 32;
	uint32_t below = (1u << (db->alloc_hint % 32)) - 1u;

	/* The hint's word is visited twice: from the hint, then (k == words) below it. */
	for (uint32_t k = 0; k <= words; k++) {
		uint32_t w = (start + k) % words;
		uint32_t busy = db->bm_used[w] | db->bm_limbo[w] | db->bm_pinned[w] |
				db->bm_freed[w] | db->bm_temp[w];

		busy |= k == 0 ? below : k == words ? ~below : 0u;
		if (busy == 0xffffffffu) {
			continue;
		}
		for (uint32_t b = 0; b < 32; b++) {
			uint32_t p = w * 32 + b;

			if ((busy & (1u << b)) || !valid_pgno(db, p) ||
			    (p >= db->gc_lo && p <= db->gc_hi)) {
				continue;
			}
			if (erase_based(db) && bm_test(db->bm_dirty, p)) {
				int rc;

				/* Programmed since the last erase: only usable with its whole unit. */
				if (!unit_free(db, p)) {
					continue;
				}
				rc = unit_erase(db, p);
				if (rc) {
					return rc;
				}
			}
			*pgno = p;
			next_hint(db, p + 1 > db->npages ? 1 : p + 1);
			return SQNANO_OK;
		}
	}
	return SQNANO_FULL;
}

int pg_alloc(struct sqnano_txn *t, struct sqnano_frame **out)
{
	struct sqnano_db *db = t->db;
	struct sqnano_frame *f;
	uint32_t pgno;
	int rc;

	if (!t->write) {
		return SQNANO_READONLY;
	}
	if (t->budget == 0) {
		/* Units emptied by the transaction itself may have become erasable. */
		uint32_t avail = avail_pages(db);
		uint32_t reserve = gc_reserve(db);

		t->budget = avail > reserve ? (int32_t)(avail - reserve) : 0;
		if (t->budget == 0) {
			/* The rest is kept to compact the flash. */
			return SQNANO_FULL;
		}
	}
	rc = find_free(db, &pgno);
	if (rc == SQNANO_FULL) {
		release_limbo(db);
		rc = find_free(db, &pgno);
	}
	if (rc) {
		return rc;
	}
	if (t->budget > 0) {
		t->budget--;
	}
	f = fr_find(db, pgno, false);
	if (f && !f->refs) {
		/* Stale content of a page nobody can reach any more. */
		fr_drop(f);
	}
	rc = fr_alloc(db, &f);
	if (rc) {
		return rc;
	}
	memset(f->data, 0, db->page_size);
	f->pgno = pgno;
	f->flags = FR_VALID | FR_PRIVATE | FR_DIRTY;
	bm_set(db->bm_used, pgno);
	bm_set(db->bm_alloc, pgno);
	lvl_acquired(db, pgno);
	t->gen++;
	fr_hold(db, f);
	*out = f;
	return SQNANO_OK;
}

int pg_free(struct sqnano_txn *t, uint32_t pgno)
{
	struct sqnano_db *db = t->db;
	struct sqnano_frame *f;

	if (!valid_pgno(db, pgno)) {
		return SQNANO_CORRUPT;
	}
	f = fr_find(db, pgno, true);
	if (f) {
		if (f->refs) {
			return SQNANO_INTERNAL;
		}
		if (frozen(db, pgno)) {
			/* A savepoint may bring it back: keep its content. */
			int rc = fr_flush(db, f);

			if (rc) {
				return rc;
			}
		}
		fr_drop(f);
	}
	t->gen++;
	if (bm_test(db->bm_alloc, pgno) && !frozen(db, pgno)) {
		bm_clr(db->bm_alloc, pgno);
		bm_clr(db->bm_used, pgno);
		return SQNANO_OK;
	}
	bm_clr(db->bm_used, pgno);
	bm_set(db->bm_freed, pgno);
	lvl_freed(db, pgno);
	return SQNANO_OK;
}

/* Copy the committed image of @p pgno to the next journal slot. */
static int journal_page(struct sqnano_db *db, uint32_t pgno, const uint8_t *data)
{
	int slot = db->jcount;
	uint32_t off = jslot_off(db, slot);
	uint8_t hdr[SQNANO_JRNL_HDR];
	uint32_t crc;
	int err;

	put_u16(hdr + 0, JR_MAGIC);
	put_u16(hdr + 2, 0);
	put_u32(hdr + 4, db->jtxn);
	put_u32(hdr + 8, pgno);
	crc = sqnano_crc32(sqnano_crc32(0, hdr, 12), data, db->page_size);
	put_u32(hdr + 12, crc);
	/* Image first, header last: a torn slot never validates. */
	err = db->media->api->write(db->media, off + SQNANO_JRNL_HDR, data, db->page_size);
	db->stat_writes++;
	if (!err) {
		err = db->media->api->write(db->media, off, hdr, sizeof(hdr));
		db->stat_writes++;
	}
	if (err) {
		return io_err(err);
	}
	/* The undo image must be durable before the page changes in place. */
	err = media_sync(db);
	if (err) {
		return err;
	}
	db->jmap[slot] = pgno;
	db->jcount++;
	bm_set(db->bm_jrnl, pgno);
	lvl_acquired(db, pgno);
	return SQNANO_OK;
}

int pg_write(struct sqnano_txn *t, uint32_t pgno, struct sqnano_frame **out, uint32_t *newpgno)
{
	struct sqnano_db *db = t->db;
	struct sqnano_frame *src;
	struct sqnano_frame *f;
	bool owned;
	int rc;

	if (!t->write) {
		return SQNANO_READONLY;
	}
	*newpgno = pgno;
	owned = bm_test(db->bm_alloc, pgno) || (t->inplace && bm_test(db->bm_jrnl, pgno));
	if (owned && erase_based(db) && bm_test(db->bm_dirty, pgno)) {
		/* Already programmed (evicted from the cache): copy it. */
		owned = false;
	}
	if (owned && !frozen(db, pgno)) {
		rc = pg_get(t, pgno, &f);
		if (rc) {
			return rc;
		}
		f->flags |= FR_DIRTY;
		t->gen++;
		*out = f;
		return SQNANO_OK;
	}
	rc = pg_get(t, pgno, &src);
	if (rc) {
		return rc;
	}
	if (t->inplace && !owned && !bm_test(db->bm_pinned, pgno) &&
	    db->jcount < db->jslots && db->jcount < SQNANO_JOURNAL_MAX_SLOTS) {
		rc = journal_page(db, pgno, src->data);
		if (rc) {
			pg_put(db, src);
			return rc;
		}
		rc = fr_alloc(db, &f);
		if (rc) {
			pg_put(db, src);
			return rc;
		}
		memcpy(f->data, src->data, db->page_size);
		pg_put(db, src);
		f->pgno = pgno;
		f->flags = FR_VALID | FR_PRIVATE | FR_DIRTY;
		fr_hold(db, f);
		t->gen++;
		*out = f;
		return SQNANO_OK;
	}
	/* Copy-on-write. */
	rc = pg_alloc(t, &f);
	if (rc) {
		pg_put(db, src);
		return rc;
	}
	memcpy(f->data, src->data, db->page_size);
	pg_put(db, src);
	rc = pg_free(t, pgno);
	if (rc) {
		pg_put(db, f);
		return rc;
	}
	*newpgno = f->pgno;
	*out = f;
	return SQNANO_OK;
}

/* Copy @p pgno to a new page even when the transaction owns it (compaction). */
int pg_move(struct sqnano_txn *t, uint32_t pgno, struct sqnano_frame **out, uint32_t *newpgno)
{
	struct sqnano_db *db = t->db;
	struct sqnano_frame *src, *f;
	int rc = pg_get(t, pgno, &src);

	if (rc) {
		return rc;
	}
	rc = pg_alloc(t, &f);
	if (rc) {
		pg_put(db, src);
		return rc;
	}
	memcpy(f->data, src->data, db->page_size);
	pg_put(db, src);
	rc = pg_free(t, pgno);
	if (rc) {
		pg_put(db, f);
		return rc;
	}
	*newpgno = f->pgno;
	*out = f;
	return SQNANO_OK;
}

/*
 * Flash, inside a write transaction, between row operations: when the
 * erased space runs low, move the live pages of the transaction out of erase
 * units that hold nothing else, so that the dead copies left by earlier
 * modifications can be erased. The committed state is not touched.
 */
int pg_compact_txn(struct sqnano_txn *t)
{
	struct sqnano_db *db = t->db;
	int rc = SQNANO_OK;

	if (!erase_based(db) || db->ppe == 1 || !t->write || t->budget < 0 ||
	    t->budget > 2 * (int32_t)db->ppe) {
		return SQNANO_OK;
	}
	for (int round = 0; round < 4 && rc == SQNANO_OK; round++) {
		uint32_t victim = 0, most = 0;
		uint32_t avail = avail_pages(db);
		uint32_t reserve = gc_reserve(db);

		t->budget = avail > reserve ? (int32_t)(avail - reserve) : 0;
		if (t->budget > 2 * (int32_t)db->ppe) {
			break;
		}
		for (uint32_t p = 1; p <= db->npages; p += db->ppe) {
			uint32_t dead = 0, live = 0;
			bool other = false;

			for (uint32_t q = p; q < p + db->ppe && !other; q++) {
				bool own = bm_test(db->bm_alloc, q);

				if (bm_test(db->bm_limbo, q) || bm_test(db->bm_pinned, q) ||
				    bm_test(db->bm_freed, q) || bm_test(db->bm_temp, q) ||
				    (bm_test(db->bm_used, q) && !own) ||
				    frozen(db, q)) {
					other = true;
				} else if (own) {
					live++;
				} else if (bm_test(db->bm_dirty, q)) {
					dead++;
				}
			}
			if (!other && dead > most && live < db->ppe) {
				most = dead;
				victim = p;
			}
		}
		if (!victim) {
			break;
		}
		db->gc_lo = victim;
		db->gc_hi = victim + db->ppe - 1;
		t->budget = -1;
		rc = bt_relocate(t, db->gc_lo, db->gc_hi);
		db->gc_lo = db->gc_hi = 0;
		db->stat_gc++;
		avail = avail_pages(db);
		t->budget = avail > reserve ? (int32_t)(avail - reserve) : 0;
	}
	return rc;
}

/* ---- Allocation state ---- */

void pg_mark_used(struct sqnano_db *db, uint32_t *bm, uint32_t pgno)
{
	(void)db;
	bm_set(bm, pgno);
}

static void reserve_bits(struct sqnano_db *db, uint32_t *bm)
{
	bm_set(bm, 0);
	for (uint32_t p = db->npages + 1; p < SQNANO_BM_WORDS * 32; p++) {
		bm_set(bm, p);
	}
}

struct walk_ctx {
	struct sqnano_db *db;
	uint32_t *bm;
	bool strict;
};

static int walk_mark(void *arg, uint32_t pgno)
{
	struct walk_ctx *w = arg;

	if (!valid_pgno(w->db, pgno)) {
		return SQNANO_CORRUPT;
	}
	if (w->strict && bm_test(w->bm, pgno)) {
		return SQNANO_CORRUPT;
	}
	bm_set(w->bm, pgno);
	return SQNANO_OK;
}

/* Mark every page reachable from the master tree @p master into @p bm. */
static int mark_state(struct sqnano_db *db, uint32_t master, uint32_t seq, uint32_t *bm,
		      bool strict)
{
	struct sqnano_txn t;
	struct sqnano_cursor c;
	struct walk_ctx w = {db, bm, strict};
	uint8_t val[4];
	uint16_t vlen;
	int rc;

	memset(&t, 0, sizeof(t));
	t.db = db;
	t.seq = seq;
	t.master_root = master;
	t.reader_slot = -1;
	rc = bt_walk(&t, master, walk_mark, &w);
	if (rc) {
		return rc;
	}
	rc = bt_open(&t, &c, 0);
	if (rc) {
		return rc;
	}
	rc = bt_seek(&c, NULL, 0, SQNANO_SEEK_FIRST, NULL);
	while (rc == SQNANO_OK && c.state == CUR_VALID) {
		if (c.klen != 2) {
			return SQNANO_CORRUPT;
		}
		if (c.key[0] & 0x80) {
			/* AUTOINCREMENT high-water mark, not a tree. */
			rc = bt_next(&c);
			continue;
		}
		rc = bt_value(&c, val, sizeof(val), &vlen);
		if (rc) {
			return rc;
		}
		if (vlen != 4) {
			return SQNANO_CORRUPT;
		}
		rc = bt_walk(&t, get_u32(val), walk_mark, &w);
		if (rc) {
			return rc;
		}
		rc = bt_next(&c);
	}
	return rc;
}

int pg_rebuild(struct sqnano_db *db)
{
	int rc;

	memset(db->bm_used, 0, sizeof(db->bm_used));
	memset(db->bm_alloc, 0, sizeof(db->bm_alloc));
	memset(db->bm_freed, 0, sizeof(db->bm_freed));
	memset(db->bm_limbo, 0, sizeof(db->bm_limbo));
	memset(db->bm_pinned, 0, sizeof(db->bm_pinned));
	memset(db->bm_jrnl, 0, sizeof(db->bm_jrnl));
	db->limbo_any = 0;
	db->jcount = 0;
	db->jtxn = 0;
	reserve_bits(db, db->bm_used);
	rc = mark_state(db, db->sb.master_root, db->sb.seq, db->bm_used, true);
	if (rc) {
		return rc;
	}
	if (db->sb.pinned_root) {
		rc = mark_state(db, db->sb.pinned_root, db->sb.seq, db->bm_pinned, false);
	}
	if (rc == SQNANO_OK && erase_based(db)) {
		rc = dirty_rebuild(db);
	}
	return rc;
}

/* ---- Mount ---- */

/*
 * Put back every undo image of the interrupted transaction, wherever the
 * rotation placed it. A slot left by a rolled-back transaction on the same
 * base holds the committed image too, so restoring it is harmless.
 */
static int journal_recover(struct sqnano_db *db)
{
	uint8_t hdr[SQNANO_JRNL_HDR];
	uint8_t *data = db->scratch;
	int rc;

	for (int slot = 0; slot < db->jslots; slot++) {
		uint32_t off = jslot_phys(db, slot);
		uint32_t pgno;

		rc = media_get(db, off, hdr, sizeof(hdr));
		if (rc) {
			return rc;
		}
		if (get_u16(hdr) != JR_MAGIC || get_u32(hdr + 4) != db->sb.seq + 1) {
			continue;
		}
		rc = media_get(db, off + SQNANO_JRNL_HDR, data, db->page_size);
		if (rc) {
			return rc;
		}
		if (get_u32(hdr + 12) !=
		    sqnano_crc32(sqnano_crc32(0, hdr, 12), data, db->page_size)) {
			continue;
		}
		pgno = get_u32(hdr + 8);
		if (!valid_pgno(db, pgno)) {
			return SQNANO_CORRUPT;
		}
		if (db->read_only) {
			return SQNANO_READONLY;
		}
		rc = media_put(db, page_off(db, pgno), data, db->page_size, db->page_size);
		if (rc) {
			return rc;
		}
	}
	return SQNANO_OK;
}

static int format(struct sqnano_db *db)
{
	struct sqnano_frame *f;
	struct sqnano_sb sb;
	uint8_t *p;
	int rc;

	if (db->read_only) {
		return SQNANO_CANTOPEN;
	}
	memset(&sb, 0, sizeof(sb));
	sb.page_size = db->page_size;
	sb.npages = db->npages;
	sb.master_root = 1;
	sb.seq = 1;
	if (erase_based(db)) {
		rc = media_erase(db, 0, db->sb_units * db->sb_unit);
		if (rc == SQNANO_OK) {
			rc = media_erase(db, db->data_off, db->esize);
		}
		if (rc) {
			return rc;
		}
		db->sb_sector = 0;
		db->sb_next = 0;
	} else {
		/* Records of an older database must not survive in the ring. */
		memset(db->scratch, 0, db->page_size);
		for (uint32_t off = 0; off < db->sb_units * db->sb_unit; off += db->page_size) {
			rc = media_put(db, off, db->scratch, db->page_size, db->page_size);
			if (rc) {
				return rc;
			}
		}
	}
	rc = fr_alloc(db, &f);
	if (rc) {
		return rc;
	}
	p = f->data;
	memset(p, 0, db->page_size);
	p[PG_TYPE] = PT_LEAF;
	put_u16(p + PG_CSTART, db->page_size);
	db->sb.seq = 0; /* page header seq = 1 */
	rc = write_page(db, 1, p);
	fr_drop(f);
	if (rc) {
		return rc;
	}
	rc = media_sync(db);
	if (rc == SQNANO_OK) {
		rc = sb_write(db, &sb);
	}
	if (rc == SQNANO_OK) {
		rc = media_sync(db);
	}
	if (rc) {
		return rc;
	}
	db->sb = sb;
	return SQNANO_OK;
}

int pg_mount(struct sqnano_db *db, const struct sqnano_db_config *cfg)
{
	struct sqnano_media *m = db->media;
	uint32_t e = m->caps.erase_size;
	struct sqnano_sb sb;
	bool ok;
	uint8_t mode;
	uint32_t ps;
	int rc;

	if (m->api->init) {
		rc = m->api->init(m);
		if (rc) {
			return SQNANO_CANTOPEN;
		}
	}
	e = m->caps.erase_size;
	db->read_only = cfg->read_only;
	if (cfg->mode == SQNANO_MODE_AUTO) {
		db->mode = e || m->caps.prefer_shadow ? SQNANO_MODE_SHADOW : SQNANO_MODE_JOURNAL;
	} else {
		db->mode = (uint8_t)cfg->mode;
	}
	if (db->mode == SQNANO_MODE_JOURNAL && e) {
		/* Erase-based media cannot be updated in place. */
		return SQNANO_CANTOPEN;
	}
	ps = cfg->page_size;
	if (!ps) {
		ps = db->mode == SQNANO_MODE_SHADOW && e ? e : 256;
		if (ps > SQNANO_MAX_PAGE_SIZE) {
			ps = SQNANO_MAX_PAGE_SIZE;
		}
		if (ps < 256) {
			ps = 256;
		}
		if (m->caps.page_hint) {
			ps = m->caps.page_hint;
		}
	}
	if (ps < SQNANO_MIN_PAGE_SIZE || ps > SQNANO_MAX_PAGE_SIZE ||
	    (e && ps % e && e % ps) ||
	    (m->caps.write_granularity && ps % m->caps.write_granularity)) {
		return SQNANO_CANTOPEN;
	}
	db->page_size = (uint16_t)ps;
	db->jslots = 0;
	db->jstart = 0;
	/* The superblock ring size is recorded in its records. */
	db->sb_unit = e ? e : ps;
	rc = sb_scan(db, 0, &sb, &mode, &ok);
	if (rc) {
		return rc;
	}
	if (!ok) {
		uint32_t units = m->size / db->sb_unit;

		/* About 1/16 of a flash, 1/32 of a copy-on-write EEPROM. */
		units = cfg->sb_units          ? cfg->sb_units
			: e                    ? units / 16
			: db->mode == SQNANO_MODE_SHADOW ? units / 32
						 : 2;
		units = units < 2 ? 2 : units > 8 && !cfg->sb_units ? 8 : units;
		db->sb_units = (uint8_t)(units > SB_UNITS_MAX ? 0 : units);
	}
	if (db->sb_units < 2 || db->sb_units > SB_UNITS_MAX) {
		return SQNANO_CANTOPEN;
	}
	if (e) {
		/* Pages grouped by erase unit after the superblock log. */
		db->esize = e > ps ? e : ps;
		db->ppe = (uint16_t)(db->esize / ps);
		db->jrnl_off = db->sb_units * e;
		db->data_off = db->jrnl_off;
		if (m->size < db->data_off + 2 * db->esize) {
			return SQNANO_CANTOPEN;
		}
		db->npages = (m->size - db->data_off) / db->esize * db->ppe;
		if (db->npages > SQNANO_MAX_PAGES) {
			db->npages = SQNANO_MAX_PAGES / db->ppe * db->ppe;
		}
	} else {
		db->esize = 0;
		db->ppe = 1;
		db->jrnl_off = db->sb_units * ps;
		if (db->mode == SQNANO_MODE_JOURNAL) {
			db->jslots = cfg->journal_slots ? cfg->journal_slots : 16;
			if (db->jslots > SQNANO_JOURNAL_MAX_SLOTS) {
				db->jslots = SQNANO_JOURNAL_MAX_SLOTS;
			}
		}
		db->data_off = db->jrnl_off + (uint32_t)db->jslots * (ps + SQNANO_JRNL_HDR);
		if (m->size < db->data_off + 4 * ps) {
			return SQNANO_CANTOPEN;
		}
		db->npages = (m->size - db->data_off) / ps;
		if (db->npages > SQNANO_MAX_PAGES) {
			db->npages = SQNANO_MAX_PAGES;
		}
	}
	if (db->npages < 4) {
		return SQNANO_CANTOPEN;
	}
	for (int i = 0; i < SQNANO_PAGE_CACHE_COUNT; i++) {
		db->frames[i].data = db->cache[i];
		db->frames[i].flags = 0;
		db->frames[i].refs = 0;
	}

	if (ok) {
		rc = sb_scan(db, db->sb_units, &sb, &mode, &ok);
		if (rc) {
			return rc;
		}
	}
	if (ok && (sb.page_size != ps || sb.format != SQNANO_FORMAT_VERSION ||
		   mode != db->mode || sb.npages != db->npages)) {
		return SQNANO_CANTOPEN;
	}
	if (!ok) {
		if (!cfg->create) {
			return SQNANO_CANTOPEN;
		}
		rc = format(db);
		if (rc) {
			return rc;
		}
	} else {
		db->sb = sb;
	}
	if (db->mode == SQNANO_MODE_JOURNAL) {
		rc = journal_recover(db);
		if (rc == SQNANO_OK) {
			rc = media_sync(db);
		}
		if (rc) {
			return rc;
		}
	}
	db->alloc_hint = 1;
	db->alloc_lap = 1;
	db->alloc_rng = db->sb.seq * 2654435761u | 1u;
	memset(db->bm_temp, 0, sizeof(db->bm_temp));
	rc = pg_rebuild(db);
	for (uint32_t p = 1; rc == SQNANO_OK && m->api->discard && p <= db->npages; p++) {
		/* Free pages kept by the media from before the restart. */
		if (!bm_test(db->bm_used, p) && !bm_test(db->bm_pinned, p)) {
			media_discard(db, p);
		}
	}
	return rc;
}

/* ---- Temporary pages ---- */

int pg_temp_alloc(struct sqnano_db *db, uint32_t *pgno)
{
	int rc;

	if (db->read_only) {
		return SQNANO_READONLY;
	}
	rc = find_free(db, pgno);
	if (rc == SQNANO_OK) {
		bm_set(db->bm_temp, *pgno);
	}
	return rc;
}

int pg_temp_write(struct sqnano_db *db, uint32_t pgno, uint8_t *data)
{
	data[PG_TYPE] = PT_TEMP;
	return write_page(db, pgno, data);
}

int pg_temp_read(struct sqnano_db *db, uint32_t pgno, uint8_t *data)
{
	return read_page(db, pgno, data);
}

void pg_temp_free(struct sqnano_db *db, uint32_t pgno)
{
	bm_clr(db->bm_temp, pgno);
	media_discard(db, pgno);
}

/* ---- Transactions ---- */

static void writer_init(struct sqnano_db *db, struct sqnano_txn *t, uint8_t flags);

/*
 * Flash compaction, run by a writer before its transaction: move the live
 * pages out of the erase unit with the fewest of them, in a transaction of
 * its own, so that the unit can be erased. Returns the pages then available.
 */
/*
 * Compact until half of the free space can be allocated directly: a
 * transaction cannot compact, it gets what is erased when it starts.
 */
static uint32_t gc_target(const struct sqnano_db *db)
{
	uint32_t used = 0;
	uint32_t half;

	for (uint32_t w = 0; w < SQNANO_BM_WORDS; w++) {
		uint32_t v = db->bm_used[w];

		while (v) {
			v &= v - 1;
			used++;
		}
	}
	used -= SQNANO_BM_WORDS * 32 - db->npages; /* reserved bits */
	half = (db->npages - used) / 2;
	return gc_reserve(db) + (half > 2u * db->ppe ? half : 2u * db->ppe);
}

static int pg_gc(struct sqnano_db *db, uint32_t *avail, uint32_t target)
{
	uint32_t rounds = target == UINT32_MAX ? db->npages / db->ppe : 8;

	for (uint32_t round = 0; round < rounds && *avail < target; round++) {
		struct sqnano_txn g;
		uint32_t victim = 0, best = UINT32_MAX;
		uint32_t before = *avail;
		int rc;

		for (uint32_t p = 1; p <= db->npages; p += db->ppe) {
			uint32_t live = 0, spare = 0;
			bool stuck = false;

			for (uint32_t q = p; q < p + db->ppe; q++) {
				if (bm_test(db->bm_limbo, q) || bm_test(db->bm_pinned, q) ||
				    bm_test(db->bm_freed, q) || bm_test(db->bm_temp, q)) {
					stuck = true;
				} else if (bm_test(db->bm_used, q)) {
					live++;
				} else if (bm_test(db->bm_dirty, q)) {
					spare++;
				}
			}
			/* Only units with dead pages to win back, and no snapshot inside. */
			if (!stuck && live && spare && live < best) {
				best = live;
				victim = p;
			}
		}
		if (!victim) {
			break;
		}
		writer_init(db, &g, SQNANO_TXN_COW);
		db->gc_lo = victim;
		db->gc_hi = victim + db->ppe - 1;
		rc = bt_relocate(&g, db->gc_lo, db->gc_hi);
		db->gc_lo = db->gc_hi = 0;
		if (rc == SQNANO_OK) {
			rc = pg_commit(&g);
		} else {
			(void)pg_rollback(&g);
		}
		db->writer_active = 1;
		if (rc) {
			return rc;
		}
		db->stat_gc++;
		*avail = avail_pages(db);
		if (*avail <= before) {
			break;
		}
	}
	return SQNANO_OK;
}

int pg_begin(struct sqnano_db *db, struct sqnano_txn *t, uint8_t flags, int32_t timeout_ms)
{
	memset(t, 0, sizeof(*t));
	t->db = db;
	t->reader_slot = -1;
	if (db->broken) {
		return SQNANO_IOERR;
	}
	if (!(flags & SQNANO_TXN_WRITE)) {
		for (int i = 0; i < SQNANO_MAX_READERS; i++) {
			if (!db->reader_used[i]) {
				db->reader_used[i] = 1;
				db->readers[i] = db->sb.seq;
				t->reader_slot = (int8_t)i;
				t->seq = db->sb.seq;
				t->master_root = db->sb.master_root;
				t->sb = db->sb;
				t->active = 1;
				return SQNANO_OK;
			}
		}
		return SQNANO_BUSY;
	}
	if (db->read_only) {
		return SQNANO_READONLY;
	}
	if (db->writer_active) {
		int64_t deadline = sqnano_uptime_ms() + (timeout_ms < 0 ? 0 : timeout_ms);

		while (db->writer_active) {
			int32_t left = (int32_t)(deadline - sqnano_uptime_ms());

			if (timeout_ms >= 0 && left <= 0) {
				return SQNANO_BUSY;
			}
			(void)sqnano_cond_wait_ms(&db->cond, &db->lock, timeout_ms < 0 ? -1 : left);
		}
		if (db->broken) {
			return SQNANO_IOERR;
		}
	}
	db->writer_active = 1;
	writer_init(db, t, flags);
	t->budget = -1;
	if (erase_based(db) && db->ppe > 1) {
		uint32_t avail = avail_pages(db);
		uint32_t reserve = gc_reserve(db);
		uint32_t target = gc_target(db);

		if (avail < target) {
			int rc = pg_gc(db, &avail, target);

			writer_init(db, t, flags);
			if (rc) {
				db->writer_active = 0;
				sqnano_cond_broadcast(&db->cond);
				return rc;
			}
		}
		t->budget = avail > reserve ? (int32_t)(avail - reserve) : 0;
	}
	return SQNANO_OK;
}

static void writer_init(struct sqnano_db *db, struct sqnano_txn *t, uint8_t flags)
{
	memset(t, 0, sizeof(*t));
	t->db = db;
	t->reader_slot = -1;
	db->writer_active = 1;
	t->write = 1;
	t->active = 1;
	t->seq = db->sb.seq;
	t->master_root = db->sb.master_root;
	t->sb = db->sb;
	t->budget = -1;
	t->cow = (flags & SQNANO_TXN_COW) || db->mode == SQNANO_MODE_SHADOW;
	if (!t->cow) {
		bool old_reader = false;

		for (int i = 0; i < SQNANO_MAX_READERS; i++) {
			if (db->reader_used[i] && db->readers[i] < db->sb.seq) {
				old_reader = true;
			}
		}
		if (old_reader) {
			/* An old snapshot still needs the journal: copy-on-write instead. */
			t->cow = 1;
		} else {
			t->inplace = 1;
			/* Start one slot further than the previous transaction (wear). */
			db->jstart = (uint16_t)((db->jstart + 1u) % db->jslots);
			db->jtxn = db->sb.seq + 1;
			db->jcount = 0;
			memset(db->bm_jrnl, 0, sizeof(db->bm_jrnl));
		}
	}
}

static void end_reader(struct sqnano_txn *t)
{
	struct sqnano_db *db = t->db;

	if (t->reader_slot >= 0) {
		db->reader_used[t->reader_slot] = 0;
		t->reader_slot = -1;
	}
	t->active = 0;
	release_limbo(db);
}

static void end_writer(struct sqnano_txn *t)
{
	struct sqnano_db *db = t->db;

	memset(db->bm_alloc, 0, sizeof(db->bm_alloc));
	memset(db->bm_freed, 0, sizeof(db->bm_freed));
	db->writer_active = 0;
	t->active = 0;
	release_limbo(db);
	sqnano_cond_broadcast(&db->cond);
}

int pg_rollback(struct sqnano_txn *t)
{
	struct sqnano_db *db = t->db;
	int rc = SQNANO_OK;

	if (!t->active) {
		return SQNANO_OK;
	}
	if (!t->write) {
		end_reader(t);
		return SQNANO_OK;
	}
	for (int i = 0; i < SQNANO_PAGE_CACHE_COUNT; i++) {
		if (db->frames[i].flags & FR_PRIVATE) {
			fr_drop(&db->frames[i]);
		}
	}
	if (t->inplace) {
		/* Put the undo images back. */
		for (int i = 0; i < db->jcount && rc == SQNANO_OK; i++) {
			rc = jread(db, i, db->scratch);
			if (rc == SQNANO_OK) {
				rc = media_put(db, page_off(db, db->jmap[i]), db->scratch,
					       db->page_size, db->page_size);
			}
		}
		if (rc) {
			db->broken = 1;
		}
		db->jtxn = 0;
		db->jcount = 0;
		memset(db->bm_jrnl, 0, sizeof(db->bm_jrnl));
	}
	for (uint32_t w = 0; w < SQNANO_BM_WORDS; w++) {
		db->bm_used[w] = (db->bm_used[w] | db->bm_freed[w]) & ~db->bm_alloc[w];
	}
#if SQNANO_MAX_SAVEPOINTS > 0
	db->sp_depth = 0;
#endif
	end_writer(t);
	return rc;
}

int pg_commit(struct sqnano_txn *t)
{
	struct sqnano_db *db = t->db;
	struct sqnano_sb sb;
	bool freed = false;
	int rc;

	if (!t->active) {
		return SQNANO_MISUSE;
	}
	if (!t->write) {
		end_reader(t);
		return SQNANO_OK;
	}
	if (db->broken) {
		pg_rollback(t);
		return SQNANO_IOERR;
	}
	if (t->poisoned) {
		pg_rollback(t);
		return SQNANO_ABORT;
	}
	if (t->fk_deferred > 0 && db->fk_on) {
		/* Deferred foreign key violations: the transaction stays open. */
		return SQNANO_CONSTRAINT_FOREIGNKEY;
	}
	sp_merge(db, 0);
	sb = t->sb;
	sb.master_root = t->master_root;
	sb.seq = db->sb.seq;
	if (t->gen == 0 && memcmp(&sb, &db->sb, sizeof(sb)) == 0) {
		end_writer(t);
		return SQNANO_OK;
	}
	for (int i = 0; i < SQNANO_PAGE_CACHE_COUNT; i++) {
		struct sqnano_frame *f = &db->frames[i];

		if ((f->flags & FR_PRIVATE) && (f->flags & FR_DIRTY)) {
			rc = fr_flush(db, f);
			if (rc) {
				pg_rollback(t);
				return rc;
			}
		}
	}
	sb.seq = db->sb.seq + 1;
	/* The pages before their superblock, the superblock before returning. */
	rc = media_sync(db);
	if (rc) {
		pg_rollback(t);
		return rc;
	}
	rc = sb_write(db, &sb);
	if (rc == SQNANO_OK) {
		rc = media_sync(db);
	}
	if (rc) {
		/* The superblock may or may not have reached the media. */
		db->broken = 1;
		pg_rollback(t);
		return rc;
	}
	db->sb = sb;
	for (int i = 0; i < SQNANO_PAGE_CACHE_COUNT; i++) {
		struct sqnano_frame *f = &db->frames[i];

		if (f->flags & FR_PRIVATE) {
			struct sqnano_frame *old = fr_find(db, f->pgno, false);

			if (old) {
				fr_drop(old);
			}
			f->flags &= (uint8_t)~FR_PRIVATE;
		}
	}
	for (uint32_t w = 0; w < SQNANO_BM_WORDS; w++) {
		if (db->bm_freed[w]) {
			db->bm_limbo[w] |= db->bm_freed[w];
			freed = true;
		}
	}
	if (freed) {
		db->limbo_any = 1;
		db->limbo_seq = sb.seq;
	}
	if (sb.pinned_root == 0) {
		memset(db->bm_pinned, 0, sizeof(db->bm_pinned));
	}
	end_writer(t);
	return SQNANO_OK;
}

#if SQNANO_MAX_SAVEPOINTS > 0
int pg_savepoint(struct sqnano_txn *t, uint32_t *id)
{
	struct sqnano_db *db = t->db;
	struct sqnano_savepoint *sp;

	if (!t->active || !t->write) {
		return SQNANO_MISUSE;
	}
	if (db->sp_depth >= SQNANO_MAX_SAVEPOINTS) {
		return SQNANO_FULL;
	}
	if (db->sp_depth == 0) {
		memset(db->sp_lvl, 0, sizeof(db->sp_lvl));
	}
	sp = &db->sp[db->sp_depth++];
	if (++db->sp_serial == 0) {
		db->sp_serial = 1;
	}
	sp->serial = db->sp_serial;
	sp->master_root = t->master_root;
	sp->sb = t->sb;
	sp->fk_deferred = t->fk_deferred;
	sp->jcount = db->jcount;
	sp->dead = 0;
	*id = sp->serial;
	return SQNANO_OK;
}

int pg_release(struct sqnano_txn *t, uint32_t id, bool only)
{
	struct sqnano_db *db = t->db;
	int i = sp_find(db, id);
	uint8_t d;

	if (i < 0) {
		return SQNANO_NOTFOUND;
	}
	if (only && i != db->sp_depth - 1) {
		/* Savepoints opened later still need it: drop it when they go. */
		db->sp[i].dead = 1;
		return SQNANO_OK;
	}
	d = (uint8_t)i;
	while (d && db->sp[d - 1].dead) {
		d--;
	}
	sp_merge(db, d);
	return SQNANO_OK;
}

static int sp_drop_private(struct sqnano_db *db, uint32_t pgno)
{
	struct sqnano_frame *f = fr_find(db, pgno, true);

	if (f) {
		if (f->refs) {
			return SQNANO_INTERNAL;
		}
		fr_drop(f);
	}
	return SQNANO_OK;
}

int pg_rollback_to(struct sqnano_txn *t, uint32_t id)
{
	struct sqnano_db *db = t->db;
	int i = sp_find(db, id);
	int rc = SQNANO_OK;

	if (i < 0 || !t->active || !t->write) {
		return SQNANO_MISUSE;
	}
	/* Journaled pages: put the committed image back in place. */
	while (db->jcount > db->sp[i].jcount && rc == SQNANO_OK) {
		uint32_t p = db->jmap[db->jcount - 1];

		rc = sp_drop_private(db, p);
		if (rc == SQNANO_OK) {
			rc = jread(db, db->jcount - 1, db->scratch);
		}
		if (rc == SQNANO_OK) {
			rc = media_put(db, page_off(db, p), db->scratch, db->page_size,
				       db->page_size);
		}
		if (rc == SQNANO_OK) {
			if (bm_test(db->bm_freed, p)) {
				bm_clr(db->bm_freed, p);
				bm_set(db->bm_used, p);
			}
			bm_clr(db->bm_jrnl, p);
			db->sp_lvl[p] = 0;
			db->jcount--;
		}
	}
	for (uint32_t p = 1; p <= db->npages && rc == SQNANO_OK; p++) {
		uint8_t v = db->sp_lvl[p];

		if (bm_test(db->bm_alloc, p) && LVL_ACQ(v) > (unsigned int)i) {
			/* Allocated after the savepoint. */
			rc = sp_drop_private(db, p);
			bm_clr(db->bm_alloc, p);
			bm_clr(db->bm_used, p);
			bm_clr(db->bm_freed, p);
			db->sp_lvl[p] = 0;
		} else if (bm_test(db->bm_freed, p) && LVL_FREE(v) > (unsigned int)i) {
			bm_clr(db->bm_freed, p);
			bm_set(db->bm_used, p);
			db->sp_lvl[p] = LVL_ACQ(v);
		}
	}
	if (rc) {
		/* Half undone: only a full rollback is safe now. */
		t->poisoned = 1;
		return rc;
	}
	t->master_root = db->sp[i].master_root;
	t->sb = db->sp[i].sb;
	t->fk_deferred = db->sp[i].fk_deferred;
	t->gen++;
	db->sp_depth = (uint8_t)(i + 1);
	db->sp[i].dead = 0;
	return SQNANO_OK;
}

int sqnano_txn_savepoint(struct sqnano_txn *t, uint32_t *id)
{
	int rc;

	if (!t->active) {
		return SQNANO_MISUSE;
	}
	db_lock(t->db);
	rc = pg_savepoint(t, id);
	db_unlock(t->db);
	return rc;
}

int sqnano_txn_release(struct sqnano_txn *t, uint32_t id)
{
	int rc;

	if (!t->active) {
		return SQNANO_MISUSE;
	}
	db_lock(t->db);
	rc = pg_release(t, id, false);
	db_unlock(t->db);
	return rc;
}

int sqnano_txn_rollback_to(struct sqnano_txn *t, uint32_t id)
{
	int rc;

	if (!t->active) {
		return SQNANO_MISUSE;
	}
	db_lock(t->db);
	rc = pg_rollback_to(t, id);
	db_unlock(t->db);
	return rc;
}
#else
int sqnano_txn_savepoint(struct sqnano_txn *t, uint32_t *id)
{
	(void)t;
	(void)id;
	return SQNANO_FULL;
}

int sqnano_txn_release(struct sqnano_txn *t, uint32_t id)
{
	(void)t;
	(void)id;
	return SQNANO_NOTFOUND;
}

int sqnano_txn_rollback_to(struct sqnano_txn *t, uint32_t id)
{
	(void)t;
	(void)id;
	return SQNANO_MISUSE;
}
#endif

int sqnano_db_compact(struct sqnano_db *db, int32_t timeout_ms)
{
	struct sqnano_txn t;
	uint32_t avail;
	int rc;

	if (!db || !db->is_open) {
		return SQNANO_MISUSE;
	}
	if (!erase_based(db) || db->ppe == 1) {
		return SQNANO_OK;
	}
	db_lock(db);
	rc = pg_begin(db, &t, SQNANO_TXN_WRITE, timeout_ms);
	if (rc == SQNANO_OK) {
		avail = avail_pages(db);
		rc = pg_gc(db, &avail, UINT32_MAX);
		(void)pg_rollback(&t);
	}
	db_unlock(db);
	return rc;
}

/* ---- Public transaction API ---- */

int sqnano_txn_begin(struct sqnano_db *db, struct sqnano_txn *t, uint8_t flags, int32_t timeout_ms)
{
	int rc;

	if (!db || !db->is_open) {
		return SQNANO_MISUSE;
	}
	db_lock(db);
	rc = pg_begin(db, t, flags, timeout_ms);
	db_unlock(db);
	return rc;
}

int sqnano_txn_commit(struct sqnano_txn *t)
{
	int rc;

	if (!t->active) {
		return SQNANO_MISUSE;
	}
	db_lock(t->db);
	rc = pg_commit(t);
	db_unlock(t->db);
	return rc;
}

int sqnano_txn_rollback(struct sqnano_txn *t)
{
	int rc;

	if (!t->active) {
		return SQNANO_OK;
	}
	db_lock(t->db);
	rc = pg_rollback(t);
	db_unlock(t->db);
	return rc;
}
