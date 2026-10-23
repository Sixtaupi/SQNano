/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * SQL value semantics, following SQLite: type conversions, affinity,
 * comparison, arithmetic, LIKE/GLOB and scalar functions.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../sqnano_int.h"

#define I64_MAX ((int64_t)0x7fffffffffffffffLL)
#define I64_MIN (-I64_MAX - 1)

void *sqnano_scratch_alloc(struct sqnano_exec *x, uint32_t n)
{
	void *p;

	if (!x || (uint32_t)x->scratch.used + n > x->scratch.cap) {
		return NULL;
	}
	p = x->scratch.buf + x->scratch.used;
	x->scratch.used = (uint16_t)(x->scratch.used + n);
	return p;
}

static int nomem(struct sqnano_exec *x)
{
	if (x) {
		return sqnano_exec_error(x, SQNANO_NOMEM, "out of scratch memory");
	}
	return SQNANO_NOMEM;
}

static bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/*
 * Parse the longest numeric prefix of z[0..n). The result is an integer when
 * the prefix has no fraction or exponent and fits in 64 bits. @p whole tells
 * whether the complete text (ignoring surrounding spaces) is a number.
 * Returns the number of digits seen (0: not a number, value 0).
 */
int num_parse(const char *z, uint32_t n, sqnano_value *out, bool *whole)
{
	uint32_t i = 0, start, end_num;
	int digits = 0;
	bool is_int = true;
	bool neg = false;
	uint64_t acc = 0;
	bool overflow = false;

	sqnano_v_int(out, 0);
	*whole = false;
	while (i < n && is_space(z[i])) {
		i++;
	}
	start = i;
	if (i < n && (z[i] == '+' || z[i] == '-')) {
		neg = z[i] == '-';
		i++;
	}
	while (i < n && is_digit(z[i])) {
		if (acc > (uint64_t)I64_MAX / 10 + 1) {
			overflow = true;
		} else {
			acc = acc * 10 + (uint64_t)(z[i] - '0');
		}
		digits++;
		i++;
	}
	if (i < n && z[i] == '.') {
		uint32_t j = i + 1;
		int fd = 0;

		while (j < n && is_digit(z[j])) {
			fd++;
			j++;
		}
		if (digits + fd > 0) {
			is_int = false;
			digits += fd;
			i = j;
		}
	}
	if (digits == 0) {
		return 0;
	}
	if (i < n && (z[i] == 'e' || z[i] == 'E')) {
		uint32_t j = i + 1;

		if (j < n && (z[j] == '+' || z[j] == '-')) {
			j++;
		}
		if (j < n && is_digit(z[j])) {
			while (j < n && is_digit(z[j])) {
				j++;
			}
			is_int = false;
			i = j;
		}
	}
	end_num = i;
	while (i < n && is_space(z[i])) {
		i++;
	}
	*whole = i == n;
	if (is_int && !overflow &&
	    (neg ? acc <= (uint64_t)I64_MAX + 1 : acc <= (uint64_t)I64_MAX)) {
		sqnano_v_int(out, neg ? (int64_t)(0 - acc) : (int64_t)acc);
	} else {
		char buf[128];
		uint32_t len = end_num - start;

		if (len >= sizeof(buf)) {
			len = sizeof(buf) - 1;
		}
		memcpy(buf, z + start, len);
		buf[len] = 0;
		sqnano_v_real(out, strtod(buf, NULL));
	}
	return digits;
}

static int64_t double_to_int(double r)
{
	if (r != r) {
		return 0;
	}
	if (r <= -9223372036854775808.0) {
		return I64_MIN;
	}
	if (r >= 9223372036854775807.0) {
		return I64_MAX;
	}
	return (int64_t)r;
}

/* Integer prefix of a text, as sqlite3Atoi64 (no fraction, no exponent). */
static int64_t text_to_int(const char *z, uint32_t n)
{
	uint32_t i = 0;
	bool neg = false;
	uint64_t acc = 0;

	while (i < n && is_space(z[i])) {
		i++;
	}
	if (i < n && (z[i] == '+' || z[i] == '-')) {
		neg = z[i] == '-';
		i++;
	}
	while (i < n && is_digit(z[i])) {
		if (acc > (uint64_t)I64_MAX + 1) {
			return neg ? I64_MIN : I64_MAX;
		}
		acc = acc * 10 + (uint64_t)(z[i] - '0');
		i++;
	}
	if (neg) {
		return acc > (uint64_t)I64_MAX ? I64_MIN : -(int64_t)acc;
	}
	return acc > (uint64_t)I64_MAX ? I64_MAX : (int64_t)acc;
}

int64_t sqnano_v_as_int(const sqnano_value *v)
{
	switch (v->type) {
	case SQNANO_INTEGER:
		return v->u.i;
	case SQNANO_FLOAT:
		return double_to_int(v->u.r);
	case SQNANO_TEXT:
	case SQNANO_BLOB:
		return text_to_int(v->u.z, v->n);
	default:
		return 0;
	}
}

