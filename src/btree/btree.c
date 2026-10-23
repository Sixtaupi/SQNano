/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * B+tree with byte-comparable keys.
 *
 * Page layout:
 *   [0..20)   header: crc, type, flags, ncells, content start, seq, right child
 *   [20..)    cell pointer array (u16 offsets), growing upwards
 *   [..ps)    cell contents, growing downwards
 *
 * Leaf cell:     varint klen, varint (vlen << 1 | big), [varint local], key,
 *                value (local bytes when big), [u32 first overflow page]
 * Interior cell: u32 child, varint klen, key
 *
 * A value that does not fit in a cell keeps its first bytes in the cell and
 * the rest in a chain of overflow pages (header, then data; PG_RIGHT links
 * the next page). Overflow pages are written once and never modified, so
 * old snapshots share them like any other page.
 *
 * Interior cell i points to keys lower than key_i; the right child holds the
 * keys greater than or equal to the last key. Modifications copy every page
 * on the path that is not yet private to the transaction (shadow paging);
 * the pager decides whether a page is copied or journaled in place.
 *
 * Empty leaves are removed from their parent. A leaf left less than a
 * quarter full by a delete absorbs a sibling of the same parent when both
 * fit in three quarters of a page; interior pages are not merged.
 */

#include "../sqnano_int.h"

struct cell {
	const uint8_t *key;
	const uint8_t *val;
	uint32_t child;
	/* First overflow page, or 0. */
	uint32_t ovfl;
	uint16_t klen;
	/* Total value length; the cell holds the first @c local bytes. */
	uint16_t vlen;
	uint16_t local;
	uint16_t size;
};

static inline uint16_t pg_n(const uint8_t *p)
{
	return get_u16(p + PG_NCELLS);
}

static inline uint16_t cell_ptr(const uint8_t *p, int i)
{
	return get_u16(p + SQNANO_PAGE_HDR + 2 * i);
}

uint16_t bt_max_cell(const struct sqnano_db *db)
{
	return (uint16_t)((pg_usable(db) - 6) / 3);
}

uint16_t bt_max_key(const struct sqnano_db *db)
{
	uint16_t k = (uint16_t)(bt_max_cell(db) - 8);

	return k < SQNANO_MAX_KEY_SIZE ? k : SQNANO_MAX_KEY_SIZE;
}

static int cell_parse(const uint8_t *pg, uint16_t ps, uint16_t off, struct cell *c)
{
	const uint8_t *p = pg + off;
	const uint8_t *end = pg + ps;
	uint64_t kl, vl = 0, local;
	bool big = false;
	int n;

	if (off < SQNANO_PAGE_HDR || off >= ps) {
		return SQNANO_CORRUPT;
	}
	if (pg[PG_TYPE] == PT_INTERIOR) {
		if (p + 4 > end) {
			return SQNANO_CORRUPT;
		}
		c->child = get_u32(p);
		p += 4;
	} else {
		c->child = 0;
	}
	n = varint_get(p, end, &kl);
	if (n < 0) {
		return SQNANO_CORRUPT;
	}
	p += n;
	if (pg[PG_TYPE] == PT_LEAF) {
		n = varint_get(p, end, &vl);
		if (n < 0) {
			return SQNANO_CORRUPT;
		}
		p += n;
		big = vl & 1;
		vl >>= 1;
	}
	local = vl;
	if (big) {
		n = varint_get(p, end, &local);
		if (n < 0 || local >= vl) {
			return SQNANO_CORRUPT;
		}
		p += n;
	}
	if (vl > 0xffff || kl > (uint64_t)(end - p) ||
	    local + (big ? 4 : 0) > (uint64_t)(end - p) - kl) {
		return SQNANO_CORRUPT;
	}
	c->key = p;
	c->klen = (uint16_t)kl;
	c->val = p + kl;
	c->vlen = (uint16_t)vl;
	c->local = (uint16_t)local;
	c->ovfl = big ? get_u32(p + kl + local) : 0;
	c->size = (uint16_t)(p + kl + local + (big ? 4 : 0) - (pg + off));
	if (big && c->ovfl == 0) {
		return SQNANO_CORRUPT;
	}
	return SQNANO_OK;
}

static int cell_at(const uint8_t *pg, uint16_t ps, int i, struct cell *c)
{
	return cell_parse(pg, ps, cell_ptr(pg, i), c);
}

static int keycmp(const uint8_t *a, uint16_t al, const uint8_t *b, uint16_t bl)
{
	int r = memcmp(a, b, al < bl ? al : bl);

	if (r) {
		return r;
	}
	return al < bl ? -1 : (al > bl ? 1 : 0);
}

static int node_check(const uint8_t *pg, uint16_t ps)
{
	uint16_t n = pg_n(pg);

	if ((pg[PG_TYPE] != PT_LEAF && pg[PG_TYPE] != PT_INTERIOR) ||
	    SQNANO_PAGE_HDR + 2u * n > get_u16(pg + PG_CSTART) || get_u16(pg + PG_CSTART) > ps) {
		return SQNANO_CORRUPT;
	}
	return SQNANO_OK;
}

static uint32_t child_at(const uint8_t *pg, uint16_t ps, int i)
{
	struct cell c;

	if (i >= pg_n(pg)) {
		return get_u32(pg + PG_RIGHT);
	}
	if (cell_at(pg, ps, i, &c)) {
		return 0;
	}
	return c.child;
}

static void set_child(uint8_t *pg, int i, uint32_t child)
{
	if (i >= pg_n(pg)) {
		put_u32(pg + PG_RIGHT, child);
	} else {
		put_u32(pg + cell_ptr(pg, i), child);
	}
}

/* Leaf: first index with key >= k. Interior: first index with k < key. */
static int node_search(const uint8_t *pg, uint16_t ps, const uint8_t *k, uint16_t kl, bool *exact)
{
	int lo = 0, hi = pg_n(pg);
	bool leaf = pg[PG_TYPE] == PT_LEAF;

	if (exact) {
		*exact = false;
	}
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		struct cell c;
		int r;

		if (cell_at(pg, ps, mid, &c)) {
			return -1;
		}
		r = keycmp(c.key, c.klen, k, kl);
		if (leaf) {
			if (r < 0) {
				lo = mid + 1;
			} else {
				if (r == 0 && exact) {
					*exact = true;
				}
				hi = mid;
			}
		} else {
			if (r <= 0) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
	}
	return lo;
}

static uint16_t node_used(const uint8_t *pg, uint16_t ps)
{
	uint16_t n = pg_n(pg);
	uint32_t used = 0;

	for (int i = 0; i < n; i++) {
		struct cell c;

		if (cell_at(pg, ps, i, &c) == SQNANO_OK) {
			used += c.size;
		}
	}
	return (uint16_t)used;
}

static void node_init(uint8_t *pg, uint16_t ps, uint8_t type)
{
	memset(pg, 0, SQNANO_PAGE_HDR);
	pg[PG_TYPE] = type;
	put_u16(pg + PG_CSTART, ps);
}

