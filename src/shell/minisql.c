/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Restricted SQL interpreter and dot commands of the diagnostic shell.
 *
 *   SELECT cols FROM table [WHERE cond AND ...] [ORDER BY col [ASC|DESC]] [LIMIT n]
 *   cols: * | col | count(*) | count(col) | sum(col) | total(col) | avg(col) | min(col) | max(col)
 *   cond: col op literal | col IS [NOT] NULL | col BETWEEN lit AND lit | col [NOT] LIKE 'pat'
 *
 * A statement becomes a declarative plan: no loops, no jumps, a bounded
 * number of filters, and its access path is chosen (and shown by EXPLAIN)
 * before it runs. Reads run on a snapshot.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <sqnano/shell.h>

#include "../sqnano_int.h"

#define MS_MAX_COLS    16
#define MS_MAX_FILTERS 8
#define MS_LIT_SPACE   256


enum {
	F_CMP,
	F_ISNULL,
	F_NOTNULL,
	F_LIKE,
	F_NOTLIKE,
};

struct ms_filter {
	int16_t col; /* -1: rowid */
	uint8_t kind;
	uint8_t op;
	sqnano_value v;
};

struct ms_plan {
	const struct sqnano_table *t;
	const struct sqnano_index *idx;
	bool use_rowid;
	uint8_t neq;
	int8_t eq[MS_MAX_FILTERS];
	int8_t lo;
	int8_t hi;
	bool desc;
	bool order_ok;
	uint8_t ncols;
	int16_t cols[MS_MAX_COLS];
	uint8_t agg[MS_MAX_COLS];
	const char *names[MS_MAX_COLS];
	char namebuf[MS_MAX_COLS][24];
	uint8_t nfilters;
	struct ms_filter f[MS_MAX_FILTERS];
	int16_t order_col; /* -2: none, -1: rowid */
	bool order_desc;
	int64_t limit;
	bool explain;
};

/* Working memory of one command; the shell runs one command at a time. */
static struct {
	struct sqnano_exec x;
	uint8_t scratch[256];
	struct sqnano_scan scan;
	sqnano_value cols[SQNANO_MAX_COLUMNS];
	struct sqnano_agg agg[MS_MAX_COLS];
	struct ms_plan plan;
	char lits[MS_LIT_SPACE];
	uint16_t lused;
} w;
static SQNANO_MUTEX_DEFINE(shell_lock);

/* ---- Lexer ---- */

enum {
	T_END,
	T_ID,
	T_NUM,
	T_STR,
	T_BLOB,
	T_OP,
};

struct lex {
	const char *p;
	int kind;
	const char *s;
	size_t n;
};

static bool ms_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool ms_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (unsigned char)c >= 0x80;
}

static bool ms_digit(char c)
{
	return c >= '0' && c <= '9';
}

static void next(struct lex *l)
{
	const char *p = l->p;

	while (ms_space(*p)) {
		p++;
	}
	l->s = p;
	if (!*p) {
		l->kind = T_END;
		l->n = 0;
	} else if ((*p == 'x' || *p == 'X') && p[1] == '\'') {
		p += 2;
		while (*p && *p != '\'') {
			p++;
		}
		l->kind = T_BLOB;
		l->n = (size_t)(p - l->s) + (*p ? 1 : 0);
		p += *p ? 1 : 0;
	} else if (ms_alpha(*p)) {
		while (ms_alpha(*p) || ms_digit(*p)) {
			p++;
		}
		l->kind = T_ID;
		l->n = (size_t)(p - l->s);
	} else if (*p == '"') {
		p++;
		while (*p && *p != '"') {
			p++;
		}
		l->kind = T_ID;
		l->s++;
		l->n = (size_t)(p - l->s);
		p += *p ? 1 : 0;
	} else if (ms_digit(*p) || (*p == '.' && ms_digit(p[1]))) {
		while (ms_digit(*p) || *p == '.' || *p == 'e' || *p == 'E' ||
		       ((*p == '+' || *p == '-') && (p[-1] == 'e' || p[-1] == 'E'))) {
			p++;
		}
		l->kind = T_NUM;
		l->n = (size_t)(p - l->s);
	} else if (*p == '\'') {
		p++;
		while (*p) {
			if (*p == '\'' && p[1] == '\'') {
				p += 2;
				continue;
			}
			if (*p == '\'') {
				break;
			}
			p++;
		}
		l->kind = T_STR;
		l->n = (size_t)(p - l->s) + (*p ? 1 : 0);
		p += *p ? 1 : 0;
	} else {
		static const char *const ops[] = {"<=", ">=", "==", "!=", "<>"};

		l->kind = T_OP;
		l->n = 1;
		for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
			if (p[0] == ops[i][0] && p[1] == ops[i][1]) {
				l->n = 2;
			}
		}
		p += l->n;
	}
	l->p = p;
}

static bool is_word(const struct lex *l, const char *word)
{
	size_t n = strlen(word);

	if (l->kind != T_ID || l->n != n) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		char c = l->s[i];

		if ((c >= 'A' && c <= 'Z' ? c + 32 : c) != word[i]) {
			return false;
		}
	}
	return true;
}

static bool accept(struct lex *l, const char *word)
{
	if (is_word(l, word)) {
		next(l);
		return true;
	}
	return false;
}

static bool is_op(const struct lex *l, const char *op)
{
	return l->kind == T_OP && l->n == strlen(op) && memcmp(l->s, op, l->n) == 0;
}

static bool name_eq(const char *a, const char *b, size_t bn)
{
	for (size_t i = 0; i < bn; i++) {
		char x = a[i], y = b[i];

		if (!x) {
			return false;
		}
		if ((x >= 'A' && x <= 'Z' ? x + 32 : x) != (y >= 'A' && y <= 'Z' ? y + 32 : y)) {
			return false;
		}
	}
	return a[bn] == 0;
}

/* ---- Output ---- */

