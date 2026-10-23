/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Runtime used by generated queries: statement driver, scans, row
 * operations with constraint checks and index maintenance, sorter and
 * aggregates.
 *
 * A statement step runs with the database lock held. Long scans release it
 * every SQNANO_YIELD_ROWS rows so that other threads keep running; this is
 * safe because cursors only keep page numbers and saved keys.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include "../sqnano_int.h"

const struct sqnano_index sqnano_rowid_target = {.name = "rowid"};

int sqnano_exec_error(struct sqnano_exec *x, int rc, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(x->errmsg, sizeof(x->errmsg), fmt, ap);
	va_end(ap);
	return rc;
}

/* ---- Statement driver ---- */

int sqnano_exec_open(struct sqnano_exec *x, const struct sqnano_query *q, struct sqnano_db *db,
		     struct sqnano_txn *txn)
{
	if (!q || !db) {
		return SQNANO_MISUSE;
	}
	memset(x, 0, q->state_size);
	x->q = q;
	x->db = db;
	x->ext_txn = txn;
	x->busy_ms = -1;
	q->init(x);
	return SQNANO_OK;
}

static int finish(struct sqnano_exec *x, int rc)
{
	struct sqnano_db *db = x->db;

	db_lock(db);
	x->q->close(x);
	if (rc == SQNANO_DONE && x->fk_count > 0) {
		/* Immediate foreign key violations left at the end of the statement. */
		rc = sqnano_exec_error(x, SQNANO_CONSTRAINT_FOREIGNKEY, "FOREIGN KEY constraint failed");
	}
#if SQNANO_MAX_SAVEPOINTS > 0
	if (x->has_sp) {
		x->has_sp = 0;
		if (rc != SQNANO_DONE && x->txn->active && x->on_error != SQNANO_OE_FAIL) {
			bool top = db->sp_depth && db->sp[db->sp_depth - 1].serial == x->sp_id;

			/* Undo this statement only; otherwise the transaction is lost. */
			if (!top || pg_rollback_to(x->txn, x->sp_id) != SQNANO_OK) {
				x->txn->poisoned = 1;
			}
		}
		if (x->txn->active) {
			(void)pg_release(x->txn, x->sp_id, true);
		}
	}
#endif
	if (rc != SQNANO_DONE && !x->own && x->on_error == SQNANO_OE_ROLLBACK && x->txn->active) {
		/* INSERT OR ROLLBACK: the whole transaction goes. */
		(void)pg_rollback(x->txn);
	}
	if (x->own) {
		int rc2;

		if (rc == SQNANO_DONE || x->on_error == SQNANO_OE_FAIL) {
			/* OR FAIL keeps the rows changed before the failure. */
			rc2 = pg_commit(&x->own_txn);
			if (rc2 == SQNANO_CONSTRAINT_FOREIGNKEY) {
				/* Deferred violations: the statement does not commit. */
				(void)pg_rollback(&x->own_txn);
				rc = sqnano_exec_error(x, rc2, "FOREIGN KEY constraint failed");
			} else if (rc2) {
				rc = rc2;
				(void)sqnano_exec_error(x, rc, "commit failed");
			}
		} else {
			(void)pg_rollback(&x->own_txn);
		}
		x->own = 0;
	}
	db_unlock(db);
	x->txn = NULL;
	x->running = 0;
	return rc;
}

int sqnano_exec_step(struct sqnano_exec *x)
{
	struct sqnano_db *db = x->db;
	bool own;
	int rc;

restart:
	if (!x->running) {
		x->pc = 0;
		x->yielded = 0;
		x->changes = 0;
		x->last_rowid = INT64_MIN;
		x->errmsg[0] = 0;
		x->on_error = SQNANO_OE_ABORT;
		x->fk_count = 0;
		x->fk_depth = 0;
		if (x->q->kind == SQNANO_Q_FAIL) {
			return sqnano_exec_error(x, SQNANO_ERROR,
						 "schema changes are applied by SQNano migrations");
		}
		if (x->q->kind != SQNANO_Q_SELECT && x->q->kind != SQNANO_Q_WRITE) {
			return SQNANO_DONE;
		}
		x->gen0 = x->ext_txn ? x->ext_txn->gen : 0;
		if (x->ext_txn) {
			if (!x->ext_txn->active) {
				return SQNANO_MISUSE;
			}
			if (x->q->kind == SQNANO_Q_WRITE && !x->ext_txn->write) {
				return sqnano_exec_error(x, SQNANO_READONLY,
							 "attempt to write in a read transaction");
			}
			x->txn = x->ext_txn;
#if SQNANO_MAX_SAVEPOINTS > 0
			if (x->q->kind == SQNANO_Q_WRITE) {
				db_lock(db);
				x->has_sp = pg_savepoint(x->txn, &x->sp_id) == SQNANO_OK;
				db_unlock(db);
			}
#endif
		} else {
			uint8_t flags = x->q->kind == SQNANO_Q_WRITE ? SQNANO_TXN_WRITE : 0;

			rc = sqnano_txn_begin(db, &x->own_txn, flags, x->busy_ms);
			if (rc) {
				return sqnano_exec_error(x, rc,
							 rc == SQNANO_BUSY ? "database is locked"
									   : "cannot begin transaction");
			}
			x->txn = &x->own_txn;
			x->own = 1;
		}
		x->running = 1;
	}
	db_lock(db);
	sqnano_scratch_reset(x);
	rc = x->q->step(x);
	db_unlock(db);
	if (rc == SQNANO_ROW) {
		x->yielded = 1;
		return rc;
	}
	if (rc != SQNANO_DONE && !x->errmsg[0]) {
		(void)sqnano_exec_error(x, rc, "statement failed (%d)", rc);
	}
	if (rc != SQNANO_DONE && !x->own && !x->has_sp && x->txn && x->txn->write &&
	    x->txn->gen != x->gen0 && x->on_error != SQNANO_OE_FAIL) {
		/*
		 * No statement journal: a statement that fails half-way leaves the
		 * transaction unusable rather than committing a partial change.
		 */
		x->txn->poisoned = 1;
	}
	own = x->own;
	rc = finish(x, rc);
	if (rc == SQNANO_FULL && own && !x->yielded && !x->retried) {
		/*
		 * Flash: the transaction only had the space erased when it began.
		 * Compact everything and run the statement once more.
		 */
		x->retried = 1;
		if (sqnano_db_compact(db, x->busy_ms) == SQNANO_OK) {
			x->errmsg[0] = 0;
			goto restart;
		}
	}
	x->retried = 0;
	return rc;
}

int sqnano_exec_reset(struct sqnano_exec *x)
{
	if (!x->running) {
		return SQNANO_OK;
	}
	/* An interrupted autocommit statement keeps what it has done, as SQLite. */
	return finish(x, SQNANO_DONE) == SQNANO_DONE ? SQNANO_OK : SQNANO_IOERR;
}

int sqnano_exec_run(struct sqnano_exec *x)
{
	int rc;

	do {
		rc = sqnano_exec_step(x);
	} while (rc == SQNANO_ROW);
	return rc == SQNANO_DONE ? SQNANO_OK : rc;
}

const struct sqnano_query *sqnano_query_find(const struct sqnano_schema *s, const char *name)
{
	for (uint16_t i = 0; i < s->nqueries; i++) {
		if (s->queries[i]->name && strcmp(s->queries[i]->name, name) == 0) {
			return s->queries[i];
		}
	}
	return NULL;
}

int sqnano_limit_value(struct sqnano_exec *x, const sqnano_value *v, int64_t *out)
{
	sqnano_value t = *v;

	(void)sqnano_affinity(x, &t, SQNANO_AFF_NUMERIC);
	if (t.type != SQNANO_INTEGER) {
		return sqnano_exec_error(x, SQNANO_MISMATCH, "datatype mismatch");
	}
	*out = t.u.i;
	return SQNANO_OK;
}

/* ---- Rows ---- */

/*
 * A record older than a lazy migration of its table: every slot is decoded,
 * the steps it has not seen rewrite them, then the columns are picked.
 */
static int decode_lazy(struct sqnano_exec *x, const struct sqnano_table *t, int64_t rowid,
		       const uint8_t *rec, uint16_t len, sqnano_value *cols)
{
	sqnano_value s[SQNANO_MAX_COLUMNS];
	uint8_t have[SQNANO_MAX_COLUMNS];
	uint16_t ver = sqnano_record_version(rec, len);
	int rc;

	if (t->nslots > SQNANO_MAX_COLUMNS) {
		return SQNANO_TOOBIG;
	}
	for (uint16_t i = 0; i < t->nslots; i++) {
		rc = sqnano_record_get(rec, len, i, &s[i]);
		if (rc && rc != SQNANO_NOTFOUND) {
			return rc;
		}
		have[i] = rc == SQNANO_OK;
	}
	for (int i = 0; i < t->nlazy; i++) {
		if (t->lazy[i].version > ver) {
			rc = t->lazy[i].fn(x, s, have);
			if (rc) {
				return rc;
			}
		}
	}
	for (int c = 0; c < t->ncols; c++) {
		const struct sqnano_column *col = &t->cols[c];

		if (c == t->ipk) {
			sqnano_v_int(&cols[c], rowid);
			continue;
		}
		cols[c] = have[col->slot] ? s[col->slot] : col->dflt;
		if (col->aff == SQNANO_AFF_REAL && cols[c].type == SQNANO_INTEGER) {
			sqnano_v_real(&cols[c], (double)cols[c].u.i);
		}
	}
	return SQNANO_OK;
}

int sqnano_row_decode(struct sqnano_exec *x, const struct sqnano_table *t, int64_t rowid,
		      const uint8_t *rec, uint16_t len, sqnano_value *cols)
{
	if (t->nlazy && sqnano_record_version(rec, len) < t->lazy[t->nlazy - 1].version) {
		return decode_lazy(x, t, rowid, rec, len, cols);
	}
	for (int c = 0; c < t->ncols; c++) {
		const struct sqnano_column *col = &t->cols[c];
		int rc;

		if (c == t->ipk) {
			sqnano_v_int(&cols[c], rowid);
			continue;
		}
		rc = sqnano_record_get(rec, len, col->slot, &cols[c]);
		if (rc == SQNANO_NOTFOUND) {
			/* Row written before the column existed. */
			cols[c] = col->dflt;
		} else if (rc) {
			return rc;
		}
		if (col->aff == SQNANO_AFF_REAL && cols[c].type == SQNANO_INTEGER) {
			/* Integral reals are stored as integers, as SQLite does. */
			sqnano_v_real(&cols[c], (double)cols[c].u.i);
		}
	}
	return SQNANO_OK;
}

static int key_prefix_cmp(const uint8_t *k, uint16_t kl, const uint8_t *p, uint16_t pl)
{
	int r = memcmp(k, p, kl < pl ? kl : pl);

	if (r) {
		return r;
	}
	return kl < pl ? -1 : 0;
}

static int key_cmp(const uint8_t *a, uint16_t al, const uint8_t *b, uint16_t bl)
{
	int r = memcmp(a, b, al < bl ? al : bl);

	if (r) {
		return r;
	}
	return al < bl ? -1 : (al > bl ? 1 : 0);
}

/* ---- Row keys ---- */

/*
 * A row is identified by its key in the table tree: the rowid (8 bytes) or,
 * in a WITHOUT ROWID table, its PRIMARY KEY columns encoded as an index key.
 * Index entries end with that key.
 */
static int row_key(const struct sqnano_table *t, int64_t rowid, const sqnano_value *cols,
		   struct sqnano_keybuf *rk)
{
	int rc = SQNANO_OK;

	rk->len = 0;
	if (!t->pk) {
		return sqnano_key_append_rowid(rk, rowid);
	}
	for (int i = 0; i < t->pk->ncols && rc == SQNANO_OK; i++) {
		rc = sqnano_key_append(rk, &cols[t->pk->cols[i]], t->pk->coll[i], t->pk->desc[i], false);
	}
	return rc;
}

static int64_t rk_rowid(const struct sqnano_table *t, const struct sqnano_keybuf *rk)
{
	return t->pk ? 0 : sqnano_key_get_rowid(rk->b);
}

static bool rk_eq(const struct sqnano_keybuf *a, const struct sqnano_keybuf *b)
{
	return a->len == b->len && memcmp(a->b, b->b, a->len) == 0;
}

