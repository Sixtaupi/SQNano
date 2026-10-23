/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Database lifecycle: mount, schema creation, versioned migrations and the
 * MCUboot protocol.
 *
 * A migration runs in a copy-on-write transaction. Its commit records the
 * previous master root as "pinned": the pages of the pre-migration state
 * are not reused until sqnano_migration_confirm(). If MCUboot reverts to the
 * previous image, the older firmware finds a newer schema whose pinned
 * state matches its own catalog, and reopens that state.
 *
 * A lazy migration (vN_lazy.sql) changes no record: each record carries the
 * version it was written with, and the decoder applies the steps it has not
 * seen. The superblock keeps the oldest version still to apply everywhere
 * (lazy_pending) until sqnano_migration_step() has rewritten every older
 * record; a firmware without that version's conversion refuses the database.
 */

#include "../sqnano_int.h"

int sqnano_index_build(struct sqnano_exec *x, const struct sqnano_table *t,
		       const struct sqnano_index *idx);
int sqnano_row_rewrite(struct sqnano_exec *x, const struct sqnano_table *t,
		       const struct sqnano_keybuf *rk, const uint8_t *rec, uint16_t len);

/* Minimal query used to run engine code with an exec header. */
static void noop_init(struct sqnano_exec *x)
{
	(void)x;
}

static int noop_step(struct sqnano_exec *x)
{
	(void)x;
	return SQNANO_DONE;
}

static void noop_close(struct sqnano_exec *x)
{
	(void)x;
}

/* An exec header with a scratch area: lazy conversions compute text values. */
struct internal_exec {
	struct sqnano_exec x;
	uint8_t scratch[SQNANO_SCRATCH_SIZE];
};

static const struct sqnano_query internal_query = {
	.name = "<internal>",
	.init = noop_init,
	.step = noop_step,
	.close = noop_close,
	.state_size = sizeof(struct internal_exec),
	.kind = SQNANO_Q_WRITE,
};

static int create_catalog(struct sqnano_txn *t, const struct sqnano_catalog *cat)
{
	int rc = SQNANO_OK;

	for (int i = 0; i < cat->ntables && rc == SQNANO_OK; i++) {
		rc = bt_tree_create(t, cat->tables[i]->tree);
	}
	for (int i = 0; i < cat->nindexes && rc == SQNANO_OK; i++) {
		rc = bt_tree_create(t, cat->indexes[i]->tree);
	}
	return rc;
}

static void set_version(struct sqnano_txn *t, const struct sqnano_catalog *cat)
{
	t->sb.schema_version = cat->version;
	t->sb.compat_version = cat->compat;
	t->sb.schema_hash = cat->hash;
}

static int create_schema(struct sqnano_db *db, const struct sqnano_catalog *cat)
{
	struct sqnano_txn t;
	int rc;

	db_lock(db);
	rc = pg_begin(db, &t, SQNANO_TXN_WRITE, -1);
	if (rc == SQNANO_OK) {
		rc = create_catalog(&t, cat);
		set_version(&t, cat);
		rc = rc ? (pg_rollback(&t), rc) : pg_commit(&t);
	}
	db_unlock(db);
	return rc;
}

static int run_steps(struct sqnano_exec *x, const struct sqnano_catalog *cat)
{
	int rc = SQNANO_OK;

	for (int i = 0; i < cat->nsteps && rc == SQNANO_OK; i++) {
		const struct sqnano_mig_step *s = &cat->steps[i];

		switch (s->op) {
		case SQNANO_MIG_CREATE_TREE:
			rc = bt_tree_create(x->txn, s->tree);
			break;
		case SQNANO_MIG_DROP_TREE:
			rc = bt_tree_drop(x->txn, s->tree);
			break;
		case SQNANO_MIG_BUILD_INDEX:
			rc = sqnano_index_build(x, s->table, s->index);
			break;
		default:
			rc = SQNANO_INTERNAL;
			break;
		}
	}
	return rc;
}

static void internal_open(struct internal_exec *ix, struct sqnano_db *db, struct sqnano_txn *t)
{
	(void)sqnano_exec_open(&ix->x, &internal_query, db, t);
	ix->x.txn = t;
	ix->x.scratch.buf = ix->scratch;
	ix->x.scratch.cap = sizeof(ix->scratch);
}