static void put_value(struct sqnano_shell *sh, const sqnano_value *v, bool quote)
{
	char num[40];

	switch (v->type) {
	case SQNANO_INTEGER:
	case SQNANO_FLOAT:
		(void)sqnano_format_number(v, num, sizeof(num));
		sh->print(sh->arg, "%s", num);
		break;
	case SQNANO_TEXT:
		if (quote) {
			sh->print(sh->arg, "\"");
			for (uint32_t i = 0; i < v->n; i++) {
				char c = v->u.z[i];

				if (c == '"' || c == '\\') {
					sh->print(sh->arg, "\\%c", c);
				} else if ((unsigned char)c < 0x20) {
					sh->print(sh->arg, "\\u%04x", c);
				} else {
					sh->print(sh->arg, "%c", c);
				}
			}
			sh->print(sh->arg, "\"");
		} else {
			sh->print(sh->arg, "%s", v->u.z);
		}
		break;
	case SQNANO_BLOB:
		sh->print(sh->arg, quote ? "\"X'" : "X'");
		for (uint32_t i = 0; i < v->n; i++) {
			sh->print(sh->arg, "%02X", v->u.p[i]);
		}
		sh->print(sh->arg, quote ? "'\"" : "'");
		break;
	default:
		sh->print(sh->arg, quote ? "null" : "");
		break;
	}
}

static int value_width(const sqnano_value *v)
{
	char num[40];

	switch (v->type) {
	case SQNANO_INTEGER:
	case SQNANO_FLOAT:
		return sqnano_format_number(v, num, sizeof(num));
	case SQNANO_TEXT:
		return (int)v->n;
	case SQNANO_BLOB:
		return (int)(2 * v->n + 3);
	default:
		return 0;
	}
}

static int col_width(const char *name)
{
	int n = (int)strlen(name);

	return n < 10 ? 10 : n;
}

static void header(struct sqnano_shell *sh, int n, const char *const *names)
{
	if (sh->mode == SQNANO_SHELL_TABLE) {
		for (int i = 0; i < n; i++) {
			sh->print(sh->arg, "%s%-*s", i ? "  " : "", col_width(names[i]), names[i]);
		}
		sh->print(sh->arg, "\n");
		for (int i = 0; i < n; i++) {
			sh->print(sh->arg, "%s", i ? "  " : "");
			for (int k = 0; k < col_width(names[i]); k++) {
				sh->print(sh->arg, "-");
			}
		}
		sh->print(sh->arg, "\n");
	} else if (sh->mode == SQNANO_SHELL_CSV) {
		for (int i = 0; i < n; i++) {
			sh->print(sh->arg, "%s%s", i ? "," : "", names[i]);
		}
		sh->print(sh->arg, "\n");
	}
}

static void row(struct sqnano_shell *sh, int n, const char *const *names, const sqnano_value *v)
{
	switch (sh->mode) {
	case SQNANO_SHELL_TABLE:
		for (int i = 0; i < n; i++) {
			int pad = col_width(names[i]) - value_width(&v[i]);

			sh->print(sh->arg, "%s", i ? "  " : "");
			put_value(sh, &v[i], false);
			while (i + 1 < n && pad-- > 0) {
				sh->print(sh->arg, " ");
			}
		}
		sh->print(sh->arg, "\n");
		break;
	case SQNANO_SHELL_CSV:
		for (int i = 0; i < n; i++) {
			sh->print(sh->arg, "%s", i ? "," : "");
			put_value(sh, &v[i], v[i].type == SQNANO_TEXT && (strchr(v[i].u.z, ',') ||
									    strchr(v[i].u.z, '"')));
		}
		sh->print(sh->arg, "\n");
		break;
	case SQNANO_SHELL_JSON:
		sh->print(sh->arg, "{");
		for (int i = 0; i < n; i++) {
			sh->print(sh->arg, "%s\"%s\":", i ? "," : "", names[i]);
			put_value(sh, &v[i], true);
		}
		sh->print(sh->arg, "}\n");
		break;
	default:
		for (int i = 0; i < n; i++) {
			sh->print(sh->arg, "%*s = ", col_width(names[i]), names[i]);
			put_value(sh, &v[i], false);
			sh->print(sh->arg, "\n");
		}
		sh->print(sh->arg, "\n");
		break;
	}
}

static int fail(struct sqnano_shell *sh, int rc, const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	sh->print(sh->arg, "error: %s\n", buf);
	return rc;
}

#define UNSUPPORTED "not supported by the shell: use sqnano-cli from the host, or add it to queries.sql"

/* ---- Parser ---- */

static const struct sqnano_catalog *catalog(struct sqnano_db *db)
{
	return db->schema ? SQNANO_CATALOG(db->schema) : NULL;
}

static const struct sqnano_table *find_table(struct sqnano_db *db, const char *s, size_t n)
{
	const struct sqnano_catalog *c = catalog(db);

	for (int i = 0; c && i < c->ntables; i++) {
		if (name_eq(c->tables[i]->name, s, n)) {
			return c->tables[i];
		}
	}
	return NULL;
}

/* Column number, -1 for the rowid, -2 when unknown. */
static int find_col(const struct sqnano_table *t, const char *s, size_t n)
{
	for (int i = 0; i < t->ncols; i++) {
		if (name_eq(t->cols[i].name, s, n)) {
			return i == t->ipk ? -1 : i;
		}
	}
	if (!t->pk && (name_eq("rowid", s, n) || name_eq("oid", s, n) || name_eq("_rowid_", s, n))) {
		return -1;
	}
	return -2;
}