static void rk_set(struct sqnano_keybuf *rk, const uint8_t *k, uint16_t len)
{
	rk->len = len <= SQNANO_MAX_KEY_SIZE ? len : SQNANO_MAX_KEY_SIZE;
	memcpy(rk->b, k, rk->len);
}

/* Bytes taken by the first @p n values of an encoded key, or -1 (see record.c). */
static int key_skip(const uint8_t *k, uint16_t len, uint8_t n, const uint8_t *desc)
{
	uint32_t at = 0;

	for (uint8_t i = 0; i < n; i++) {
		uint8_t m = desc && desc[i] ? 0xff : 0x00;
		uint8_t tag;

		if (at >= len) {
			return -1;
		}
		tag = (uint8_t)(k[at] ^ m);
		if (tag == 0x05) {
			at++;
		} else if (tag == 0x15) {
			uint64_t u = 0;
			double r;

			if (at + 9 > len) {
				return -1;
			}
			for (int j = 1; j <= 8; j++) {
				u = u << 8 | (uint8_t)(k[at + j] ^ m);
			}
			u = (u & 0x8000000000000000ull) ? u ^ 0x8000000000000000ull : ~u;
			memcpy(&r, &u, 8);
			at += 9;
			if (r >= 9007199254740992.0 || r <= -9007199254740992.0) {
				at += 9; /* exact tie-break */
			}
		} else if (tag == 0x30 || tag == 0x40) {
			for (at++;; at++) {
				if (at + 1 >= len) {
					return -1;
				}
				if ((k[at] ^ m) == 0) {
					at++;
					if ((k[at] ^ m) == 0) {
						at++;
						break;
					}
				}
			}
		} else {
			return -1;
		}
	}
	return at <= len ? (int)at : -1;
}

/* Row key at the end of an index entry. */
static int entry_row_key(const struct sqnano_table *t, const struct sqnano_index *idx,
			 const uint8_t *k, uint16_t klen, struct sqnano_keybuf *rk)
{
	int at;

	if (idx->flags & SQNANO_IDX_PRIMARY) {
		rk_set(rk, k, klen);
		return SQNANO_OK;
	}
	if (!t->pk) {
		if (klen < 8) {
			return SQNANO_CORRUPT;
		}
		rk_set(rk, k + klen - 8, 8);
		return SQNANO_OK;
	}
	at = key_skip(k, klen, idx->ncols, idx->desc);
	if (at < 0 || at >= klen) {
		return SQNANO_CORRUPT;
	}
	rk_set(rk, k + at, (uint16_t)(klen - at));
	return SQNANO_OK;
}

static int load_row(struct sqnano_exec *x, struct sqnano_cursor *tc, const struct sqnano_table *tab,
		    const struct sqnano_keybuf *rk, uint8_t *buf, uint16_t *len, sqnano_value *cols,
		    bool *found)
{
	bool exact;
	int rc;

	*found = false;
	rc = bt_open(x->txn, tc, tab->tree);
	if (rc == SQNANO_OK) {
		rc = bt_seek(tc, rk->b, rk->len, SQNANO_SEEK_EQ, &exact);
	}
	if (rc || tc->state != CUR_VALID) {
		return rc;
	}
	rc = bt_value(tc, buf, SQNANO_MAX_RECORD_SIZE, len);
	if (rc) {
		return rc;
	}
	*found = true;
	return cols ? sqnano_row_decode(x, tab, rk_rowid(tab, rk), buf, *len, cols) : SQNANO_OK;
}

/* ---- Scans ---- */

void sqnano_scan_init(struct sqnano_scan *s, struct sqnano_exec *x, const struct sqnano_table *t,
		      const struct sqnano_index *idx, sqnano_value *cols, uint8_t flags)
{
	s->x = x;
	s->tab = t;
	s->idx = idx ? idx : t->pk;
	s->cols = cols;
	s->flags = flags & SQNANO_SCAN_DESC;
	s->lo.len = 0;
	s->hi.len = 0;
	s->nrows = 0;
	s->rowid = 0;
	s->rowlen = 0;
}

int sqnano_scan_eq(struct sqnano_scan *s, const sqnano_value *v, uint8_t aff, uint8_t coll,
		   bool desc)
{
	sqnano_value t = *v;
	int rc = sqnano_affinity(s->x, &t, aff);

	if (rc) {
		return rc;
	}
	if (t.type == SQNANO_NULL) {
		s->flags |= SQNANO_SCAN_EMPTY;
		return SQNANO_OK;
	}
	if (!s->idx) {
		if (t.type != SQNANO_INTEGER) {
			s->flags |= SQNANO_SCAN_EMPTY;
			return SQNANO_OK;
		}
		rc = sqnano_key_append_rowid(&s->lo, t.u.i);
		if (rc == SQNANO_OK) {
			rc = sqnano_key_append_rowid(&s->hi, t.u.i);
		}
	} else {
		rc = sqnano_key_append(&s->lo, &t, coll, desc, false);
		if (rc == SQNANO_OK) {
			rc = sqnano_key_append(&s->hi, &t, coll, desc, false);
		}
	}
	s->flags |= SQNANO_SCAN_LO | SQNANO_SCAN_HI;
	return rc == SQNANO_TOOBIG ? sqnano_exec_error(s->x, rc, "key too large") : rc;
}

int sqnano_scan_range(struct sqnano_scan *s, uint8_t which, const sqnano_value *v, uint8_t aff,
		      uint8_t coll, bool desc)
{
	struct sqnano_keybuf *kb = which == SQNANO_SCAN_LO ? &s->lo : &s->hi;
	sqnano_value t = *v;
	int rc = sqnano_affinity(s->x, &t, aff);

	if (rc) {
		return rc;
	}
	if (t.type == SQNANO_NULL) {
		s->flags |= SQNANO_SCAN_EMPTY;
		return SQNANO_OK;
	}
	if (!s->idx) {
		int64_t r;

		if (t.type == SQNANO_INTEGER) {
			r = t.u.i;
		} else if (t.type == SQNANO_FLOAT && t.u.r > -9.2e18 && t.u.r < 9.2e18) {
			r = (int64_t)(which == SQNANO_SCAN_LO ? ceil(t.u.r) : floor(t.u.r));
		} else {
			/* Not comparable as a rowid range: the filter decides. */
			return SQNANO_OK;
		}
		if (kb->len) {
			return SQNANO_OK;
		}
		rc = sqnano_key_append_rowid(kb, r);
	} else {
		rc = sqnano_key_append(kb, &t, coll, desc, true);
	}
	s->flags |= which;
	return rc == SQNANO_TOOBIG ? sqnano_exec_error(s->x, rc, "key too large") : rc;
}

static int scan_position(struct sqnano_scan *s, struct sqnano_cursor *c)
{
	bool desc = s->flags & SQNANO_SCAN_DESC;

	if (!desc) {
		if (s->flags & SQNANO_SCAN_LO) {
			return bt_seek(c, s->lo.b, s->lo.len, SQNANO_SEEK_GE, NULL);
		}
		return bt_seek(c, NULL, 0, SQNANO_SEEK_FIRST, NULL);
	}
	if (s->flags & SQNANO_SCAN_HI) {
		return bt_seek_prefix_end(c, s->hi.b, s->hi.len);
	}
	return bt_seek(c, NULL, 0, SQNANO_SEEK_LAST, NULL);
}

int sqnano_scan_next(struct sqnano_scan *s)
{
	struct sqnano_txn *t = s->x->txn;
	struct sqnano_cursor *c = s->idx ? &s->ic : &s->tc;
	bool desc = s->flags & SQNANO_SCAN_DESC;
	int rc;

	if (s->flags & (SQNANO_SCAN_EMPTY | SQNANO_SCAN_EOF)) {
		return SQNANO_DONE;
	}
	if (!(s->flags & SQNANO_SCAN_STARTED)) {
		rc = bt_open(t, c, s->idx ? s->idx->tree : s->tab->tree);
		if (rc == SQNANO_OK) {
			rc = scan_position(s, c);
		}
		s->flags |= SQNANO_SCAN_STARTED;
	} else {
		if (++s->nrows % SQNANO_YIELD_ROWS == 0) {
			db_unlock(t->db);
			sqnano_yield();
			db_lock(t->db);
		}
		rc = desc ? bt_prev(c) : bt_next(c);
	}
	if (rc) {
		return rc;
	}
	if (c->state != CUR_VALID) {
		s->flags |= SQNANO_SCAN_EOF;
		return SQNANO_DONE;
	}
	if ((!desc && (s->flags & SQNANO_SCAN_HI) &&
	     key_prefix_cmp(c->key, c->klen, s->hi.b, s->hi.len) > 0) ||
	    (desc && (s->flags & SQNANO_SCAN_LO) &&
	     key_cmp(c->key, c->klen, s->lo.b, s->lo.len) < 0)) {
		s->flags |= SQNANO_SCAN_EOF;
		return SQNANO_DONE;
	}
	if (s->idx && !(s->idx->flags & SQNANO_IDX_PRIMARY)) {
		struct sqnano_keybuf rk;
		bool found;

		rc = entry_row_key(s->tab, s->idx, c->key, c->klen, &rk);
		if (rc) {
			return rc;
		}
		s->rowid = rk_rowid(s->tab, &rk);
		rc = load_row(s->x, &s->tc, s->tab, &rk, s->row, &s->rowlen, s->cols, &found);
		if (rc == SQNANO_OK && !found) {
			rc = sqnano_exec_error(s->x, SQNANO_CORRUPT, "index %s points to a missing row",
					       s->idx->name);
		}
		return rc;
	}
	if (!s->tab->pk && c->klen != 8) {
		return SQNANO_CORRUPT;
	}
	s->rowid = s->tab->pk ? 0 : sqnano_key_get_rowid(c->key);
	rc = bt_value(c, s->row, sizeof(s->row), &s->rowlen);
	if (rc) {
		return rc;
	}
	return sqnano_row_decode(s->x, s->tab, s->rowid, s->row, s->rowlen, s->cols);
}

static int fetch_key(struct sqnano_scan *s, const struct sqnano_keybuf *rk)
{
	bool found;
	int rc = load_row(s->x, &s->tc, s->tab, rk, s->row, &s->rowlen, s->cols, &found);

	if (rc) {
		return rc;
	}
	if (!found) {
		return SQNANO_NOTFOUND;
	}
	s->rowid = rk_rowid(s->tab, rk);
	return SQNANO_OK;
}

int sqnano_row_fetch(struct sqnano_scan *s, int64_t rowid)
{
	struct sqnano_keybuf rk;

	if (s->tab->pk) {
		return SQNANO_MISUSE;
	}
	(void)row_key(s->tab, rowid, NULL, &rk);
	return fetch_key(s, &rk);
}

int sqnano_row_fetch_pk(struct sqnano_scan *s, const sqnano_value *pk)
{
	const struct sqnano_index *idx = s->tab->pk;
	struct sqnano_keybuf rk;
	int rc = SQNANO_OK;

	if (!idx) {
		return SQNANO_MISUSE;
	}
	rk.len = 0;
	for (int i = 0; i < idx->ncols && rc == SQNANO_OK; i++) {
		rc = sqnano_key_append(&rk, &pk[i], idx->coll[i], idx->desc[i], false);
	}
	return rc ? rc : fetch_key(s, &rk);
}

int sqnano_row_fetch_conflict(struct sqnano_scan *s)
{
	if (!s->tab->pk) {
		return sqnano_row_fetch(s, s->x->conflict_rowid);
	}
	return fetch_key(s, &s->x->db->conflict_key);
}

/* ---- Row operations ---- */

/* Entry of @p row in @p idx, ending with the row key @p rk (NULL: the columns only). */
static int index_key(struct sqnano_keybuf *kb, const struct sqnano_index *idx,
		     const sqnano_value *row, const struct sqnano_keybuf *rk, bool *has_null)
{
	int rc = SQNANO_OK;

	kb->len = 0;
	*has_null = false;
	for (int i = 0; i < idx->ncols && rc == SQNANO_OK; i++) {
		const sqnano_value *v = &row[idx->cols[i]];

		if (v->type == SQNANO_NULL) {
			*has_null = true;
		}
		rc = sqnano_key_append(kb, v, idx->coll[i], idx->desc[i], false);
	}
	if (rc == SQNANO_OK && rk) {
		if (kb->len + rk->len > SQNANO_MAX_KEY_SIZE) {
			return SQNANO_TOOBIG;
		}
		memcpy(kb->b + kb->len, rk->b, rk->len);
		kb->len = (uint16_t)(kb->len + rk->len);
	}
	return rc;
}