/* Rebuild a page with contiguous free space. */
static void node_defrag(struct sqnano_db *db, uint8_t *pg)
{
	uint16_t ps = db->page_size;
	uint16_t n = pg_n(pg);
	uint8_t *tmp = db->scratch;
	uint16_t cs = ps;

	memcpy(tmp, pg, ps);
	for (int i = 0; i < n; i++) {
		struct cell c;

		(void)cell_at(tmp, ps, i, &c);
		cs -= c.size;
		memcpy(pg + cs, tmp + cell_ptr(tmp, i), c.size);
		put_u16(pg + SQNANO_PAGE_HDR + 2 * i, cs);
	}
	put_u16(pg + PG_CSTART, cs);
}

static bool node_insert(struct sqnano_db *db, uint8_t *pg, int pos, const uint8_t *cell,
			uint16_t len)
{
	uint16_t ps = db->page_size;
	uint16_t n = pg_n(pg);
	uint16_t cs = get_u16(pg + PG_CSTART);
	uint16_t ptr_end = (uint16_t)(SQNANO_PAGE_HDR + 2 * n);

	if (cs - ptr_end < len + 2) {
		if (ps - ptr_end - node_used(pg, ps) < len + 2) {
			return false;
		}
		node_defrag(db, pg);
		cs = get_u16(pg + PG_CSTART);
	}
	cs -= len;
	memcpy(pg + cs, cell, len);
	memmove(pg + SQNANO_PAGE_HDR + 2 * (pos + 1), pg + SQNANO_PAGE_HDR + 2 * pos,
		2u * (n - pos));
	put_u16(pg + SQNANO_PAGE_HDR + 2 * pos, cs);
	put_u16(pg + PG_NCELLS, (uint16_t)(n + 1));
	put_u16(pg + PG_CSTART, cs);
	return true;
}

static void node_remove(uint8_t *pg, uint16_t ps, int pos)
{
	uint16_t n = pg_n(pg);
	struct cell c;

	if (cell_at(pg, ps, pos, &c) == SQNANO_OK && cell_ptr(pg, pos) == get_u16(pg + PG_CSTART)) {
		put_u16(pg + PG_CSTART, (uint16_t)(get_u16(pg + PG_CSTART) + c.size));
	}
	memmove(pg + SQNANO_PAGE_HDR + 2 * pos, pg + SQNANO_PAGE_HDR + 2 * (pos + 1),
		2u * (n - pos - 1));
	put_u16(pg + PG_NCELLS, (uint16_t)(n - 1));
}

/* Leaf cell holding the first @p local bytes of a @p vl byte value (@p ovfl: the rest). */
static uint16_t leaf_cell_build(uint8_t *out, const uint8_t *k, uint16_t kl, const uint8_t *v,
				uint16_t vl, uint16_t local, uint32_t ovfl)
{
	int n = varint_put(out, kl);

	n += varint_put(out + n, (uint64_t)vl << 1 | (ovfl ? 1u : 0u));
	if (ovfl) {
		n += varint_put(out + n, local);
	}
	memcpy(out + n, k, kl);
	n += kl;
	if (local) {
		memcpy(out + n, v, local);
	}
	n += local;
	if (ovfl) {
		put_u32(out + n, ovfl);
		n += 4;
	}
	return (uint16_t)n;
}

/* ---- Overflow chains ---- */

static uint16_t ovfl_room(const struct sqnano_db *db)
{
	return pg_usable(db);
}

/* Write @p len bytes to a new chain of overflow pages. */
static int ovfl_write(struct sqnano_txn *t, const uint8_t *v, uint16_t len, uint32_t *first)
{
	struct sqnano_db *db = t->db;
	struct sqnano_frame *prev = NULL;
	uint16_t room = ovfl_room(db);
	int rc = SQNANO_OK;

	*first = 0;
	while (len) {
		struct sqnano_frame *f;
		uint16_t n = len < room ? len : room;

		rc = pg_alloc(t, &f);
		if (rc) {
			break;
		}
		f->data[PG_TYPE] = PT_OVERFLOW;
		memcpy(f->data + SQNANO_PAGE_HDR, v, n);
		if (prev) {
			put_u32(prev->data + PG_RIGHT, f->pgno);
			pg_put(db, prev);
		} else {
			*first = f->pgno;
		}
		prev = f;
		v += n;
		len = (uint16_t)(len - n);
	}
	if (prev) {
		put_u32(prev->data + PG_RIGHT, 0);
		pg_put(db, prev);
	}
	return rc;
}

/* Visit the pages of a chain (the callback may free the page it gets). */
static int ovfl_walk(struct sqnano_txn *t, uint32_t pgno, int (*cb)(void *arg, uint32_t pgno),
		     void *arg)
{
	struct sqnano_db *db = t->db;
	uint32_t hops = 0;

	while (pgno) {
		struct sqnano_frame *f;
		uint32_t next;
		int rc = pg_get(t, pgno, &f);

		if (rc) {
			return rc;
		}
		if (f->data[PG_TYPE] != PT_OVERFLOW || ++hops > db->npages) {
			pg_put(db, f);
			return SQNANO_CORRUPT;
		}
		next = get_u32(f->data + PG_RIGHT);
		pg_put(db, f);
		rc = cb(arg, pgno);
		if (rc) {
			return rc;
		}
		pgno = next;
	}
	return SQNANO_OK;
}

static int ovfl_free_cb(void *arg, uint32_t pgno)
{
	return pg_free((struct sqnano_txn *)arg, pgno);
}

/* Read the rest of a big value after its first @p have bytes. */
static int ovfl_read(struct sqnano_txn *t, uint32_t pgno, uint8_t *buf, uint16_t have,
		     uint16_t len)
{
	struct sqnano_db *db = t->db;
	uint16_t room = ovfl_room(db);

	while (have < len) {
		struct sqnano_frame *f;
		uint16_t n = (uint16_t)(len - have) < room ? (uint16_t)(len - have) : room;
		int rc;

		if (!pgno) {
			return SQNANO_CORRUPT;
		}
		rc = pg_get(t, pgno, &f);
		if (rc) {
			return rc;
		}
		if (f->data[PG_TYPE] != PT_OVERFLOW) {
			pg_put(db, f);
			return SQNANO_CORRUPT;
		}
		memcpy(buf + have, f->data + SQNANO_PAGE_HDR, n);
		have = (uint16_t)(have + n);
		pgno = get_u32(f->data + PG_RIGHT);
		pg_put(db, f);
	}
	return pgno ? SQNANO_CORRUPT : SQNANO_OK;
}

static uint16_t int_cell_build(uint8_t *out, uint32_t child, const uint8_t *k, uint16_t kl)
{
	int n;

	put_u32(out, child);
	n = 4 + varint_put(out + 4, kl);
	memcpy(out + n, k, kl);
	return (uint16_t)(n + kl);
}

/* ---- Tree roots ---- */

static void tree_key(uint8_t out[2], uint16_t tree)
{
	put_be16(out, tree);
}

int bt_tree_root(struct sqnano_txn *t, uint16_t tree, uint32_t *root)
{
	struct sqnano_cursor c;
	uint8_t k[2];
	uint8_t v[4];
	uint16_t vl;
	bool exact;
	int rc;

	if (tree == 0) {
		*root = t->master_root;
		return SQNANO_OK;
	}
	tree_key(k, tree);
	rc = bt_open(t, &c, 0);
	if (rc == SQNANO_OK) {
		rc = bt_seek(&c, k, 2, SQNANO_SEEK_GE, &exact);
	}
	if (rc) {
		return rc;
	}
	if (c.state != CUR_VALID || !exact) {
		return SQNANO_NOTFOUND;
	}
	rc = bt_value(&c, v, sizeof(v), &vl);
	if (rc) {
		return rc;
	}
	if (vl != 4) {
		return SQNANO_CORRUPT;
	}
	*root = get_u32(v);
	return SQNANO_OK;
}

