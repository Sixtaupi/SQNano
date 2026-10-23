/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Remote cursor server: stateless requests on read snapshots, CBOR in and
 * out. The CBOR subset is the one SMP clients produce: definite lengths,
 * integers, text and byte strings, arrays, maps, null, booleans and floats.
 */

#include <math.h>

#include <sqnano/exec.h>
#include <sqnano/remote.h>

#include "../sqnano_int.h"

#define MAX_PARAMS 16

/* ---- CBOR writer ---- */

/* CBOR is big-endian (the engine's put_u16/put_u32 are not). */
static void be_put(uint8_t *p, uint64_t v, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		p[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
	}
}

struct cw {
	uint8_t *b;
	size_t cap;
	size_t len;
	bool full;
};

static void cw_raw(struct cw *w, const void *p, size_t n)
{
	if (w->full || w->len > w->cap || n > w->cap - w->len) {
		w->full = true;
		return;
	}
	memcpy(w->b + w->len, p, n);
	w->len += n;
}

static void cw_head(struct cw *w, uint8_t major, uint64_t v)
{
	uint8_t h[9];
	size_t n;

	h[0] = (uint8_t)(major << 5);
	if (v < 24) {
		h[0] |= (uint8_t)v;
		n = 1;
	} else if (v <= 0xff) {
		h[0] |= 24;
		h[1] = (uint8_t)v;
		n = 2;
	} else if (v <= 0xffff) {
		h[0] |= 25;
		n = 3;
	} else if (v <= 0xffffffffu) {
		h[0] |= 26;
		n = 5;
	} else {
		h[0] |= 27;
		n = 9;
	}
	if (n > 2) {
		be_put(h + 1, v, n - 1);
	}
	cw_raw(w, h, n);
}

static void cw_int(struct cw *w, int64_t i)
{
	if (i >= 0) {
		cw_head(w, 0, (uint64_t)i);
	} else {
		cw_head(w, 1, (uint64_t)(-1 - i));
	}
}

static void cw_text(struct cw *w, const char *s)
{
	size_t n = s ? strlen(s) : 0;

	cw_head(w, 3, n);
	cw_raw(w, s, n);
}

static void cw_simple(struct cw *w, uint8_t b)
{
	cw_raw(w, &b, 1);
}

static void cw_value(struct cw *w, const sqnano_value *v)
{
	switch (v->type) {
	case SQNANO_INTEGER:
		cw_int(w, v->u.i);
		break;
	case SQNANO_FLOAT: {
		uint8_t b[9];
		uint64_t u;

		memcpy(&u, &v->u.r, 8);
		b[0] = 0xfb;
		be_put(b + 1, u, 8);
		cw_raw(w, b, 9);
		break;
	}
	case SQNANO_TEXT:
		cw_head(w, 3, v->n);
		cw_raw(w, v->u.p, v->n);
		break;
	case SQNANO_BLOB:
		cw_head(w, 2, v->n);
		cw_raw(w, v->u.p, v->n);
		break;
	default:
		cw_simple(w, 0xf6);
		break;
	}
}

/* An array whose length is patched at the end (always 2 length bytes). */
static size_t cw_array16(struct cw *w)
{
	static const uint8_t h[3] = {0x99, 0, 0};
	size_t at = w->len;

	cw_raw(w, h, 3);
	return at;
}

static void cw_patch16(struct cw *w, size_t at, uint16_t n)
{
	if (!w->full) {
		be_put(w->b + at + 1, n, 2);
	}
}

/* ---- CBOR reader ---- */

struct cr {
	const uint8_t *p;
	const uint8_t *end;
};

static bool cr_head(struct cr *r, uint8_t *major, uint8_t *info, uint64_t *v)
{
	uint8_t ai;
	size_t n;

	if (r->p >= r->end) {
		return false;
	}
	*major = *r->p >> 5;
	ai = *r->p & 0x1f;
	*info = ai;
	r->p++;
	if (ai < 24) {
		*v = ai;
		return true;
	}
	if (ai > 27) {
		return false;
	}
	n = (size_t)1 << (ai - 24);
	if ((size_t)(r->end - r->p) < n) {
		return false;
	}
	*v = 0;
	for (size_t i = 0; i < n; i++) {
		*v = *v << 8 | r->p[i];
	}
	r->p += n;
	return true;
}