static int migrate(struct sqnano_db *db, const struct sqnano_schema *s, int from)
{
	struct internal_exec ix;
	struct sqnano_exec *x = &ix.x;
	struct sqnano_txn t;
	int rc;

	db_lock(db);
	rc = pg_begin(db, &t, SQNANO_TXN_WRITE | SQNANO_TXN_COW, -1);
	db_unlock(db);
	if (rc) {
		return rc;
	}
	internal_open(&ix, db, &t);
	for (int v = from + 1; v < s->nversions && rc == SQNANO_OK; v++) {
		const struct sqnano_catalog *cat = s->versions[v];

		db_lock(db);
		rc = run_steps(x, cat);
		db_unlock(db);
		if (rc == SQNANO_OK && cat->up) {
			rc = cat->up(x);
		}
		if (cat->lazy && !t.sb.lazy_pending) {
			/* Records stay as they are: they are converted when read. */
			t.sb.lazy_pending = cat->version;
			db->lazy_table = 0;
			db->lazy_key.len = 0;
		}
		set_version(&t, cat);
	}
	db_lock(db);
	if (rc == SQNANO_OK) {
		/* Pin the pre-migration state until the image is confirmed. */
		t.sb.pinned_root = db->sb.master_root;
		t.sb.pinned_version = db->sb.schema_version;
		t.sb.pinned_compat = db->sb.compat_version;
		t.sb.pinned_hash = db->sb.schema_hash;
		t.sb.pinned_seq = db->sb.seq;
		t.sb.pinned_lazy = db->sb.lazy_pending;
		for (uint32_t w = 0; w < SQNANO_BM_WORDS; w++) {
			db->bm_pinned[w] = (db->bm_used[w] | db->bm_freed[w]) & ~db->bm_alloc[w];
		}
		rc = pg_commit(&t);
		if (rc == SQNANO_OK) {
			rc = pg_rebuild(db);
		}
	} else {
		(void)pg_rollback(&t);
	}
	db_unlock(db);
	return rc;
}

/* An older firmware after an MCUboot revert: go back to the pinned state. */
static int revert(struct sqnano_db *db)
{
	struct sqnano_txn t;
	int rc;

	db_lock(db);
	rc = pg_begin(db, &t, SQNANO_TXN_WRITE | SQNANO_TXN_COW, -1);
	if (rc == SQNANO_OK) {
		t.master_root = db->sb.pinned_root;
		t.sb.schema_version = db->sb.pinned_version;
		t.sb.compat_version = db->sb.pinned_compat;
		t.sb.schema_hash = db->sb.pinned_hash;
		t.sb.lazy_pending = db->sb.pinned_lazy;
		t.sb.pinned_root = 0;
		t.sb.pinned_version = 0;
		t.sb.pinned_compat = 0;
		t.sb.pinned_hash = 0;
		t.sb.pinned_seq = 0;
		t.sb.pinned_lazy = 0;
		t.gen++;
		rc = pg_commit(&t);
		if (rc == SQNANO_OK) {
			rc = pg_rebuild(db);
		}
	}
	db_unlock(db);
	return rc;
}

static int schema_sync(struct sqnano_db *db, const struct sqnano_schema *s)
{
	const struct sqnano_catalog *cur = SQNANO_CATALOG(s);
	const struct sqnano_sb *sb = &db->sb;

	if (sb->lazy_pending && sb->lazy_pending < s->versions[0]->version &&
	    !(sb->pinned_root && sb->pinned_version == cur->version && sb->pinned_hash == cur->hash)) {
		/* Records may still need a conversion this firmware does not have. */
		return SQNANO_CANTOPEN_SCHEMA;
	}
	if (sb->schema_version == 0) {
		if (db->read_only) {
			return SQNANO_READONLY;
		}
		return create_schema(db, cur);
	}
	if (sb->schema_version == cur->version) {
		return sb->schema_hash == cur->hash ? SQNANO_OK : SQNANO_CANTOPEN_SCHEMA;
	}
	if (sb->schema_version < cur->version) {
		for (int v = 0; v < s->nversions; v++) {
			if (s->versions[v]->version == sb->schema_version) {
				if (s->versions[v]->hash != sb->schema_hash) {
					return SQNANO_CANTOPEN_SCHEMA;
				}
				if (db->read_only) {
					return SQNANO_READONLY;
				}
				return migrate(db, s, v);
			}
		}
		/* Older than CONFIG_SQNANO_MIGRATE_FROM_MIN. */
		return SQNANO_CANTOPEN_SCHEMA;
	}
	if (sb->pinned_root && sb->pinned_version == cur->version && sb->pinned_hash == cur->hash) {
		if (db->read_only) {
			return SQNANO_READONLY;
		}
		return revert(db);
	}
	if (cur->version >= sb->compat_version) {
		/* Newer database, compatible with this firmware (expand/contract). */
		return SQNANO_OK;
	}
	return SQNANO_CANTOPEN_SCHEMA;
}

