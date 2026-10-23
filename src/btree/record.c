/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Records and keys.
 *
 * Record: varint schema version, varint slot count, one varint serial type
 * per slot, then the bodies. Serial types follow SQLite: 0 NULL, 1..6
 * big-endian integers of 1, 2, 3, 4, 6 and 8 bytes, 7 IEEE double, 8 and 9
 * the constants 0 and 1, 12+2n a blob of n bytes, 13+2n a text of n bytes.
 * Text bodies carry a trailing NUL so that decoded text is a C string.
 *
 * Keys are compared with memcmp(). Each value is encoded with a type tag
 * that follows the SQLite order (NULL < numbers < text < blob):
 *   NULL   05
 *   number 15, order-preserving double, then for |v| >= 2^53 an exact
 *          64-bit tie-break and one byte (1 only for reals >= 2^63)
 *   text   30, bytes with 00 escaped as 00 ff, terminator 00 00
 *   blob   40, same escaping as text
 * A descending column inverts every byte of its encoding.
 */

#include "../sqnano_int.h"

static const uint8_t int_sizes[7] = {0, 1, 2, 3, 4, 6, 8};

static uint64_t serial_type(const sqnano_value *v)
{
	switch (v->type) {
	case SQNANO_INTEGER: {
		int64_t i = v->u.i;

		if (i == 0) {
			return 8;
		}
		if (i == 1) {
			return 9;
		}
		if (i >= -128 && i <= 127) {
			return 1;
		}
		if (i >= -32768 && i <= 32767) {
			return 2;
		}
		if (i >= -8388608 && i <= 8388607) {
			return 3;
		}
		if (i >= -2147483647LL - 1 && i <= 2147483647LL) {
			return 4;
		}
		if (i >= -140737488355328LL && i <= 140737488355327LL) {
			return 5;
		}
		return 6;
	}
	case SQNANO_FLOAT:
		return 7;
	case SQNANO_TEXT:
		return 13 + 2 * (uint64_t)v->n;
	case SQNANO_BLOB:
		return 12 + 2 * (uint64_t)v->n;
	default:
		return 0;
	}
}

/* 64-bit: a corrupt record may declare sizes that overflow 32 bits. */
static uint64_t body_size(uint64_t st)
{
	if (st >= 12) {
		return (st - 12) / 2 + (st & 1);
	}
	if (st >= 1 && st <= 6) {
		return int_sizes[st];
	}
	return st == 7 ? 8 : 0;
}

int sqnano_record_build(uint8_t *buf, uint16_t cap, const sqnano_value *vals, uint16_t n,
			uint16_t schema_version, uint16_t *len)
{
	uint32_t hdr = (uint32_t)varint_len(schema_version) + (uint32_t)varint_len(n);
	uint32_t total;
	uint32_t pos;
	uint32_t body;

	for (uint16_t i = 0; i < n; i++) {
		hdr += (uint32_t)varint_len(serial_type(&vals[i]));
	}
	total = hdr;
	for (uint16_t i = 0; i < n; i++) {
		total += body_size(serial_type(&vals[i]));
	}
	if (total > cap) {
		return SQNANO_TOOBIG;
	}
	pos = (uint32_t)varint_put(buf, schema_version);
	pos += (uint32_t)varint_put(buf + pos, n);
	for (uint16_t i = 0; i < n; i++) {
		pos += (uint32_t)varint_put(buf + pos, serial_type(&vals[i]));
	}
	body = pos;
	for (uint16_t i = 0; i < n; i++) {
		const sqnano_value *v = &vals[i];
		uint64_t st = serial_type(v);
		uint32_t sz = (uint32_t)body_size(st);

		if (st >= 1 && st <= 6) {
			uint64_t u = (uint64_t)v->u.i;

			for (uint32_t k = 0; k < sz; k++) {
				buf[body + k] = (uint8_t)(u >> (8 * (sz - 1 - k)));
			}
		} else if (st == 7) {
			uint64_t u;

			memcpy(&u, &v->u.r, 8);
			for (uint32_t k = 0; k < 8; k++) {
				buf[body + k] = (uint8_t)(u >> (8 * (7 - k)));
			}
		} else if (st >= 12) {
			if (v->n) {
				memcpy(buf + body, v->u.p, v->n);
			}
			if (st & 1) {
				buf[body + v->n] = 0;
			}
		}
		body += sz;
	}
	*len = (uint16_t)total;
	return SQNANO_OK;
}

