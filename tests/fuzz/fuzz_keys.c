/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Properties of the encodings, on values decoded from the input:
 *  - keys compare with memcmp() as the values compare in SQLite, for every
 *    collation, ascending and descending, and for two-column keys (each
 *    encoding must end where the next one starts);
 *  - a record gives back each value exactly, and a truncated or random
 *    record is rejected, never read past its end.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <sqnano/exec.h>

#define NV 6

static const uint8_t *in, *in_end;

static uint8_t take(void)
{
	return in < in_end ? *in++ : 0;
}

static void value(sqnano_value *v, uint8_t *store)
{
	uint8_t kind = take();
	uint64_t u = 0;
	double d;
	size_t n;

	switch (kind % 5) {
	case 0:
		sqnano_v_null(v);
		break;
	case 1:
		for (int i = 0; i < 8; i++) {
			u = u << 8 | take();
		}
		/* Small numbers often, so that ties and neighbours happen. */
		sqnano_v_int(v, kind & 0x80 ? (int64_t)u : (int64_t)(int8_t)u);
		break;
	case 2:
		for (int i = 0; i < 8; i++) {
			u = u << 8 | take();
		}
		memcpy(&d, &u, 8);
		if (kind & 0x80) {
			d = (double)(int8_t)u + (kind & 0x40 ? 0.5 : 0);
		}
		if (isnan(d)) {
			d = 0; /* NaN is stored as NULL by SQLite */
		}
		sqnano_v_real(v, d);
		break;
	default:
		n = take() % 12;
		for (size_t i = 0; i < n; i++) {
			uint8_t c = take();

			/* Mostly letters, spaces and zero bytes: the cases collations care about. */
			store[i] = kind & 0x80 ? c : (uint8_t)("aAbB  \0zZ~"[c % 10]);
		}
		if (kind % 5 == 3) {
			sqnano_v_text(v, (const char *)store, (uint32_t)n);
		} else {
			sqnano_v_blob(v, store, (uint32_t)n);
		}
	}
}

static int sign(int x)
{
	return (x > 0) - (x < 0);
}

static int key_cmp(const struct sqnano_keybuf *a, const struct sqnano_keybuf *b)
{
	int r = memcmp(a->b, b->b, a->len < b->len ? a->len : b->len);

	return r ? sign(r) : sign((int)a->len - (int)b->len);
}

static void key(struct sqnano_keybuf *kb, const sqnano_value *v, int n, uint8_t coll, bool desc)
{
	kb->len = 0;
	for (int i = 0; i < n; i++) {
		if (sqnano_key_append(kb, &v[i], coll, desc, false)) {
			abort();
		}
	}
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static uint8_t store[NV][16];
	static uint8_t rec[1024];
	sqnano_value v[NV], back;
	uint16_t len;

	in = data;
	in_end = data + size;
	for (int i = 0; i < NV; i++) {
		value(&v[i], store[i]);
	}
	for (uint8_t coll = 0; coll <= SQNANO_COLL_RTRIM; coll++) {
		for (int i = 0; i + 1 < NV; i += 2) {
			struct sqnano_keybuf ka, kb;
			int want = sign(sqnano_v_compare(&v[i], &v[i + 1], coll));

			key(&ka, &v[i], 1, coll, false);
			key(&kb, &v[i + 1], 1, coll, false);
			if (key_cmp(&ka, &kb) != want) {
				abort();
			}
			key(&ka, &v[i], 1, coll, true);
			key(&kb, &v[i + 1], 1, coll, true);
			if (key_cmp(&ka, &kb) != -want) {
				abort();
			}
		}
		/* (v0, v1) against (v2, v3): the first column decides unless equal. */
		{
			struct sqnano_keybuf ka, kb;
			int want = sign(sqnano_v_compare(&v[0], &v[2], coll));

			if (!want) {
				want = sign(sqnano_v_compare(&v[1], &v[3], coll));
			}
			key(&ka, &v[0], 2, coll, false);
			key(&kb, &v[2], 2, coll, false);
			if (key_cmp(&ka, &kb) != want) {
				abort();
			}
		}
	}
	if (sqnano_record_build(rec, sizeof(rec), v, NV, 3, &len)) {
		abort();
	}
	if (sqnano_record_version(rec, len) != 3 || sqnano_record_count(rec, len) != NV) {
		abort();
	}
	for (uint16_t i = 0; i < NV; i++) {
		if (sqnano_record_get(rec, len, i, &back) || back.type != v[i].type ||
		    sqnano_v_compare(&back, &v[i], SQNANO_COLL_BINARY) != 0) {
			abort();
		}
	}
	/* The record cut short, then the rest of the input as a record. */
	for (uint16_t cut = 0; cut < len; cut += 1 + len / 8) {
		for (uint16_t i = 0; i < NV; i++) {
			(void)sqnano_record_get(rec, cut, i, &back);
		}
	}
	if (in < in_end) {
		size_t n = (size_t)(in_end - in) > sizeof(rec) ? sizeof(rec) : (size_t)(in_end - in);

		memcpy(rec, in, n);
		(void)sqnano_record_count(rec, (uint16_t)n);
		for (uint16_t i = 0; i < 40; i++) {
			(void)sqnano_record_get(rec, (uint16_t)n, i, &back);
		}
	}
	return 0;
}