double sqnano_v_as_real(const sqnano_value *v)
{
	sqnano_value t;
	bool whole;

	switch (v->type) {
	case SQNANO_INTEGER:
		return (double)v->u.i;
	case SQNANO_FLOAT:
		return v->u.r;
	case SQNANO_TEXT:
	case SQNANO_BLOB:
		(void)num_parse(v->u.z, v->n, &t, &whole);
		return t.type == SQNANO_INTEGER ? (double)t.u.i : t.u.r;
	default:
		return 0.0;
	}
}

/* Numeric value used by arithmetic: integer when the prefix is an integer. */
static void to_numeric(const sqnano_value *v, sqnano_value *out)
{
	bool whole;

	if (v->type == SQNANO_INTEGER || v->type == SQNANO_FLOAT) {
		*out = *v;
	} else if (v->type == SQNANO_TEXT || v->type == SQNANO_BLOB) {
		(void)num_parse(v->u.z, v->n, out, &whole);
	} else {
		sqnano_v_null(out);
	}
}

int sqnano_format_number(const sqnano_value *v, char *buf, size_t cap)
{
	double r;
	int n;
	bool dot = false;
	int epos = -1;

	if (v->type == SQNANO_INTEGER) {
		return snprintf(buf, cap, "%lld", (long long)v->u.i);
	}
	r = v->u.r;
	if (isinf(r)) {
		return snprintf(buf, cap, r < 0 ? "-Inf" : "Inf");
	}
	if (r == 0) {
		r = 0;
	}
	n = snprintf(buf, cap, "%.15g", r);
	if (strtod(buf, NULL) != r) {
		n = snprintf(buf, cap, "%.17g", r);
	}
	for (int i = 0; i < n; i++) {
		if (buf[i] == '.') {
			dot = true;
		} else if (buf[i] == 'e') {
			epos = i;
		} else if (buf[i] == 'n' || buf[i] == 'N') {
			return n; /* nan */
		}
	}
	if (!dot && n + 2 < (int)cap) {
		if (epos < 0) {
			buf[n++] = '.';
			buf[n++] = '0';
			buf[n] = 0;
		} else {
			memmove(buf + epos + 2, buf + epos, (size_t)(n - epos + 1));
			buf[epos] = '.';
			buf[epos + 1] = '0';
			n += 2;
		}
	}
	return n;
}

int sqnano_v_to_text(struct sqnano_exec *x, sqnano_value *v)
{
	char tmp[40];
	char *p;
	int n;

	switch (v->type) {
	case SQNANO_INTEGER:
	case SQNANO_FLOAT:
		n = sqnano_format_number(v, tmp, sizeof(tmp));
		p = sqnano_scratch_alloc(x, (uint32_t)n + 1);
		if (!p) {
			return nomem(x);
		}
		memcpy(p, tmp, (size_t)n + 1);
		sqnano_v_text(v, p, (uint32_t)n);
		return SQNANO_OK;
	case SQNANO_BLOB:
		p = sqnano_scratch_alloc(x, v->n + 1);
		if (!p) {
			return nomem(x);
		}
		if (v->n) {
			memcpy(p, v->u.p, v->n);
		}
		p[v->n] = 0;
		sqnano_v_text(v, p, v->n);
		return SQNANO_OK;
	default:
		return SQNANO_OK;
	}
}

int sqnano_affinity(struct sqnano_exec *x, sqnano_value *v, uint8_t aff)
{
	sqnano_value t;
	bool whole;

	switch (aff) {
	case SQNANO_AFF_TEXT:
		if (v->type == SQNANO_INTEGER || v->type == SQNANO_FLOAT) {
			return sqnano_v_to_text(x, v);
		}
		return SQNANO_OK;
	case SQNANO_AFF_NUMERIC:
	case SQNANO_AFF_INTEGER:
	case SQNANO_AFF_REAL:
		if (v->type == SQNANO_TEXT) {
			if (num_parse(v->u.z, v->n, &t, &whole) && whole) {
				*v = t;
			} else {
				return SQNANO_OK;
			}
		}
		if (aff == SQNANO_AFF_REAL) {
			if (v->type == SQNANO_INTEGER) {
				sqnano_v_real(v, (double)v->u.i);
			}
		} else if (v->type == SQNANO_FLOAT) {
			double r = v->u.r;

			if (r > -9223372036854775808.0 && r < 9223372036854775808.0 &&
			    (double)(int64_t)r == r) {
				sqnano_v_int(v, (int64_t)r);
			}
		}
		return SQNANO_OK;
	default:
		return SQNANO_OK;
	}
}

/* ---- Comparison ---- */

static int type_class(uint8_t t)
{
	switch (t) {
	case SQNANO_NULL:
		return 0;
	case SQNANO_INTEGER:
	case SQNANO_FLOAT:
		return 1;
	case SQNANO_TEXT:
		return 2;
	default:
		return 3;
	}
}

static int int_real_cmp(int64_t i, double r)
{
	int64_t y;
	double s;

	if (r != r) {
		return 1;
	}
	if (r < -9223372036854775808.0) {
		return 1;
	}
	if (r >= 9223372036854775808.0) {
		return -1;
	}
	y = (int64_t)r;
	if (i < y) {
		return -1;
	}
	if (i > y) {
		return 1;
	}
	s = (double)i;
	return s < r ? -1 : (s > r ? 1 : 0);
}