static int literal(struct sqnano_shell *sh, struct lex *l, sqnano_value *v)
{
	bool neg = false;

	if (is_op(l, "-")) {
		neg = true;
		next(l);
	}
	if (l->kind == T_NUM) {
		bool whole;
		char buf[48];
		size_t n = l->n < sizeof(buf) - 2 ? l->n : sizeof(buf) - 2;

		buf[0] = '-';
		memcpy(buf + 1, l->s, n);
		buf[n + 1] = 0;
		if (!num_parse(neg ? buf : buf + 1, (uint32_t)strlen(neg ? buf : buf + 1), v, &whole) ||
		    !whole) {
			return fail(sh, SQNANO_ERROR, "bad number");
		}
		next(l);
		return SQNANO_OK;
	}
	if (neg) {
		return fail(sh, SQNANO_ERROR, "expected a number after '-'");
	}
	if (l->kind == T_STR || l->kind == T_BLOB) {
		bool blob = l->kind == T_BLOB;
		const char *p = l->s + (blob ? 2 : 1);
		const char *e = l->s + l->n - 1;
		char *dst = w.lits + w.lused;
		uint32_t k = 0;

		if (w.lused + (size_t)(e - p) + 1 > sizeof(w.lits)) {
			return fail(sh, SQNANO_TOOBIG, "literals too long");
		}
		while (p < e) {
			if (blob) {
				char hex[3] = {p[0], p + 1 < e ? p[1] : '0', 0};

				dst[k++] = (char)strtol(hex, NULL, 16);
				p += 2;
				continue;
			}
			if (*p == '\'' && p + 1 < e && p[1] == '\'') {
				p++;
			}
			dst[k++] = *p++;
		}
		dst[k] = 0;
		w.lused = (uint16_t)(w.lused + k + 1);
		if (blob) {
			sqnano_v_blob(v, dst, k);
		} else {
			sqnano_v_text(v, dst, k);
		}
		next(l);
		return SQNANO_OK;
	}
	if (accept(l, "null")) {
		sqnano_v_null(v);
		return SQNANO_OK;
	}
	return fail(sh, SQNANO_ERROR, "expected a literal value near '%.*s'", (int)l->n, l->s);
}

static uint8_t col_aff(const struct sqnano_table *t, int col)
{
	uint8_t a = col < 0 ? SQNANO_AFF_INTEGER : t->cols[col].aff;

	if (a == SQNANO_AFF_INTEGER || a == SQNANO_AFF_REAL || a == SQNANO_AFF_NUMERIC) {
		return SQNANO_AFF_NUMERIC;
	}
	return a == SQNANO_AFF_TEXT ? SQNANO_AFF_TEXT : SQNANO_AFF_NONE;
}

static uint8_t col_coll(const struct sqnano_table *t, int col)
{
	return col < 0 ? SQNANO_COLL_BINARY : t->cols[col].coll;
}

static int add_filter(struct sqnano_shell *sh, struct ms_plan *p, int col, uint8_t kind, uint8_t op,
		      const sqnano_value *v)
{
	struct ms_filter *f;

	if (p->nfilters >= MS_MAX_FILTERS) {
		return fail(sh, SQNANO_TOOBIG, "too many conditions (max %d)", MS_MAX_FILTERS);
	}
	f = &p->f[p->nfilters++];
	f->col = (int16_t)col;
	f->kind = kind;
	f->op = op;
	if (v) {
		f->v = *v;
		if (kind == F_CMP) {
			(void)sqnano_affinity(&w.x, &f->v, col_aff(p->t, col));
		}
	}
	return SQNANO_OK;
}

static int parse_where(struct sqnano_shell *sh, struct lex *l, struct ms_plan *p)
{
	do {
		sqnano_value v, v2;
		int col;
		int rc;

		if (l->kind != T_ID) {
			return fail(sh, SQNANO_ERROR, "expected a column name in WHERE");
		}
		col = find_col(p->t, l->s, l->n);
		if (col == -2) {
			return fail(sh, SQNANO_ERROR, "no such column: %.*s", (int)l->n, l->s);
		}
		next(l);
		if (accept(l, "is")) {
			bool neg = accept(l, "not");

			if (!accept(l, "null")) {
				return fail(sh, SQNANO_ERROR, "IS only supports NULL in the shell");
			}
			rc = add_filter(sh, p, col, neg ? F_NOTNULL : F_ISNULL, 0, NULL);
		} else if (accept(l, "between")) {
			rc = literal(sh, l, &v);
			if (rc == SQNANO_OK && !accept(l, "and")) {
				rc = fail(sh, SQNANO_ERROR, "expected AND in BETWEEN");
			}
			if (rc == SQNANO_OK) {
				rc = literal(sh, l, &v2);
			}
			if (rc == SQNANO_OK) {
				rc = add_filter(sh, p, col, F_CMP, SQNANO_OP_GE, &v);
			}
			if (rc == SQNANO_OK) {
				rc = add_filter(sh, p, col, F_CMP, SQNANO_OP_LE, &v2);
			}
		} else if (is_word(l, "like") || is_word(l, "not")) {
			bool neg = accept(l, "not");

			if (!accept(l, "like")) {
				return fail(sh, SQNANO_ERROR, "expected LIKE");
			}
			rc = literal(sh, l, &v);
			if (rc == SQNANO_OK) {
				rc = add_filter(sh, p, col, neg ? F_NOTLIKE : F_LIKE, 0, &v);
			}
		} else if (l->kind == T_OP) {
			static const struct {
				const char *s;
				uint8_t op;
			} ops[] = {{"=", SQNANO_OP_EQ}, {"==", SQNANO_OP_EQ}, {"!=", SQNANO_OP_NE},
				   {"<>", SQNANO_OP_NE}, {"<", SQNANO_OP_LT},  {"<=", SQNANO_OP_LE},
				   {">", SQNANO_OP_GT},  {">=", SQNANO_OP_GE}};
			uint8_t op = 0;

			for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
				if (is_op(l, ops[i].s)) {
					op = ops[i].op;
				}
			}
			if (!op) {
				return fail(sh, SQNANO_ERROR, "unsupported operator '%.*s'", (int)l->n,
					    l->s);
			}
			next(l);
			rc = literal(sh, l, &v);
			if (rc == SQNANO_OK) {
				rc = add_filter(sh, p, col, F_CMP, op, &v);
			}
		} else {
			return fail(sh, SQNANO_ERROR, "condition " UNSUPPORTED);
		}
		if (rc) {
			return rc;
		}
	} while (accept(l, "and"));
	if (is_word(l, "or")) {
		return fail(sh, SQNANO_ERROR, "OR " UNSUPPORTED);
	}
	return SQNANO_OK;
}

