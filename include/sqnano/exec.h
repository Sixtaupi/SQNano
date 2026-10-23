/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Compiled schema, compiled queries and the runtime they call.
 *
 * sqnanoc translates the schema and every SQL statement into C code that
 * uses the declarations of this header. Application code normally uses the
 * typed API generated for each named query, or the sqlite3.h facade.
 */

#ifndef SQNANO_EXEC_H_
#define SQNANO_EXEC_H_

#include <sqnano/sqnano.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @name Column affinities (same letters as SQLite)
 * @{
 */
#define SQNANO_AFF_NONE    0
#define SQNANO_AFF_BLOB    'A'
#define SQNANO_AFF_TEXT    'B'
#define SQNANO_AFF_NUMERIC 'C'
#define SQNANO_AFF_INTEGER 'D'
#define SQNANO_AFF_REAL    'E'
/** @} */

/** @name Collations
 * @{
 */
#define SQNANO_COLL_BINARY 0
#define SQNANO_COLL_NOCASE 1
#define SQNANO_COLL_RTRIM  2
/** @} */

/** @name Conflict resolution
 * @{
 */
#define SQNANO_OE_ABORT    0
#define SQNANO_OE_ROLLBACK 1
#define SQNANO_OE_FAIL     2
#define SQNANO_OE_IGNORE   3
#define SQNANO_OE_REPLACE  4
#define SQNANO_OE_UPSERT   5
/** @} */

/** Returned by sqnano_row_insert() with SQNANO_OE_UPSERT on a conflict. */
#define SQNANO_CONFLICT 1019

struct sqnano_index;
/** UPSERT conflict target designating the rowid (INTEGER PRIMARY KEY). */
extern const struct sqnano_index sqnano_rowid_target;

/** Size of the error message buffer of a statement. */
#define SQNANO_ERRMSG_SIZE 96

#define SQNANO_COL_NOTNULL     0x01
#define SQNANO_COL_IPK         0x02
#define SQNANO_COL_HAS_DEFAULT 0x04

#define SQNANO_IDX_UNIQUE  0x01
#define SQNANO_IDX_AUTO    0x02
/** The PRIMARY KEY of a WITHOUT ROWID table: its tree is the table's. */
#define SQNANO_IDX_PRIMARY 0x04

struct sqnano_exec;

/** A column of a compiled table. */
struct sqnano_column {
	const char *name;
	/** Declared type, as written in CREATE TABLE. */
	const char *type;
	/** Default value (NULL when none). */
	sqnano_value dflt;
	/** Physical position in records; never reused for another column. */
	uint8_t slot;
	uint8_t aff;
	uint8_t coll;
	uint8_t flags;
};

/** A compiled index. */
struct sqnano_index {
	const char *name;
	const char *sql;
	/** Column numbers in the table. */
	const uint8_t *cols;
	const uint8_t *desc;
	const uint8_t *coll;
	uint16_t tree;
	uint8_t table;
	uint8_t ncols;
	uint8_t flags;
};

/** @name Foreign key actions
 * @{
 */
#define SQNANO_FK_NOACTION   0
#define SQNANO_FK_RESTRICT   1
#define SQNANO_FK_SETNULL    2
#define SQNANO_FK_SETDEFAULT 3
#define SQNANO_FK_CASCADE    4
/** @} */
#define SQNANO_FK_MAX_COLS 8

struct sqnano_table;

/** A FOREIGN KEY constraint, enforced while PRAGMA foreign_keys is on. */
struct sqnano_fkey {
	const struct sqnano_table *child;
	const struct sqnano_table *parent;
	/** UNIQUE index on the parent key, or NULL when it is the rowid. */
	const struct sqnano_index *pidx;
	/** Index of the child table led by the key columns, or NULL (scan). */
	const struct sqnano_index *cidx;
	/** Child column of each key column, in parent index order. */
	const uint8_t *ccols;
	/** Parent column of each key column. */
	const uint8_t *pcols;
	/** With cidx: key column of each leading cidx column. */
	const uint8_t *cpos;
	uint8_t ncols;
	uint8_t on_delete;
	uint8_t on_update;
	uint8_t deferred;
};

/**
 * One step of a lazy migration (an UPDATE of vN_lazy.sql) for a table:
 * applied when a record older than @c version is decoded. @p s holds every
 * slot of the record, @p have tells which ones the record had (the others
 * take their default in the function).
 */
struct sqnano_lazy {
	uint16_t version;
	int (*fn)(struct sqnano_exec *x, sqnano_value *s, uint8_t *have);
};

/**
 * A compiled table. A rowid table is a tree keyed by rowid; a WITHOUT ROWID
 * table is keyed by its PRIMARY KEY (pk), and its index entries end with
 * that key instead of the rowid.
 */
struct sqnano_table {
	const char *name;
	const char *sql;
	const struct sqnano_column *cols;
	const struct sqnano_index *const *indexes;
	/** CHECK constraints, or NULL. Returns SQNANO_OK or an error. */
	int (*check)(struct sqnano_exec *x, const sqnano_value *row);
	uint32_t sql_hash;
	uint16_t tree;
	uint8_t ncols;
	uint8_t nslots;
	/** Column that aliases the rowid (INTEGER PRIMARY KEY), or -1. */
	int8_t ipk;
	uint8_t nindexes;
	uint8_t flags;
	/** Foreign keys where this table is the child or the parent. */
	const struct sqnano_fkey *const *fks;
	uint8_t nfks;
	/** PRIMARY KEY of a WITHOUT ROWID table, NULL for a rowid table. */
	const struct sqnano_index *pk;
	/** Lazy migration steps, oldest first. */
	const struct sqnano_lazy *lazy;
	uint8_t nlazy;
	/** Catalog version: written into the records. */
	uint16_t version;
};

/** Rowids are never reused (AUTOINCREMENT). */
#define SQNANO_TBL_AUTOINC 0x01

#define SQNANO_MIG_CREATE_TREE 1
#define SQNANO_MIG_DROP_TREE   2
#define SQNANO_MIG_BUILD_INDEX 3

/** One structural migration step. */
struct sqnano_mig_step {
	uint8_t op;
	uint16_t tree;
	const struct sqnano_table *table;
	const struct sqnano_index *index;
};

/** One version of the catalog. */
struct sqnano_catalog {
	const struct sqnano_table *const *tables;
	const struct sqnano_index *const *indexes;
	/** Steps that lead from the previous version to this one. */
	const struct sqnano_mig_step *steps;
	/** Data transformation (vN_up.sql), or NULL. */
	int (*up)(struct sqnano_exec *x);
	uint32_t hash;
	uint16_t version;
	/** Oldest version whose firmware can still use this database. */
	uint16_t compat;
	uint8_t ntables;
	uint8_t nindexes;
	uint8_t nsteps;
	/** This version has a lazy migration (vN_lazy.sql). */
	uint8_t lazy;
};

struct sqnano_query;

/** Everything sqnanoc compiled for an application. */
struct sqnano_schema {
	/** Catalog versions in increasing order; the last one is current. */
	const struct sqnano_catalog *const *versions;
	/** Queries sorted by hash. */
	const struct sqnano_query *const *queries;
	/** Statement states for the sqlite3.h facade, the shell and the remote server, or NULL. */
	struct sqnano_pool *pool;
	uint16_t nqueries;
	uint8_t nversions;
};

/** @name Statement state pool
 * Generated with the queries, see src/core/pool.c.
 * @{
 */
#define SQNANO_POOL_BLOCKS (SQNANO_POOL_SLOTS + 1)

struct sqnano_pool {
	uint8_t *buf;
	uint32_t size;
	/** Fixed slot size (the largest state), or 0 for a first-fit arena. */
	uint32_t slot;
	/** Largest state of the application. */
	uint32_t largest;
	uint32_t used;
	uint32_t peak;
	uint32_t fails;
	uint8_t nblk;
	struct {
		uint32_t off;
		uint32_t len;
	} blk[SQNANO_POOL_BLOCKS];
};

struct sqnano_pool_stats {
	uint32_t size;
	/** Bytes of the states in use now, and at most since boot. */
	uint32_t used;
	uint32_t peak;
	/** Allocations refused for lack of room. */
	uint32_t fails;
	uint32_t largest;
	uint32_t statements;
};

/** A statement state of @p n bytes (8-byte aligned), or NULL. */
void *sqnano_state_alloc(const struct sqnano_schema *s, uint32_t n);
void sqnano_state_free(const struct sqnano_schema *s, void *mem);
int sqnano_pool_stats(const struct sqnano_schema *s, struct sqnano_pool_stats *st);
/** @} */

/** Current catalog of a schema. */
#define SQNANO_CATALOG(s) ((s)->versions[(s)->nversions - 1])

#define SQNANO_Q_SELECT   1
#define SQNANO_Q_WRITE    2
#define SQNANO_Q_BEGIN    3
#define SQNANO_Q_COMMIT   4
#define SQNANO_Q_ROLLBACK 5
/** Statement accepted without effect (CREATE ... IF NOT EXISTS matching the schema). */
#define SQNANO_Q_NOOP     6
/** Schema change found in application code: fails when executed. */
#define SQNANO_Q_FAIL     7

/** A compiled statement. */
struct sqnano_query {
	const char *name;
	/** Normalized SQL text. */
	const char *sql;
	/** Parameter names (":id", "?3"), one per parameter. */
	const char *const *params;
	const char *const *cols;
	const char *const *decltypes;
	/** Point the exec header at the parameter and output arrays. */
	void (*init)(struct sqnano_exec *x);
	int (*step)(struct sqnano_exec *x);
	/** Release cursors and sort buffers. */
	void (*close)(struct sqnano_exec *x);
	uint32_t hash;
	uint16_t state_size;
	uint8_t nparams;
	uint8_t ncols;
	uint8_t kind;
};

/** Bump allocator for computed text values, reset for every row. */
struct sqnano_scratch {
	uint8_t *buf;
	uint16_t used;
	uint16_t cap;
};

/**
 * @brief Execution header placed at the start of every query state.
 */
struct sqnano_exec {
	const struct sqnano_query *q;
	struct sqnano_db *db;
	struct sqnano_txn *txn;
	sqnano_value *params;
	sqnano_value *out;
	struct sqnano_scratch scratch;
	int64_t changes;
	int64_t last_rowid;
	int64_t conflict_rowid;
	int32_t busy_ms;
	uint32_t gen0;
	/* Statement savepoint inside an explicit transaction. */
	uint32_t sp_id;
	uint16_t pc;
	uint8_t own;
	uint8_t running;
	uint8_t has_sp;
	/* A row was returned: the statement cannot be restarted. */
	uint8_t yielded;
	uint8_t retried;
	/* Conflict resolution of the constraint that failed (SQNANO_OE_*). */
	uint8_t on_error;
	/* Nesting of foreign key actions. */
	uint8_t fk_depth;
	/* Immediate foreign key violations of the statement. */
	int32_t fk_count;
	struct sqnano_txn *ext_txn;
	struct sqnano_txn own_txn;
	char errmsg[SQNANO_ERRMSG_SIZE];
};

/**
 * @brief Prepare a query state.
 *
 * @param x State memory of q->state_size bytes.
 * @param txn Transaction to run in, or NULL for autocommit.
 */
int sqnano_exec_open(struct sqnano_exec *x, const struct sqnano_query *q, struct sqnano_db *db,
		     struct sqnano_txn *txn);
/** Run until the next row (SQNANO_ROW), completion (SQNANO_DONE) or error. */
int sqnano_exec_step(struct sqnano_exec *x);
/** Stop the statement; an autocommit write transaction is committed. */
int sqnano_exec_reset(struct sqnano_exec *x);
/** Run a statement to completion. */
int sqnano_exec_run(struct sqnano_exec *x);
/** Find a compiled query by name. */
const struct sqnano_query *sqnano_query_find(const struct sqnano_schema *s, const char *name);
/** Set a formatted error message and return @p rc. */
int sqnano_exec_error(struct sqnano_exec *x, int rc, const char *fmt, ...);

/** @name Expression runtime
 * Every function returns SQNANO_OK or an error code.
 * @{
 */
void *sqnano_scratch_alloc(struct sqnano_exec *x, uint32_t n);
/**
 * @brief Copy @p src into @p dst, text and blob bytes into @p buf.
 *
 * Keeps a subquery result valid after the row it came from is gone.
 * @retval SQNANO_TOOBIG The value does not fit in @p cap bytes.
 */
int sqnano_value_save(struct sqnano_exec *x, sqnano_value *dst, const sqnano_value *src,
		      uint8_t *buf, uint32_t cap);
static inline void sqnano_scratch_reset(struct sqnano_exec *x)
{
	x->scratch.used = 0;
}

#define SQNANO_OP_EQ    1
#define SQNANO_OP_NE    2
#define SQNANO_OP_LT    3
#define SQNANO_OP_LE    4
#define SQNANO_OP_GT    5
#define SQNANO_OP_GE    6
#define SQNANO_OP_IS    7
#define SQNANO_OP_ISNOT 8

#define SQNANO_OP_ADD    1
#define SQNANO_OP_SUB    2
#define SQNANO_OP_MUL    3
#define SQNANO_OP_DIV    4
#define SQNANO_OP_REM    5
#define SQNANO_OP_BITAND 6
#define SQNANO_OP_BITOR  7
#define SQNANO_OP_SHL    8
#define SQNANO_OP_SHR    9

/** Total order of SQLite: NULL < numbers < text < blob. */
int sqnano_v_compare(const sqnano_value *a, const sqnano_value *b, uint8_t coll);
/** Truth value: 1, 0, or -1 for NULL. */
int sqnano_truth(const sqnano_value *v);
/** Apply a column affinity. */
int sqnano_affinity(struct sqnano_exec *x, sqnano_value *v, uint8_t aff);
int sqnano_op_cmp(sqnano_value *r, const sqnano_value *a, const sqnano_value *b, int op,
		  uint8_t coll);
int sqnano_op_arith(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *a,
		    const sqnano_value *b, int op);
int sqnano_op_concat(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *a,
		     const sqnano_value *b);
int sqnano_op_neg(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *a);
int sqnano_op_pos(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *a);
int sqnano_op_bitnot(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *a);
int sqnano_op_not(sqnano_value *r, const sqnano_value *a);
/** Set @p r to the 3-valued boolean @p t (1, 0, -1). */
void sqnano_v_bool(sqnano_value *r, int t);
int sqnano_op_like(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *s,
		   const sqnano_value *pat, const sqnano_value *esc, bool glob);
int sqnano_cast(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *v, uint8_t aff);
/** Convert to text in place (numbers are formatted into the scratch area). */
int sqnano_v_to_text(struct sqnano_exec *x, sqnano_value *v);
/** Format a number as SQLite does; returns the length. */
int sqnano_format_number(const sqnano_value *v, char *buf, size_t cap);

typedef int (*sqnano_fn_t)(struct sqnano_exec *x, sqnano_value *r, int argc,
			   const sqnano_value *argv);
int sqnano_fn_abs(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_length(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_lower(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_upper(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_substr(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_round(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_min(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_max(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_coalesce(struct sqnano_exec *x, sqnano_value *r, int argc,
		       const sqnano_value *argv);
int sqnano_fn_nullif(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_typeof(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_instr(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_trim(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_ltrim(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_rtrim(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_replace(struct sqnano_exec *x, sqnano_value *r, int argc,
		      const sqnano_value *argv);
int sqnano_fn_hex(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
int sqnano_fn_sign(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv);
/** @} */

/** @name Table scans
 * @{
 */
#define SQNANO_SCAN_DESC    0x01
#define SQNANO_SCAN_LO      0x02
#define SQNANO_SCAN_HI      0x04
#define SQNANO_SCAN_EMPTY   0x08
#define SQNANO_SCAN_STARTED 0x10
#define SQNANO_SCAN_EOF     0x20

/** A scan over a table, by rowid or through an index, between two key bounds. */
struct sqnano_scan {
	struct sqnano_cursor tc;
	struct sqnano_cursor ic;
	struct sqnano_exec *x;
	const struct sqnano_table *tab;
	const struct sqnano_index *idx;
	/** Decoded columns of the current row, in table column order. */
	sqnano_value *cols;
	int64_t rowid;
	uint32_t nrows;
	struct sqnano_keybuf lo;
	struct sqnano_keybuf hi;
	uint8_t flags;
	uint16_t rowlen;
	uint8_t row[SQNANO_MAX_RECORD_SIZE];
};

/**
 * Prepare a scan; @p idx NULL scans the table in key order (rowid, or the
 * PRIMARY KEY of a WITHOUT ROWID table).
 */
void sqnano_scan_init(struct sqnano_scan *s, struct sqnano_exec *x, const struct sqnano_table *t,
		      const struct sqnano_index *idx, sqnano_value *cols, uint8_t flags);
/** Constrain the next key column to be equal to @p v. */
int sqnano_scan_eq(struct sqnano_scan *s, const sqnano_value *v, uint8_t aff, uint8_t coll,
		   bool desc);
/** Add a lower (SQNANO_SCAN_LO) or upper (SQNANO_SCAN_HI) bound on the next key column. */
int sqnano_scan_range(struct sqnano_scan *s, uint8_t which, const sqnano_value *v, uint8_t aff,
		      uint8_t coll, bool desc);
/** Load the next row: SQNANO_OK, SQNANO_DONE or an error. */
int sqnano_scan_next(struct sqnano_scan *s);
/**
 * Decode the record @p rec of table @p t into @p cols, converting a record
 * older than a lazy migration (computed values go to the scratch area).
 */
int sqnano_row_decode(struct sqnano_exec *x, const struct sqnano_table *t, int64_t rowid,
		      const uint8_t *rec, uint16_t len, sqnano_value *cols);
/** @} */

/** @name Row operations
 * @{
 */
/**
 * @brief Insert a row.
 *
 * @param row Values in table column order; modified by affinity conversion.
 * @param oe Conflict resolution (SQNANO_OE_*).
 * @param target With SQNANO_OE_UPSERT, the conflict target (NULL: rowid or any).
 */
int sqnano_row_insert(struct sqnano_exec *x, const struct sqnano_table *t, sqnano_value *row,
		      uint8_t oe, const struct sqnano_index *target);
/** Delete a row; for a WITHOUT ROWID table @p rowid is ignored and @p cols give its key. */
int sqnano_row_delete(struct sqnano_exec *x, const struct sqnano_table *t, int64_t rowid,
		      const sqnano_value *cols);
/**
 * @brief Update a row.
 *
 * Record slots unknown to this firmware are preserved from @p oldrec.
 */
int sqnano_row_update(struct sqnano_exec *x, const struct sqnano_table *t, int64_t rowid,
		      const sqnano_value *oldcols, sqnano_value *newcols, const uint8_t *oldrec,
		      uint16_t oldlen, uint8_t oe);
/** Load the row @p rowid into @p s (positioned scan without bounds). */
int sqnano_row_fetch(struct sqnano_scan *s, int64_t rowid);
/** WITHOUT ROWID: load the row whose PRIMARY KEY columns are @p pk (in key order). */
int sqnano_row_fetch_pk(struct sqnano_scan *s, const sqnano_value *pk);
/** Load the row that the last sqnano_row_insert() returning SQNANO_CONFLICT met. */
int sqnano_row_fetch_conflict(struct sqnano_scan *s);
/** @} */

/** @name Sorter
 * Rows are sorted in a buffer taken from a static pool. A sort exceeding the
 * buffer spills sorted runs to temporary pages and merges them
 * (SQNANO_SORT_MAX_RUNS > 0), or returns SQNANO_FULL. All calls are made
 * with the database lock held.
 * @{
 */
struct sqnano_sorter {
	uint8_t *buf;
	/* Entries wanted at most (ORDER BY ... LIMIT), UINT32_MAX for all. */
	uint32_t limit;
	uint16_t used;
	uint16_t n;
	uint16_t pos;
	int8_t slot;
	uint16_t klen;
	const uint8_t *key;
};

int sqnano_sorter_open(struct sqnano_sorter *s, struct sqnano_exec *x);
int sqnano_sorter_add(struct sqnano_sorter *s, struct sqnano_exec *x,
		      const struct sqnano_keybuf *key, const sqnano_value *vals, uint8_t n);
int sqnano_sorter_sort(struct sqnano_sorter *s, struct sqnano_exec *x);
/**
 * Next entry: SQNANO_OK or SQNANO_DONE. The values and the key point into
 * the sort buffer and stay valid until the next call.
 */
int sqnano_sorter_next(struct sqnano_sorter *s, struct sqnano_exec *x, sqnano_value *vals,
		       uint8_t n);
/**
 * Keep the current entry: decode it again into @p vals, from a copy that
 * stays valid until the next hold. The key then points into the copy too.
 */
int sqnano_sorter_hold(struct sqnano_sorter *s, struct sqnano_exec *x, sqnano_value *vals,
		       uint8_t n);
void sqnano_sorter_close(struct sqnano_sorter *s, struct sqnano_exec *x);
/**
 * Only the first @p limit + @p offset entries in sort order will be read
 * (a negative @p limit means all): the sorter may drop the others early.
 */
void sqnano_sorter_limit(struct sqnano_sorter *s, int64_t limit, int64_t offset);
/** @} */

/** @name Aggregates
 * @{
 */
#define SQNANO_AGG_COUNT_STAR 1
#define SQNANO_AGG_COUNT      2
#define SQNANO_AGG_SUM        3
#define SQNANO_AGG_TOTAL      4
#define SQNANO_AGG_AVG        5
#define SQNANO_AGG_MIN        6
#define SQNANO_AGG_MAX        7

struct sqnano_agg {
	int64_t count;
	int64_t isum;
	double rsum;
	double rerr;
	uint8_t approx;
	uint8_t overflow;
	uint8_t has;
	sqnano_value v;
	uint8_t buf[SQNANO_AGG_TEXT_SIZE];
};

void sqnano_agg_reset(struct sqnano_agg *a);
int sqnano_agg_step(struct sqnano_exec *x, struct sqnano_agg *a, int kind, const sqnano_value *v,
		    uint8_t coll);
int sqnano_agg_final(struct sqnano_exec *x, struct sqnano_agg *a, int kind, sqnano_value *r);
/** @} */

/** @name Parameters
 * @{
 */
/** Evaluate a LIMIT or OFFSET value. */
int sqnano_limit_value(struct sqnano_exec *x, const sqnano_value *v, int64_t *out);
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* SQNANO_EXEC_H_ */