int sqnano_db_open(struct sqnano_db *db, struct sqnano_media *media,
		   const struct sqnano_db_config *cfg, const struct sqnano_schema *schema)
{
	struct sqnano_db_config def = {.create = true};
	int rc;

	if (!db || !media || !media->api) {
		return SQNANO_MISUSE;
	}
	memset(db, 0, sizeof(*db));
	db->media = media;
	db->schema = schema;
	sqnano_mutex_init(&db->lock);
	sqnano_cond_init(&db->cond);
	db_lock(db);
	rc = pg_mount(db, cfg ? cfg : &def);
	if (rc == SQNANO_OK) {
		db->is_open = 1;
	}
	db_unlock(db);
	if (rc == SQNANO_OK && schema) {
		rc = schema_sync(db, schema);
		if (rc) {
			db->is_open = 0;
		}
	}
	return rc;
}

int sqnano_db_close(struct sqnano_db *db)
{
	int busy = 0;

	if (!db->is_open) {
		return SQNANO_OK;
	}
	db_lock(db);
	busy = db->writer_active;
	for (int i = 0; i < SQNANO_MAX_READERS; i++) {
		busy |= db->reader_used[i];
	}
	if (!busy) {
		db->is_open = 0;
	}
	db_unlock(db);
	return busy ? SQNANO_BUSY : SQNANO_OK;
}

int sqnano_migration_confirm(struct sqnano_db *db)
{
	struct sqnano_txn t;
	int rc;

	if (!db->is_open) {
		return SQNANO_MISUSE;
	}
	if (!db->sb.pinned_root) {
		return SQNANO_OK;
	}
	db_lock(db);
	rc = pg_begin(db, &t, SQNANO_TXN_WRITE, -1);
	if (rc == SQNANO_OK) {
		t.sb.pinned_root = 0;
		t.sb.pinned_version = 0;
		t.sb.pinned_compat = 0;
		t.sb.pinned_hash = 0;
		t.sb.pinned_seq = 0;
		t.sb.pinned_lazy = 0;
		rc = pg_commit(&t);
	}
	db_unlock(db);
	return rc;
}

/* ---- Lazy migrations ---- */

int sqnano_migration_step(struct sqnano_db *db, uint32_t max_rows)
{
	const struct sqnano_catalog *cat;
	struct internal_exec ix;
	struct sqnano_exec *x = &ix.x;
	struct sqnano_txn t;
	uint32_t done = 0;
	bool more = false;
	int rc;

	if (!db || !db->is_open || !db->schema) {
		return SQNANO_MISUSE;
	}
	if (!db->sb.lazy_pending) {
		return SQNANO_DONE;
	}
	cat = SQNANO_CATALOG(db->schema);
	db_lock(db);
	rc = pg_begin(db, &t, SQNANO_TXN_WRITE, -1);
	if (rc) {
		db_unlock(db);
		return rc;
	}
	internal_open(&ix, db, &t);
	for (; db->lazy_table < cat->ntables && rc == SQNANO_OK && !more; db->lazy_table++) {
		const struct sqnano_table *tab = cat->tables[db->lazy_table];
		struct sqnano_cursor c;
		uint16_t last;

		if (!tab->nlazy) {
			db->lazy_key.len = 0;
			continue;
		}
		last = tab->lazy[tab->nlazy - 1].version;
		rc = bt_open(&t, &c, tab->tree);
		if (rc == SQNANO_OK) {
			/* Resume after the last key converted (or from the start). */
			rc = db->lazy_key.len ? bt_seek(&c, db->lazy_key.b, db->lazy_key.len,
							SQNANO_SEEK_GE, NULL)
					      : bt_seek(&c, NULL, 0, SQNANO_SEEK_FIRST, NULL);
		}
		while (rc == SQNANO_OK && c.state == CUR_VALID) {
			uint16_t rl;
			bool exact;

			rc = bt_value(&c, db->rowbuf, sizeof(db->rowbuf), &rl);
			if (rc) {
				break;
			}
			if (sqnano_record_version(db->rowbuf, rl) >= last) {
				rc = bt_next(&c);
				continue;
			}
			if (done == max_rows) {
				more = true;
				break;
			}
			sqnano_scratch_reset(x);
			db->lazy_key.len = c.klen;
			memcpy(db->lazy_key.b, c.key, c.klen);
			rc = sqnano_row_rewrite(x, tab, &db->lazy_key, db->rowbuf, rl);
			done++;
			if (rc == SQNANO_OK) {
				/* The tree changed under the cursor: find the next key again. */
				rc = bt_open(&t, &c, tab->tree);
			}
			if (rc == SQNANO_OK) {
				rc = bt_seek(&c, db->lazy_key.b, db->lazy_key.len, SQNANO_SEEK_GE, &exact);
			}
			if (rc == SQNANO_OK && c.state == CUR_VALID && exact) {
				rc = bt_next(&c);
			}
		}
		if (!more) {
			db->lazy_key.len = 0;
		} else {
			break;
		}
	}
	if (rc == SQNANO_OK && !more) {
		/* Every record is converted: the old versions are not needed any more. */
		t.sb.lazy_pending = 0;
		db->lazy_table = 0;
	}
	rc = rc ? (pg_rollback(&t), rc) : pg_commit(&t);
	db_unlock(db);
	if (rc) {
		db->lazy_table = 0;
		db->lazy_key.len = 0;
		return rc;
	}
	return more ? SQNANO_OK : SQNANO_DONE;
}