static int set_root(struct sqnano_txn *t, uint16_t tree, uint32_t root)
{
	struct sqnano_cursor c;
	uint8_t k[2];
	uint8_t v[4];
	int rc;

	if (tree == 0) {
		t->master_root = root;
		return SQNANO_OK;
	}
	tree_key(k, tree);
	put_u32(v, root);
	rc = bt_open(t, &c, 0);
	if (rc) {
		return rc;
	}
	return bt_insert(&c, k, 2, v, 4);
}

int bt_new_root(struct sqnano_txn *t, uint32_t *root)
{
	struct sqnano_frame *f;
	int rc = pg_alloc(t, &f);

	if (rc) {
		return rc;
	}
	node_init(f->data, t->db->page_size, PT_LEAF);
	*root = f->pgno;
	pg_put(t->db, f);
	return SQNANO_OK;
}

int bt_tree_create(struct sqnano_txn *t, uint16_t tree)
{
	uint32_t root;
	int rc;

	if (bt_tree_root(t, tree, &root) == SQNANO_OK) {
		return SQNANO_OK;
	}
	rc = bt_new_root(t, &root);
	if (rc) {
		return rc;
	}
	return set_root(t, tree, root);
}

static int free_cb(void *arg, uint32_t pgno)
{
	return pg_free((struct sqnano_txn *)arg, pgno);
}

int bt_tree_drop(struct sqnano_txn *t, uint16_t tree)
{
	struct sqnano_cursor c;
	uint32_t root;
	uint8_t k[2];
	bool exact;
	int rc;

	rc = bt_tree_root(t, tree, &root);
	if (rc == SQNANO_NOTFOUND) {
		return SQNANO_OK;
	}
	if (rc) {
		return rc;
	}
	rc = bt_walk(t, root, free_cb, t);
	if (rc) {
		return rc;
	}
	tree_key(k, tree);
	rc = bt_open(t, &c, 0);
	if (rc == SQNANO_OK) {
		rc = bt_seek(&c, k, 2, SQNANO_SEEK_EQ, &exact);
	}
	if (rc == SQNANO_OK && c.state == CUR_VALID) {
		rc = bt_delete(&c);
	}
	/* AUTOINCREMENT high-water mark of the tree, if any. */
	tree_key(k, (uint16_t)(0x8000u | tree));
	if (rc == SQNANO_OK) {
		rc = bt_seek(&c, k, 2, SQNANO_SEEK_EQ, &exact);
	}
	if (rc == SQNANO_OK && c.state == CUR_VALID) {
		rc = bt_delete(&c);
	}
	return rc;
}

/* ---- Cursor positioning ---- */

int bt_open(struct sqnano_txn *t, struct sqnano_cursor *c, uint16_t tree)
{
	c->txn = t;
	c->tree = tree;
	c->depth = 0;
	c->state = CUR_INVALID;
	c->gen = t->gen;
	c->klen = 0;
	return SQNANO_OK;
}

static int load_key(struct sqnano_cursor *c)
{
	struct sqnano_db *db = c->txn->db;
	struct sqnano_frame *f;
	struct cell cl;
	int rc = pg_get(c->txn, c->pg[c->depth - 1], &f);

	if (rc) {
		return rc;
	}
	rc = cell_at(f->data, db->page_size, c->ix[c->depth - 1], &cl);
	if (rc == SQNANO_OK) {
		if (cl.klen > SQNANO_MAX_KEY_SIZE) {
			rc = SQNANO_CORRUPT;
		} else {
			memcpy(c->key, cl.key, cl.klen);
			c->klen = cl.klen;
			c->state = CUR_VALID;
			c->gen = c->txn->gen;
		}
	}
	pg_put(db, f);
	return rc;
}

/* Descend from @p pgno at level @p lvl to the first (or last) leaf entry. */
static int descend(struct sqnano_cursor *c, int lvl, uint32_t pgno, bool last)
{
	struct sqnano_db *db = c->txn->db;

	for (;;) {
		struct sqnano_frame *f;
		uint16_t n;
		int rc;

		if (lvl >= SQNANO_MAX_DEPTH) {
			return SQNANO_CORRUPT;
		}
		rc = pg_get(c->txn, pgno, &f);
		if (rc) {
			return rc;
		}
		rc = node_check(f->data, db->page_size);
		if (rc) {
			pg_put(db, f);
			return rc;
		}
		n = pg_n(f->data);
		c->pg[lvl] = pgno;
		if (f->data[PG_TYPE] == PT_LEAF) {
			c->ix[lvl] = last ? n : 0;
			c->depth = (uint8_t)(lvl + 1);
			pg_put(db, f);
			return SQNANO_OK;
		}
		c->ix[lvl] = last ? n : 0;
		pgno = child_at(f->data, db->page_size, c->ix[lvl]);
		pg_put(db, f);
		lvl++;
	}
}

static int leaf_count(struct sqnano_cursor *c, int lvl, uint16_t *n)
{
	struct sqnano_frame *f;
	int rc = pg_get(c->txn, c->pg[lvl], &f);

	if (rc) {
		return rc;
	}
	*n = pg_n(f->data);
	pg_put(c->txn->db, f);
	return SQNANO_OK;
}

/* Make the position valid, moving forward when it is past the end of a leaf. */
static int fix_forward(struct sqnano_cursor *c)
{
	for (;;) {
		int leaf = c->depth - 1;
		uint16_t n;
		int lvl;
		int rc = leaf_count(c, leaf, &n);

		if (rc) {
			return rc;
		}
		if (c->ix[leaf] < n) {
			return load_key(c);
		}
		for (lvl = leaf - 1; lvl >= 0; lvl--) {
			rc = leaf_count(c, lvl, &n);
			if (rc) {
				return rc;
			}
			if (c->ix[lvl] < n) {
				break;
			}
		}
		if (lvl < 0) {
			c->state = CUR_EOF;
			return SQNANO_OK;
		}
		{
			struct sqnano_frame *f;
			uint32_t child;

			c->ix[lvl]++;
			rc = pg_get(c->txn, c->pg[lvl], &f);
			if (rc) {
				return rc;
			}
			child = child_at(f->data, c->txn->db->page_size, c->ix[lvl]);
			pg_put(c->txn->db, f);
			rc = descend(c, lvl + 1, child, false);
			if (rc) {
				return rc;
			}
		}
	}
}

/* Move to the entry before the current position (leaf index ix - 1). */
static int step_back(struct sqnano_cursor *c)
{
	for (;;) {
		int leaf = c->depth - 1;
		int lvl;
		int rc;

		if (c->ix[leaf] > 0) {
			c->ix[leaf]--;
			return load_key(c);
		}
		for (lvl = leaf - 1; lvl >= 0; lvl--) {
			if (c->ix[lvl] > 0) {
				break;
			}
		}
		if (lvl < 0) {
			c->state = CUR_EOF;
			return SQNANO_OK;
		}
		{
			struct sqnano_frame *f;
			uint32_t child;

			c->ix[lvl]--;
			rc = pg_get(c->txn, c->pg[lvl], &f);
			if (rc) {
				return rc;
			}
			child = child_at(f->data, c->txn->db->page_size, c->ix[lvl]);
			pg_put(c->txn->db, f);
			rc = descend(c, lvl + 1, child, true);
			if (rc) {
				return rc;
			}
		}
	}
}