uint16_t sqnano_record_version(const uint8_t *rec, uint16_t len)
{
	uint64_t ver;

	if (varint_get(rec, rec + len, &ver) < 0 || ver > 0xffff) {
		return 0;
	}
	return (uint16_t)ver;
}

int sqnano_record_count(const uint8_t *rec, uint16_t len)
{
	uint64_t ver, n;
	int a = varint_get(rec, rec + len, &ver);
	int b;

	if (a < 0) {
		return -1;
	}
	b = varint_get(rec + a, rec + len, &n);
	if (b < 0 || n > 0xffff) {
		return -1;
	}
	return (int)n;
}

int sqnano_record_get(const uint8_t *rec, uint16_t len, uint16_t slot, sqnano_value *out)
{
	const uint8_t *end = rec + len;
	const uint8_t *p = rec;
	uint64_t ver, n, st = 0;
	uint64_t hdr_end, off;
	int k;

	sqnano_v_null(out);
	k = varint_get(p, end, &ver);
	if (k < 0) {
		return SQNANO_CORRUPT;
	}
	p += k;
	k = varint_get(p, end, &n);
	if (k < 0) {
		return SQNANO_CORRUPT;
	}
	p += k;
	if (slot >= n) {
		return SQNANO_NOTFOUND;
	}
	/* Header end = offset after the last serial type. */
	{
		const uint8_t *q = p;

		off = 0;
		for (uint64_t i = 0; i < n; i++) {
			uint64_t t;

			k = varint_get(q, end, &t);
			if (k < 0) {
				return SQNANO_CORRUPT;
			}
			q += k;
			if (i < slot) {
				off += body_size(t);
			} else if (i == slot) {
				st = t;
			}
		}
		hdr_end = (uint64_t)(q - rec);
	}
	off += hdr_end;
	if (off > len || body_size(st) > len - off) {
		return SQNANO_CORRUPT;
	}
	p = rec + off;
	if (st == 0) {
		return SQNANO_OK;
	}
	if (st == 8 || st == 9) {
		sqnano_v_int(out, st == 9);
	} else if (st <= 6) {
		uint32_t sz = int_sizes[st];
		uint64_t u = (p[0] & 0x80) ? ~(uint64_t)0 : 0;

		for (uint32_t i = 0; i < sz; i++) {
			u = (u << 8) | p[i];
		}
		sqnano_v_int(out, (int64_t)u);
	} else if (st == 7) {
		uint64_t u = 0;
		double r;

		for (int i = 0; i < 8; i++) {
			u = (u << 8) | p[i];
		}
		memcpy(&r, &u, 8);
		sqnano_v_real(out, r);
	} else if (st >= 12) {
		uint32_t sz = (uint32_t)((st - 12) / 2);

		if (st & 1) {
			if (p[sz] != 0) {
				return SQNANO_CORRUPT;
			}
			sqnano_v_text(out, (const char *)p, sz);
		} else {
			sqnano_v_blob(out, p, sz);
		}
	} else {
		return SQNANO_CORRUPT;
	}
	return SQNANO_OK;
}

/* ---- Keys ---- */

void sqnano_key_rowid(uint8_t out[8], int64_t rowid)
{
	uint64_t u = (uint64_t)rowid ^ 0x8000000000000000ull;

	for (int i = 0; i < 8; i++) {
		out[i] = (uint8_t)(u >> (8 * (7 - i)));
	}
}

int64_t sqnano_key_get_rowid(const uint8_t in[8])
{
	uint64_t u = 0;

	for (int i = 0; i < 8; i++) {
		u = (u << 8) | in[i];
	}
	return (int64_t)(u ^ 0x8000000000000000ull);
}

static int kb_put(struct sqnano_keybuf *kb, uint8_t b)
{
	if (kb->len >= SQNANO_MAX_KEY_SIZE) {
		return SQNANO_TOOBIG;
	}
	kb->b[kb->len++] = b;
	return SQNANO_OK;
}