static uint8_t lc(uint8_t c)
{
	return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c;
}

int text_compare(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn, uint8_t coll)
{
	uint32_t n;
	int r;

	if (coll == SQNANO_COLL_RTRIM) {
		while (an > 0 && a[an - 1] == ' ') {
			an--;
		}
		while (bn > 0 && b[bn - 1] == ' ') {
			bn--;
		}
	}
	n = an < bn ? an : bn;
	if (coll == SQNANO_COLL_NOCASE) {
		for (uint32_t i = 0; i < n; i++) {
			if (lc(a[i]) != lc(b[i])) {
				return lc(a[i]) < lc(b[i]) ? -1 : 1;
			}
		}
		r = 0;
	} else {
		r = n ? memcmp(a, b, n) : 0;
	}
	if (r) {
		return r < 0 ? -1 : 1;
	}
	return an < bn ? -1 : (an > bn ? 1 : 0);
}

int sqnano_v_compare(const sqnano_value *a, const sqnano_value *b, uint8_t coll)
{
	int ca = type_class(a->type);
	int cb = type_class(b->type);

	if (ca != cb) {
		return ca < cb ? -1 : 1;
	}
	switch (ca) {
	case 0:
		return 0;
	case 1:
		if (a->type == SQNANO_INTEGER && b->type == SQNANO_INTEGER) {
			return a->u.i < b->u.i ? -1 : (a->u.i > b->u.i ? 1 : 0);
		}
		if (a->type == SQNANO_FLOAT && b->type == SQNANO_FLOAT) {
			return a->u.r < b->u.r ? -1 : (a->u.r > b->u.r ? 1 : 0);
		}
		if (a->type == SQNANO_INTEGER) {
			return int_real_cmp(a->u.i, b->u.r);
		}
		return -int_real_cmp(b->u.i, a->u.r);
	case 2:
		return text_compare(a->u.p, a->n, b->u.p, b->n, coll);
	default:
		return text_compare(a->u.p, a->n, b->u.p, b->n, SQNANO_COLL_BINARY);
	}
}

int sqnano_truth(const sqnano_value *v)
{
	switch (v->type) {
	case SQNANO_NULL:
		return -1;
	case SQNANO_INTEGER:
		return v->u.i != 0;
	case SQNANO_FLOAT:
		return v->u.r != 0.0;
	default:
		return sqnano_v_as_real(v) != 0.0;
	}
}

void sqnano_v_bool(sqnano_value *r, int t)
{
	if (t < 0) {
		sqnano_v_null(r);
	} else {
		sqnano_v_int(r, t ? 1 : 0);
	}
}

int sqnano_op_cmp(sqnano_value *r, const sqnano_value *a, const sqnano_value *b, int op,
		  uint8_t coll)
{
	int c;

	if (op == SQNANO_OP_IS || op == SQNANO_OP_ISNOT) {
		bool eq;

		if (a->type == SQNANO_NULL || b->type == SQNANO_NULL) {
			eq = a->type == b->type;
		} else {
			eq = sqnano_v_compare(a, b, coll) == 0;
		}
		sqnano_v_int(r, (op == SQNANO_OP_IS) == eq);
		return SQNANO_OK;
	}
	if (a->type == SQNANO_NULL || b->type == SQNANO_NULL) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	c = sqnano_v_compare(a, b, coll);
	switch (op) {
	case SQNANO_OP_EQ:
		c = c == 0;
		break;
	case SQNANO_OP_NE:
		c = c != 0;
		break;
	case SQNANO_OP_LT:
		c = c < 0;
		break;
	case SQNANO_OP_LE:
		c = c <= 0;
		break;
	case SQNANO_OP_GT:
		c = c > 0;
		break;
	default:
		c = c >= 0;
		break;
	}
	sqnano_v_int(r, c);
	return SQNANO_OK;
}

/* ---- Arithmetic ---- */

static void set_real(sqnano_value *r, double d)
{
	if (d != d) {
		sqnano_v_null(r);
	} else {
		sqnano_v_real(r, d);
	}
}

static bool add_overflow(int64_t a, int64_t b, int64_t *r)
{
	if ((b > 0 && a > I64_MAX - b) || (b < 0 && a < I64_MIN - b)) {
		return true;
	}
	*r = a + b;
	return false;
}

static bool mul_overflow(int64_t a, int64_t b, int64_t *r)
{
	if (a == 0 || b == 0) {
		*r = 0;
		return false;
	}
	if ((a == -1 && b == I64_MIN) || (b == -1 && a == I64_MIN)) {
		return true;
	}
	if (a > 0 ? (b > 0 ? a > I64_MAX / b : b < I64_MIN / a)
		  : (b > 0 ? a < I64_MIN / b : a < I64_MAX / b)) {
		return true;
	}
	*r = a * b;
	return false;
}

static int64_t shift(int64_t a, int64_t b, bool left)
{
	if (b < 0) {
		left = !left;
		b = b < -64 ? 64 : -b;
	}
	if (b >= 64) {
		return (!left && a < 0) ? -1 : 0;
	}
	if (left) {
		return (int64_t)((uint64_t)a << b);
	}
	return a >> b;
}

