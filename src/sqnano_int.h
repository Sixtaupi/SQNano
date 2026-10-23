/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Private contracts between the engine layers.
 *
 * Functions prefixed with pg_ (pager) and bt_ (B+tree) expect the database
 * lock to be held by the caller.
 */

#ifndef SQNANO_INT_H_
#define SQNANO_INT_H_

#include <string.h>

#include <sqnano/exec.h>
#include <sqnano/sqnano.h>

/* Byte order helpers: the on-media format is little-endian except keys. */
static inline uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

uint32_t sqnano_crc32(uint32_t crc, const void *buf, size_t len);

/* Unsigned LEB128 varints (lengths and serial types). */
int varint_put(uint8_t *p, uint64_t v);
int varint_get(const uint8_t *p, const uint8_t *end, uint64_t *v);
int varint_len(uint64_t v);

/* Bitmap helpers. */
static inline bool bm_test(const uint32_t *bm, uint32_t i)
{
	return (bm[i >> 5] >> (i & 31)) & 1u;
}

static inline void bm_set(uint32_t *bm, uint32_t i)
{
	bm[i >> 5] |= 1u << (i & 31);
}

static inline void bm_clr(uint32_t *bm, uint32_t i)
{
	bm[i >> 5] &= ~(1u << (i & 31));
}

/* Page layout. */
#define PG_CRC     0
#define PG_TYPE    4
#define PG_FLAGS   5
#define PG_NCELLS  6
#define PG_CSTART  8
#define PG_SEQ     12
#define PG_RIGHT   16
#define PT_LEAF     1
#define PT_INTERIOR 2
#define PT_OVERFLOW 3
#define PT_TEMP     4

/* Frame flags. */
#define FR_VALID   0x01
#define FR_PRIVATE 0x02
#define FR_DIRTY   0x04
#define FR_TEMP    0x08

/* Pager (src/pager/pager.c). */
int pg_mount(struct sqnano_db *db, const struct sqnano_db_config *cfg);
int pg_get(struct sqnano_txn *t, uint32_t pgno, struct sqnano_frame **out);
void pg_put(struct sqnano_db *db, struct sqnano_frame *f);
int pg_write(struct sqnano_txn *t, uint32_t pgno, struct sqnano_frame **out, uint32_t *newpgno);
int pg_alloc(struct sqnano_txn *t, struct sqnano_frame **out);
int pg_free(struct sqnano_txn *t, uint32_t pgno);
int pg_move(struct sqnano_txn *t, uint32_t pgno, struct sqnano_frame **out, uint32_t *newpgno);
int pg_compact_txn(struct sqnano_txn *t);

/*
 * Temporary pages (external sort): taken from the free pages, never
 * reachable from a superblock, each written once. Lock held.
 */
int pg_temp_alloc(struct sqnano_db *db, uint32_t *pgno);
int pg_temp_write(struct sqnano_db *db, uint32_t pgno, uint8_t *data);
int pg_temp_read(struct sqnano_db *db, uint32_t pgno, uint8_t *data);
void pg_temp_free(struct sqnano_db *db, uint32_t pgno);
int pg_begin(struct sqnano_db *db, struct sqnano_txn *t, uint8_t flags, int32_t timeout_ms);
int pg_commit(struct sqnano_txn *t);
int pg_rollback(struct sqnano_txn *t);
int pg_rebuild(struct sqnano_db *db);
#if SQNANO_MAX_SAVEPOINTS > 0
int pg_savepoint(struct sqnano_txn *t, uint32_t *id);
/** Release @p id and the savepoints opened after it, or only @p id. */
int pg_release(struct sqnano_txn *t, uint32_t id, bool only);
int pg_rollback_to(struct sqnano_txn *t, uint32_t id);
#endif
uint16_t pg_usable(const struct sqnano_db *db);
void pg_mark_used(struct sqnano_db *db, uint32_t *bm, uint32_t pgno);

/* B+tree (src/btree/btree.c). */
uint16_t bt_max_cell(const struct sqnano_db *db);
uint16_t bt_max_key(const struct sqnano_db *db);
int bt_new_root(struct sqnano_txn *t, uint32_t *root);
int bt_tree_root(struct sqnano_txn *t, uint16_t tree, uint32_t *root);
int bt_tree_create(struct sqnano_txn *t, uint16_t tree);
int bt_tree_drop(struct sqnano_txn *t, uint16_t tree);
int bt_open(struct sqnano_txn *t, struct sqnano_cursor *c, uint16_t tree);
int bt_seek(struct sqnano_cursor *c, const uint8_t *key, uint16_t klen, enum sqnano_seek op,
	    bool *exact);
int bt_seek_prefix_end(struct sqnano_cursor *c, const uint8_t *prefix, uint16_t plen);
int bt_next(struct sqnano_cursor *c);
int bt_prev(struct sqnano_cursor *c);
int bt_value(struct sqnano_cursor *c, uint8_t *buf, uint16_t cap, uint16_t *len);
int bt_insert(struct sqnano_cursor *c, const uint8_t *key, uint16_t klen, const uint8_t *val,
	      uint16_t vlen);
int bt_delete(struct sqnano_cursor *c);
/** Visit every page of the tree rooted at @p root. */
int bt_walk(struct sqnano_txn *t, uint32_t root, int (*cb)(void *arg, uint32_t pgno),
	    void *arg);
int bt_check(struct sqnano_txn *t, uint32_t root, uint32_t *seen);
/** Copy every page in [lo, hi] (and its ancestors) elsewhere, in all trees. */
int bt_relocate(struct sqnano_txn *t, uint32_t lo, uint32_t hi);

#define CUR_INVALID 0
#define CUR_VALID   1
#define CUR_EOF     2

/* Database layer (src/core/db.c). */
static inline void db_lock(struct sqnano_db *db)
{
	sqnano_mutex_lock(&db->lock);
}

static inline void db_unlock(struct sqnano_db *db)
{
	sqnano_mutex_unlock(&db->lock);
}

/* Value helpers (src/core/value.c). */
int num_parse(const char *z, uint32_t n, sqnano_value *out, bool *whole);
int text_compare(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn, uint8_t coll);

#endif /* SQNANO_INT_H_ */