/* Fill the path down to the leaf lower bound of @p key. */
static int seek_raw(struct sqnano_cursor *c, const uint8_t *key, uint16_t klen, bool *exact)
{
	struct sqnano_db *db = c->txn->db;
	uint32_t pgno;
	int lvl = 0;
	int rc = bt_tree_root(c->txn, c->tree, &pgno);

	if (rc) {
		return rc;
	}
	for (;;) {
		struct sqnano_frame *f;
		int i;

		if (lvl >= SQNANO_MAX_DEPTH) {
			return SQNANO_CORRUPT;
		}
		rc = pg_get(c->txn, pgno, &f);
		if (rc) {
			return rc;
		}
		rc = node_check(f->data, db->page_size);
		if (rc) {
			pg_put(db, f);
			return rc;
		}
		i = node_search(f->data, db->page_size, key, klen, exact);
		if (i < 0) {
			pg_put(db, f);
			return SQNANO_CORRUPT;
		}
		c->pg[lvl] = pgno;
		c->ix[lvl] = (uint16_t)i;
		if (f->data[PG_TYPE] == PT_LEAF) {
			pg_put(db, f);
			c->depth = (uint8_t)(lvl + 1);
			return SQNANO_OK;
		}
		pgno = child_at(f->data, db->page_size, i);
		pg_put(db, f);
		lvl++;
	}
}

int bt_seek(struct sqnano_cursor *c, const uint8_t *key, uint16_t klen, enum sqnano_seek op,
	    bool *exact)
{
	bool ex = false;
	uint32_t root;
	int rc;

	if (exact) {
		*exact = false;
	}
	c->state = CUR_INVALID;
	if (op == SQNANO_SEEK_FIRST || op == SQNANO_SEEK_LAST) {
		rc = bt_tree_root(c->txn, c->tree, &root);
		if (rc == SQNANO_OK) {
			rc = descend(c, 0, root, op == SQNANO_SEEK_LAST);
		}
		if (rc) {
			return rc;
		}
		return op == SQNANO_SEEK_FIRST ? fix_forward(c) : step_back(c);
	}
	rc = seek_raw(c, key, klen, &ex);
	if (rc) {
		return rc;
	}
	if (exact) {
		*exact = ex;
	}
	switch (op) {
	case SQNANO_SEEK_EQ:
		if (!ex) {
			c->state = CUR_EOF;
			return SQNANO_OK;
		}
		return load_key(c);
	case SQNANO_SEEK_LE:
		if (ex) {
			return load_key(c);
		}
		return step_back(c);
	default:
		return fix_forward(c);
	}
}

int bt_seek_prefix_end(struct sqnano_cursor *c, const uint8_t *prefix, uint16_t plen)
{
	uint8_t succ[SQNANO_MAX_KEY_SIZE];
	int n = plen;
	int rc;

	memcpy(succ, prefix, plen);
	while (n > 0 && succ[n - 1] == 0xff) {
		n--;
	}
	if (n == 0) {
		return bt_seek(c, NULL, 0, SQNANO_SEEK_LAST, NULL);
	}
	succ[n - 1]++;
	rc = bt_seek(c, succ, (uint16_t)n, SQNANO_SEEK_GE, NULL);
	if (rc) {
		return rc;
	}
	if (c->state == CUR_EOF) {
		return bt_seek(c, NULL, 0, SQNANO_SEEK_LAST, NULL);
	}
	return step_back(c);
}

/* Reposition on the saved key after the tree was modified. */
static int reseek(struct sqnano_cursor *c, bool *exact)
{
	uint8_t k[SQNANO_MAX_KEY_SIZE];
	uint16_t kl = c->klen;

	memcpy(k, c->key, kl);
	return bt_seek(c, k, kl, SQNANO_SEEK_GE, exact);
}

int bt_next(struct sqnano_cursor *c)
{
	int rc;

	if (c->state == CUR_EOF) {
		return SQNANO_OK;
	}
	if (c->state != CUR_VALID) {
		return SQNANO_MISUSE;
	}
	if (c->gen != c->txn->gen) {
		bool exact;

		rc = reseek(c, &exact);
		if (rc || c->state != CUR_VALID || !exact) {
			return rc;
		}
	}
	c->ix[c->depth - 1]++;
	return fix_forward(c);
}

int bt_prev(struct sqnano_cursor *c)
{
	int rc;

	if (c->state == CUR_EOF) {
		return SQNANO_OK;
	}
	if (c->state != CUR_VALID) {
		return SQNANO_MISUSE;
	}
	if (c->gen != c->txn->gen) {
		bool exact;

		rc = reseek(c, &exact);
		if (rc) {
			return rc;
		}
		if (c->state == CUR_EOF) {
			return bt_seek(c, NULL, 0, SQNANO_SEEK_LAST, NULL);
		}
	}
	return step_back(c);
}

/* Bring a cursor back on its saved key; NOTFOUND when the entry is gone. */
static int ensure_current(struct sqnano_cursor *c)
{
	bool exact;
	int rc;

	if (c->state != CUR_VALID) {
		return SQNANO_MISUSE;
	}
	if (c->gen == c->txn->gen) {
		return SQNANO_OK;
	}
	rc = reseek(c, &exact);
	if (rc) {
		return rc;
	}
	return (c->state == CUR_VALID && exact) ? SQNANO_OK : SQNANO_NOTFOUND;
}

int bt_value(struct sqnano_cursor *c, uint8_t *buf, uint16_t cap, uint16_t *len)
{
	struct sqnano_db *db = c->txn->db;
	struct sqnano_frame *f;
	struct cell cl;
	int rc = ensure_current(c);

	if (rc) {
		return rc;
	}
	rc = pg_get(c->txn, c->pg[c->depth - 1], &f);
	if (rc) {
		return rc;
	}
	rc = cell_at(f->data, db->page_size, c->ix[c->depth - 1], &cl);
	if (rc == SQNANO_OK) {
		if (cl.vlen > cap) {
			rc = SQNANO_TOOBIG;
		} else {
			memcpy(buf, cl.val, cl.local);
			*len = cl.vlen;
		}
	}
	pg_put(db, f);
	if (rc == SQNANO_OK && cl.ovfl) {
		rc = ovfl_read(c->txn, cl.ovfl, buf, cl.local, cl.vlen);
	}
	return rc;
}

/* ---- Modification ---- */

/* Make every page of the cursor path private to the transaction. */
static int path_writable(struct sqnano_cursor *c)
{
	struct sqnano_txn *t = c->txn;

	for (int lvl = 0; lvl < c->depth; lvl++) {
		struct sqnano_frame *f;
		uint32_t np;
		int rc = pg_write(t, c->pg[lvl], &f, &np);

		if (rc) {
			return rc;
		}
		pg_put(t->db, f);
		if (np == c->pg[lvl]) {
			continue;
		}
		if (lvl == 0) {
			rc = set_root(t, c->tree, np);
		} else {
			uint32_t same;

			rc = pg_write(t, c->pg[lvl - 1], &f, &same);
			if (rc == SQNANO_OK) {
				set_child(f->data, c->ix[lvl - 1], np);
				pg_put(t->db, f);
			}
		}
		if (rc) {
			return rc;
		}
		c->pg[lvl] = np;
	}
	return SQNANO_OK;
}