static int constraint_error(struct sqnano_exec *x, int rc, const struct sqnano_table *t,
			    const struct sqnano_index *idx)
{
	char *p = x->errmsg;
	size_t cap = sizeof(x->errmsg);
	int n = snprintf(p, cap, "UNIQUE constraint failed: ");

	if (!idx && !t->pk) {
		const char *col = t->ipk >= 0 ? t->cols[t->ipk].name : "rowid";

		(void)snprintf(p + n, cap - (size_t)n, "%s.%s", t->name, col);
		return rc;
	}
	if (!idx) {
		idx = t->pk;
	}
	for (int i = 0; i < idx->ncols && n > 0 && (size_t)n < cap; i++) {
		n += snprintf(p + n, cap - (size_t)n, "%s%s.%s", i ? ", " : "", t->name,
			      t->cols[idx->cols[i]].name);
	}
	return rc;
}

static int delete_row(struct sqnano_exec *x, const struct sqnano_table *t,
		      const struct sqnano_keybuf *rk, const sqnano_value *cols);

/* ---- Foreign keys ---- */

/*
 * As SQLite (fkey.c): a violation increments a counter and a fix decrements
 * it, so that a statement may break and repair a constraint; the counter of
 * the statement must be zero when it ends, the deferred counter of the
 * transaction when it commits. ON DELETE / ON UPDATE actions run after the
 * parent row changed, as nested row operations.
 */
#if SQNANO_FOREIGN_KEYS
#define FKA_COUNT      0
#define FKA_DELETE     1
#define FKA_SETNULL    2
#define FKA_SETDEFAULT 3
#define FKA_UPDATE     4

/* A copy of a key: it outlives the row it was taken from. */
struct fk_key {
	sqnano_value v[SQNANO_FK_MAX_COLS];
	uint8_t buf[SQNANO_MAX_KEY_SIZE];
	uint16_t used;
	bool null;
};

static int row_update(struct sqnano_exec *x, const struct sqnano_table *t, int64_t rowid,
		      const sqnano_value *oldcols, sqnano_value *newcols, const uint8_t *oldrec,
		      uint16_t oldlen, uint8_t oe);
static int load_row(struct sqnano_exec *x, struct sqnano_cursor *tc, const struct sqnano_table *tab,
		    const struct sqnano_keybuf *rk, uint8_t *buf, uint16_t *len, sqnano_value *cols,
		    bool *found);

static int fk_error(struct sqnano_exec *x)
{
	return sqnano_exec_error(x, SQNANO_CONSTRAINT_FOREIGNKEY, "FOREIGN KEY constraint failed");
}

static void fk_count(struct sqnano_exec *x, const struct sqnano_fkey *fk, int32_t n)
{
	if (fk->deferred) {
		x->txn->fk_deferred += n;
	} else {
		x->fk_count += n;
	}
}

static int32_t fk_pending(const struct sqnano_exec *x, const struct sqnano_fkey *fk)
{
	return fk->deferred ? x->txn->fk_deferred : x->fk_count;
}

/* Take the child (or parent) key of a row image. */
static int fk_take(struct sqnano_exec *x, struct fk_key *k, const struct sqnano_fkey *fk,
		   const sqnano_value *row, int64_t rowid, bool parent)
{
	k->used = 0;
	k->null = false;
	for (int i = 0; i < fk->ncols; i++) {
		sqnano_value v;

		if (parent && !fk->pidx) {
			sqnano_v_int(&v, rowid);
		} else {
			v = row[parent ? fk->pcols[i] : fk->ccols[i]];
		}
		if (v.type == SQNANO_NULL) {
			k->null = true;
		}
		if (v.type == SQNANO_TEXT || v.type == SQNANO_BLOB) {
			if (k->used + v.n + 1u > sizeof(k->buf)) {
				return sqnano_exec_error(x, SQNANO_TOOBIG, "foreign key too large");
			}
			memcpy(k->buf + k->used, v.u.p, v.n);
			k->buf[k->used + v.n] = 0;
			v.u.p = k->buf + k->used;
			k->used = (uint16_t)(k->used + v.n + 1);
		}
		k->v[i] = v;
	}
	return SQNANO_OK;
}

static bool fk_same(const struct sqnano_fkey *fk, const struct fk_key *a, const struct fk_key *b)
{
	for (int i = 0; i < fk->ncols; i++) {
		if (a->v[i].type != b->v[i].type ||
		    sqnano_v_compare(&a->v[i], &b->v[i], SQNANO_COLL_BINARY) != 0) {
			return false;
		}
	}
	return true;
}

static uint8_t fk_coll(const struct sqnano_fkey *fk, int i)
{
	return fk->pidx ? fk->pidx->coll[i] : SQNANO_COLL_BINARY;
}

/* Is there a parent row for the child key @p k? */
static int fk_parent_found(struct sqnano_exec *x, const struct sqnano_fkey *fk,
			   const struct fk_key *k, bool *found)
{
	uint16_t mark = x->scratch.used;
	struct sqnano_cursor c;
	struct sqnano_keybuf kb;
	bool exact = false;
	int rc = SQNANO_OK;

	*found = false;
	kb.len = 0;
	for (int i = 0; i < fk->ncols && rc == SQNANO_OK; i++) {
		sqnano_value v = k->v[i];

		/* The affinity of the parent column applies to the child value. */
		rc = sqnano_affinity(x, &v, fk->parent->cols[fk->pcols[i]].aff);
		if (rc) {
			break;
		}
		if (!fk->pidx) {
			if (v.type != SQNANO_INTEGER) {
				x->scratch.used = mark;
				return SQNANO_OK;
			}
			rc = sqnano_key_append_rowid(&kb, v.u.i);
		} else {
			rc = sqnano_key_append(&kb, &v, fk->pidx->coll[i], fk->pidx->desc[i], false);
		}
	}
	x->scratch.used = mark;
	if (rc) {
		return rc == SQNANO_TOOBIG ? sqnano_exec_error(x, rc, "foreign key too large") : rc;
	}
	rc = bt_open(x->txn, &c, fk->pidx ? fk->pidx->tree : fk->parent->tree);
	if (rc == SQNANO_OK) {
		rc = bt_seek(&c, kb.b, kb.len, fk->pidx ? SQNANO_SEEK_GE : SQNANO_SEEK_EQ, &exact);
	}
	if (rc) {
		return rc;
	}
	if (fk->pidx && (fk->pidx->flags & SQNANO_IDX_PRIMARY)) {
		*found = c.state == CUR_VALID && c.klen == kb.len && memcmp(c.key, kb.b, kb.len) == 0;
	} else if (fk->pidx) {
		/* The entry goes on with the row key. */
		*found = c.state == CUR_VALID && c.klen > kb.len && memcmp(c.key, kb.b, kb.len) == 0;
	} else {
		*found = c.state == CUR_VALID && exact;
	}
	return SQNANO_OK;
}

/* Apply an action to the child row @p crow (nested: its own constraints are processed). */
static int fk_apply(struct sqnano_exec *x, const struct sqnano_fkey *fk,
		    const struct sqnano_keybuf *crow, int action, const struct fk_key *newk)
{
	struct {
		uint8_t row[SQNANO_MAX_RECORD_SIZE];
		sqnano_value cols[SQNANO_MAX_COLUMNS];
		sqnano_value ncols[SQNANO_MAX_COLUMNS];
	} f;
	const struct sqnano_table *ct = fk->child;
	struct sqnano_cursor tc;
	int64_t changes = x->changes;
	uint16_t len;
	bool found;
	int rc;

	if (ct->ncols > SQNANO_MAX_COLUMNS) {
		return SQNANO_TOOBIG;
	}
	if (x->fk_depth >= SQNANO_FK_MAX_DEPTH) {
		return sqnano_exec_error(x, SQNANO_ERROR, "foreign key actions nested too deeply "
							 "(CONFIG_SQNANO_FK_MAX_DEPTH)");
	}
	x->fk_depth++;
	rc = load_row(x, &tc, ct, crow, f.row, &len, f.cols, &found);
	if (rc == SQNANO_OK && found) {
		if (action == FKA_DELETE) {
			rc = delete_row(x, ct, crow, f.cols);
		} else {
			int64_t rowid = ct->pk ? 0 : sqnano_key_get_rowid(crow->b);

			memcpy(f.ncols, f.cols, sizeof(sqnano_value) * ct->ncols);
			for (int i = 0; i < fk->ncols; i++) {
				const struct sqnano_column *col = &ct->cols[fk->ccols[i]];

				if (action == FKA_SETNULL) {
					sqnano_v_null(&f.ncols[fk->ccols[i]]);
				} else if (action == FKA_SETDEFAULT) {
					f.ncols[fk->ccols[i]] = col->dflt;
				} else {
					f.ncols[fk->ccols[i]] = newk->v[i];
				}
			}
			rc = row_update(x, ct, rowid, f.cols, f.ncols, f.row, len, SQNANO_OE_ABORT);
		}
	}
	x->fk_depth--;
	x->changes = changes;
	return rc;
}

/*
 * Visit the child rows that reference the parent key @p k (except the row
 * @p self of a self-referencing table), counting them in @p n and applying
 * @p action to each.
 */
static int fk_children(struct sqnano_exec *x, const struct sqnano_fkey *fk, const struct fk_key *k,
		       const struct sqnano_keybuf *self, int action, const struct fk_key *newk,
		       int32_t *n)
{
	const struct sqnano_table *ct = fk->child;
	bool selfref = ct == fk->parent;
	uint16_t mark = x->scratch.used;
	struct sqnano_cursor c;
	int rc = SQNANO_OK;

	*n = 0;
	if (fk->cidx) {
		struct sqnano_keybuf kb;

		kb.len = 0;
		for (int j = 0; j < fk->ncols && rc == SQNANO_OK; j++) {
			sqnano_value v = k->v[fk->cpos[j]];

			rc = sqnano_affinity(x, &v, ct->cols[fk->cidx->cols[j]].aff);
			if (rc == SQNANO_OK) {
				rc = sqnano_key_append(&kb, &v, fk->cidx->coll[j], fk->cidx->desc[j],
						       false);
			}
		}
		x->scratch.used = mark;
		if (rc) {
			return rc == SQNANO_TOOBIG ? sqnano_exec_error(x, rc, "foreign key too large")
						   : rc;
		}
		rc = bt_open(x->txn, &c, fk->cidx->tree);
		if (rc == SQNANO_OK) {
			rc = bt_seek(&c, kb.b, kb.len, SQNANO_SEEK_GE, NULL);
		}
		/* The index may have more columns than the key; its entries end with the row key. */
		while (rc == SQNANO_OK && c.state == CUR_VALID && c.klen > kb.len &&
		       memcmp(c.key, kb.b, kb.len) == 0) {
			struct sqnano_keybuf r;

			rc = entry_row_key(ct, fk->cidx, c.key, c.klen, &r);
			if (rc == SQNANO_OK && (!selfref || !rk_eq(&r, self))) {
				(*n)++;
				if (action != FKA_COUNT) {
					rc = fk_apply(x, fk, &r, action, newk);
				}
			}
			if (rc == SQNANO_OK) {
				rc = bt_next(&c);
			}
		}
		return rc;
	}
	/* No index on the child key: compare every row. */
	rc = bt_open(x->txn, &c, ct->tree);
	if (rc == SQNANO_OK) {
		rc = bt_seek(&c, NULL, 0, SQNANO_SEEK_FIRST, NULL);
	}
	while (rc == SQNANO_OK && c.state == CUR_VALID) {
		uint8_t rec[SQNANO_MAX_RECORD_SIZE];
		struct sqnano_keybuf r;
		bool match;
		uint16_t len;

		rk_set(&r, c.key, c.klen);
		match = !selfref || !rk_eq(&r, self);

		rc = bt_value(&c, rec, sizeof(rec), &len);
		for (int i = 0; i < fk->ncols && match && rc == SQNANO_OK; i++) {
			const struct sqnano_column *col = &ct->cols[fk->ccols[i]];
			sqnano_value v;

			if (fk->ccols[i] == ct->ipk) {
				sqnano_v_int(&v, sqnano_key_get_rowid(r.b));
			} else if (sqnano_record_get(rec, len, col->slot, &v) == SQNANO_NOTFOUND) {
				v = col->dflt;
			}
			if (v.type == SQNANO_NULL) {
				match = false;
				break;
			}
			rc = sqnano_affinity(x, &v, fk->parent->cols[fk->pcols[i]].aff);
			match = rc == SQNANO_OK && v.type == k->v[i].type &&
				sqnano_v_compare(&v, &k->v[i], fk_coll(fk, i)) == 0;
		}
		x->scratch.used = mark;
		if (rc == SQNANO_OK && match) {
			(*n)++;
			if (action != FKA_COUNT) {
				rc = fk_apply(x, fk, &r, action, newk);
			}
		}
		if (rc == SQNANO_OK) {
			rc = bt_next(&c);
		}
	}
	return rc;
}

