/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/* Key/value helpers on top of the internal B+tree API, with a reference model. */

#ifndef SQNANO_TEST_KV_H_
#define SQNANO_TEST_KV_H_

#include "sqnano_int.h"
#include "testutil.h"

#define KV_TREE 7
#define KV_MAX  600

static int kv_verbose = 1;

struct kv_model {
	uint8_t present[KV_MAX];
	uint32_t val[KV_MAX];
};

static inline uint16_t kv_vlen(uint32_t v, uint16_t maxlen)
{
	return (uint16_t)(1 + v % maxlen);
}

static inline void kv_key(uint8_t k[4], uint32_t key)
{
	k[0] = (uint8_t)(key >> 24);
	k[1] = (uint8_t)(key >> 16);
	k[2] = (uint8_t)(key >> 8);
	k[3] = (uint8_t)key;
}

static inline void kv_fill(uint8_t *buf, uint32_t v, uint16_t len)
{
	for (uint16_t i = 0; i < len; i++) {
		buf[i] = (uint8_t)(v * 31 + i);
	}
}

static inline int kv_put(struct sqnano_txn *t, uint32_t key, uint32_t v, uint16_t maxlen)
{
	struct sqnano_cursor c;
	uint8_t k[4];
	uint8_t buf[512];
	uint16_t len = kv_vlen(v, maxlen);
	int rc;

	kv_key(k, key);
	kv_fill(buf, v, len);
	db_lock(t->db);
	rc = bt_open(t, &c, KV_TREE);
	if (rc == SQNANO_OK) {
		rc = bt_insert(&c, k, 4, buf, len);
	}
	db_unlock(t->db);
	return rc;
}

static inline int kv_del(struct sqnano_txn *t, uint32_t key, bool *found)
{
	struct sqnano_cursor c;
	uint8_t k[4];
	bool exact;
	int rc;

	kv_key(k, key);
	db_lock(t->db);
	rc = bt_open(t, &c, KV_TREE);
	if (rc == SQNANO_OK) {
		rc = bt_seek(&c, k, 4, SQNANO_SEEK_EQ, &exact);
	}
	*found = rc == SQNANO_OK && c.state == CUR_VALID;
	if (*found) {
		rc = bt_delete(&c);
	}
	db_unlock(t->db);
	return rc;
}

/* Compare the tree with the model: forward, backward and point lookups. */
static inline int kv_verify(struct sqnano_txn *t, const struct kv_model *m, uint16_t maxlen)
{
	struct sqnano_cursor c;
	uint8_t buf[512], want[512];
	uint16_t len;
	uint32_t expect = 0;
	int rc;

	db_lock(t->db);
	rc = bt_open(t, &c, KV_TREE);
	if (rc == SQNANO_OK) {
		rc = bt_seek(&c, NULL, 0, SQNANO_SEEK_FIRST, NULL);
	}
	while (rc == SQNANO_OK && c.state == CUR_VALID) {
		uint32_t key = ((uint32_t)c.key[0] << 24) | ((uint32_t)c.key[1] << 16) |
			       ((uint32_t)c.key[2] << 8) | c.key[3];

		while (expect < KV_MAX && !m->present[expect]) {
			expect++;
		}
		if (expect >= KV_MAX || key != expect) {
			if (kv_verbose) fprintf(stderr, "kv_verify: key %u, expected %u\n", key, expect);
			rc = -1;
			break;
		}
		rc = bt_value(&c, buf, sizeof(buf), &len);
		if (rc) {
			break;
		}
		kv_fill(want, m->val[key], kv_vlen(m->val[key], maxlen));
		if (len != kv_vlen(m->val[key], maxlen) || memcmp(buf, want, len) != 0) {
			if (kv_verbose) fprintf(stderr, "kv_verify: value mismatch at %u\n", key);
			rc = -1;
			break;
		}
		expect++;
		rc = bt_next(&c);
	}
	if (rc == SQNANO_OK) {
		while (expect < KV_MAX && !m->present[expect]) {
			expect++;
		}
		if (expect != KV_MAX) {
			if (kv_verbose) fprintf(stderr, "kv_verify: missing key %u\n", expect);
			rc = -1;
		}
	}
	/* Backward scan count. */
	if (rc == SQNANO_OK) {
		int n = 0, want_n = 0;

		for (int i = 0; i < KV_MAX; i++) {
			want_n += m->present[i];
		}
		rc = bt_seek(&c, NULL, 0, SQNANO_SEEK_LAST, NULL);
		while (rc == SQNANO_OK && c.state == CUR_VALID) {
			n++;
			rc = bt_prev(&c);
		}
		if (rc == SQNANO_OK && n != want_n) {
			if (kv_verbose) fprintf(stderr, "kv_verify: backward count %d, expected %d\n", n, want_n);
			rc = -1;
		}
	}
	/* LE/GE seeks on a few keys. */
	for (uint32_t key = 0; rc == SQNANO_OK && key < KV_MAX; key += 37) {
		uint8_t k[4];
		bool exact;

		kv_key(k, key);
		rc = bt_seek(&c, k, 4, SQNANO_SEEK_GE, &exact);
		if (rc == SQNANO_OK && exact != (bool)m->present[key]) {
			if (kv_verbose) fprintf(stderr, "kv_verify: exact mismatch at %u\n", key);
			rc = -1;
		}
	}
	db_unlock(t->db);
	return rc;
}

static inline uint32_t kv_checksum(const struct kv_model *m)
{
	uint32_t h = 2166136261u;

	for (int i = 0; i < KV_MAX; i++) {
		uint32_t v = m->present[i] ? m->val[i] + 1 : 0;

		h = (h ^ v) * 16777619u;
	}
	return h;
}

#endif /* SQNANO_TEST_KV_H_ */