int sqnano_op_arith(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *a,
		    const sqnano_value *b, int op)
{
	sqnano_value na, nb;

	(void)x;
	if (a->type == SQNANO_NULL || b->type == SQNANO_NULL) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	if (op >= SQNANO_OP_BITAND) {
		int64_t ia = sqnano_v_as_int(a), ib = sqnano_v_as_int(b);

		if (a->type == SQNANO_TEXT || a->type == SQNANO_BLOB) {
			to_numeric(a, &na);
			ia = na.type == SQNANO_INTEGER ? na.u.i : double_to_int(na.u.r);
		}
		if (b->type == SQNANO_TEXT || b->type == SQNANO_BLOB) {
			to_numeric(b, &nb);
			ib = nb.type == SQNANO_INTEGER ? nb.u.i : double_to_int(nb.u.r);
		}
		switch (op) {
		case SQNANO_OP_BITAND:
			sqnano_v_int(r, ia & ib);
			break;
		case SQNANO_OP_BITOR:
			sqnano_v_int(r, ia | ib);
			break;
		case SQNANO_OP_SHL:
			sqnano_v_int(r, shift(ia, ib, true));
			break;
		default:
			sqnano_v_int(r, shift(ia, ib, false));
			break;
		}
		return SQNANO_OK;
	}
	to_numeric(a, &na);
	to_numeric(b, &nb);
	if (na.type == SQNANO_INTEGER && nb.type == SQNANO_INTEGER) {
		int64_t ia = na.u.i, ib = nb.u.i, res;

		switch (op) {
		case SQNANO_OP_ADD:
			if (!add_overflow(ia, ib, &res)) {
				sqnano_v_int(r, res);
				return SQNANO_OK;
			}
			break;
		case SQNANO_OP_SUB:
			if (ib != I64_MIN && !add_overflow(ia, -ib, &res)) {
				sqnano_v_int(r, res);
				return SQNANO_OK;
			}
			if (ib == I64_MIN && ia < 0) {
				sqnano_v_int(r, ia - ib);
				return SQNANO_OK;
			}
			break;
		case SQNANO_OP_MUL:
			if (!mul_overflow(ia, ib, &res)) {
				sqnano_v_int(r, res);
				return SQNANO_OK;
			}
			break;
		case SQNANO_OP_DIV:
			if (ib == 0) {
				sqnano_v_null(r);
				return SQNANO_OK;
			}
			if (ia == I64_MIN && ib == -1) {
				break;
			}
			sqnano_v_int(r, ia / ib);
			return SQNANO_OK;
		default:
			if (ib == 0) {
				sqnano_v_null(r);
				return SQNANO_OK;
			}
			if (ib == -1) {
				ib = 1;
			}
			sqnano_v_int(r, ia % ib);
			return SQNANO_OK;
		}
	}
	{
		double ra = na.type == SQNANO_INTEGER ? (double)na.u.i : na.u.r;
		double rb = nb.type == SQNANO_INTEGER ? (double)nb.u.i : nb.u.r;

		switch (op) {
		case SQNANO_OP_ADD:
			set_real(r, ra + rb);
			break;
		case SQNANO_OP_SUB:
			set_real(r, ra - rb);
			break;
		case SQNANO_OP_MUL:
			set_real(r, ra * rb);
			break;
		case SQNANO_OP_DIV:
			if (rb == 0.0) {
				sqnano_v_null(r);
			} else {
				set_real(r, ra / rb);
			}
			break;
		default: {
			int64_t ia = double_to_int(ra), ib = double_to_int(rb);

			if (ib == 0) {
				sqnano_v_null(r);
				break;
			}
			if (ib == -1) {
				ib = 1;
			}
			set_real(r, (double)(ia % ib));
			break;
		}
		}
	}
	return SQNANO_OK;
}

int sqnano_op_neg(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *a)
{
	sqnano_value n;

	(void)x;
	to_numeric(a, &n);
	if (n.type == SQNANO_NULL) {
		sqnano_v_null(r);
	} else if (n.type == SQNANO_INTEGER) {
		if (n.u.i == I64_MIN) {
			sqnano_v_real(r, 9223372036854775808.0);
		} else {
			sqnano_v_int(r, -n.u.i);
		}
	} else {
		sqnano_v_real(r, -n.u.r);
	}
	return SQNANO_OK;
}

int sqnano_op_pos(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *a)
{
	(void)x;
	*r = *a;
	return SQNANO_OK;
}

int sqnano_op_bitnot(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *a)
{
	sqnano_value n;

	(void)x;
	if (a->type == SQNANO_NULL) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	to_numeric(a, &n);
	sqnano_v_int(r, ~(n.type == SQNANO_INTEGER ? n.u.i : double_to_int(n.u.r)));
	return SQNANO_OK;
}

int sqnano_op_not(sqnano_value *r, const sqnano_value *a)
{
	int t = sqnano_truth(a);

	sqnano_v_bool(r, t < 0 ? -1 : !t);
	return SQNANO_OK;
}

/* Make @p v a text value (copying numbers and blobs into the scratch area). */
static int as_text(struct sqnano_exec *x, const sqnano_value *v, sqnano_value *out)
{
	*out = *v;
	if (v->type == SQNANO_TEXT || v->type == SQNANO_NULL) {
		return SQNANO_OK;
	}
	return sqnano_v_to_text(x, out);
}