static int fk_action_of(uint8_t a)
{
	switch (a) {
	case SQNANO_FK_CASCADE:
		return FKA_DELETE;
	case SQNANO_FK_SETNULL:
		return FKA_SETNULL;
	case SQNANO_FK_SETDEFAULT:
		return FKA_SETDEFAULT;
	default:
		return FKA_COUNT;
	}
}

/* A row is about to leave table @p t (DELETE, REPLACE). */
static int fk_delete_before(struct sqnano_exec *x, const struct sqnano_table *t,
			    const struct sqnano_keybuf *rk, const sqnano_value *old)
{
	int64_t rowid = rk_rowid(t, rk);
	struct fk_key k;
	int rc = SQNANO_OK;

	for (int i = 0; i < t->nfks && rc == SQNANO_OK; i++) {
		const struct sqnano_fkey *fk = t->fks[i];
		bool found;
		int32_t n;

		if (fk->child == t) {
			rc = fk_take(x, &k, fk, old, rowid, false);
			if (rc == SQNANO_OK && !k.null) {
				rc = fk_parent_found(x, fk, &k, &found);
				if (rc == SQNANO_OK && !found) {
					fk_count(x, fk, -1); /* an orphan goes away */
				}
			}
		}
		if (fk->parent == t && rc == SQNANO_OK) {
			rc = fk_take(x, &k, fk, old, rowid, true);
			if (rc == SQNANO_OK && !k.null) {
				rc = fk_children(x, fk, &k, rk, FKA_COUNT, NULL, &n);
				if (rc == SQNANO_OK && n && fk->on_delete == SQNANO_FK_RESTRICT) {
					rc = fk_error(x);
				}
				fk_count(x, fk, n);
			}
		}
	}
	return rc;
}

static int fk_delete_after(struct sqnano_exec *x, const struct sqnano_table *t,
			   const struct sqnano_keybuf *rk, const sqnano_value *old)
{
	int64_t rowid = rk_rowid(t, rk);
	struct fk_key k;
	int rc = SQNANO_OK;

	for (int i = 0; i < t->nfks && rc == SQNANO_OK; i++) {
		const struct sqnano_fkey *fk = t->fks[i];
		int action = fk_action_of(fk->on_delete);
		int32_t n;

		if (fk->parent != t || action == FKA_COUNT) {
			continue;
		}
		rc = fk_take(x, &k, fk, old, rowid, true);
		if (rc == SQNANO_OK && !k.null) {
			rc = fk_children(x, fk, &k, rk, action, NULL, &n);
		}
	}
	return rc;
}

/* A row was added to table @p t. */
static int fk_insert_after(struct sqnano_exec *x, const struct sqnano_table *t,
			   const struct sqnano_keybuf *rk, const sqnano_value *row)
{
	int64_t rowid = rk_rowid(t, rk);
	struct fk_key k;
	int rc = SQNANO_OK;

	for (int i = 0; i < t->nfks && rc == SQNANO_OK; i++) {
		const struct sqnano_fkey *fk = t->fks[i];
		bool found;
		int32_t n;

		if (fk->parent == t && fk_pending(x, fk) > 0) {
			/* New parent key: children inserted earlier stop being orphans. */
			rc = fk_take(x, &k, fk, row, rowid, true);
			if (rc == SQNANO_OK && !k.null) {
				rc = fk_children(x, fk, &k, rk, FKA_COUNT, NULL, &n);
				fk_count(x, fk, -n);
			}
		}
		if (fk->child == t && rc == SQNANO_OK) {
			rc = fk_take(x, &k, fk, row, rowid, false);
			if (rc == SQNANO_OK && !k.null) {
				rc = fk_parent_found(x, fk, &k, &found);
				if (rc == SQNANO_OK && !found) {
					fk_count(x, fk, 1);
				}
			}
		}
	}
	return rc;
}

static int fk_update_before(struct sqnano_exec *x, const struct sqnano_table *t,
			    const struct sqnano_keybuf *ro, const struct sqnano_keybuf *rn,
			    const sqnano_value *old, const sqnano_value *new)
{
	int64_t rowid = rk_rowid(t, ro), newid = rk_rowid(t, rn);
	struct fk_key ko, kn;
	int rc = SQNANO_OK;

	for (int i = 0; i < t->nfks && rc == SQNANO_OK; i++) {
		const struct sqnano_fkey *fk = t->fks[i];
		bool found;
		int32_t n;

		if (fk->child == t) {
			rc = fk_take(x, &ko, fk, old, rowid, false);
			if (rc == SQNANO_OK) {
				rc = fk_take(x, &kn, fk, new, newid, false);
			}
			if (rc == SQNANO_OK && !ko.null && !fk_same(fk, &ko, &kn)) {
				rc = fk_parent_found(x, fk, &ko, &found);
				if (rc == SQNANO_OK && !found) {
					fk_count(x, fk, -1);
				}
			}
		}
		if (fk->parent == t && rc == SQNANO_OK) {
			rc = fk_take(x, &ko, fk, old, rowid, true);
			if (rc == SQNANO_OK) {
				rc = fk_take(x, &kn, fk, new, newid, true);
			}
			if (rc || fk_same(fk, &ko, &kn)) {
				continue;
			}
			if (!ko.null) {
				rc = fk_children(x, fk, &ko, ro, FKA_COUNT, NULL, &n);
				if (rc == SQNANO_OK && n && fk->on_update == SQNANO_FK_RESTRICT) {
					rc = fk_error(x);
				}
				fk_count(x, fk, n);
			}
			if (rc == SQNANO_OK && !kn.null && fk_pending(x, fk) > 0) {
				rc = fk_children(x, fk, &kn, ro, FKA_COUNT, NULL, &n);
				fk_count(x, fk, -n);
			}
		}
	}
	return rc;
}

static int fk_update_after(struct sqnano_exec *x, const struct sqnano_table *t,
			   const struct sqnano_keybuf *ro, const struct sqnano_keybuf *rn,
			   const sqnano_value *old, const sqnano_value *new)
{
	int64_t rowid = rk_rowid(t, ro), newid = rk_rowid(t, rn);
	struct fk_key ko, kn;
	int rc = SQNANO_OK;

	for (int i = 0; i < t->nfks && rc == SQNANO_OK; i++) {
		const struct sqnano_fkey *fk = t->fks[i];
		bool found;
		int32_t n;

		if (fk->child == t) {
			rc = fk_take(x, &ko, fk, old, rowid, false);
			if (rc == SQNANO_OK) {
				rc = fk_take(x, &kn, fk, new, newid, false);
			}
			if (rc == SQNANO_OK && !kn.null && !fk_same(fk, &ko, &kn)) {
				rc = fk_parent_found(x, fk, &kn, &found);
				if (rc == SQNANO_OK && !found) {
					fk_count(x, fk, 1);
				}
			}
		}
		if (fk->parent == t && rc == SQNANO_OK) {
			int action = fk->on_update == SQNANO_FK_CASCADE ? FKA_UPDATE
								      : fk_action_of(fk->on_update);

			if (action == FKA_COUNT) {
				continue;
			}
			rc = fk_take(x, &ko, fk, old, rowid, true);
			if (rc == SQNANO_OK) {
				rc = fk_take(x, &kn, fk, new, newid, true);
			}
			if (rc == SQNANO_OK && !ko.null && !fk_same(fk, &ko, &kn)) {
				rc = fk_children(x, fk, &ko, rn, action, &kn, &n);
			}
		}
	}
	return rc;
}

#define FK_ACTIVE(x, t) ((x)->db->fk_on && (t)->nfks)
#else
#define FK_ACTIVE(x, t) false
#define fk_delete_before(x, t, r, o)       SQNANO_OK
#define fk_delete_after(x, t, r, o)        SQNANO_OK
#define fk_insert_after(x, t, r, o)        SQNANO_OK
#define fk_update_before(x, t, r, n, o, w) SQNANO_OK
#define fk_update_after(x, t, r, n, o, w)  SQNANO_OK
#endif

/* Delete the row of key @p rk, whose decoded columns are @p cols. */
static int delete_row(struct sqnano_exec *x, const struct sqnano_table *t,
		      const struct sqnano_keybuf *rk, const sqnano_value *cols)
{
	struct sqnano_txn *txn = x->txn;
	struct sqnano_cursor c;
	struct sqnano_keybuf kb;
	bool exact, has_null;
	int rc;

	if (FK_ACTIVE(x, t)) {
		rc = fk_delete_before(x, t, rk, cols);
		if (rc) {
			return rc;
		}
	}
	for (int i = 0; i < t->nindexes; i++) {
		const struct sqnano_index *idx = t->indexes[i];

		rc = index_key(&kb, idx, cols, rk, &has_null);
		if (rc == SQNANO_OK) {
			rc = bt_open(txn, &c, idx->tree);
		}
		if (rc == SQNANO_OK) {
			rc = bt_seek(&c, kb.b, kb.len, SQNANO_SEEK_EQ, &exact);
		}
		if (rc == SQNANO_OK && c.state == CUR_VALID) {
			rc = bt_delete(&c);
		}
		if (rc) {
			return rc;
		}
	}
	rc = bt_open(txn, &c, t->tree);
	if (rc == SQNANO_OK) {
		rc = bt_seek(&c, rk->b, rk->len, SQNANO_SEEK_EQ, &exact);
	}
	if (rc == SQNANO_OK && c.state == CUR_VALID) {
		rc = bt_delete(&c);
	}
	if (rc == SQNANO_OK && FK_ACTIVE(x, t)) {
		rc = fk_delete_after(x, t, rk, cols);
	}
	return rc;
}

/* Delete a conflicting row identified by its key (REPLACE). */
static int replace_row(struct sqnano_exec *x, const struct sqnano_table *t,
		       const struct sqnano_keybuf *rk)
{
	struct sqnano_db *db = x->db;
	struct sqnano_cursor c;
	uint16_t len;
	bool found;
	int rc;

	if (t->ncols > SQNANO_MAX_COLUMNS) {
		return SQNANO_TOOBIG;
	}
	if (FK_ACTIVE(x, t)) {
		/* Foreign key actions may reuse the shared row buffers. */
		uint8_t row[SQNANO_MAX_RECORD_SIZE];
		sqnano_value cols[SQNANO_MAX_COLUMNS];

		rc = load_row(x, &c, t, rk, row, &len, cols, &found);
		if (rc || !found) {
			return rc;
		}
		return delete_row(x, t, rk, cols);
	}
	rc = load_row(x, &c, t, rk, db->rowbuf, &len, db->rowvals, &found);
	if (rc || !found) {
		return rc;
	}
	return delete_row(x, t, rk, db->rowvals);
}