/* A scalar value; strings point into the request. */
static bool cr_value(struct cr *r, sqnano_value *out)
{
	uint8_t major, info;
	uint64_t v;

	if (!cr_head(r, &major, &info, &v)) {
		return false;
	}
	switch (major) {
	case 0:
		if (v > INT64_MAX) {
			return false;
		}
		sqnano_v_int(out, (int64_t)v);
		return true;
	case 1:
		if (v > INT64_MAX) {
			return false;
		}
		sqnano_v_int(out, -1 - (int64_t)v);
		return true;
	case 2:
	case 3:
		if (v > (uint64_t)(r->end - r->p)) {
			return false;
		}
		if (major == 3) {
			sqnano_v_text(out, (const char *)r->p, (uint32_t)v);
		} else {
			sqnano_v_blob(out, r->p, (uint32_t)v);
		}
		r->p += v;
		return true;
	case 7:
		if (info == 20 || info == 21) {
			sqnano_v_int(out, info == 21);
		} else if (info == 22 || info == 23) {
			sqnano_v_null(out);
		} else if (info == 27) {
			double d;

			memcpy(&d, &v, 8);
			sqnano_v_real(out, d);
		} else if (info == 26) {
			float f;
			uint32_t u = (uint32_t)v;

			memcpy(&f, &u, 4);
			sqnano_v_real(out, f);
		} else {
			return false;
		}
		return true;
	default:
		return false;
	}
}

/* ---- Requests ---- */

struct rq {
	sqnano_value op;
	sqnano_value t;
	sqnano_value q;
	int64_t db;
	int64_t after;
	int64_t hi;
	int64_t n;
	int64_t from;
	int64_t off;
	/* WITHOUT ROWID tables page by key: the first item of the last row. */
	sqnano_value after_key;
	bool has_after;
	bool has_hi;
	uint8_t np;
	sqnano_value p[MAX_PARAMS];
};

static bool key_is(const sqnano_value *k, const char *s)
{
	return k->type == SQNANO_TEXT && k->n == strlen(s) && memcmp(k->u.p, s, k->n) == 0;
}

static bool int_field(struct cr *r, int64_t *out, bool *has)
{
	sqnano_value v;

	if (!cr_value(r, &v) || v.type != SQNANO_INTEGER) {
		return false;
	}
	*out = v.u.i;
	if (has) {
		*has = true;
	}
	return true;
}

static bool parse(struct rq *rq, const uint8_t *req, size_t len)
{
	struct cr r = {req, req + len};
	uint8_t major, info;
	uint64_t n;

	memset(rq, 0, sizeof(*rq));
	rq->n = -1;
	if (!cr_head(&r, &major, &info, &n) || major != 5) {
		return false;
	}
	for (uint64_t i = 0; i < n; i++) {
		sqnano_value k;
		bool ok;

		if (!cr_value(&r, &k) || k.type != SQNANO_TEXT) {
			return false;
		}
		if (key_is(&k, "op")) {
			ok = cr_value(&r, &rq->op);
		} else if (key_is(&k, "t")) {
			ok = cr_value(&r, &rq->t);
		} else if (key_is(&k, "q")) {
			ok = cr_value(&r, &rq->q);
		} else if (key_is(&k, "db")) {
			ok = int_field(&r, &rq->db, NULL);
		} else if (key_is(&k, "after")) {
			sqnano_value v;

			ok = cr_value(&r, &v) && (v.type == SQNANO_INTEGER || v.type == SQNANO_BLOB);
			if (ok && v.type == SQNANO_INTEGER) {
				rq->after = v.u.i;
			} else if (ok) {
				rq->after_key = v;
			}
			rq->has_after = ok;
		} else if (key_is(&k, "hi")) {
			ok = int_field(&r, &rq->hi, &rq->has_hi);
		} else if (key_is(&k, "n")) {
			ok = int_field(&r, &rq->n, NULL);
		} else if (key_is(&k, "from")) {
			ok = int_field(&r, &rq->from, NULL);
		} else if (key_is(&k, "off")) {
			ok = int_field(&r, &rq->off, NULL);
		} else if (key_is(&k, "p")) {
			uint64_t m;

			ok = cr_head(&r, &major, &info, &m) && major == 4 && m <= MAX_PARAMS;
			for (uint64_t j = 0; ok && j < m; j++) {
				ok = cr_value(&r, &rq->p[j]);
			}
			rq->np = (uint8_t)m;
		} else {
			/* Unknown keys are skipped when they hold a scalar. */
			sqnano_value skip;

			ok = cr_value(&r, &skip);
		}
		if (!ok) {
			return false;
		}
	}
	return true;
}