static int parse_select(struct sqnano_shell *sh, const char *sql, struct ms_plan *p)
{
	static const char *const aggs[] = {NULL, "count", "count", "sum", "total", "avg", "min", "max"};
	struct lex l = {.p = sql};
	struct lex cols;
	bool star = false;
	int rc;

	memset(p, 0, sizeof(*p));
	p->limit = -1;
	p->order_col = -2;
	w.lused = 0;
	next(&l);
	p->explain = accept(&l, "explain");
	if (accept(&l, "query")) {
		accept(&l, "plan");
	}
	if (!accept(&l, "select")) {
		return fail(sh, SQNANO_ERROR, "only SELECT is available in the shell (%s)",
			    "use .run for precompiled queries");
	}
	/* Columns are resolved once the table is known. */
	cols = l;
	while (l.kind != T_END && !is_word(&l, "from")) {
		next(&l);
	}
	if (!accept(&l, "from") || l.kind != T_ID) {
		return fail(sh, SQNANO_ERROR, "expected FROM table");
	}
	p->t = find_table(sh->db, l.s, l.n);
	if (!p->t) {
		return fail(sh, SQNANO_ERROR, "no such table: %.*s", (int)l.n, l.s);
	}
	next(&l);
	if (is_op(&l, ",") || is_word(&l, "join")) {
		return fail(sh, SQNANO_ERROR, "joins are " UNSUPPORTED);
	}
	if (accept(&l, "where")) {
		rc = parse_where(sh, &l, p);
		if (rc) {
			return rc;
		}
	}
	if (is_word(&l, "group")) {
		return fail(sh, SQNANO_ERROR, "GROUP BY " UNSUPPORTED);
	}
	if (accept(&l, "order")) {
		if (!accept(&l, "by") || l.kind != T_ID) {
			return fail(sh, SQNANO_ERROR, "expected ORDER BY column");
		}
		p->order_col = (int16_t)find_col(p->t, l.s, l.n);
		if (p->order_col == -2) {
			return fail(sh, SQNANO_ERROR, "no such column: %.*s", (int)l.n, l.s);
		}
		next(&l);
		if (accept(&l, "desc")) {
			p->order_desc = true;
		} else {
			accept(&l, "asc");
		}
		if (is_op(&l, ",")) {
			return fail(sh, SQNANO_ERROR, "ORDER BY on several columns " UNSUPPORTED);
		}
	}
	if (accept(&l, "limit")) {
		sqnano_value v;

		rc = literal(sh, &l, &v);
		if (rc) {
			return rc;
		}
		if (v.type != SQNANO_INTEGER) {
			return fail(sh, SQNANO_MISMATCH, "LIMIT must be an integer");
		}
		p->limit = v.u.i;
	}
	if (is_op(&l, ";")) {
		next(&l);
	}
	if (l.kind != T_END) {
		return fail(sh, SQNANO_ERROR, "near '%.*s': " UNSUPPORTED, (int)l.n, l.s);
	}

	/* Result columns. */
	l = cols;
	do {
		int k = p->ncols;
		uint8_t agg = 0;
		int col;

		if (is_op(&l, "*")) {
			if (star || k) {
				return fail(sh, SQNANO_ERROR, "* must be alone");
			}
			star = true;
			for (int i = 0; i < p->t->ncols && p->ncols < MS_MAX_COLS; i++) {
				p->cols[p->ncols] = (int16_t)(i == p->t->ipk ? -1 : i);
				p->names[p->ncols++] = p->t->cols[i].name;
			}
			next(&l);
			break;
		}
		if (k >= MS_MAX_COLS || l.kind != T_ID) {
			return fail(sh, SQNANO_ERROR, "bad result column");
		}
		for (uint8_t a = SQNANO_AGG_COUNT; a <= SQNANO_AGG_MAX; a++) {
			if (is_word(&l, aggs[a]) && l.p[0] == '(') {
				agg = a;
			}
		}
		if (!agg && l.p[0] == '(') {
			return fail(sh, SQNANO_ERROR, "function %.*s() " UNSUPPORTED, (int)l.n, l.s);
		}
		if (agg) {
			const char *start = l.s;

			next(&l);
			next(&l);
			if (agg == SQNANO_AGG_COUNT && is_op(&l, "*")) {
				agg = SQNANO_AGG_COUNT_STAR;
				col = -1;
			} else if (l.kind == T_ID) {
				col = find_col(p->t, l.s, l.n);
				if (col == -2) {
					return fail(sh, SQNANO_ERROR, "no such column: %.*s", (int)l.n,
						    l.s);
				}
			} else {
				return fail(sh, SQNANO_ERROR, "aggregate arguments " UNSUPPORTED);
			}
			next(&l);
			if (!is_op(&l, ")")) {
				return fail(sh, SQNANO_ERROR, "expected ')'");
			}
			(void)snprintf(p->namebuf[k], sizeof(p->namebuf[k]), "%.*s",
				       (int)(l.s + 1 - start), start);
			p->names[k] = p->namebuf[k];
			next(&l);
		} else {
			col = find_col(p->t, l.s, l.n);
			if (col == -2) {
				return fail(sh, SQNANO_ERROR, "no such column: %.*s", (int)l.n, l.s);
			}
			p->names[k] = col < 0 && p->t->ipk >= 0 ? p->t->cols[p->t->ipk].name
				      : col < 0		    ? "rowid"
							    : p->t->cols[col].name;
			next(&l);
			if (l.kind != T_END && !is_op(&l, ",") && !is_word(&l, "from")) {
				return fail(sh, SQNANO_ERROR, "expressions are " UNSUPPORTED);
			}
		}
		p->cols[k] = (int16_t)col;
		p->agg[k] = agg;
		p->ncols++;
		if (is_op(&l, ",")) {
			next(&l);
		} else {
			break;
		}
	} while (1);
	{
		int naggs = 0;

		for (int i = 0; i < p->ncols; i++) {
			naggs += p->agg[i] != 0;
		}
		if (naggs && naggs != p->ncols) {
			return fail(sh, SQNANO_ERROR, "mixing aggregates and columns needs GROUP BY: %s",
				    UNSUPPORTED);
		}
	}
	return SQNANO_OK;
}