static int prepare_row(struct sqnano_exec *x, const struct sqnano_table *t, sqnano_value *row,
		       uint8_t oe, bool *skip)
{
	int rc;

	*skip = false;
	for (int c = 0; c < t->ncols; c++) {
		rc = sqnano_affinity(x, &row[c], t->cols[c].aff);
		if (rc) {
			return rc;
		}
	}
	for (int c = 0; c < t->ncols; c++) {
		const struct sqnano_column *col = &t->cols[c];

		if (c == t->ipk || !(col->flags & SQNANO_COL_NOTNULL) || row[c].type != SQNANO_NULL) {
			continue;
		}
		if (oe == SQNANO_OE_REPLACE && (col->flags & SQNANO_COL_HAS_DEFAULT) &&
		    col->dflt.type != SQNANO_NULL) {
			row[c] = col->dflt;
			continue;
		}
		if (oe == SQNANO_OE_IGNORE) {
			*skip = true;
			return SQNANO_OK;
		}
		return sqnano_exec_error(x, SQNANO_CONSTRAINT_NOTNULL, "NOT NULL constraint failed: %s.%s",
					 t->name, col->name);
	}
	if (t->check) {
		rc = t->check(x, row);
		if (rc == SQNANO_CONSTRAINT_CHECK && oe == SQNANO_OE_IGNORE) {
			*skip = true;
			return SQNANO_OK;
		}
		if (rc) {
			return rc;
		}
	}
	return SQNANO_OK;
}

/* Remember the row an UPSERT conflicts with. */
static int conflict(struct sqnano_exec *x, const struct sqnano_table *t,
		    const struct sqnano_keybuf *rk)
{
	x->conflict_rowid = rk_rowid(t, rk);
	x->db->conflict_key = *rk;
	return SQNANO_CONFLICT;
}

/*
 * Check the PRIMARY KEY and UNIQUE constraints of a new row image of key
 * @p rk. @p self is the key of the row being updated (NULL for an insert).
 */
static int check_unique(struct sqnano_exec *x, const struct sqnano_table *t, const sqnano_value *row,
			const struct sqnano_keybuf *rk, const struct sqnano_keybuf *self, uint8_t oe,
			const struct sqnano_index *target, bool *skip)
{
	struct sqnano_txn *txn = x->txn;
	struct sqnano_cursor c;
	int rc;

	*skip = false;
	if (!self || !rk_eq(rk, self)) {
		bool exact;

		rc = bt_open(txn, &c, t->tree);
		if (rc == SQNANO_OK) {
			rc = bt_seek(&c, rk->b, rk->len, SQNANO_SEEK_EQ, &exact);
		}
		if (rc) {
			return rc;
		}
		if (c.state == CUR_VALID) {
			if (oe == SQNANO_OE_UPSERT &&
			    (target == NULL || target == &sqnano_rowid_target || target == t->pk)) {
				return conflict(x, t, rk);
			}
			if (oe == SQNANO_OE_IGNORE) {
				*skip = true;
				return SQNANO_OK;
			}
			if (oe == SQNANO_OE_REPLACE) {
				rc = replace_row(x, t, rk);
				if (rc) {
					return rc;
				}
			} else {
				return constraint_error(x, SQNANO_CONSTRAINT_PRIMARYKEY, t, NULL);
			}
		}
	}
	for (int i = 0; i < t->nindexes; i++) {
		const struct sqnano_index *idx = t->indexes[i];
		struct sqnano_keybuf kb, other;
		bool has_null;

		if (!(idx->flags & SQNANO_IDX_UNIQUE)) {
			continue;
		}
		rc = index_key(&kb, idx, row, NULL, &has_null);
		if (rc) {
			return rc == SQNANO_TOOBIG ? sqnano_exec_error(x, rc, "index key too large") : rc;
		}
		if (has_null) {
			continue;
		}
		for (;;) {
			rc = bt_open(txn, &c, idx->tree);
			if (rc == SQNANO_OK) {
				rc = bt_seek(&c, kb.b, kb.len, SQNANO_SEEK_GE, NULL);
			}
			if (rc) {
				return rc;
			}
			/* Entries are the columns then the row key. */
			if (c.state != CUR_VALID || c.klen <= kb.len || memcmp(c.key, kb.b, kb.len) != 0) {
				break;
			}
			rk_set(&other, c.key + kb.len, (uint16_t)(c.klen - kb.len));
			if (self && rk_eq(&other, self)) {
				/* The row's own entry; look past it. */
				rc = bt_next(&c);
				if (rc) {
					return rc;
				}
				if (c.state != CUR_VALID || c.klen <= kb.len ||
				    memcmp(c.key, kb.b, kb.len) != 0) {
					break;
				}
				rk_set(&other, c.key + kb.len, (uint16_t)(c.klen - kb.len));
			}
			if (oe == SQNANO_OE_UPSERT && (target == NULL || target == idx)) {
				return conflict(x, t, &other);
			}
			if (oe == SQNANO_OE_IGNORE) {
				*skip = true;
				return SQNANO_OK;
			}
			if (oe != SQNANO_OE_REPLACE) {
				return constraint_error(x, SQNANO_CONSTRAINT_UNIQUE, t, idx);
			}
			rc = replace_row(x, t, &other);
			if (rc) {
				return rc;
			}
		}
	}
	return SQNANO_OK;
}

/*
 * Build the record of @p row. Slots unknown to this firmware (at or beyond
 * t->nslots) are copied from @p old so that a newer schema keeps its data.
 */
static int build_record(struct sqnano_exec *x, const struct sqnano_table *t, const sqnano_value *row,
			const uint8_t *old, uint16_t oldlen, uint16_t *len)
{
	struct sqnano_db *db = x->db;
	int nold = old ? sqnano_record_count(old, oldlen) : 0;
	int nslots = t->nslots > nold ? t->nslots : nold;
	sqnano_value *vals = db->rowvals;
	int rc;

	if (nslots > SQNANO_MAX_COLUMNS) {
		return sqnano_exec_error(x, SQNANO_TOOBIG, "too many columns");
	}
	for (int s = 0; s < nslots; s++) {
		sqnano_v_null(&vals[s]);
		if (s >= t->nslots && old) {
			(void)sqnano_record_get(old, oldlen, (uint16_t)s, &vals[s]);
		}
	}
	for (int c = 0; c < t->ncols; c++) {
		sqnano_value *v = &vals[t->cols[c].slot];

		if (c == t->ipk) {
			continue;
		}
		*v = row[c];
		if (t->cols[c].aff == SQNANO_AFF_REAL && v->type == SQNANO_FLOAT &&
		    v->u.r >= -9223372036854775808.0 && v->u.r < 9223372036854775808.0 &&
		    (double)(int64_t)v->u.r == v->u.r) {
			/* Smaller record; read back as a real (sqnano_row_decode). */
			sqnano_v_int(v, (int64_t)v->u.r);
		}
	}
	/* The version of the catalog: lazy migration steps up to it are applied. */
	rc = sqnano_record_build(db->recbuf, sizeof(db->recbuf), vals, (uint16_t)nslots,
				 t->version ? t->version : db->sb.schema_version, len);
	if (rc == SQNANO_TOOBIG) {
		return sqnano_exec_error(x, rc, "row too large (CONFIG_SQNANO_MAX_RECORD_SIZE)");
	}
	return rc;
}

static int insert_indexes(struct sqnano_exec *x, const struct sqnano_table *t, const sqnano_value *row,
			  const struct sqnano_keybuf *rk)
{
	struct sqnano_cursor c;
	struct sqnano_keybuf kb;
	bool has_null;
	int rc;

	for (int i = 0; i < t->nindexes; i++) {
		const struct sqnano_index *idx = t->indexes[i];

		rc = index_key(&kb, idx, row, rk, &has_null);
		if (rc == SQNANO_OK) {
			rc = bt_open(x->txn, &c, idx->tree);
		}
		if (rc == SQNANO_OK) {
			rc = bt_insert(&c, kb.b, kb.len, NULL, 0);
		}
		if (rc) {
			return rc == SQNANO_TOOBIG ? sqnano_exec_error(x, rc, "index key too large") : rc;
		}
	}
	return SQNANO_OK;
}

/* AUTOINCREMENT high-water mark, kept in the master tree under 0x8000 | tree. */
static int seq_get(struct sqnano_txn *txn, const struct sqnano_table *t, int64_t *seq)
{
	struct sqnano_cursor c;
	uint8_t k[2], v[8];
	uint16_t vl;
	bool exact;
	int rc;

	*seq = 0;
	put_be16(k, (uint16_t)(0x8000u | t->tree));
	rc = bt_open(txn, &c, 0);
	if (rc == SQNANO_OK) {
		rc = bt_seek(&c, k, 2, SQNANO_SEEK_EQ, &exact);
	}
	if (rc || c.state != CUR_VALID) {
		return rc;
	}
	rc = bt_value(&c, v, sizeof(v), &vl);
	if (rc == SQNANO_OK && vl == 8) {
		*seq = sqnano_key_get_rowid(v);
	}
	return rc;
}

static int seq_put(struct sqnano_txn *txn, const struct sqnano_table *t, int64_t seq)
{
	struct sqnano_cursor c;
	uint8_t k[2], v[8];
	int rc;

	put_be16(k, (uint16_t)(0x8000u | t->tree));
	sqnano_key_rowid(v, seq);
	rc = bt_open(txn, &c, 0);
	return rc ? rc : bt_insert(&c, k, 2, v, 8);
}

static int row_insert(struct sqnano_exec *x, const struct sqnano_table *t, sqnano_value *row,
		      uint8_t oe, const struct sqnano_index *target)
{
	int64_t seq = 0;
	struct sqnano_txn *txn = x->txn;
	struct sqnano_cursor c;
	struct sqnano_keybuf rk;
	int64_t rowid = 0;
	uint16_t len;
	bool skip;
	int rc;

	if (!txn->write) {
		return sqnano_exec_error(x, SQNANO_READONLY, "attempt to write a readonly database");
	}
	rc = pg_compact_txn(txn);
	if (rc) {
		return rc;
	}
	rc = prepare_row(x, t, row, oe, &skip);
	if (rc || skip) {
		return rc;
	}
	if (t->flags & SQNANO_TBL_AUTOINC) {
		rc = seq_get(txn, t, &seq);
		if (rc) {
			return rc;
		}
	}
	if (t->pk) {
		/* WITHOUT ROWID: the key is the PRIMARY KEY (NOT NULL, see prepare_row). */
	} else if (t->ipk >= 0 && row[t->ipk].type != SQNANO_NULL) {
		if (row[t->ipk].type != SQNANO_INTEGER) {
			return sqnano_exec_error(x, SQNANO_MISMATCH, "datatype mismatch");
		}
		rowid = row[t->ipk].u.i;
	} else {
		rc = bt_open(txn, &c, t->tree);
		if (rc == SQNANO_OK) {
			rc = bt_seek(&c, NULL, 0, SQNANO_SEEK_LAST, NULL);
		}
		if (rc) {
			return rc;
		}
		rowid = 1;
		if (c.state == CUR_VALID) {
			rowid = sqnano_key_get_rowid(c.key);
			if (rowid == INT64_MAX) {
				return sqnano_exec_error(x, SQNANO_FULL, "rowid space exhausted");
			}
			rowid = rowid < 0 ? 1 : rowid + 1;
		}
		if ((t->flags & SQNANO_TBL_AUTOINC) && rowid <= seq) {
			if (seq == INT64_MAX) {
				return sqnano_exec_error(x, SQNANO_FULL, "database or disk is full");
			}
			rowid = seq + 1;
		}
		if (t->ipk >= 0) {
			sqnano_v_int(&row[t->ipk], rowid);
		}
	}
	rc = row_key(t, rowid, row, &rk);
	if (rc) {
		return rc == SQNANO_TOOBIG ? sqnano_exec_error(x, rc, "PRIMARY KEY too large") : rc;
	}
	rc = check_unique(x, t, row, &rk, NULL, oe, target, &skip);
	if (rc || skip) {
		return rc;
	}
	rc = build_record(x, t, row, NULL, 0, &len);
	if (rc) {
		return rc;
	}
	rc = bt_open(txn, &c, t->tree);
	if (rc == SQNANO_OK) {
		rc = bt_insert(&c, rk.b, rk.len, x->db->recbuf, len);
	}
	if (rc == SQNANO_OK) {
		rc = insert_indexes(x, t, row, &rk);
	}
	if (rc == SQNANO_OK && (t->flags & SQNANO_TBL_AUTOINC) && rowid > seq) {
		rc = seq_put(txn, t, rowid);
	}
	if (rc == SQNANO_OK && FK_ACTIVE(x, t)) {
		rc = fk_insert_after(x, t, &rk, row);
	}
	if (rc) {
		return rc;
	}
	x->changes++;
	if (!t->pk) {
		/* As SQLite: inserts into WITHOUT ROWID tables leave it alone. */
		x->last_rowid = rowid;
	}
	return SQNANO_OK;
}