static int writable(struct sqnano_cursor *c, int lvl, struct sqnano_frame **f)
{
	uint32_t np;
	int rc = pg_write(c->txn, c->pg[lvl], f, &np);

	if (rc == SQNANO_OK && np != c->pg[lvl]) {
		pg_put(c->txn->db, *f);
		return SQNANO_INTERNAL;
	}
	return rc;
}

/* Parse a cell given by address and size (cells built in memory are well formed). */
static void raw_cell(uint8_t type, const uint8_t *p, uint16_t len, struct cell *c)
{
	const uint8_t *end = p + len;
	uint64_t kl = 0, vl = 0;

	c->child = 0;
	if (type == PT_INTERIOR) {
		c->child = get_u32(p);
		p += 4;
	}
	p += varint_get(p, end, &kl);
	if (type == PT_LEAF) {
		uint64_t local;

		p += varint_get(p, end, &vl);
		if (vl & 1) {
			p += varint_get(p, end, &local);
		}
		vl >>= 1;
	}
	c->key = p;
	c->klen = (uint16_t)kl;
	c->val = p + kl;
	c->vlen = (uint16_t)vl;
	c->size = len;
}

/* Cell j of the page with @p cell virtually inserted at @p pos. */
static void vcell(const uint8_t *old, uint16_t ps, int pos, const uint8_t *cell, uint16_t clen,
		  int j, const uint8_t **p, uint16_t *len)
{
	struct cell c;

	if (j == pos) {
		*p = cell;
		*len = clen;
		return;
	}
	(void)cell_at(old, ps, j < pos ? j : j - 1, &c);
	*p = old + cell_ptr(old, j < pos ? j : j - 1);
	*len = c.size;
}

static void build_node(uint8_t *dst, uint16_t ps, uint8_t type, uint32_t right, const uint8_t *old,
		       int pos, const uint8_t *cell, uint16_t clen, int from, int to)
{
	uint16_t cs = ps;
	int k = 0;

	node_init(dst, ps, type);
	put_u32(dst + PG_RIGHT, right);
	for (int j = from; j < to; j++) {
		const uint8_t *p;
		uint16_t len;

		vcell(old, ps, pos, cell, clen, j, &p, &len);
		cs -= len;
		memcpy(dst + cs, p, len);
		put_u16(dst + SQNANO_PAGE_HDR + 2 * k, cs);
		k++;
	}
	put_u16(dst + PG_NCELLS, (uint16_t)k);
	put_u16(dst + PG_CSTART, cs);
}

/*
 * Split the page at @p lvl while inserting @p cell at @p pos. The left half
 * keeps the page number; the separator and the new right page are returned.
 */
static int node_split(struct sqnano_cursor *c, int lvl, int pos, const uint8_t *cell,
		      uint16_t clen, uint32_t *right, uint8_t *sep, uint16_t *seplen)
{
	struct sqnano_db *db = c->txn->db;
	uint16_t ps = db->page_size;
	uint8_t *old = db->scratch;
	struct sqnano_frame *lf, *rf;
	uint32_t total = 0, cum = 0;
	uint8_t type;
	int n, m;
	int rc;

	rc = writable(c, lvl, &lf);
	if (rc) {
		return rc;
	}
	memcpy(old, lf->data, ps);
	type = old[PG_TYPE];
	n = pg_n(old);
	for (int j = 0; j <= n; j++) {
		const uint8_t *p;
		uint16_t len;

		vcell(old, ps, pos, cell, clen, j, &p, &len);
		total += len + 2u;
	}
	for (m = 0; m <= n; m++) {
		const uint8_t *p;
		uint16_t len;

		vcell(old, ps, pos, cell, clen, m, &p, &len);
		if (cum + len + 2u > total / 2 && m > 0) {
			break;
		}
		cum += len + 2u;
	}
	if (type == PT_LEAF && m > n) {
		m = n;
	}
	if (type == PT_INTERIOR && m > n) {
		m = n;
	}
	rc = pg_alloc(c->txn, &rf);
	if (rc) {
		pg_put(db, lf);
		return rc;
	}
	{
		const uint8_t *p;
		uint16_t len;
		struct cell sc;

		vcell(old, ps, pos, cell, clen, m, &p, &len);
		raw_cell(type, p, len, &sc);
		if (sc.klen > SQNANO_MAX_KEY_SIZE) {
			pg_put(db, rf);
			pg_put(db, lf);
			return SQNANO_CORRUPT;
		}
		memcpy(sep, sc.key, sc.klen);
		*seplen = sc.klen;
		if (type == PT_LEAF) {
			build_node(lf->data, ps, PT_LEAF, 0, old, pos, cell, clen, 0, m);
			build_node(rf->data, ps, PT_LEAF, 0, old, pos, cell, clen, m, n + 1);
		} else {
			build_node(lf->data, ps, PT_INTERIOR, sc.child, old, pos, cell, clen, 0, m);
			build_node(rf->data, ps, PT_INTERIOR, get_u32(old + PG_RIGHT), old, pos, cell,
				   clen, m + 1, n + 1);
		}
	}
	*right = rf->pgno;
	pg_put(db, rf);
	pg_put(db, lf);
	return SQNANO_OK;
}

/* Insert @p cell at (lvl, pos), splitting up to the root as needed. */
static int insert_cell(struct sqnano_cursor *c, int lvl, int pos, uint16_t clen)
{
	struct sqnano_db *db = c->txn->db;
	uint8_t sep[SQNANO_MAX_KEY_SIZE];
	uint16_t seplen;

	for (;;) {
		struct sqnano_frame *f;
		uint32_t right;
		bool ok;
		int rc = writable(c, lvl, &f);

		if (rc) {
			return rc;
		}
		ok = node_insert(db, f->data, pos, db->cellbuf, clen);
		pg_put(db, f);
		if (ok) {
			return SQNANO_OK;
		}
		rc = node_split(c, lvl, pos, db->cellbuf, clen, &right, sep, &seplen);
		if (rc) {
			return rc;
		}
		if (lvl == 0) {
			struct sqnano_frame *nr;

			rc = pg_alloc(c->txn, &nr);
			if (rc) {
				return rc;
			}
			node_init(nr->data, db->page_size, PT_INTERIOR);
			put_u32(nr->data + PG_RIGHT, right);
			clen = int_cell_build(db->cellbuf, c->pg[0], sep, seplen);
			(void)node_insert(db, nr->data, 0, db->cellbuf, clen);
			rc = set_root(c->txn, c->tree, nr->pgno);
			pg_put(db, nr);
			return rc;
		}
		rc = writable(c, lvl - 1, &f);
		if (rc) {
			return rc;
		}
		set_child(f->data, c->ix[lvl - 1], right);
		pg_put(db, f);
		clen = int_cell_build(db->cellbuf, c->pg[lvl], sep, seplen);
		pos = c->ix[lvl - 1];
		lvl--;
	}
}