/* Choose the access path, as the host planner does. */
static void choose_plan(struct ms_plan *p)
{
	const struct sqnano_table *t = p->t;
	int best = -1;

	/* Candidate -1: the rowid, or the PRIMARY KEY of a WITHOUT ROWID table. */
	for (int cand = -1; cand < t->nindexes; cand++) {
		const struct sqnano_index *idx = cand < 0 ? t->pk : t->indexes[cand];
		int ncols = idx ? idx->ncols : 1;
		int8_t eq[MS_MAX_FILTERS];
		uint8_t neq = 0;
		int8_t lo = -1, hi = -1;
		bool order_ok = false, desc = false;
		int next_col = -3;
		int score;

		for (int k = 0; k < ncols; k++) {
			int col = idx ? idx->cols[k] : -1;
			int found = -1;

			if (col == t->ipk) {
				col = -1;
			}
			for (int i = 0; i < p->nfilters; i++) {
				if (p->f[i].col == col && p->f[i].kind == F_CMP && p->f[i].op == SQNANO_OP_EQ &&
				    (!idx || idx->coll[k] == col_coll(t, col))) {
					found = i;
				}
			}
			if (found >= 0) {
				eq[neq++] = (int8_t)found;
				continue;
			}
			next_col = col;
			for (int i = 0; i < p->nfilters; i++) {
				const struct ms_filter *f = &p->f[i];
				bool lower;

				if (f->col != col || f->kind != F_CMP || f->op == SQNANO_OP_EQ ||
				    f->op == SQNANO_OP_NE || (idx && idx->coll[k] != col_coll(t, col))) {
					continue;
				}
				lower = f->op == SQNANO_OP_GT || f->op == SQNANO_OP_GE;
				if (idx && idx->desc[k]) {
					lower = !lower;
				}
				if (lower && lo < 0) {
					lo = (int8_t)i;
				} else if (!lower && hi < 0) {
					hi = (int8_t)i;
				}
			}
			break;
		}
		if (next_col == -3) {
			next_col = -1; /* every index column is constrained: rowid order */
		}
		if (p->order_col != -2) {
			int k = neq;

			if (idx && k < idx->ncols) {
				int c = idx->cols[k] == t->ipk ? -1 : idx->cols[k];

				order_ok = c == p->order_col;
				desc = order_ok && (p->order_desc != (bool)idx->desc[k]);
			} else {
				order_ok = p->order_col == -1;
				desc = p->order_desc;
			}
		}
		score = neq * 10 + ((lo >= 0 || hi >= 0) ? 5 : 0) + (order_ok ? 3 : 0) +
			(cand < 0 && (neq || lo >= 0 || hi >= 0) ? 1 : 0);
		if (score > best) {
			best = score;
			p->idx = idx;
			p->use_rowid = !idx;
			p->neq = neq;
			memcpy(p->eq, eq, sizeof(eq));
			p->lo = lo;
			p->hi = hi;
			p->order_ok = order_ok || p->order_col == -2;
			p->desc = order_ok && desc;
		}
	}
}

static void describe(struct sqnano_shell *sh, const struct ms_plan *p)
{
	if (p->idx && p->idx == p->t->pk) {
		sh->print(sh->arg, "%s %s USING PRIMARY KEY",
			  p->neq || p->lo >= 0 || p->hi >= 0 ? "SEARCH" : "SCAN", p->t->name);
	} else if (p->idx) {
		sh->print(sh->arg, "%s %s USING INDEX %s", p->neq || p->lo >= 0 || p->hi >= 0 ? "SEARCH" : "SCAN",
			  p->t->name, p->idx->name);
	} else if (p->neq || p->lo >= 0 || p->hi >= 0) {
		sh->print(sh->arg, "SEARCH %s USING ROWID", p->t->name);
	} else {
		sh->print(sh->arg, "FULL SCAN %s", p->t->name);
	}
	if (p->neq || p->lo >= 0 || p->hi >= 0) {
		sh->print(sh->arg, " (%d equality%s%s)", p->neq, p->neq == 1 ? "" : "s",
			  p->lo >= 0 || p->hi >= 0 ? ", range" : "");
	}
	if (p->desc) {
		sh->print(sh->arg, " DESC");
	}
	if ((!p->idx || p->idx == p->t->pk) && !p->neq && p->lo < 0 && p->hi < 0) {
		for (int i = 0; i < p->nfilters; i++) {
			if (p->f[i].col >= 0) {
				sh->print(sh->arg, " - no index on %s", p->t->cols[p->f[i].col].name);
				break;
			}
		}
	}
	sh->print(sh->arg, "\n");
}

static bool filters_pass(const struct ms_plan *p, const sqnano_value *cols, int64_t rowid)
{
	for (int i = 0; i < p->nfilters; i++) {
		const struct ms_filter *f = &p->f[i];
		sqnano_value rv, r;
		const sqnano_value *v = &rv;

		if (f->col < 0) {
			sqnano_v_int(&rv, rowid);
		} else {
			v = &cols[f->col];
		}
		switch (f->kind) {
		case F_ISNULL:
			if (v->type != SQNANO_NULL) {
				return false;
			}
			continue;
		case F_NOTNULL:
			if (v->type == SQNANO_NULL) {
				return false;
			}
			continue;
		case F_LIKE:
		case F_NOTLIKE:
			if (sqnano_op_like(&w.x, &r, v, &f->v, NULL, false) ||
			    sqnano_truth(&r) != (f->kind == F_LIKE ? 1 : 0)) {
				return false;
			}
			continue;
		default: {
			sqnano_value a = *v;

			(void)sqnano_affinity(&w.x, &a, col_aff(p->t, f->col));
			(void)sqnano_op_cmp(&r, &a, &f->v, f->op, col_coll(p->t, f->col));
			if (sqnano_truth(&r) != 1) {
				return false;
			}
		}
		}
	}
	return true;
}