int sqnano_row_delete(struct sqnano_exec *x, const struct sqnano_table *t, int64_t rowid,
		      const sqnano_value *cols)
{
	struct sqnano_keybuf rk;
	int rc;

	if (!x->txn->write) {
		return sqnano_exec_error(x, SQNANO_READONLY, "attempt to write a readonly database");
	}
	rc = pg_compact_txn(x->txn);
	if (rc == SQNANO_OK) {
		rc = row_key(t, rowid, cols, &rk);
	}
	if (rc == SQNANO_OK) {
		rc = delete_row(x, t, &rk, cols);
	}
	if (rc == SQNANO_OK) {
		x->changes++;
	}
	return rc;
}

static int row_update(struct sqnano_exec *x, const struct sqnano_table *t, int64_t rowid,
		      const sqnano_value *oldcols, sqnano_value *newcols, const uint8_t *oldrec,
		      uint16_t oldlen, uint8_t oe)
{
	struct sqnano_txn *txn = x->txn;
	struct sqnano_cursor c;
	struct sqnano_keybuf ko, kn, ro, rn;
	int64_t newid = rowid;
	uint16_t len;
	bool skip, hn;
	int rc;

	if (!txn->write) {
		return sqnano_exec_error(x, SQNANO_READONLY, "attempt to write a readonly database");
	}
	rc = pg_compact_txn(txn);
	if (rc) {
		return rc;
	}
	rc = row_key(t, rowid, oldcols, &ro);
	if (rc) {
		return rc;
	}
	rc = prepare_row(x, t, newcols, oe, &skip);
	if (rc || skip) {
		return rc;
	}
	if (t->ipk >= 0) {
		if (newcols[t->ipk].type != SQNANO_INTEGER) {
			return sqnano_exec_error(x, SQNANO_MISMATCH, "datatype mismatch");
		}
		newid = newcols[t->ipk].u.i;
	}
	rc = row_key(t, newid, newcols, &rn);
	if (rc) {
		return rc == SQNANO_TOOBIG ? sqnano_exec_error(x, rc, "PRIMARY KEY too large") : rc;
	}
	rc = check_unique(x, t, newcols, &rn, &ro, oe == SQNANO_OE_UPSERT ? SQNANO_OE_ABORT : oe,
			  NULL, &skip);
	if (rc || skip) {
		return rc;
	}
	if (FK_ACTIVE(x, t)) {
		rc = fk_update_before(x, t, &ro, &rn, oldcols, newcols);
		if (rc) {
			return rc;
		}
	}
	/* The old record is overwritten below: keep the slots to preserve. */
	rc = build_record(x, t, newcols, oldrec, oldlen, &len);
	if (rc) {
		return rc;
	}
	for (int i = 0; i < t->nindexes; i++) {
		const struct sqnano_index *idx = t->indexes[i];

		rc = index_key(&ko, idx, oldcols, &ro, &hn);
		if (rc == SQNANO_OK) {
			rc = index_key(&kn, idx, newcols, &rn, &hn);
		}
		if (rc) {
			return rc == SQNANO_TOOBIG ? sqnano_exec_error(x, rc, "index key too large") : rc;
		}
		if (ko.len == kn.len && memcmp(ko.b, kn.b, ko.len) == 0) {
			continue;
		}
		rc = bt_open(txn, &c, idx->tree);
		if (rc == SQNANO_OK) {
			bool exact;

			rc = bt_seek(&c, ko.b, ko.len, SQNANO_SEEK_EQ, &exact);
			if (rc == SQNANO_OK && c.state == CUR_VALID) {
				rc = bt_delete(&c);
			}
		}
		if (rc == SQNANO_OK) {
			rc = bt_insert(&c, kn.b, kn.len, NULL, 0);
		}
		if (rc) {
			return rc;
		}
	}
	rc = bt_open(txn, &c, t->tree);
	if (rc == SQNANO_OK && !rk_eq(&ro, &rn)) {
		bool exact;

		rc = bt_seek(&c, ro.b, ro.len, SQNANO_SEEK_EQ, &exact);
		if (rc == SQNANO_OK && c.state == CUR_VALID) {
			rc = bt_delete(&c);
		}
	}
	if (rc == SQNANO_OK) {
		rc = bt_insert(&c, rn.b, rn.len, x->db->recbuf, len);
	}
	if (rc == SQNANO_OK && FK_ACTIVE(x, t)) {
		rc = fk_update_after(x, t, &ro, &rn, oldcols, newcols);
	}
	if (rc == SQNANO_OK) {
		x->changes++;
	}
	return rc;
}

/* Store the record @p rec of key @p rk again, converted (lazy migrations). Lock held. */
int sqnano_row_rewrite(struct sqnano_exec *x, const struct sqnano_table *t,
		       const struct sqnano_keybuf *rk, const uint8_t *rec, uint16_t len)
{
	sqnano_value cols[SQNANO_MAX_COLUMNS];
	struct sqnano_cursor c;
	uint16_t nl;
	int rc;

	if (t->ncols > SQNANO_MAX_COLUMNS) {
		return SQNANO_TOOBIG;
	}
	rc = sqnano_row_decode(x, t, rk_rowid(t, rk), rec, len, cols);
	if (rc == SQNANO_OK) {
		/* Indexed columns are never converted: only the record changes. */
		rc = build_record(x, t, cols, rec, len, &nl);
	}
	if (rc == SQNANO_OK) {
		rc = bt_open(x->txn, &c, t->tree);
	}
	if (rc == SQNANO_OK) {
		rc = bt_insert(&c, rk->b, rk->len, x->db->recbuf, nl);
	}
	return rc;
}

/* Build index @p idx from the rows of @p t (migrations). Lock held. */
/* Remember the conflict resolution of a failed constraint (FAIL, ROLLBACK). */
static int note_conflict(struct sqnano_exec *x, int rc, uint8_t oe)
{
	if ((rc & 0xff) == SQNANO_CONSTRAINT) {
		x->on_error = oe;
	}
	return rc;
}

int sqnano_row_insert(struct sqnano_exec *x, const struct sqnano_table *t, sqnano_value *row,
		      uint8_t oe, const struct sqnano_index *target)
{
	return note_conflict(x, row_insert(x, t, row, oe, target), oe);
}

int sqnano_row_update(struct sqnano_exec *x, const struct sqnano_table *t, int64_t rowid,
		      const sqnano_value *oldcols, sqnano_value *newcols, const uint8_t *oldrec,
		      uint16_t oldlen, uint8_t oe)
{
	return note_conflict(x, row_update(x, t, rowid, oldcols, newcols, oldrec, oldlen, oe), oe);
}

int sqnano_index_build(struct sqnano_exec *x, const struct sqnano_table *t,
		       const struct sqnano_index *idx)
{
	struct sqnano_db *db = x->db;
	struct sqnano_cursor tc, ic;
	struct sqnano_keybuf kb;
	uint16_t len;
	bool has_null;
	int rc;

	if (t->ncols > SQNANO_MAX_COLUMNS) {
		return SQNANO_TOOBIG;
	}
	rc = bt_open(x->txn, &tc, t->tree);
	if (rc == SQNANO_OK) {
		rc = bt_seek(&tc, NULL, 0, SQNANO_SEEK_FIRST, NULL);
	}
	while (rc == SQNANO_OK && tc.state == CUR_VALID) {
		struct sqnano_keybuf rk;

		rk_set(&rk, tc.key, tc.klen);
		rc = bt_value(&tc, db->rowbuf, sizeof(db->rowbuf), &len);
		if (rc == SQNANO_OK) {
			rc = sqnano_row_decode(x, t, rk_rowid(t, &rk), db->rowbuf, len, db->rowvals);
		}
		if (rc == SQNANO_OK && (idx->flags & SQNANO_IDX_UNIQUE)) {
			rc = index_key(&kb, idx, db->rowvals, NULL, &has_null);
			if (rc == SQNANO_OK && !has_null) {
				rc = bt_open(x->txn, &ic, idx->tree);
				if (rc == SQNANO_OK) {
					rc = bt_seek(&ic, kb.b, kb.len, SQNANO_SEEK_GE, NULL);
				}
				if (rc == SQNANO_OK && ic.state == CUR_VALID && ic.klen > kb.len &&
				    memcmp(ic.key, kb.b, kb.len) == 0) {
					rc = constraint_error(x, SQNANO_CONSTRAINT_UNIQUE, t, idx);
				}
			}
		}
		if (rc == SQNANO_OK) {
			rc = index_key(&kb, idx, db->rowvals, &rk, &has_null);
		}
		if (rc == SQNANO_OK) {
			rc = bt_open(x->txn, &ic, idx->tree);
		}
		if (rc == SQNANO_OK) {
			rc = bt_insert(&ic, kb.b, kb.len, NULL, 0);
		}
		if (rc == SQNANO_OK) {
			rc = bt_next(&tc);
		}
	}
	return rc;
}

/* ---- Sorter ---- */

/*
 * Entries are [u16 key length][u16 record length][key][record]. In RAM they
 * are appended from the start of the buffer, and an array of 16-bit offsets
 * grows down from its end. When the buffer is full, the external sort sorts
 * the entries and writes them as a run: a chain of temporary pages, each
 * [header][u32 next page][entry bytes], an entry possibly continuing on the
 * next page. The runs are then merged with one page of each in the buffer;
 * when they are too many for that, groups of them are merged into new runs
 * first. Runs keep the order of insertion and the merge prefers the earlier
 * run, so the sort stays stable.
 */

#define SORT_CAP ((uint16_t)(SQNANO_SORT_RAM_SIZE & ~3u))

static uint32_t sort_pool[SQNANO_SORT_BUFFERS][(SQNANO_SORT_RAM_SIZE + 3) / 4];
static uint8_t sort_busy[SQNANO_SORT_BUFFERS];
static SQNANO_MUTEX_DEFINE(sort_lock);

#if SQNANO_SORT_MAX_RUNS > 0
#define RUN_DATA (SQNANO_PAGE_HDR + 4)

struct sort_run {
	uint32_t first;
	uint32_t count;
};

/* A run being merged; page and head are offsets in the sort buffer. */
struct sort_in {
	uint32_t pgno;
	uint32_t left;
	uint16_t page;
	uint16_t head;
	uint16_t off;
	uint8_t valid;
};

struct sort_ext {
	struct sort_run runs[SQNANO_SORT_MAX_RUNS];
	/* Temporary pages owned, freed at close at the latest. */
	uint32_t pages[SQNANO_BM_WORDS];
	uint16_t maxent;
	uint16_t hold;
	uint8_t nruns;
	uint8_t nin;
	int8_t last;
	uint8_t merging;
};

static struct sort_ext sort_ext[SQNANO_SORT_BUFFERS];
#endif

int sqnano_sorter_open(struct sqnano_sorter *s, struct sqnano_exec *x)
{
	s->slot = -1;
	s->used = 0;
	s->n = 0;
	s->pos = 0;
	s->limit = UINT32_MAX;
	sqnano_mutex_lock(&sort_lock);
	for (int i = 0; i < SQNANO_SORT_BUFFERS; i++) {
		if (!sort_busy[i]) {
			sort_busy[i] = 1;
			s->slot = (int8_t)i;
			break;
		}
	}
	sqnano_mutex_unlock(&sort_lock);
	if (s->slot < 0) {
		return sqnano_exec_error(x, SQNANO_NOMEM, "no free sort buffer");
	}
	s->buf = (uint8_t *)sort_pool[s->slot];
#if SQNANO_SORT_MAX_RUNS > 0
	memset(&sort_ext[s->slot], 0, sizeof(sort_ext[0]));
	sort_ext[s->slot].last = -1;
#endif
	return SQNANO_OK;
}

void sqnano_sorter_close(struct sqnano_sorter *s, struct sqnano_exec *x)
{
	if (s->slot < 0) {
		return;
	}
#if SQNANO_SORT_MAX_RUNS > 0
	{
		struct sort_ext *e = &sort_ext[s->slot];

		for (uint32_t w = 0; w < SQNANO_BM_WORDS; w++) {
			for (uint32_t b = 0; e->pages[w] && b < 32; b++) {
				if (e->pages[w] & (1u << b)) {
					pg_temp_free(x->db, w * 32 + b);
					e->pages[w] &= ~(1u << b);
				}
			}
		}
	}
#else
	(void)x;
#endif
	sqnano_mutex_lock(&sort_lock);
	sort_busy[s->slot] = 0;
	sqnano_mutex_unlock(&sort_lock);
	s->slot = -1;
}