/* ---- Responses ---- */

/* An error response; the message is cut to what the buffer holds. */
static int error(struct cw *w, int rc, const char *msg)
{
	size_t n = strlen(msg);

	w->len = 0;
	w->full = false;
	if (w->cap < 16) {
		return SQNANO_TOOBIG;
	}
	n = n > w->cap - 16 ? w->cap - 16 : n;
	cw_head(w, 5, 2);
	cw_text(w, "err");
	cw_int(w, rc);
	cw_text(w, "msg");
	cw_head(w, 3, n);
	cw_raw(w, msg, n);
	return w->full ? SQNANO_TOOBIG : SQNANO_OK;
}

/* Keep @p tail bytes back for the end of the response; *saved restores it. */
static void tail_reserve(struct cw *w, size_t *saved, size_t tail)
{
	*saved = w->cap;
	w->cap = w->cap > tail ? w->cap - tail : 0;
}

/* Undo a partial item; returns true when the item fitted. */
static bool item_fits(struct cw *w, size_t before)
{
	if (!w->full) {
		return true;
	}
	w->full = false;
	w->len = before;
	return false;
}

static const struct sqnano_catalog *catalog(struct sqnano_db *db)
{
	return db->schema ? SQNANO_CATALOG(db->schema) : NULL;
}

static int op_info(const struct sqnano_remote_db *dbs, uint8_t ndbs, struct cw *w)
{
	cw_head(w, 5, 1);
	cw_text(w, "dbs");
	cw_head(w, 4, ndbs);
	for (uint8_t i = 0; i < ndbs; i++) {
		struct sqnano_stats st;

		if (!dbs[i].db || sqnano_db_stats(dbs[i].db, &st)) {
			memset(&st, 0, sizeof(st));
		}
		cw_head(w, 5, 7);
		cw_text(w, "path");
		cw_text(w, dbs[i].path);
		cw_text(w, "ps");
		cw_int(w, st.page_size);
		cw_text(w, "total");
		cw_int(w, st.pages_total);
		cw_text(w, "used");
		cw_int(w, st.pages_used);
		cw_text(w, "mode");
		cw_text(w, st.mode == SQNANO_MODE_SHADOW ? "shadow" : "journal");
		cw_text(w, "sv");
		cw_int(w, st.schema_version);
		cw_text(w, "seq");
		cw_int(w, st.seq);
	}
	return w->full ? error(w, SQNANO_TOOBIG, "response buffer too small") : SQNANO_OK;
}

/*
 * Tables, then indexes, as [kind (0 table, 1 index), name, sql bytes, done].
 * An SQL text longer than the response continues in the next one: "from"
 * and "off" say where.
 */