static int run_select(struct sqnano_shell *sh, const char *sql)
{
	struct ms_plan *p = &w.plan;
	struct sqnano_db *db = sh->db;
	struct sqnano_txn txn;
	sqnano_value out[MS_MAX_COLS];
	uint32_t rows = 0, scanned = 0;
	int64_t start = sqnano_uptime_ms();
	uint64_t cap;
	bool aggregate;
	int rc;

	memset(&w.x, 0, sizeof(w.x));
	w.x.db = db;
	w.x.scratch.buf = w.scratch;
	w.x.scratch.cap = sizeof(w.scratch);
	rc = parse_select(sh, sql, p);
	if (rc) {
		return rc;
	}
	choose_plan(p);
	if (p->explain) {
		describe(sh, p);
		return SQNANO_OK;
	}
	aggregate = p->agg[0] != 0;
	if (!p->order_ok && !aggregate) {
		return fail(sh, SQNANO_ERROR, "ORDER BY %s needs an index on that column; %s",
			    p->order_col >= 0 ? p->t->cols[p->order_col].name : "rowid", UNSUPPORTED);
	}
	cap = p->limit >= 0 ? (uint64_t)p->limit : sh->limit;
	rc = sqnano_txn_begin(db, &txn, SQNANO_TXN_READ, 0);
	if (rc) {
		return fail(sh, rc, "cannot open a snapshot (%d)", rc);
	}
	w.x.txn = &txn;
	db_lock(db);
	sqnano_scan_init(&w.scan, &w.x, p->t, p->idx, w.cols, p->desc ? SQNANO_SCAN_DESC : 0);
	for (int i = 0; i < p->neq && rc == SQNANO_OK; i++) {
		const struct ms_filter *f = &p->f[p->eq[i]];
		int k = p->idx ? i : 0;

		rc = sqnano_scan_eq(&w.scan, &f->v, col_aff(p->t, f->col),
				    p->idx ? p->idx->coll[k] : SQNANO_COLL_BINARY,
				    p->idx ? p->idx->desc[k] : false);
	}
	for (int which = 0; which < 2 && rc == SQNANO_OK; which++) {
		int fi = which ? p->hi : p->lo;
		int k = p->neq;

		if (fi >= 0) {
			rc = sqnano_scan_range(&w.scan, which ? SQNANO_SCAN_HI : SQNANO_SCAN_LO, &p->f[fi].v,
					       col_aff(p->t, p->f[fi].col),
					       p->idx ? p->idx->coll[k] : SQNANO_COLL_BINARY,
					       p->idx ? p->idx->desc[k] : false);
		}
	}
	for (int i = 0; i < p->ncols; i++) {
		sqnano_agg_reset(&w.agg[i]);
	}
	db_unlock(db);
	if (!aggregate) {
		header(sh, p->ncols, p->names);
	}
	while (rc == SQNANO_OK && (aggregate || rows < cap)) {
		db_lock(db);
		rc = sqnano_scan_next(&w.scan);
		if (rc == SQNANO_OK) {
			scanned++;
			sqnano_scratch_reset(&w.x);
			if (filters_pass(p, w.cols, w.scan.rowid)) {
				for (int i = 0; i < p->ncols; i++) {
					sqnano_value rv;

					if (p->cols[i] < 0) {
						sqnano_v_int(&rv, w.scan.rowid);
					} else {
						rv = w.cols[p->cols[i]];
					}
					if (aggregate) {
						(void)sqnano_agg_step(&w.x, &w.agg[i], p->agg[i], &rv,
								      col_coll(p->t, p->cols[i]));
					} else {
						out[i] = rv;
					}
				}
				if (!aggregate) {
					row(sh, p->ncols, p->names, out);
				}
				rows++;
			}
		}
		db_unlock(db);
		if (sh->timeout_ms && sqnano_uptime_ms() - start > (int64_t)sh->timeout_ms) {
			rc = SQNANO_INTERRUPT;
		}
	}
	if (aggregate && rc == SQNANO_DONE) {
		for (int i = 0; i < p->ncols; i++) {
			rc = sqnano_agg_final(&w.x, &w.agg[i], p->agg[i], &out[i]);
			if (rc) {
				break;
			}
		}
		if (rc == SQNANO_OK || rc == SQNANO_DONE) {
			header(sh, p->ncols, p->names);
			row(sh, p->ncols, p->names, out);
			rows = 1;
			rc = SQNANO_DONE;
		}
	}
	(void)sqnano_txn_commit(&txn);
	if (rc != SQNANO_DONE && rc != SQNANO_OK) {
		return fail(sh, rc, rc == SQNANO_INTERRUPT ? "timeout after %u rows" : "scan failed (%d)",
			    rc == SQNANO_INTERRUPT ? rows : (uint32_t)rc);
	}
	sh->print(sh->arg, "(%u row%s, %s%s, %u scanned, %lld ms)\n", rows, rows == 1 ? "" : "s",
		  p->idx && p->idx == p->t->pk		  ? "primary key"
		  : p->idx				  ? "idx "
		  : (p->neq || p->lo >= 0 || p->hi >= 0) ? "rowid"
							  : "full scan",
		  p->idx && p->idx != p->t->pk ? p->idx->name : "", scanned,
		  (long long)(sqnano_uptime_ms() - start));
	if (!aggregate && rc == SQNANO_OK && rows >= cap && p->limit < 0) {
		sh->print(sh->arg, "(stopped at .limit %u)\n", sh->limit);
	}
	return SQNANO_OK;
}