int32_t sqnano_db_user_version(struct sqnano_db *db)
{
	return db->sb.user_version;
}

int sqnano_db_stats(struct sqnano_db *db, struct sqnano_stats *st)
{
	memset(st, 0, sizeof(*st));
	db_lock(db);
	st->page_size = db->page_size;
	st->pages_total = db->npages;
	for (uint32_t p = 1; p <= db->npages; p++) {
		st->pages_used += bm_test(db->bm_used, p);
		st->pages_limbo += bm_test(db->bm_limbo, p);
		st->pages_pinned += bm_test(db->bm_pinned, p) && !bm_test(db->bm_used, p);
	}
	st->seq = db->sb.seq;
	st->schema_version = db->sb.schema_version;
	st->compat_version = db->sb.compat_version;
	st->mode = db->mode;
	st->pinned = db->sb.pinned_root != 0;
	st->reads = db->stat_reads;
	st->writes = db->stat_writes;
	st->erases = db->stat_erases;
	st->cache_hits = db->stat_hits;
	st->compactions = db->stat_gc;
	st->sort_runs = db->stat_sort_runs;
	st->lazy_pending = db->sb.lazy_pending;
	db_unlock(db);
	return SQNANO_OK;
}

int sqnano_db_check(struct sqnano_db *db)
{
	static uint32_t seen[SQNANO_BM_WORDS];
	static SQNANO_MUTEX_DEFINE(check_lock);
	struct sqnano_txn t;
	struct sqnano_cursor c;
	uint8_t val[4];
	uint16_t vlen;
	int rc;

	if (!db->is_open) {
		return SQNANO_MISUSE;
	}
	sqnano_mutex_lock(&check_lock);
	memset(seen, 0, sizeof(seen));
	db_lock(db);
	rc = pg_begin(db, &t, SQNANO_TXN_READ, 0);
	if (rc == SQNANO_OK) {
		rc = bt_check(&t, t.master_root, seen);
		if (rc == SQNANO_OK) {
			rc = bt_open(&t, &c, 0);
		}
		if (rc == SQNANO_OK) {
			rc = bt_seek(&c, NULL, 0, SQNANO_SEEK_FIRST, NULL);
		}
		while (rc == SQNANO_OK && c.state == CUR_VALID) {
			bool seq = c.klen == 2 && (c.key[0] & 0x80);

			if (!seq) {
				rc = bt_value(&c, val, sizeof(val), &vlen);
				if (rc == SQNANO_OK && vlen != 4) {
					rc = SQNANO_CORRUPT;
				}
				if (rc == SQNANO_OK) {
					rc = bt_check(&t, get_u32(val), seen);
				}
			}
			if (rc == SQNANO_OK) {
				rc = bt_next(&c);
			}
		}
		if (rc == SQNANO_OK && !db->writer_active) {
			for (uint32_t p = 1; p <= db->npages; p++) {
				if (bm_test(seen, p) != bm_test(db->bm_used, p)) {
					rc = SQNANO_CORRUPT;
					break;
				}
			}
		}
		(void)pg_commit(&t);
	}
	db_unlock(db);
	sqnano_mutex_unlock(&check_lock);
	return rc;
}

int sqnano_db_foreign_keys(struct sqnano_db *db, int on)
{
	int prev = db->fk_on;

	if (!SQNANO_FOREIGN_KEYS) {
		return on > 0 ? -1 : 0;
	}
	if (on >= 0) {
		db->fk_on = on ? 1 : 0;
	}
	return prev;
}

int sqnano_db_set_user_version(struct sqnano_db *db, struct sqnano_txn *txn, int32_t v)
{
	struct sqnano_txn t;
	int rc;

	if (txn) {
		if (!txn->write) {
			return SQNANO_READONLY;
		}
		txn->sb.user_version = v;
		return SQNANO_OK;
	}
	db_lock(db);
	rc = pg_begin(db, &t, SQNANO_TXN_WRITE, -1);
	if (rc == SQNANO_OK) {
		t.sb.user_version = v;
		rc = pg_commit(&t);
	}
	db_unlock(db);
	return rc;
}