static int op_tables(struct sqnano_db *db, const struct rq *rq, struct cw *w)
{
	const struct sqnano_catalog *c = catalog(db);
	uint32_t total, k;
	uint32_t off = rq->off > 0 ? (uint32_t)rq->off : 0;
	uint16_t count = 0;
	size_t at, cap;
	bool more = false;

	if (!c) {
		return error(w, SQNANO_NOTFOUND, "no schema");
	}
	total = (uint32_t)c->ntables + c->nindexes;
	cw_head(w, 5, 5);
	cw_text(w, "sv");
	cw_int(w, c->version);
	cw_text(w, "s");
	at = cw_array16(w);
	/* "more", "from", "off" and their values. */
	tail_reserve(w, &cap, 6 + 6 + 4 + 9);
	for (k = rq->from > 0 ? (uint32_t)rq->from : 0; k < total; k++, off = 0) {
		size_t before = w->len;
		bool table = k < c->ntables;
		const char *name = table ? c->tables[k]->name : c->indexes[k - c->ntables]->name;
		const char *sql = table ? c->tables[k]->sql : c->indexes[k - c->ntables]->sql;
		uint32_t len = sql ? (uint32_t)strlen(sql) : 0;
		uint32_t n;

		off = off > len ? len : off;
		n = len - off;
		cw_head(w, 4, 4);
		cw_int(w, table ? 0 : 1);
		cw_text(w, name);
		if (!w->full && count == 0) {
			/* The first entry may be cut: send what fits of its text. */
			size_t room = w->cap - w->len;
			size_t head = n < 24 ? 1 : n < 256 ? 2 : 3;

			if (room < head + 2) {
				w->full = true;
			} else if (n > room - head - 1) {
				n = (uint32_t)(room - head - 1);
			}
		}
		cw_head(w, 2, n);
		cw_raw(w, sql ? sql + off : "", n);
		cw_simple(w, off + n == len ? 0xf5 : 0xf4);
		if (!item_fits(w, before)) {
			more = true;
			break;
		}
		count++;
		if (off + n < len) {
			off += n;
			more = true;
			break;
		}
	}
	w->cap = cap;
	if (count == 0 && more) {
		return error(w, SQNANO_TOOBIG, "response buffer too small for the schema");
	}
	cw_patch16(w, at, count);
	cw_text(w, "more");
	cw_simple(w, more ? 0xf5 : 0xf4);
	cw_text(w, "from");
	cw_int(w, k);
	cw_text(w, "off");
	cw_int(w, more ? off : 0);
	return w->full ? error(w, SQNANO_TOOBIG, "response buffer too small") : SQNANO_OK;
}

/* Working memory of a scan; requests are served one at a time. */
static struct {
	struct sqnano_exec x;
	uint8_t scratch[256];
	struct sqnano_scan scan;
	sqnano_value cols[SQNANO_MAX_COLUMNS];
} w_scan;
static SQNANO_MUTEX_DEFINE(remote_lock);

static bool name_eq(const char *a, const char *b, uint32_t n)
{
	if (strlen(a) != n) {
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		char ca = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
		char cb = b[i] >= 'A' && b[i] <= 'Z' ? (char)(b[i] + 32) : b[i];

		if (ca != cb) {
			return false;
		}
	}
	return true;
}

static const struct sqnano_table *find_table(struct sqnano_db *db, const sqnano_value *name)
{
	const struct sqnano_catalog *c = catalog(db);

	for (int i = 0; c && name->type == SQNANO_TEXT && i < c->ntables; i++) {
		if (name_eq(c->tables[i]->name, (const char *)name->u.p, name->n)) {
			return c->tables[i];
		}
	}
	return NULL;
}