int bt_insert(struct sqnano_cursor *c, const uint8_t *key, uint16_t klen, const uint8_t *val,
	      uint16_t vlen)
{
	struct sqnano_db *db = c->txn->db;
	uint16_t clen;
	bool exact;
	int rc;

	uint16_t local = vlen;
	uint32_t ovfl = 0;

	if (!c->txn->write) {
		return SQNANO_READONLY;
	}
	if (klen == 0 || klen > bt_max_key(db)) {
		return SQNANO_TOOBIG;
	}
	if ((uint32_t)varint_len(klen) + varint_len((uint64_t)vlen << 1) + klen + vlen >
	    bt_max_cell(db)) {
		/* Big value: fill the cell, the rest goes to overflow pages. */
		uint32_t fixed = (uint32_t)varint_len(klen) + varint_len((uint64_t)vlen << 1) +
				 varint_len(vlen) + klen + 4;

		if (fixed >= bt_max_cell(db)) {
			return SQNANO_TOOBIG;
		}
		local = (uint16_t)(bt_max_cell(db) - fixed);
	}
	rc = seek_raw(c, key, klen, &exact);
	if (rc == SQNANO_OK) {
		rc = path_writable(c);
	}
	if (rc) {
		return rc;
	}
	if (exact) {
		struct sqnano_frame *f;
		struct cell old;

		rc = writable(c, c->depth - 1, &f);
		if (rc) {
			return rc;
		}
		rc = cell_at(f->data, db->page_size, c->ix[c->depth - 1], &old);
		if (rc == SQNANO_OK) {
			node_remove(f->data, db->page_size, c->ix[c->depth - 1]);
		}
		pg_put(db, f);
		if (rc == SQNANO_OK && old.ovfl) {
			rc = ovfl_walk(c->txn, old.ovfl, ovfl_free_cb, c->txn);
		}
		if (rc) {
			return rc;
		}
	}
	if (local < vlen) {
		rc = ovfl_write(c->txn, val + local, (uint16_t)(vlen - local), &ovfl);
		if (rc) {
			return rc;
		}
		/* The path is private: allocating the chain does not move it. */
	}
	clen = leaf_cell_build(db->cellbuf, key, klen, val, vlen, local, ovfl);
	rc = insert_cell(c, c->depth - 1, c->ix[c->depth - 1], clen);
	c->txn->gen++;
	memcpy(c->key, key, klen);
	c->klen = klen;
	c->state = rc == SQNANO_OK ? CUR_VALID : CUR_INVALID;
	c->gen = c->txn->gen - 1;
	return rc;
}

/* Remove the pointer at @p pos of the interior page at @p lvl. */
static int collapse_root(struct sqnano_cursor *c);

static int remove_child(struct sqnano_cursor *c, int lvl, int pos)
{
	struct sqnano_db *db = c->txn->db;
	uint16_t ps = db->page_size;
	struct sqnano_frame *f;
	uint16_t n;
	int rc = writable(c, lvl, &f);

	if (rc) {
		return rc;
	}
	n = pg_n(f->data);
	if (pos < n) {
		node_remove(f->data, ps, pos);
	} else if (n > 0) {
		put_u32(f->data + PG_RIGHT, child_at(f->data, ps, n - 1));
		node_remove(f->data, ps, n - 1);
	} else {
		/* The page has lost its only child. */
		pg_put(db, f);
		if (lvl == 0) {
			rc = writable(c, 0, &f);
			if (rc) {
				return rc;
			}
			node_init(f->data, ps, PT_LEAF);
			pg_put(db, f);
			return SQNANO_OK;
		}
		rc = pg_free(c->txn, c->pg[lvl]);
		if (rc) {
			return rc;
		}
		return remove_child(c, lvl - 1, c->ix[lvl - 1]);
	}
	pg_put(db, f);
	return lvl == 0 ? collapse_root(c) : SQNANO_OK;
}

/* Collapse a root interior page without separators. */
static int collapse_root(struct sqnano_cursor *c)
{
	struct sqnano_db *db = c->txn->db;
	struct sqnano_frame *f;
	int rc;

	for (;;) {
		uint32_t root = c->pg[0];
		uint32_t child;

		rc = pg_get(c->txn, root, &f);
		if (rc) {
			return rc;
		}
		if (f->data[PG_TYPE] != PT_INTERIOR || pg_n(f->data) != 0) {
			pg_put(db, f);
			return SQNANO_OK;
		}
		child = get_u32(f->data + PG_RIGHT);
		pg_put(db, f);
		rc = pg_free(c->txn, root);
		if (rc == SQNANO_OK) {
			rc = set_root(c->txn, c->tree, child);
		}
		if (rc) {
			return rc;
		}
		c->pg[0] = child;
	}
}

/*
 * After a delete at leaf level @p lvl: merge an underfull leaf with a sibling.
 * The leaf (already private) takes the sibling's cells, overflow chains
 * included; the sibling is freed and the separator between them removed.
 */
static int leaf_merge(struct sqnano_cursor *c, int lvl)
{
	struct sqnano_db *db = c->txn->db;
	uint16_t ps = db->page_size;
	uint16_t room = pg_usable(db);
	struct sqnano_frame *lf, *pf, *sf;
	int i = c->ix[lvl - 1];
	uint32_t sib;
	uint32_t used, sused;
	int si, np, lo;
	int rc = writable(c, lvl, &lf);

	if (rc) {
		return rc;
	}
	used = node_used(lf->data, ps) + 2u * pg_n(lf->data);
	if (used >= room / 4u) {
		pg_put(db, lf);
		return SQNANO_OK;
	}
	rc = writable(c, lvl - 1, &pf);
	if (rc) {
		pg_put(db, lf);
		return rc;
	}
	np = pg_n(pf->data);
	si = i < np ? i + 1 : i - 1;
	sib = si >= 0 ? child_at(pf->data, ps, si) : 0;
	rc = sib ? pg_get(c->txn, sib, &sf) : SQNANO_DONE;
	if (rc == SQNANO_OK) {
		uint16_t sn = pg_n(sf->data);
		int base = si > i ? pg_n(lf->data) : 0;

		sused = node_used(sf->data, ps) + 2u * sn;
		if (sf->data[PG_TYPE] != PT_LEAF || used + sused > room * 3u / 4u) {
			rc = SQNANO_DONE;
		}
		for (int k = 0; k < sn && rc == SQNANO_OK; k++) {
			struct cell cl;

			rc = cell_at(sf->data, ps, k, &cl);
			if (rc == SQNANO_OK &&
			    !node_insert(db, lf->data, base + k, sf->data + cell_ptr(sf->data, k),
					 cl.size)) {
				rc = SQNANO_INTERNAL;
			}
		}
		pg_put(db, sf);
	}
	if (rc == SQNANO_OK) {
		/* The pair (lo, lo + 1) of the parent becomes the leaf alone. */
		lo = si < i ? si : i;
		set_child(pf->data, lo + 1, c->pg[lvl]);
		node_remove(pf->data, ps, lo);
		c->ix[lvl - 1] = lo;
	}
	pg_put(db, pf);
	pg_put(db, lf);
	if (rc == SQNANO_DONE) {
		return SQNANO_OK;
	}
	if (rc == SQNANO_OK) {
		rc = pg_free(c->txn, sib);
	}
	if (rc == SQNANO_OK && lvl - 1 == 0) {
		rc = collapse_root(c);
	}
	return rc;
}