static uint16_t sort_off(struct sqnano_sorter *s, uint16_t i)
{
	return get_u16(s->buf + SORT_CAP - 2u * (i + 1u));
}

static void sort_set(struct sqnano_sorter *s, uint16_t i, uint16_t off)
{
	put_u16(s->buf + SORT_CAP - 2u * (i + 1u), off);
}

static uint16_t entry_len(const uint8_t *e)
{
	return (uint16_t)(4u + get_u16(e) + get_u16(e + 2));
}

/* Append an entry in RAM; SQNANO_FULL when it does not fit. */
static int ram_add(struct sqnano_sorter *s, const struct sqnano_keybuf *key,
		   const sqnano_value *vals, uint8_t n)
{
	uint32_t room;
	uint16_t rlen;
	uint16_t off = s->used;
	uint16_t idx_bytes = (uint16_t)(2 * (s->n + 1));
	uint16_t klen = key ? key->len : 0;

	if ((uint32_t)off + 4 + klen + idx_bytes + 1 > SORT_CAP) {
		return SQNANO_FULL;
	}
	room = SORT_CAP - idx_bytes - off - 4 - klen;
	if (sqnano_record_build(s->buf + off + 4 + klen, (uint16_t)(room > 0xffff ? 0xffff : room),
				vals, n, 0, &rlen)) {
		return SQNANO_FULL;
	}
	put_u16(s->buf + off, klen);
	put_u16(s->buf + off + 2, rlen);
	if (klen) {
		memcpy(s->buf + off + 4, key->b, klen);
	}
	sort_set(s, s->n, off);
	s->n++;
	s->used = (uint16_t)(off + 4 + klen + rlen);
	return SQNANO_OK;
}

static int sort_cmp(struct sqnano_sorter *s, uint16_t a, uint16_t b)
{
	uint16_t al = get_u16(s->buf + a), bl = get_u16(s->buf + b);
	int r = key_cmp(s->buf + a + 4, al, s->buf + b + 4, bl);

	/* Equal keys keep their insertion order. */
	return r ? r : (a < b ? -1 : (a > b ? 1 : 0));
}

static void sort_ram(struct sqnano_sorter *s)
{
	static const uint16_t gaps[] = {1750, 701, 301, 132, 57, 23, 10, 4, 1};

	for (size_t g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++) {
		uint16_t gap = gaps[g];

		for (uint16_t i = gap; i < s->n; i++) {
			uint16_t v = sort_off(s, i);
			uint16_t j = i;

			while (j >= gap && sort_cmp(s, sort_off(s, j - gap), v) > 0) {
				sort_set(s, j, sort_off(s, j - gap));
				j -= gap;
			}
			sort_set(s, j, v);
		}
	}
	s->pos = 0;
}

void sqnano_sorter_limit(struct sqnano_sorter *s, int64_t limit, int64_t offset)
{
	uint64_t n;

	if (limit < 0) {
		return;
	}
	n = (uint64_t)limit + (uint64_t)(offset > 0 ? offset : 0);
	s->limit = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}

/*
 * Top-N: keep the first s->limit entries in sort order. They are packed
 * back in address order, which is the insertion order, so that the final
 * sort stays stable.
 */
static void sort_trim(struct sqnano_sorter *s)
{
	uint16_t keep = (uint16_t)s->limit;
	uint16_t to = 0;

	sort_ram(s);
	for (uint16_t i = 1; i < keep; i++) {
		uint16_t v = sort_off(s, i);
		uint16_t j = i;

		while (j > 0 && sort_off(s, j - 1) > v) {
			sort_set(s, j, sort_off(s, j - 1));
			j--;
		}
		sort_set(s, j, v);
	}
	for (uint16_t i = 0; i < keep; i++) {
		uint16_t off = sort_off(s, i);
		uint16_t len = entry_len(s->buf + off);

		memmove(s->buf + to, s->buf + off, len);
		sort_set(s, i, to);
		to = (uint16_t)(to + len);
	}
	s->n = keep;
	s->used = to;
}

#if SQNANO_SORT_MAX_RUNS > 0
static uint16_t al4(uint32_t v)
{
	return (uint16_t)((v + 3u) & ~3u);
}

static int sort_fail(struct sqnano_exec *x, int rc)
{
	if (rc == SQNANO_READONLY || rc == SQNANO_FULL) {
		return sqnano_exec_error(x, SQNANO_FULL, "sort does not fit in RAM nor in free pages");
	}
	return sqnano_exec_error(x, rc, "external sort failed");
}

static int temp_alloc(struct sqnano_db *db, struct sort_ext *e, uint32_t *pgno)
{
	int rc = pg_temp_alloc(db, pgno);

	if (rc == SQNANO_OK) {
		bm_set(e->pages, *pgno);
	}
	return rc;
}

static void temp_free(struct sqnano_db *db, struct sort_ext *e, uint32_t pgno)
{
	bm_clr(e->pages, pgno);
	pg_temp_free(db, pgno);
}

/* Run writer; the page is assembled in the database scratch (lock held). */
struct run_w {
	struct sqnano_db *db;
	struct sort_ext *e;
	uint8_t *page;
	uint32_t pgno;
	uint16_t off;
};

static int rw_open(struct run_w *w, struct sqnano_db *db, struct sort_ext *e, struct sort_run *r)
{
	int rc;

	w->db = db;
	w->e = e;
	w->page = db->scratch;
	rc = temp_alloc(db, e, &w->pgno);
	if (rc) {
		return rc;
	}
	r->first = w->pgno;
	r->count = 0;
	db->stat_sort_runs++;
	memset(w->page, 0, db->page_size);
	w->off = RUN_DATA;
	return SQNANO_OK;
}

static int rw_put(struct run_w *w, const uint8_t *src, uint32_t len)
{
	uint16_t ps = w->db->page_size;

	while (len) {
		uint32_t n;

		if (w->off == ps) {
			uint32_t next;
			int rc = temp_alloc(w->db, w->e, &next);

			if (rc == SQNANO_OK) {
				put_u32(w->page + SQNANO_PAGE_HDR, next);
				rc = pg_temp_write(w->db, w->pgno, w->page);
			}
			if (rc) {
				return rc;
			}
			memset(w->page, 0, ps);
			w->pgno = next;
			w->off = RUN_DATA;
		}
		n = ps - w->off;
		n = n > len ? len : n;
		memcpy(w->page + w->off, src, n);
		w->off = (uint16_t)(w->off + n);
		src += n;
		len -= n;
	}
	return SQNANO_OK;
}

static int rw_close(struct run_w *w)
{
	put_u32(w->page + SQNANO_PAGE_HDR, 0);
	return pg_temp_write(w->db, w->pgno, w->page);
}

static int ri_get(struct sqnano_sorter *s, struct sqnano_db *db, struct sort_ext *e,
		  struct sort_in *in, uint8_t *dst, uint32_t len)
{
	uint8_t *page = s->buf + in->page;
	uint16_t ps = db->page_size;

	while (len) {
		uint32_t n;

		if (in->off == ps) {
			uint32_t next = get_u32(page + SQNANO_PAGE_HDR);
			int rc;

			temp_free(db, e, in->pgno);
			in->pgno = 0;
			if (next == 0 || !bm_test(e->pages, next)) {
				return SQNANO_CORRUPT;
			}
			rc = pg_temp_read(db, next, page);
			if (rc) {
				return rc;
			}
			in->pgno = next;
			in->off = RUN_DATA;
		}
		n = ps - in->off;
		n = n > len ? len : n;
		memcpy(dst, page + in->off, n);
		in->off = (uint16_t)(in->off + n);
		dst += n;
		len -= n;
	}
	return SQNANO_OK;
}

/* Load the next entry of a run into its head; free its last page at the end. */
static int ri_load(struct sqnano_sorter *s, struct sqnano_db *db, struct sort_ext *e,
		   struct sort_in *in)
{
	uint8_t *head = s->buf + in->head;
	int rc;

	if (in->left == 0) {
		in->valid = 0;
		if (in->pgno) {
			temp_free(db, e, in->pgno);
			in->pgno = 0;
		}
		return SQNANO_OK;
	}
	rc = ri_get(s, db, e, in, head, 4);
	if (rc == SQNANO_OK && entry_len(head) > e->maxent) {
		rc = SQNANO_CORRUPT;
	}
	if (rc == SQNANO_OK) {
		rc = ri_get(s, db, e, in, head + 4, entry_len(head) - 4u);
	}
	in->left--;
	in->valid = rc == SQNANO_OK;
	return rc;
}

/* Inputs a merge can read at once: hold area, then a page and an entry each. */
static uint8_t sort_fanin(struct sqnano_db *db, struct sort_ext *e)
{
	uint32_t per = sizeof(struct sort_in) + al4(db->page_size) + al4(e->maxent);
	uint32_t f = (SORT_CAP - al4(e->maxent)) / per;

	return (uint8_t)(f > SQNANO_SORT_MAX_RUNS ? SQNANO_SORT_MAX_RUNS : f);
}

static struct sort_in *sort_inputs(struct sqnano_sorter *s)
{
	return (struct sort_in *)(void *)s->buf;
}

/* Lay out the buffer for @p k inputs and open them on runs[first..first+k). */
static int sort_open_inputs(struct sqnano_sorter *s, struct sqnano_db *db, struct sort_ext *e,
			    uint8_t first, uint8_t k)
{
	struct sort_in *in = sort_inputs(s);
	uint16_t off = al4((uint32_t)k * sizeof(struct sort_in));
	int rc = SQNANO_OK;

	e->hold = off;
	off = (uint16_t)(off + al4(e->maxent));
	for (uint8_t i = 0; i < k && rc == SQNANO_OK; i++) {
		in[i].page = off;
		in[i].head = (uint16_t)(off + al4(db->page_size));
		off = (uint16_t)(in[i].head + al4(e->maxent));
		in[i].pgno = e->runs[first + i].first;
		in[i].left = e->runs[first + i].count;
		in[i].off = RUN_DATA;
		rc = pg_temp_read(db, in[i].pgno, s->buf + in[i].page);
		if (rc == SQNANO_OK) {
			rc = ri_load(s, db, e, &in[i]);
		}
	}
	return rc;
}

/* Input holding the smallest entry, the earliest run on ties; -1 when all ended. */
static int sort_pick(struct sqnano_sorter *s, uint8_t k)
{
	struct sort_in *in = sort_inputs(s);
	int best = -1;

	for (uint8_t i = 0; i < k; i++) {
		const uint8_t *h = s->buf + in[i].head;

		if (in[i].valid &&
		    (best < 0 || key_cmp(h + 4, get_u16(h), s->buf + in[best].head + 4,
					 get_u16(s->buf + in[best].head)) < 0)) {
			best = i;
		}
	}
	return best;
}

/* Merge runs[first..first+k) into a new run. */
static int sort_merge(struct sqnano_sorter *s, struct sqnano_db *db, struct sort_ext *e,
		      uint8_t first, uint8_t k, struct sort_run *out)
{
	struct sort_in *in = sort_inputs(s);
	struct run_w w;
	int rc = sort_open_inputs(s, db, e, first, k);

	if (rc == SQNANO_OK) {
		rc = rw_open(&w, db, e, out);
	}
	while (rc == SQNANO_OK) {
		int b = sort_pick(s, k);
		const uint8_t *h;

		if (b < 0) {
			rc = rw_close(&w);
			break;
		}
		h = s->buf + in[b].head;
		if (out->count == s->limit) {
			/* Top-N: the rest is never read; its pages go at close. */
			rc = rw_close(&w);
			break;
		}
		rc = rw_put(&w, h, entry_len(h));
		out->count++;
		if (rc == SQNANO_OK) {
			rc = ri_load(s, db, e, &in[b]);
		}
	}
	return rc;
}