static void put_ordered_double(uint8_t out[8], double d)
{
	uint64_t u;

	if (d == 0) {
		d = 0; /* -0.0 sorts as 0.0 */
	}
	memcpy(&u, &d, 8);
	if (u & 0x8000000000000000ull) {
		u = ~u;
	} else {
		u |= 0x8000000000000000ull;
	}
	for (int i = 0; i < 8; i++) {
		out[i] = (uint8_t)(u >> (8 * (7 - i)));
	}
}

static uint8_t fold(uint8_t c, uint8_t coll)
{
	if (coll == SQNANO_COLL_NOCASE && c >= 'A' && c <= 'Z') {
		return (uint8_t)(c + 32);
	}
	return c;
}

int sqnano_key_append(struct sqnano_keybuf *kb, const sqnano_value *v, uint8_t coll, bool desc,
		      bool bound)
{
	uint16_t start = kb->len;
	int rc = SQNANO_OK;

	switch (v->type) {
	case SQNANO_INTEGER:
	case SQNANO_FLOAT: {
		uint8_t d[8];
		double r = v->type == SQNANO_INTEGER ? (double)v->u.i : v->u.r;

		if (r != r) {
			rc = kb_put(kb, 0x05);
			break;
		}
		put_ordered_double(d, r);
		rc = kb_put(kb, 0x15);
		for (int i = 0; i < 8 && rc == SQNANO_OK; i++) {
			rc = kb_put(kb, d[i]);
		}
		/*
		 * Below 2^53 the double is exact and distinct values never share
		 * it. Above, integers that round to the same double are ordered
		 * by an exact 64-bit tie-break (integral reals use their integer
		 * value, so 2^60 and 2^60.0 encode identically).
		 */
		if (!bound && rc == SQNANO_OK && (r >= 9007199254740992.0 || r <= -9007199254740992.0)) {
			uint8_t e[8];
			int64_t iv;
			uint8_t extra = 0;

			if (v->type == SQNANO_INTEGER) {
				iv = v->u.i;
			} else if (r >= 9223372036854775808.0) {
				iv = INT64_MAX;
				extra = 1;
			} else if (r < -9223372036854775808.0) {
				iv = INT64_MIN;
			} else {
				iv = (int64_t)r;
			}
			sqnano_key_rowid(e, iv);
			for (int i = 0; i < 8 && rc == SQNANO_OK; i++) {
				rc = kb_put(kb, e[i]);
			}
			if (rc == SQNANO_OK) {
				rc = kb_put(kb, extra);
			}
		}
		break;
	}
	case SQNANO_TEXT:
	case SQNANO_BLOB: {
		uint32_t n = v->n;

		if (v->type == SQNANO_TEXT && coll == SQNANO_COLL_RTRIM) {
			while (n > 0 && v->u.p[n - 1] == ' ') {
				n--;
			}
		}
		rc = kb_put(kb, v->type == SQNANO_TEXT ? 0x30 : 0x40);
		for (uint32_t i = 0; i < n && rc == SQNANO_OK; i++) {
			uint8_t c = v->type == SQNANO_TEXT ? fold(v->u.p[i], coll) : v->u.p[i];

			rc = kb_put(kb, c);
			if (c == 0 && rc == SQNANO_OK) {
				rc = kb_put(kb, 0xff);
			}
		}
		if (rc == SQNANO_OK) {
			rc = kb_put(kb, 0x00);
		}
		if (rc == SQNANO_OK) {
			rc = kb_put(kb, 0x00);
		}
		break;
	}
	default:
		rc = kb_put(kb, 0x05);
		break;
	}
	if (rc == SQNANO_OK && desc) {
		for (uint16_t i = start; i < kb->len; i++) {
			kb->b[i] = (uint8_t)~kb->b[i];
		}
	}
	return rc;
}

int sqnano_key_append_rowid(struct sqnano_keybuf *kb, int64_t rowid)
{
	if (kb->len + 8 > SQNANO_MAX_KEY_SIZE) {
		return SQNANO_TOOBIG;
	}
	sqnano_key_rowid(kb->b + kb->len, rowid);
	kb->len += 8;
	return SQNANO_OK;
}