static int op_scan(struct sqnano_db *db, const struct rq *rq, struct cw *w)
{
	const struct sqnano_table *t = find_table(db, &rq->t);
	struct sqnano_scan *s = &w_scan.scan;
	struct sqnano_txn txn;
	uint16_t count = 0;
	size_t at, cap;
	bool more = false;
	int rc;

	if (!t) {
		return error(w, SQNANO_NOTFOUND, "no such table");
	}
	memset(&w_scan.x, 0, sizeof(w_scan.x));
	w_scan.x.db = db;
	w_scan.x.scratch.buf = w_scan.scratch;
	w_scan.x.scratch.cap = sizeof(w_scan.scratch);
	rc = sqnano_txn_begin(db, &txn, SQNANO_TXN_READ, 0);
	if (rc) {
		return error(w, rc, "cannot open a snapshot");
	}
	w_scan.x.txn = &txn;
	cw_head(w, 5, 2);
	cw_text(w, "rows");
	at = cw_array16(w);
	tail_reserve(w, &cap, 6);
	db_lock(db);
	sqnano_scan_init(s, &w_scan.x, t, NULL, w_scan.cols, 0);
	if (t->pk && rq->has_after) {
		/* Keys compare as bytes: the first key after K is at least K 00. */
		if (rq->after_key.type != SQNANO_BLOB || rq->after_key.n + 1u > sizeof(s->lo.b)) {
			rc = SQNANO_MISMATCH;
		} else {
			memcpy(s->lo.b, rq->after_key.u.p, rq->after_key.n);
			s->lo.b[rq->after_key.n] = 0;
			s->lo.len = (uint16_t)(rq->after_key.n + 1u);
			s->flags |= SQNANO_SCAN_LO;
		}
	} else if (rq->has_after) {
		sqnano_value lo;

		if (rq->after == INT64_MAX) {
			s->flags |= SQNANO_SCAN_EMPTY;
		}
		sqnano_v_int(&lo, rq->after == INT64_MAX ? rq->after : rq->after + 1);
		rc = sqnano_scan_range(s, SQNANO_SCAN_LO, &lo, SQNANO_AFF_INTEGER, SQNANO_COLL_BINARY,
				       false);
	}
	if (rc == SQNANO_OK && rq->has_hi && !t->pk) {
		sqnano_value hi;

		sqnano_v_int(&hi, rq->hi);
		rc = sqnano_scan_range(s, SQNANO_SCAN_HI, &hi, SQNANO_AFF_INTEGER, SQNANO_COLL_BINARY,
				       false);
	}
	while (rc == SQNANO_OK) {
		size_t before = w->len;

		if (rq->n >= 0 && count >= rq->n) {
			more = true;
			break;
		}
		if (count == UINT16_MAX) {
			more = true;
			break;
		}
		rc = sqnano_scan_next(s);
		if (rc != SQNANO_OK) {
			break;
		}
		sqnano_scratch_reset(&w_scan.x);
		cw_head(w, 4, 1u + t->ncols);
		if (t->pk) {
			/* A WITHOUT ROWID row is named by its key. */
			sqnano_value k;

			sqnano_v_blob(&k, s->ic.key, s->ic.klen);
			cw_value(w, &k);
		} else {
			cw_int(w, s->rowid);
		}
		for (uint8_t i = 0; i < t->ncols; i++) {
			cw_value(w, &w_scan.cols[i]);
		}
		if (!item_fits(w, before)) {
			more = true;
			break;
		}
		count++;
	}
	db_unlock(db);
	(void)sqnano_txn_commit(&txn);
	w->cap = cap;
	if (rc != SQNANO_OK && rc != SQNANO_DONE) {
		return error(w, rc, w_scan.x.errmsg[0] ? w_scan.x.errmsg : "scan failed");
	}
	if (count == 0 && more && rq->n != 0) {
		return error(w, SQNANO_TOOBIG, "row larger than the response buffer");
	}
	cw_patch16(w, at, count);
	cw_text(w, "more");
	cw_simple(w, more ? 0xf5 : 0xf4);
	return w->full ? error(w, SQNANO_TOOBIG, "response buffer too small") : SQNANO_OK;
}

static int run_with(struct sqnano_db *db, const struct rq *rq, struct cw *w,
		    const struct sqnano_query *q, struct sqnano_exec *x);

/* A precompiled query; its state comes from the statement pool. */
static int op_run(struct sqnano_db *db, const struct rq *rq, struct cw *w, bool allow_write)
{
	const struct sqnano_query *q = NULL;
	struct sqnano_exec *x;
	char name[64];
	int rc;

	if (rq->q.type == SQNANO_TEXT && rq->q.n < sizeof(name) && db->schema) {
		memcpy(name, rq->q.u.p, rq->q.n);
		name[rq->q.n] = 0;
		q = sqnano_query_find(db->schema, name);
	}
	if (!q) {
		return error(w, SQNANO_NOTFOUND, "no precompiled query of that name");
	}
	if (q->kind != SQNANO_Q_SELECT && (q->kind != SQNANO_Q_WRITE || !allow_write)) {
		return error(w, SQNANO_READONLY, "this query writes; enable CONFIG_SQNANO_REMOTE_WRITE");
	}
	if (rq->np > q->nparams) {
		return error(w, SQNANO_RANGE, "too many parameters");
	}
	x = sqnano_state_alloc(db->schema, q->state_size);
	if (!x) {
		return error(w, SQNANO_NOMEM, "statement pool full (CONFIG_SQNANO_STMT_POOL_SIZE)");
	}
	rc = run_with(db, rq, w, q, x);
	sqnano_state_free(db->schema, x);
	return rc;
}