/* ---- Dot commands ---- */

static void print_schema(struct sqnano_shell *sh, const struct sqnano_table *t)
{
	const struct sqnano_catalog *c = catalog(sh->db);

	if (t->sql && t->sql[0]) {
		sh->print(sh->arg, "%s;\n", t->sql);
	} else {
		sh->print(sh->arg, "CREATE TABLE %s (", t->name);
		for (int i = 0; i < t->ncols; i++) {
			const struct sqnano_column *col = &t->cols[i];

			sh->print(sh->arg, "%s%s%s%s%s%s", i ? ", " : "", col->name, col->type[0] ? " " : "",
				  col->type, i == t->ipk ? " PRIMARY KEY" : "",
				  (col->flags & SQNANO_COL_NOTNULL) ? " NOT NULL" : "");
		}
		sh->print(sh->arg, ");\n");
	}
	for (int i = 0; i < c->nindexes; i++) {
		const struct sqnano_index *idx = c->indexes[i];

		if (c->tables[idx->table] == t && idx->sql && idx->sql[0]) {
			sh->print(sh->arg, "%s;\n", idx->sql);
		}
	}
}

static int run_with(struct sqnano_shell *sh, struct lex *l, const struct sqnano_query *q,
		    struct sqnano_exec *x, const char *name);

static int run_query(struct sqnano_shell *sh, struct lex *l)
{
	const struct sqnano_query *q;
	struct sqnano_exec *x;
	char name[48];
	int rc;

	if (!sh->db->schema || l->kind != T_ID || l->n >= sizeof(name)) {
		return fail(sh, SQNANO_ERROR, "usage: .run <query> [param=value ...]");
	}
	memcpy(name, l->s, l->n);
	name[l->n] = 0;
	q = sqnano_query_find(sh->db->schema, name);
	if (!q) {
		return fail(sh, SQNANO_NOTFOUND, "no precompiled query named %s (see .queries)", name);
	}
	x = sqnano_state_alloc(sh->db->schema, q->state_size);
	if (!x) {
		return fail(sh, SQNANO_NOMEM, "statement pool full (CONFIG_SQNANO_STMT_POOL_SIZE)");
	}
	rc = run_with(sh, l, q, x, name);
	(void)sqnano_exec_reset(x);
	sqnano_state_free(sh->db->schema, x);
	return rc;
}

static int run_with(struct sqnano_shell *sh, struct lex *l, const struct sqnano_query *q,
		    struct sqnano_exec *x, const char *name)
{
	uint32_t rows = 0;
	int64_t start = sqnano_uptime_ms();
	int rc;

	if (q->kind == SQNANO_Q_WRITE && (!sh->allow_write || sh->read_only)) {
		return fail(sh, SQNANO_READONLY, "%s writes; %s", name,
			    sh->allow_write ? "use .readonly off first"
					    : "writes need CONFIG_SQNANO_SHELL_WRITE");
	}
	(void)sqnano_exec_open(x, q, sh->db, NULL);
	memset(&w.x, 0, sizeof(w.x));
	w.lused = 0;
	next(l);
	while (l->kind == T_ID) {
		const char *pn = l->s;
		size_t pl = l->n;
		sqnano_value v;
		int idx = -1;

		next(l);
		if (!is_op(l, "=")) {
			return fail(sh, SQNANO_ERROR, "expected name=value");
		}
		next(l);
		if (l->kind == T_ID && !is_word(l, "null")) {
			/* Bare word: a text value. */
			char *dst = w.lits + w.lused;

			if (w.lused + l->n + 1 > sizeof(w.lits)) {
				return fail(sh, SQNANO_TOOBIG, "values too long");
			}
			memcpy(dst, l->s, l->n);
			dst[l->n] = 0;
			w.lused = (uint16_t)(w.lused + l->n + 1);
			sqnano_v_text(&v, dst, (uint32_t)l->n);
			next(l);
		} else if (literal(sh, l, &v)) {
			return SQNANO_ERROR;
		}
		for (int i = 0; i < q->nparams; i++) {
			const char *qn = q->params[i];

			if (qn && (name_eq(qn + 1, pn, pl) || name_eq(qn, pn, pl))) {
				idx = i;
			}
		}
		if (idx < 0) {
			return fail(sh, SQNANO_RANGE, "%s has no parameter %.*s", name, (int)pl, pn);
		}
		x->params[idx] = v;
	}
	if (q->ncols) {
		header(sh, q->ncols, q->cols);
	}
	while ((rc = sqnano_exec_step(x)) == SQNANO_ROW) {
		if (rows < sh->limit) {
			row(sh, q->ncols, q->cols, x->out);
		}
		rows++;
	}
	if (rc != SQNANO_DONE) {
		return fail(sh, rc, "%s", x->errmsg);
	}
	sh->print(sh->arg, "(%u row%s, %lld changes, %lld ms)\n", rows, rows == 1 ? "" : "s",
		  (long long)x->changes, (long long)(sqnano_uptime_ms() - start));
	return SQNANO_OK;
}