int sqnano_op_concat(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *a,
		     const sqnano_value *b)
{
	sqnano_value ta, tb;
	char *p;
	int rc;

	if (a->type == SQNANO_NULL || b->type == SQNANO_NULL) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	rc = as_text(x, a, &ta);
	if (rc == SQNANO_OK) {
		rc = as_text(x, b, &tb);
	}
	if (rc) {
		return rc;
	}
	p = sqnano_scratch_alloc(x, ta.n + tb.n + 1);
	if (!p) {
		return nomem(x);
	}
	memcpy(p, ta.u.p, ta.n);
	memcpy(p + ta.n, tb.u.p, tb.n);
	p[ta.n + tb.n] = 0;
	sqnano_v_text(r, p, ta.n + tb.n);
	return SQNANO_OK;
}

/* ---- LIKE and GLOB ---- */

static uint32_t utf8_next(const uint8_t *z, uint32_t n, uint32_t *i)
{
	uint32_t c = z[(*i)++];

	if (c >= 0xc0) {
		while (*i < n && (z[*i] & 0xc0) == 0x80) {
			c = (c << 6) | (z[(*i)++] & 0x3f);
		}
	}
	return c;
}

static uint32_t lcu(uint32_t c)
{
	return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static bool pattern_match(const uint8_t *p, uint32_t pn, uint32_t pi, const uint8_t *s,
			  uint32_t sn, uint32_t si, bool glob, uint32_t esc, int depth)
{
	uint32_t many = glob ? '*' : '%';
	uint32_t one = glob ? '?' : '_';

	if (depth > 64) {
		return false;
	}
	while (pi < pn) {
		uint32_t save = pi;
		uint32_t c = utf8_next(p, pn, &pi);

		if (c == many && !(esc && c == esc)) {
			while (pi < pn) {
				uint32_t k = pi;
				uint32_t d = utf8_next(p, pn, &k);

				if (d == many) {
					pi = k;
				} else if (d == one) {
					if (si >= sn) {
						return false;
					}
					pi = k;
					(void)utf8_next(s, sn, &si);
				} else {
					break;
				}
			}
			if (pi >= pn) {
				return true;
			}
			while (si <= sn) {
				if (pattern_match(p, pn, pi, s, sn, si, glob, esc, depth + 1)) {
					return true;
				}
				if (si >= sn) {
					break;
				}
				(void)utf8_next(s, sn, &si);
			}
			return false;
		}
		if (c == one && !(esc && c == esc)) {
			if (si >= sn) {
				return false;
			}
			(void)utf8_next(s, sn, &si);
			continue;
		}
		if (glob && c == '[') {
			uint32_t sc;
			bool invert = false, seen = false;
			uint32_t prev = 0;

			if (si >= sn) {
				return false;
			}
			sc = utf8_next(s, sn, &si);
			if (pi < pn && p[pi] == '^') {
				invert = true;
				pi++;
			}
			if (pi < pn && p[pi] == ']') {
				seen = sc == ']';
				prev = ']';
				pi++;
			}
			while (pi < pn && p[pi] != ']') {
				uint32_t d = utf8_next(p, pn, &pi);

				if (d == '-' && prev && pi < pn && p[pi] != ']') {
					uint32_t hi = utf8_next(p, pn, &pi);

					if (sc >= prev && sc <= hi) {
						seen = true;
					}
					prev = 0;
				} else {
					if (d == sc) {
						seen = true;
					}
					prev = d;
				}
			}
			if (pi >= pn) {
				return false;
			}
			pi++;
			if (seen == invert) {
				return false;
			}
			continue;
		}
		if (esc && c == esc && !glob) {
			if (pi >= pn) {
				return false;
			}
			c = utf8_next(p, pn, &pi);
		}
		(void)save;
		if (si >= sn) {
			return false;
		}
		{
			uint32_t d = utf8_next(s, sn, &si);

			if (glob ? c != d : lcu(c) != lcu(d)) {
				return false;
			}
		}
	}
	return si >= sn;
}

int sqnano_op_like(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *s,
		   const sqnano_value *pat, const sqnano_value *esc, bool glob)
{
	sqnano_value ts, tp, te;
	uint32_t e = 0;
	int rc;

	if (s->type == SQNANO_NULL || pat->type == SQNANO_NULL ||
	    (esc && esc->type == SQNANO_NULL)) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	rc = as_text(x, s, &ts);
	if (rc == SQNANO_OK) {
		rc = as_text(x, pat, &tp);
	}
	if (rc == SQNANO_OK && esc) {
		uint32_t i = 0;

		rc = as_text(x, esc, &te);
		if (rc == SQNANO_OK) {
			if (te.n == 0) {
				return sqnano_exec_error(x, SQNANO_ERROR,
							 "ESCAPE expression must be a single character");
			}
			e = utf8_next(te.u.p, te.n, &i);
			if (i != te.n) {
				return sqnano_exec_error(x, SQNANO_ERROR,
							 "ESCAPE expression must be a single character");
			}
		}
	}
	if (rc) {
		return rc;
	}
	sqnano_v_int(r, pattern_match(tp.u.p, tp.n, 0, ts.u.p, ts.n, 0, glob, e, 0));
	return SQNANO_OK;
}

int sqnano_cast(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *v, uint8_t aff)
{
	sqnano_value n;
	bool whole;

	*r = *v;
	if (v->type == SQNANO_NULL) {
		return SQNANO_OK;
	}
	switch (aff) {
	case SQNANO_AFF_TEXT:
		return as_text(x, v, r);
	case SQNANO_AFF_BLOB:
		if (v->type == SQNANO_INTEGER || v->type == SQNANO_FLOAT) {
			int rc = sqnano_v_to_text(x, r);

			if (rc) {
				return rc;
			}
		}
		r->type = SQNANO_BLOB;
		return SQNANO_OK;
	case SQNANO_AFF_INTEGER:
		sqnano_v_int(r, sqnano_v_as_int(v));
		return SQNANO_OK;
	case SQNANO_AFF_REAL:
		sqnano_v_real(r, sqnano_v_as_real(v));
		return SQNANO_OK;
	default:
		if (v->type == SQNANO_TEXT || v->type == SQNANO_BLOB) {
			(void)num_parse(v->u.z, v->n, &n, &whole);
			*r = n;
		}
		if (r->type == SQNANO_FLOAT && r->u.r > -9223372036854775808.0 &&
		    r->u.r < 9223372036854775808.0 && (double)(int64_t)r->u.r == r->u.r) {
			sqnano_v_int(r, (int64_t)r->u.r);
		}
		return SQNANO_OK;
	}
}

/* ---- Scalar functions ---- */

static uint32_t utf8_len(const uint8_t *z, uint32_t n)
{
	uint32_t c = 0;

	for (uint32_t i = 0; i < n; i++) {
		if ((z[i] & 0xc0) != 0x80) {
			c++;
		}
	}
	return c;
}

/* Byte offset of character @p ch. */
static uint32_t utf8_offset(const uint8_t *z, uint32_t n, uint32_t ch)
{
	uint32_t i = 0;

	while (ch > 0 && i < n) {
		i++;
		while (i < n && (z[i] & 0xc0) == 0x80) {
			i++;
		}
		ch--;
	}
	return i;
}

int sqnano_fn_abs(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	(void)argc;
	switch (argv[0].type) {
	case SQNANO_NULL:
		sqnano_v_null(r);
		break;
	case SQNANO_INTEGER:
		if (argv[0].u.i == I64_MIN) {
			return sqnano_exec_error(x, SQNANO_ERROR, "integer overflow");
		}
		sqnano_v_int(r, argv[0].u.i < 0 ? -argv[0].u.i : argv[0].u.i);
		break;
	default:
		sqnano_v_real(r, fabs(sqnano_v_as_real(&argv[0])));
		break;
	}
	return SQNANO_OK;
}

int sqnano_fn_length(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	sqnano_value t;
	int rc;

	(void)argc;
	switch (argv[0].type) {
	case SQNANO_NULL:
		sqnano_v_null(r);
		return SQNANO_OK;
	case SQNANO_BLOB:
		sqnano_v_int(r, argv[0].n);
		return SQNANO_OK;
	default:
		rc = as_text(x, &argv[0], &t);
		if (rc) {
			return rc;
		}
		sqnano_v_int(r, utf8_len(t.u.p, t.n));
		return SQNANO_OK;
	}
}

static int case_fn(struct sqnano_exec *x, sqnano_value *r, const sqnano_value *a, bool upper)
{
	sqnano_value t;
	char *p;
	int rc;

	if (a->type == SQNANO_NULL) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	rc = as_text(x, a, &t);
	if (rc) {
		return rc;
	}
	p = sqnano_scratch_alloc(x, t.n + 1);
	if (!p) {
		return nomem(x);
	}
	for (uint32_t i = 0; i < t.n; i++) {
		char c = t.u.z[i];

		if (upper && c >= 'a' && c <= 'z') {
			c = (char)(c - 32);
		} else if (!upper && c >= 'A' && c <= 'Z') {
			c = (char)(c + 32);
		}
		p[i] = c;
	}
	p[t.n] = 0;
	sqnano_v_text(r, p, t.n);
	return SQNANO_OK;
}

int sqnano_fn_lower(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	(void)argc;
	return case_fn(x, r, &argv[0], false);
}

int sqnano_fn_upper(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	(void)argc;
	return case_fn(x, r, &argv[0], true);
}

int sqnano_fn_substr(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	sqnano_value t;
	int64_t p1, p2, len;
	bool neg = false;
	bool blob;
	uint32_t b0, b1;
	char *p;
	int rc;

	if (argv[0].type == SQNANO_NULL || argv[1].type == SQNANO_NULL ||
	    (argc == 3 && argv[2].type == SQNANO_NULL)) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	blob = argv[0].type == SQNANO_BLOB;
	t = argv[0];
	if (!blob) {
		rc = as_text(x, &argv[0], &t);
		if (rc) {
			return rc;
		}
	}
	len = blob ? t.n : utf8_len(t.u.p, t.n);
	p1 = sqnano_v_as_int(&argv[1]);
	if (argc == 3) {
		p2 = sqnano_v_as_int(&argv[2]);
		if (p2 < 0) {
			p2 = -p2;
			neg = true;
		}
	} else {
		p2 = I64_MAX / 4;
	}
	if (p1 < 0) {
		p1 += len;
		if (p1 < 0) {
			p2 += p1;
			if (p2 < 0) {
				p2 = 0;
			}
			p1 = 0;
		}
	} else if (p1 > 0) {
		p1--;
	} else if (p2 > 0) {
		p2--;
	}
	if (neg) {
		p1 -= p2;
		if (p1 < 0) {
			p2 += p1;
			p1 = 0;
		}
	}
	if (p1 > len) {
		p1 = len;
	}
	if (p2 > len - p1) {
		p2 = len - p1;
	}
	if (blob) {
		b0 = (uint32_t)p1;
		b1 = (uint32_t)(p1 + p2);
	} else {
		b0 = utf8_offset(t.u.p, t.n, (uint32_t)p1);
		b1 = b0 + utf8_offset(t.u.p + b0, t.n - b0, (uint32_t)p2);
	}
	p = sqnano_scratch_alloc(x, b1 - b0 + 1);
	if (!p) {
		return nomem(x);
	}
	memcpy(p, t.u.p + b0, b1 - b0);
	p[b1 - b0] = 0;
	if (blob) {
		sqnano_v_blob(r, p, b1 - b0);
	} else {
		sqnano_v_text(r, p, b1 - b0);
	}
	return SQNANO_OK;
}

int sqnano_fn_round(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	int64_t n = 0;
	double v;

	(void)x;
	if (argv[0].type == SQNANO_NULL || (argc == 2 && argv[1].type == SQNANO_NULL)) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	if (argc == 2) {
		n = sqnano_v_as_int(&argv[1]);
		n = n < 0 ? 0 : (n > 30 ? 30 : n);
	}
	v = sqnano_v_as_real(&argv[0]);
	if (n == 0) {
		if (v > -4503599627370496.0 && v < 4503599627370496.0) {
			v = (double)(int64_t)(v + (v < 0 ? -0.5 : 0.5));
		}
	} else if (v > -1e300 && v < 1e300) {
		char buf[400];

		(void)snprintf(buf, sizeof(buf), "%.*f", (int)n, v);
		v = strtod(buf, NULL);
	}
	sqnano_v_real(r, v);
	return SQNANO_OK;
}

static int minmax(sqnano_value *r, int argc, const sqnano_value *argv, int sign)
{
	int best = 0;

	for (int i = 0; i < argc; i++) {
		if (argv[i].type == SQNANO_NULL) {
			sqnano_v_null(r);
			return SQNANO_OK;
		}
		if (i && sqnano_v_compare(&argv[i], &argv[best], SQNANO_COLL_BINARY) * sign > 0) {
			best = i;
		}
	}
	*r = argv[best];
	return SQNANO_OK;
}

int sqnano_fn_min(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	(void)x;
	return minmax(r, argc, argv, -1);
}

int sqnano_fn_max(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	(void)x;
	return minmax(r, argc, argv, 1);
}

int sqnano_fn_coalesce(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	(void)x;
	for (int i = 0; i < argc; i++) {
		if (argv[i].type != SQNANO_NULL) {
			*r = argv[i];
			return SQNANO_OK;
		}
	}
	sqnano_v_null(r);
	return SQNANO_OK;
}

int sqnano_fn_nullif(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	(void)x;
	(void)argc;
	if (argv[0].type != SQNANO_NULL && argv[1].type != SQNANO_NULL &&
	    sqnano_v_compare(&argv[0], &argv[1], SQNANO_COLL_BINARY) == 0) {
		sqnano_v_null(r);
	} else {
		*r = argv[0];
	}
	return SQNANO_OK;
}

int sqnano_fn_typeof(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	static const char *const names[] = {"null", "integer", "real", "text", "blob", "null"};
	const char *z = names[argv[0].type <= SQNANO_NULL ? argv[0].type : 0];

	(void)x;
	(void)argc;
	sqnano_v_text(r, z, (uint32_t)strlen(z));
	return SQNANO_OK;
}

int sqnano_fn_instr(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	sqnano_value a, b;
	bool blob;
	int rc;

	(void)argc;
	if (argv[0].type == SQNANO_NULL || argv[1].type == SQNANO_NULL) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	blob = argv[0].type == SQNANO_BLOB && argv[1].type == SQNANO_BLOB;
	a = argv[0];
	b = argv[1];
	if (!blob) {
		rc = as_text(x, &argv[0], &a);
		if (rc == SQNANO_OK) {
			rc = as_text(x, &argv[1], &b);
		}
		if (rc) {
			return rc;
		}
	}
	for (uint32_t i = 0; i + b.n <= a.n; i++) {
		if (memcmp(a.u.p + i, b.u.p, b.n) == 0) {
			sqnano_v_int(r, blob ? i + 1 : utf8_len(a.u.p, i) + 1);
			return SQNANO_OK;
		}
	}
	sqnano_v_int(r, 0);
	return SQNANO_OK;
}

static bool in_set(const uint8_t *set, uint32_t sn, const uint8_t *c, uint32_t cn)
{
	uint32_t i = 0;

	while (i < sn) {
		uint32_t s = i;

		(void)utf8_next(set, sn, &i);
		if (i - s == cn && memcmp(set + s, c, cn) == 0) {
			return true;
		}
	}
	return false;
}

static int trim_fn(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv,
		   bool left, bool right)
{
	sqnano_value t, s;
	uint32_t b = 0, e;
	char *p;
	int rc;

	if (argv[0].type == SQNANO_NULL || (argc == 2 && argv[1].type == SQNANO_NULL)) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	rc = as_text(x, &argv[0], &t);
	if (rc) {
		return rc;
	}
	if (argc == 2) {
		rc = as_text(x, &argv[1], &s);
		if (rc) {
			return rc;
		}
	} else {
		sqnano_v_text(&s, " ", 1);
	}
	e = t.n;
	if (left) {
		while (b < e) {
			uint32_t k = b;

			(void)utf8_next(t.u.p, e, &k);
			if (!in_set(s.u.p, s.n, t.u.p + b, k - b)) {
				break;
			}
			b = k;
		}
	}
	if (right) {
		while (e > b) {
			uint32_t k = e - 1;

			while (k > b && (t.u.p[k] & 0xc0) == 0x80) {
				k--;
			}
			if (!in_set(s.u.p, s.n, t.u.p + k, e - k)) {
				break;
			}
			e = k;
		}
	}
	p = sqnano_scratch_alloc(x, e - b + 1);
	if (!p) {
		return nomem(x);
	}
	memcpy(p, t.u.p + b, e - b);
	p[e - b] = 0;
	sqnano_v_text(r, p, e - b);
	return SQNANO_OK;
}

int sqnano_fn_trim(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	return trim_fn(x, r, argc, argv, true, true);
}

int sqnano_fn_ltrim(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	return trim_fn(x, r, argc, argv, true, false);
}

int sqnano_fn_rtrim(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	return trim_fn(x, r, argc, argv, false, true);
}

int sqnano_fn_replace(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	sqnano_value s, f, t;
	uint32_t count = 0, len;
	char *p;
	int rc;

	(void)argc;
	if (argv[0].type == SQNANO_NULL || argv[1].type == SQNANO_NULL ||
	    argv[2].type == SQNANO_NULL) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	rc = as_text(x, &argv[0], &s);
	if (rc == SQNANO_OK) {
		rc = as_text(x, &argv[1], &f);
	}
	if (rc == SQNANO_OK) {
		rc = as_text(x, &argv[2], &t);
	}
	if (rc) {
		return rc;
	}
	if (f.n == 0) {
		*r = s;
		return SQNANO_OK;
	}
	for (uint32_t i = 0; i + f.n <= s.n;) {
		if (memcmp(s.u.p + i, f.u.p, f.n) == 0) {
			count++;
			i += f.n;
		} else {
			i++;
		}
	}
	len = s.n + count * t.n - count * f.n;
	p = sqnano_scratch_alloc(x, len + 1);
	if (!p) {
		return nomem(x);
	}
	len = 0;
	for (uint32_t i = 0; i < s.n;) {
		if (i + f.n <= s.n && memcmp(s.u.p + i, f.u.p, f.n) == 0) {
			memcpy(p + len, t.u.p, t.n);
			len += t.n;
			i += f.n;
		} else {
			p[len++] = s.u.z[i++];
		}
	}
	p[len] = 0;
	sqnano_v_text(r, p, len);
	return SQNANO_OK;
}

int sqnano_fn_hex(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	static const char digits[] = "0123456789ABCDEF";
	sqnano_value t;
	char *p;
	int rc;

	(void)argc;
	t = argv[0];
	if (t.type == SQNANO_NULL) {
		sqnano_v_text(r, "", 0);
		return SQNANO_OK;
	}
	if (t.type != SQNANO_BLOB) {
		rc = as_text(x, &argv[0], &t);
		if (rc) {
			return rc;
		}
	}
	p = sqnano_scratch_alloc(x, 2 * t.n + 1);
	if (!p) {
		return nomem(x);
	}
	for (uint32_t i = 0; i < t.n; i++) {
		p[2 * i] = digits[t.u.p[i] >> 4];
		p[2 * i + 1] = digits[t.u.p[i] & 15];
	}
	p[2 * t.n] = 0;
	sqnano_v_text(r, p, 2 * t.n);
	return SQNANO_OK;
}

int sqnano_fn_sign(struct sqnano_exec *x, sqnano_value *r, int argc, const sqnano_value *argv)
{
	sqnano_value n;
	bool whole;
	double d;

	(void)x;
	(void)argc;
	n = argv[0];
	if (n.type == SQNANO_TEXT || n.type == SQNANO_BLOB) {
		if (!num_parse(argv[0].u.z, argv[0].n, &n, &whole) || !whole) {
			sqnano_v_null(r);
			return SQNANO_OK;
		}
	}
	if (n.type == SQNANO_NULL) {
		sqnano_v_null(r);
		return SQNANO_OK;
	}
	d = n.type == SQNANO_INTEGER ? (double)n.u.i : n.u.r;
	sqnano_v_int(r, d > 0 ? 1 : (d < 0 ? -1 : 0));
	return SQNANO_OK;
}