static int run_with(struct sqnano_db *db, const struct rq *rq, struct cw *w,
		    const struct sqnano_query *q, struct sqnano_exec *x)
{
	uint16_t count = 0;
	size_t at, cap;
	bool more = false;
	int rc;

	rc = sqnano_exec_open(x, q, db, NULL);
	if (rc) {
		return error(w, rc, "cannot run the query");
	}
	for (uint8_t i = 0; i < rq->np; i++) {
		x->params[i] = rq->p[i];
	}
	cw_head(w, 5, 4);
	cw_text(w, "c");
	cw_head(w, 4, q->ncols);
	for (uint8_t i = 0; i < q->ncols; i++) {
		cw_text(w, q->cols[i]);
	}
	cw_text(w, "rows");
	at = cw_array16(w);
	tail_reserve(w, &cap, 6 + 3 + 9);
	for (;;) {
		size_t before = w->len;

		if ((rq->n >= 0 && count >= rq->n) || count == UINT16_MAX) {
			more = true;
			break;
		}
		rc = sqnano_exec_step(x);
		if (rc != SQNANO_ROW) {
			break;
		}
		cw_head(w, 4, q->ncols);
		for (uint8_t i = 0; i < q->ncols; i++) {
			cw_value(w, &x->out[i]);
		}
		if (!item_fits(w, before)) {
			more = true;
			break;
		}
		count++;
	}
	w->cap = cap;
	if (more || rc == SQNANO_ROW) {
		(void)sqnano_exec_reset(x);
		rc = SQNANO_DONE;
	}
	if (rc != SQNANO_DONE) {
		char msg[SQNANO_ERRMSG_SIZE];

		memcpy(msg, x->errmsg, sizeof(msg));
		(void)sqnano_exec_reset(x);
		return error(w, rc, msg[0] ? msg : "query failed");
	}
	if (count == 0 && more && rq->n != 0) {
		return error(w, SQNANO_TOOBIG, "row larger than the response buffer");
	}
	cw_patch16(w, at, count);
	cw_text(w, "more");
	cw_simple(w, more ? 0xf5 : 0xf4);
	cw_text(w, "ch");
	cw_int(w, x->changes);
	return w->full ? error(w, SQNANO_TOOBIG, "response buffer too small") : SQNANO_OK;
}

int sqnano_remote_handle(const struct sqnano_remote_db *dbs, uint8_t ndbs, const uint8_t *req,
			 size_t len, uint8_t *out, size_t cap, size_t *olen, bool allow_write)
{
	struct cw w = {out, cap, 0, false};
	struct sqnano_db *db = NULL;
	struct rq rq;
	int rc;

	sqnano_mutex_lock(&remote_lock);
	if (!parse(&rq, req, len) || rq.op.type != SQNANO_TEXT) {
		rc = error(&w, SQNANO_MISUSE, "malformed request");
	} else if (key_is(&rq.op, "info")) {
		rc = op_info(dbs, ndbs, &w);
	} else if (rq.db < 0 || rq.db >= ndbs || !dbs[rq.db].db || !dbs[rq.db].db->is_open) {
		rc = error(&w, SQNANO_NOTFOUND, "no such database");
	} else {
		db = dbs[rq.db].db;
		if (key_is(&rq.op, "tables")) {
			rc = op_tables(db, &rq, &w);
		} else if (key_is(&rq.op, "scan")) {
			rc = op_scan(db, &rq, &w);
		} else if (key_is(&rq.op, "run")) {
			rc = op_run(db, &rq, &w, allow_write);
		} else {
			rc = error(&w, SQNANO_MISUSE, "unknown operation");
		}
	}
	sqnano_mutex_unlock(&remote_lock);
	*olen = w.len;
	return rc;
}