static int dot(struct sqnano_shell *sh, const char *line)
{
	const struct sqnano_catalog *c = catalog(sh->db);
	struct lex l = {.p = line + 1};

	next(&l);
	if (accept(&l, "tables")) {
		for (int i = 0; c && i < c->ntables; i++) {
			sh->print(sh->arg, "%s%s", i ? "  " : "", c->tables[i]->name);
		}
		sh->print(sh->arg, "\n");
	} else if (accept(&l, "schema")) {
		for (int i = 0; c && i < c->ntables; i++) {
			if (l.kind == T_END || name_eq(c->tables[i]->name, l.s, l.n)) {
				print_schema(sh, c->tables[i]);
			}
		}
	} else if (accept(&l, "indexes") || accept(&l, "indices")) {
		for (int i = 0; c && i < c->nindexes; i++) {
			const struct sqnano_index *idx = c->indexes[i];
			const struct sqnano_table *t = c->tables[idx->table];

			if (l.kind != T_END && !name_eq(t->name, l.s, l.n)) {
				continue;
			}
			sh->print(sh->arg, "%-24s %s(", idx->name, t->name);
			for (int k = 0; k < idx->ncols; k++) {
				sh->print(sh->arg, "%s%s%s", k ? ", " : "", t->cols[idx->cols[k]].name,
					  idx->desc[k] ? " DESC" : "");
			}
			sh->print(sh->arg, ")%s\n", (idx->flags & SQNANO_IDX_UNIQUE) ? " UNIQUE" : "");
		}
	} else if (accept(&l, "stats")) {
		struct sqnano_stats st;

		(void)sqnano_db_stats(sh->db, &st);
		sh->print(sh->arg, "mode        %s\n", st.mode == SQNANO_MODE_SHADOW ? "shadow paging"
										     : "undo journal");
		sh->print(sh->arg, "page size   %u\n", st.page_size);
		sh->print(sh->arg, "pages       %u used / %u (%u waiting for readers, %u pinned)\n",
			  st.pages_used, st.pages_total, st.pages_limbo, st.pages_pinned);
		sh->print(sh->arg, "commit seq  %u\n", st.seq);
		sh->print(sh->arg, "schema      v%u (compatible down to v%u)%s\n", st.schema_version,
			  st.compat_version, st.pinned ? ", migration not confirmed" : "");
		if (st.lazy_pending) {
			sh->print(sh->arg, "lazy        rows older than v%u still converted when read\n",
				  st.lazy_pending);
		}
		sh->print(sh->arg, "media       %u reads, %u writes, %u erases, %u cache hits\n",
			  st.reads, st.writes, st.erases, st.cache_hits);
		sh->print(sh->arg, "flash       %u compactions, %u sort runs\n", st.compactions,
			  st.sort_runs);
		{
			struct sqnano_pool_stats ps;

			if (sqnano_pool_stats(sh->db->schema, &ps) == SQNANO_OK) {
				sh->print(sh->arg,
					  "statements  %u open, %u bytes used, peak %u / %u "
					  "(largest state %u, %u refused)\n",
					  ps.statements, ps.used, ps.peak, ps.size, ps.largest, ps.fails);
			}
		}
	} else if (accept(&l, "queries")) {
		const struct sqnano_schema *s = sh->db->schema;

		for (uint16_t i = 0; s && i < s->nqueries; i++) {
			if (s->queries[i]->name) {
				sh->print(sh->arg, "%-20s %s\n", s->queries[i]->name, s->queries[i]->sql);
			}
		}
	} else if (accept(&l, "run")) {
		return run_query(sh, &l);
	} else if (accept(&l, "mode")) {
		if (accept(&l, "table")) {
			sh->mode = SQNANO_SHELL_TABLE;
		} else if (accept(&l, "csv")) {
			sh->mode = SQNANO_SHELL_CSV;
		} else if (accept(&l, "json")) {
			sh->mode = SQNANO_SHELL_JSON;
		} else if (accept(&l, "line")) {
			sh->mode = SQNANO_SHELL_LINE;
		} else {
			return fail(sh, SQNANO_ERROR, "usage: .mode table|csv|json|line");
		}
	} else if (accept(&l, "limit")) {
		if (l.kind == T_NUM) {
			sh->limit = (uint32_t)strtoul(l.s, NULL, 10);
		}
		sh->print(sh->arg, "limit %u rows\n", sh->limit);
	} else if (accept(&l, "timeout")) {
		if (l.kind == T_NUM) {
			sh->timeout_ms = (uint32_t)strtoul(l.s, NULL, 10);
		}
		sh->print(sh->arg, "timeout %u ms\n", sh->timeout_ms);
	} else if (accept(&l, "readonly")) {
		if (accept(&l, "off")) {
			if (!sh->allow_write) {
				return fail(sh, SQNANO_READONLY, "writes need CONFIG_SQNANO_SHELL_WRITE");
			}
			sh->read_only = false;
		} else if (accept(&l, "on")) {
			sh->read_only = true;
		}
		sh->print(sh->arg, "readonly %s\n", sh->read_only ? "on" : "off");
	} else if (accept(&l, "check")) {
		int rc = sqnano_db_check(sh->db);

		sh->print(sh->arg, "%s\n", rc == SQNANO_OK ? "ok" : "*** database is malformed");
		return rc;
	} else if (accept(&l, "help")) {
		sh->print(sh->arg,
			  ".tables | .schema [t] | .indexes [t] | .stats | .queries | .check\n"
			  ".run <query> [p=v ...] | .mode table|csv|json|line | .limit N | "
			  ".timeout MS | .readonly on|off\n"
			  "SELECT cols FROM t [WHERE c op lit AND ...] [ORDER BY c [DESC]] "
			  "[LIMIT n]; EXPLAIN SELECT ...\n");
	} else {
		return fail(sh, SQNANO_ERROR, "unknown command (try .help)");
	}
	return SQNANO_OK;
}

void sqnano_shell_init(struct sqnano_shell *sh, struct sqnano_db *db,
		       void (*print)(void *arg, const char *fmt, ...), void *arg)
{
	memset(sh, 0, sizeof(*sh));
	sh->db = db;
	sh->print = print;
	sh->arg = arg;
	sh->mode = SQNANO_SHELL_TABLE;
	sh->limit = 100;
	sh->timeout_ms = 2000;
	sh->read_only = true;
}

int sqnano_shell_exec(struct sqnano_shell *sh, const char *line)
{
	int rc;

	while (ms_space(*line)) {
		line++;
	}
	if (!sh->db || !sh->db->is_open) {
		sh->print(sh->arg, "error: no open database\n");
		return SQNANO_MISUSE;
	}
	sqnano_mutex_lock(&shell_lock);
	rc = line[0] == '.' ? dot(sh, line) : run_select(sh, line);
	sqnano_mutex_unlock(&shell_lock);
	return rc;
}