int bt_delete(struct sqnano_cursor *c)
{
	struct sqnano_db *db = c->txn->db;
	struct sqnano_frame *f;
	uint32_t ovfl;
	uint16_t n;
	int leaf;
	int rc;

	if (!c->txn->write) {
		return SQNANO_READONLY;
	}
	rc = ensure_current(c);
	if (rc == SQNANO_OK) {
		rc = path_writable(c);
	}
	if (rc) {
		return rc;
	}
	leaf = c->depth - 1;
	rc = writable(c, leaf, &f);
	if (rc) {
		return rc;
	}
	{
		struct cell old;

		rc = cell_at(f->data, db->page_size, c->ix[leaf], &old);
		if (rc) {
			pg_put(db, f);
			return rc;
		}
		ovfl = old.ovfl;
	}
	node_remove(f->data, db->page_size, c->ix[leaf]);
	n = pg_n(f->data);
	pg_put(db, f);
	if (ovfl) {
		rc = ovfl_walk(c->txn, ovfl, ovfl_free_cb, c->txn);
		if (rc) {
			return rc;
		}
	}
	if (n == 0 && leaf > 0) {
		rc = pg_free(c->txn, c->pg[leaf]);
		if (rc == SQNANO_OK) {
			rc = remove_child(c, leaf - 1, c->ix[leaf - 1]);
		}
	} else if (leaf > 0) {
		rc = leaf_merge(c, leaf);
	}
	c->txn->gen++;
	c->gen = c->txn->gen - 1;
	return rc;
}

/* ---- Walk and check ---- */

int bt_walk(struct sqnano_txn *t, uint32_t root, int (*cb)(void *arg, uint32_t pgno), void *arg)
{
	struct sqnano_db *db = t->db;
	uint32_t pg[SQNANO_MAX_DEPTH];
	int32_t ix[SQNANO_MAX_DEPTH];
	int sp = 0;

	pg[0] = root;
	ix[0] = -1;
	while (sp >= 0) {
		struct sqnano_frame *f;
		uint32_t child = 0;
		bool leaf;
		uint16_t n;
		int rc = pg_get(t, pg[sp], &f);

		if (rc) {
			return rc;
		}
		rc = node_check(f->data, db->page_size);
		if (rc) {
			pg_put(db, f);
			return rc;
		}
		leaf = f->data[PG_TYPE] == PT_LEAF;
		n = pg_n(f->data);
		if (!leaf && ix[sp] < (int32_t)n) {
			ix[sp]++;
			child = child_at(f->data, db->page_size, ix[sp]);
		}
		for (int i = 0; leaf && i < n && rc == SQNANO_OK; i++) {
			struct cell cl;

			rc = cell_at(f->data, db->page_size, i, &cl);
			if (rc == SQNANO_OK && cl.ovfl) {
				rc = ovfl_walk(t, cl.ovfl, cb, arg);
			}
		}
		pg_put(db, f);
		if (rc) {
			return rc;
		}
		if (child) {
			if (sp + 1 >= SQNANO_MAX_DEPTH) {
				return SQNANO_CORRUPT;
			}
			sp++;
			pg[sp] = child;
			ix[sp] = -1;
			continue;
		}
		/* Post-order: the callback may free the page. */
		rc = cb(arg, pg[sp]);
		if (rc) {
			return rc;
		}
		sp--;
	}
	return SQNANO_OK;
}

static int check_mark(void *arg, uint32_t pgno)
{
	uint32_t *seen = arg;

	if (bm_test(seen, pgno)) {
		return SQNANO_CORRUPT;
	}
	bm_set(seen, pgno);
	return SQNANO_OK;
}

int bt_check(struct sqnano_txn *t, uint32_t root, uint32_t *seen)
{
	struct sqnano_db *db = t->db;
	struct sqnano_cursor c;
	uint8_t prev[SQNANO_MAX_KEY_SIZE];
	uint16_t plen = 0;
	bool first = true;
	int rc;

	rc = bt_walk(t, root, check_mark, seen);
	if (rc) {
		return rc;
	}
	/* Walk the leaves in order through a cursor on a private root. */
	c.txn = t;
	c.tree = 0xffff;
	c.state = CUR_INVALID;
	rc = descend(&c, 0, root, false);
	if (rc == SQNANO_OK) {
		rc = fix_forward(&c);
	}
	while (rc == SQNANO_OK && c.state == CUR_VALID) {
		if (!first && keycmp(prev, plen, c.key, c.klen) >= 0) {
			return SQNANO_CORRUPT;
		}
		/* The key must respect every separator of its path. */
		for (int lvl = 0; lvl < c.depth - 1; lvl++) {
			struct sqnano_frame *f;
			struct cell cl;
			uint16_t n;

			rc = pg_get(t, c.pg[lvl], &f);
			if (rc) {
				return rc;
			}
			n = pg_n(f->data);
			if (c.ix[lvl] < n && (cell_at(f->data, db->page_size, c.ix[lvl], &cl) ||
					      keycmp(c.key, c.klen, cl.key, cl.klen) >= 0)) {
				rc = SQNANO_CORRUPT;
			}
			if (rc == SQNANO_OK && c.ix[lvl] > 0 &&
			    (cell_at(f->data, db->page_size, c.ix[lvl] - 1, &cl) ||
			     keycmp(c.key, c.klen, cl.key, cl.klen) < 0)) {
				rc = SQNANO_CORRUPT;
			}
			pg_put(db, f);
			if (rc) {
				return rc;
			}
		}
		memcpy(prev, c.key, c.klen);
		plen = c.klen;
		first = false;
		c.ix[c.depth - 1]++;
		rc = fix_forward(&c);
	}
	return rc;
}

/* ---- Public cursor API ---- */

int sqnano_cur_open(struct sqnano_txn *t, struct sqnano_cursor *c, uint16_t tree_id)
{
	return bt_open(t, c, tree_id);
}

int sqnano_cur_seek(struct sqnano_cursor *c, const sqnano_key *k, enum sqnano_seek op)
{
	int rc;

	db_lock(c->txn->db);
	rc = bt_seek(c, k ? k->p : NULL, k ? k->len : 0, op, NULL);
	db_unlock(c->txn->db);
	return rc;
}

int sqnano_cur_next(struct sqnano_cursor *c)
{
	int rc;

	db_lock(c->txn->db);
	rc = bt_next(c);
	db_unlock(c->txn->db);
	return rc;
}

int sqnano_cur_prev(struct sqnano_cursor *c)
{
	int rc;

	db_lock(c->txn->db);
	rc = bt_prev(c);
	db_unlock(c->txn->db);
	return rc;
}

bool sqnano_cur_valid(const struct sqnano_cursor *c)
{
	return c->state == CUR_VALID;
}

int sqnano_cur_key(struct sqnano_cursor *c, sqnano_key *k)
{
	if (c->state != CUR_VALID) {
		return SQNANO_MISUSE;
	}
	k->p = c->key;
	k->len = c->klen;
	return SQNANO_OK;
}

int sqnano_cur_value(struct sqnano_cursor *c, uint8_t *buf, uint16_t cap, uint16_t *len)
{
	int rc;

	db_lock(c->txn->db);
	rc = bt_value(c, buf, cap, len);
	db_unlock(c->txn->db);
	return rc;
}