/* Merge groups of consecutive runs until at most @p target remain. */
static int sort_reduce(struct sqnano_sorter *s, struct sqnano_db *db, struct sort_ext *e,
		       uint8_t target)
{
	uint8_t f = sort_fanin(db, e);

	if (f < 2) {
		return SQNANO_FULL;
	}
	while (e->nruns > target) {
		uint8_t n = e->nruns, i = 0, j = 0;

		while (i < n) {
			uint8_t k = (uint8_t)(n - i < f ? n - i : f);
			struct sort_run r;
			int rc;

			if (j + (n - i) <= target || k == 1) {
				memmove(&e->runs[j], &e->runs[i], (size_t)(n - i) * sizeof(e->runs[0]));
				j = (uint8_t)(j + (n - i));
				break;
			}
			/* runs[i..i+k) are read before runs[j] (j <= i) is written. */
			rc = sort_merge(s, db, e, i, k, &r);
			if (rc) {
				return rc;
			}
			e->runs[j++] = r;
			i = (uint8_t)(i + k);
		}
		e->nruns = j;
	}
	return SQNANO_OK;
}

/* Write the RAM entries as a new run and empty the buffer. */
static int sort_spill(struct sqnano_sorter *s, struct sqnano_db *db, struct sort_ext *e)
{
	struct sort_run r;
	struct run_w w;
	int rc;

	sort_ram(s);
	rc = rw_open(&w, db, e, &r);
	for (uint16_t i = 0; i < s->n && i < s->limit && rc == SQNANO_OK; i++) {
		const uint8_t *ent = s->buf + sort_off(s, i);

		rc = rw_put(&w, ent, entry_len(ent));
		r.count++;
	}
	if (rc == SQNANO_OK) {
		rc = rw_close(&w);
	}
	if (rc) {
		return rc;
	}
	e->runs[e->nruns++] = r;
	s->n = 0;
	s->used = 0;
	if (e->nruns == SQNANO_SORT_MAX_RUNS) {
		rc = sort_reduce(s, db, e, SQNANO_SORT_MAX_RUNS / 2);
	}
	return rc;
}
#endif /* SQNANO_SORT_MAX_RUNS > 0 */

int sqnano_sorter_add(struct sqnano_sorter *s, struct sqnano_exec *x,
		      const struct sqnano_keybuf *key, const sqnano_value *vals, uint8_t n)
{
	int rc;

	if (s->slot < 0) {
		return SQNANO_MISUSE;
	}
	rc = ram_add(s, key, vals, n);
	if (rc == SQNANO_FULL && s->limit < s->n) {
		sort_trim(s);
		rc = ram_add(s, key, vals, n);
	}
#if SQNANO_SORT_MAX_RUNS > 0
	{
		struct sort_ext *e = &sort_ext[s->slot];

		if (rc == SQNANO_FULL && s->n > 0) {
			/* Spill only when a merge can hold two runs with entries this large. */
			if (sort_fanin(x->db, e) < 2) {
				return sqnano_exec_error(x, SQNANO_FULL, "sort buffer full");
			}
			rc = sort_spill(s, x->db, e);
			if (rc) {
				return sort_fail(x, rc);
			}
			rc = ram_add(s, key, vals, n);
		}
		if (rc == SQNANO_OK) {
			uint16_t len = entry_len(s->buf + sort_off(s, (uint16_t)(s->n - 1u)));

			e->maxent = len > e->maxent ? len : e->maxent;
		}
	}
#endif
	if (rc) {
		return sqnano_exec_error(x, SQNANO_FULL, "sort buffer full");
	}
	return SQNANO_OK;
}

int sqnano_sorter_sort(struct sqnano_sorter *s, struct sqnano_exec *x)
{
	if (s->slot < 0) {
		return SQNANO_MISUSE;
	}
#if SQNANO_SORT_MAX_RUNS > 0
	{
		struct sort_ext *e = &sort_ext[s->slot];
		int rc = SQNANO_OK;

		if (e->nruns) {
			if (s->n) {
				rc = sort_spill(s, x->db, e);
			}
			if (rc == SQNANO_OK) {
				rc = sort_reduce(s, x->db, e, sort_fanin(x->db, e));
			}
			if (rc == SQNANO_OK) {
				e->nin = e->nruns;
				e->nruns = 0;
				e->last = -1;
				e->merging = 1;
				rc = sort_open_inputs(s, x->db, e, 0, e->nin);
			}
			return rc ? sort_fail(x, rc) : SQNANO_OK;
		}
	}
#else
	(void)x;
#endif
	sort_ram(s);
	return SQNANO_OK;
}

static int entry_decode(struct sqnano_sorter *s, const uint8_t *ent, sqnano_value *vals, uint8_t n)
{
	uint16_t klen = get_u16(ent);
	uint16_t rlen = get_u16(ent + 2);

	s->key = ent + 4;
	s->klen = klen;
	for (uint8_t i = 0; i < n; i++) {
		int rc = sqnano_record_get(s->key + klen, rlen, i, &vals[i]);

		if (rc && rc != SQNANO_NOTFOUND) {
			return rc;
		}
	}
	return SQNANO_OK;
}

int sqnano_sorter_next(struct sqnano_sorter *s, struct sqnano_exec *x, sqnano_value *vals,
		       uint8_t n)
{
#if SQNANO_SORT_MAX_RUNS > 0
	struct sort_ext *e = &sort_ext[s->slot];

	if (e->merging) {
		struct sort_in *in = sort_inputs(s);
		int b;

		if (e->last >= 0) {
			int rc = ri_load(s, x->db, e, &in[e->last]);

			if (rc) {
				return sort_fail(x, rc);
			}
		}
		b = sort_pick(s, e->nin);
		e->last = (int8_t)b;
		if (b < 0) {
			return SQNANO_DONE;
		}
		return entry_decode(s, s->buf + in[b].head, vals, n);
	}
#else
	(void)x;
#endif
	if (s->pos >= s->n) {
		return SQNANO_DONE;
	}
	return entry_decode(s, s->buf + sort_off(s, s->pos++), vals, n);
}

int sqnano_sorter_hold(struct sqnano_sorter *s, struct sqnano_exec *x, sqnano_value *vals,
		       uint8_t n)
{
	const uint8_t *ent = s->key - 4;

	(void)x;
#if SQNANO_SORT_MAX_RUNS > 0
	if (sort_ext[s->slot].merging) {
		/* Merge heads are reused by the next call: copy the entry aside. */
		uint8_t *h = s->buf + sort_ext[s->slot].hold;

		memmove(h, ent, entry_len(ent));
		ent = h;
	}
#endif
	return entry_decode(s, ent, vals, n);
}

/* ---- Aggregates ---- */

void sqnano_agg_reset(struct sqnano_agg *a)
{
	memset(a, 0, sizeof(*a));
	sqnano_v_null(&a->v);
}

static void kbn_step(struct sqnano_agg *a, double r)
{
	volatile double s = a->rsum;
	volatile double t = s + r;

	if (fabs(s) > fabs(r)) {
		a->rerr += (s - t) + r;
	} else {
		a->rerr += (r - t) + s;
	}
	a->rsum = t;
}

static void kbn_int(struct sqnano_agg *a, int64_t v)
{
	if (v <= -4503599627370496LL || v >= 4503599627370496LL) {
		int64_t sm = v % 16384;

		kbn_step(a, (double)(v - sm));
		kbn_step(a, (double)sm);
	} else {
		kbn_step(a, (double)v);
	}
}

static void kbn_init(struct sqnano_agg *a, int64_t v)
{
	if (v <= -4503599627370496LL || v >= 4503599627370496LL) {
		int64_t sm = v % 16384;

		a->rsum = (double)(v - sm);
		a->rerr = (double)sm;
	} else {
		a->rsum = (double)v;
		a->rerr = 0.0;
	}
}

int sqnano_value_save(struct sqnano_exec *x, sqnano_value *dst, const sqnano_value *src,
		      uint8_t *buf, uint32_t cap)
{
	sqnano_value v = *src;

	if (v.type == SQNANO_TEXT || v.type == SQNANO_BLOB) {
		if (v.n + 1 > cap) {
			return sqnano_exec_error(x, SQNANO_TOOBIG,
						 "subquery result too large (CONFIG_SQNANO_AGG_TEXT_SIZE)");
		}
		memmove(buf, v.u.p, v.n);
		buf[v.n] = 0;
		v.u.p = buf;
	}
	*dst = v;
	return SQNANO_OK;
}

static int agg_keep(struct sqnano_exec *x, struct sqnano_agg *a, const sqnano_value *v)
{
	a->v = *v;
	if (v->type == SQNANO_TEXT || v->type == SQNANO_BLOB) {
		if (v->n + 1 > sizeof(a->buf)) {
			return sqnano_exec_error(x, SQNANO_TOOBIG, "value too large for min()/max()");
		}
		memcpy(a->buf, v->u.p, v->n);
		a->buf[v->n] = 0;
		a->v.u.p = a->buf;
	}
	a->has = 1;
	return SQNANO_OK;
}

int sqnano_agg_step(struct sqnano_exec *x, struct sqnano_agg *a, int kind, const sqnano_value *v,
		    uint8_t coll)
{
	sqnano_value n;
	bool whole;

	if (kind == SQNANO_AGG_COUNT_STAR) {
		a->count++;
		return SQNANO_OK;
	}
	if (v->type == SQNANO_NULL) {
		return SQNANO_OK;
	}
	switch (kind) {
	case SQNANO_AGG_COUNT:
		a->count++;
		return SQNANO_OK;
	case SQNANO_AGG_MIN:
	case SQNANO_AGG_MAX: {
		int c = a->has ? sqnano_v_compare(v, &a->v, coll) : 0;

		a->count++;
		if (!a->has || (kind == SQNANO_AGG_MIN ? c < 0 : c > 0)) {
			return agg_keep(x, a, v);
		}
		return SQNANO_OK;
	}
	default:
		break;
	}
	a->count++;
	n = *v;
	if (v->type == SQNANO_TEXT || v->type == SQNANO_BLOB) {
		(void)num_parse(v->u.z, v->n, &n, &whole);
		if (!whole && n.type == SQNANO_INTEGER) {
			sqnano_v_real(&n, (double)n.u.i);
		}
	}
	if (n.type == SQNANO_INTEGER) {
		if (!a->approx) {
			int64_t s = a->isum;

			if ((n.u.i > 0 && s > INT64_MAX - n.u.i) || (n.u.i < 0 && s < INT64_MIN - n.u.i)) {
				a->overflow = 1;
				a->approx = 1;
				kbn_init(a, a->isum);
				kbn_int(a, n.u.i);
			} else {
				a->isum = s + n.u.i;
			}
		} else {
			kbn_int(a, n.u.i);
		}
	} else {
		if (!a->approx) {
			a->approx = 1;
			kbn_init(a, a->isum);
		}
		/* As SQLite: a real value turns an integer overflow into a real sum. */
		a->overflow = 0;
		kbn_step(a, n.u.r);
	}
	return SQNANO_OK;
}

int sqnano_agg_final(struct sqnano_exec *x, struct sqnano_agg *a, int kind, sqnano_value *r)
{
	double s = isinf(a->rerr) || isnan(a->rerr) ? a->rsum : a->rsum + a->rerr;

	switch (kind) {
	case SQNANO_AGG_COUNT_STAR:
	case SQNANO_AGG_COUNT:
		sqnano_v_int(r, a->count);
		return SQNANO_OK;
	case SQNANO_AGG_MIN:
	case SQNANO_AGG_MAX:
		if (a->has) {
			*r = a->v;
		} else {
			sqnano_v_null(r);
		}
		return SQNANO_OK;
	case SQNANO_AGG_SUM:
		if (a->count == 0) {
			sqnano_v_null(r);
		} else if (a->approx) {
			if (a->overflow) {
				return sqnano_exec_error(x, SQNANO_ERROR, "integer overflow");
			}
			sqnano_v_real(r, s);
		} else {
			sqnano_v_int(r, a->isum);
		}
		return SQNANO_OK;
	case SQNANO_AGG_TOTAL:
		sqnano_v_real(r, a->approx ? s : (double)a->isum);
		return SQNANO_OK;
	default:
		if (a->count == 0) {
			sqnano_v_null(r);
		} else {
			sqnano_v_real(r, (a->approx ? s : (double)a->isum) / (double)a->count);
		}
		return SQNANO_OK;
	}
}