int sqnano_cur_column(struct sqnano_cursor *c, uint16_t col, uint8_t *buf, uint16_t cap,
		      sqnano_value *out)
{
	uint16_t len;
	int rc = sqnano_cur_value(c, buf, cap, &len);

	if (rc) {
		return rc;
	}
	rc = sqnano_record_get(buf, len, col, out);
	return rc == SQNANO_NOTFOUND ? SQNANO_OK : rc;
}

int sqnano_cur_insert(struct sqnano_cursor *c, const sqnano_key *k, const sqnano_record *r)
{
	int rc;

	db_lock(c->txn->db);
	rc = bt_insert(c, k->p, k->len, r->p, r->len);
	db_unlock(c->txn->db);
	return rc;
}

int sqnano_cur_delete(struct sqnano_cursor *c)
{
	int rc;

	db_lock(c->txn->db);
	rc = bt_delete(c);
	db_unlock(c->txn->db);
	return rc;
}

/* ---- Relocation (flash compaction) ---- */

static bool in_range(uint32_t p, uint32_t lo, uint32_t hi)
{
	return p >= lo && p <= hi;
}

/* Copy an overflow chain to new pages; the old pages are freed. */
static int ovfl_copy(struct sqnano_txn *t, uint32_t pgno, uint32_t *first)
{
	struct sqnano_db *db = t->db;
	struct sqnano_frame *prev = NULL;
	int rc = SQNANO_OK;

	*first = 0;
	while (pgno && rc == SQNANO_OK) {
		struct sqnano_frame *f, *n;
		uint32_t next;

		rc = pg_get(t, pgno, &f);
		if (rc) {
			break;
		}
		if (f->data[PG_TYPE] != PT_OVERFLOW) {
			pg_put(db, f);
			rc = SQNANO_CORRUPT;
			break;
		}
		rc = pg_alloc(t, &n);
		if (rc) {
			pg_put(db, f);
			break;
		}
		memcpy(n->data, f->data, db->page_size);
		next = get_u32(f->data + PG_RIGHT);
		pg_put(db, f);
		if (prev) {
			put_u32(prev->data + PG_RIGHT, n->pgno);
			pg_put(db, prev);
		} else {
			*first = n->pgno;
		}
		prev = n;
		rc = pg_free(t, pgno);
		pgno = next;
	}
	if (prev) {
		put_u32(prev->data + PG_RIGHT, 0);
		pg_put(db, prev);
	}
	return rc;
}

static int ovfl_in_range(struct sqnano_txn *t, uint32_t pgno, uint32_t lo, uint32_t hi, bool *hit)
{
	struct sqnano_db *db = t->db;
	uint32_t hops = 0;

	*hit = false;
	while (pgno && !*hit) {
		struct sqnano_frame *f;
		int rc;

		*hit = in_range(pgno, lo, hi);
		rc = pg_get(t, pgno, &f);
		if (rc) {
			return rc;
		}
		if (f->data[PG_TYPE] != PT_OVERFLOW || ++hops > db->npages) {
			pg_put(db, f);
			return SQNANO_CORRUPT;
		}
		pgno = get_u32(f->data + PG_RIGHT);
		pg_put(db, f);
	}
	return SQNANO_OK;
}

/*
 * Copy the pages of the subtree at @p pgno that lie in [lo, hi], and their
 * ancestors; *out receives the new number of the subtree root. No frame is
 * held across the recursion.
 */
static int reloc(struct sqnano_txn *t, uint32_t pgno, uint32_t lo, uint32_t hi, int depth,
		 uint32_t *out)
{
	struct sqnano_db *db = t->db;
	uint16_t ps = db->page_size;
	struct sqnano_frame *f;
	uint32_t cur = pgno;
	bool leaf;
	uint16_t n;
	int rc;

	if (depth >= SQNANO_MAX_DEPTH) {
		return SQNANO_CORRUPT;
	}
	rc = pg_get(t, cur, &f);
	if (rc) {
		return rc;
	}
	rc = node_check(f->data, ps);
	leaf = f->data[PG_TYPE] == PT_LEAF;
	n = pg_n(f->data);
	pg_put(db, f);
	if (rc) {
		return rc;
	}
	if (in_range(cur, lo, hi)) {
		rc = pg_move(t, cur, &f, &cur);
		if (rc) {
			return rc;
		}
		pg_put(db, f);
	}
	for (int i = 0; i <= n && !leaf; i++) {
		uint32_t child, nc;

		rc = pg_get(t, cur, &f);
		if (rc) {
			return rc;
		}
		child = child_at(f->data, ps, i);
		pg_put(db, f);
		rc = reloc(t, child, lo, hi, depth + 1, &nc);
		if (rc) {
			return rc;
		}
		if (nc != child) {
			rc = pg_write(t, cur, &f, &cur);
			if (rc) {
				return rc;
			}
			set_child(f->data, i, nc);
			pg_put(db, f);
		}
	}
	for (int i = 0; i < n && leaf; i++) {
		struct cell cl;
		uint32_t first, nf;
		bool hit;

		rc = pg_get(t, cur, &f);
		if (rc) {
			return rc;
		}
		rc = cell_at(f->data, ps, i, &cl);
		first = cl.ovfl;
		pg_put(db, f);
		if (rc || !first) {
			if (rc) {
				return rc;
			}
			continue;
		}
		rc = ovfl_in_range(t, first, lo, hi, &hit);
		if (rc == SQNANO_OK && hit) {
			rc = ovfl_copy(t, first, &nf);
			if (rc == SQNANO_OK) {
				rc = pg_write(t, cur, &f, &cur);
			}
			if (rc == SQNANO_OK) {
				rc = cell_at(f->data, ps, i, &cl);
				if (rc == SQNANO_OK) {
					put_u32((uint8_t *)cl.val + cl.local, nf);
				}
				pg_put(db, f);
			}
		}
		if (rc) {
			return rc;
		}
	}
	*out = cur;
	return SQNANO_OK;
}

int bt_relocate(struct sqnano_txn *t, uint32_t lo, uint32_t hi)
{
	struct sqnano_cursor c;
	uint32_t nm;
	int rc = bt_open(t, &c, 0);

	if (rc == SQNANO_OK) {
		rc = bt_seek(&c, NULL, 0, SQNANO_SEEK_FIRST, NULL);
	}
	while (rc == SQNANO_OK && c.state == CUR_VALID) {
		uint8_t v[4];
		uint16_t vl;
		uint32_t root, nr;

		if (c.klen != 2) {
			return SQNANO_CORRUPT;
		}
		if (!(c.key[0] & 0x80)) {
			rc = bt_value(&c, v, sizeof(v), &vl);
			if (rc == SQNANO_OK && vl != 4) {
				rc = SQNANO_CORRUPT;
			}
			if (rc) {
				return rc;
			}
			root = get_u32(v);
			rc = reloc(t, root, lo, hi, 0, &nr);
			if (rc == SQNANO_OK && nr != root) {
				rc = set_root(t, (uint16_t)((c.key[0] << 8) | c.key[1]), nr);
			}
		}
		if (rc == SQNANO_OK) {
			rc = bt_next(&c);
		}
	}
	if (rc) {
		return rc;
	}
	rc = reloc(t, t->master_root, lo, hi, 0, &nm);
	if (rc == SQNANO_OK) {
		t->master_root = nm;
		t->gen++;
	}
	return rc;
}
