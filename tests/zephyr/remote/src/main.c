/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The SQNano SMP group through the mcumgr dummy transport: the packets
 * sqnano-cli sends, and the responses it decodes.
 */

#include <string.h>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/transport/smp_dummy.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <sqlite3.h>

/* ---- A few CBOR items, enough for the requests ---- */

struct enc {
	uint8_t b[256];
	size_t n;
};

static void head(struct enc *e, uint8_t major, uint32_t v)
{
	if (v < 24) {
		e->b[e->n++] = (uint8_t)(major << 5 | v);
	} else if (v < 256) {
		e->b[e->n++] = (uint8_t)(major << 5 | 24);
		e->b[e->n++] = (uint8_t)v;
	} else {
		e->b[e->n++] = (uint8_t)(major << 5 | 25);
		e->b[e->n++] = (uint8_t)(v >> 8);
		e->b[e->n++] = (uint8_t)v;
	}
}

static void text(struct enc *e, const char *s)
{
	head(e, 3, strlen(s));
	memcpy(e->b + e->n, s, strlen(s));
	e->n += strlen(s);
}

/* Send one SMP request and return the decoded "d" byte string. */
static const uint8_t *exchange(const struct enc *payload, size_t *len)
{
	static uint8_t pkt[300];
	static uint8_t rsp[1024];
	static uint8_t seq;
	struct net_buf *nb;
	const uint8_t *p;
	size_t rlen;

	pkt[0] = 0; /* read */
	pkt[1] = 0;
	pkt[2] = (uint8_t)(payload->n >> 8);
	pkt[3] = (uint8_t)payload->n;
	pkt[4] = 0;
	pkt[5] = CONFIG_SQNANO_REMOTE_GROUP;
	pkt[6] = ++seq;
	pkt[7] = 0;
	memcpy(pkt + 8, payload->b, payload->n);
	smp_dummy_clear_state();
	(void)smp_dummy_tx_pkt(pkt, 8 + payload->n);
	smp_dummy_add_data();
	zassert_true(smp_dummy_wait_for_data(3), "no SMP response");
	nb = smp_dummy_get_outgoing();
	zassert_not_null(nb);
	rlen = nb->len < sizeof(rsp) ? nb->len : sizeof(rsp);
	memcpy(rsp, nb->data, rlen);
	net_buf_unref(nb);
	zassert_true(rlen > 8);
	zassert_equal(rsp[0], 1, "response op");
	zassert_equal(rsp[5], CONFIG_SQNANO_REMOTE_GROUP);
	zassert_equal(rsp[6], seq);
	/* {"d": bytes}: an indefinite map from mcumgr, then the "d" key. */
	p = rsp + 8;
	zassert_true(p[0] == 0xbf || p[0] == 0xa1, "payload %02x", p[0]);
	zassert_true(p[1] == 0x61 && p[2] == 'd', "no d key: %02x %02x %02x %02x %02x %02x", p[1], p[2],
		     p[3], p[4], p[5], p[6]);
	p += 3;
	if (p[0] == 0x58) {
		*len = p[1];
		p += 2;
	} else if (p[0] == 0x59) {
		*len = (size_t)p[1] << 8 | p[2];
		p += 3;
	} else {
		*len = p[0] & 0x1f;
		p += 1;
	}
	return p;
}

static bool contains(const uint8_t *p, size_t n, const char *s)
{
	size_t k = strlen(s);

	for (size_t i = 0; i + k <= n; i++) {
		if (memcmp(p + i, s, k) == 0) {
			return true;
		}
	}
	return false;
}

/* Number of rows of a scan/run response: the "rows" array has a 2-byte length. */
static int rows_of(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i + 8 <= n; i++) {
		if (memcmp(p + i, "\x64rows\x99", 6) == 0) {
			return p[i + 6] << 8 | p[i + 7];
		}
	}
	return -1;
}

static void setup_rows(void)
{
	sqlite3 *c = NULL;
	sqlite3_stmt *st;

	zassert_ok(sqlite3_open("app.db", &c));
	zassert_ok(sqlite3_prepare_v2(c, "INSERT INTO sensor (id, ts, value) VALUES (:id, :ts, :value)",
				      -1, &st, NULL));
	for (int i = 0; i < 40; i++) {
		sqlite3_bind_int(st, 1, i % 4);
		sqlite3_bind_int64(st, 2, 1000 + i);
		sqlite3_bind_double(st, 3, i / 2.0);
		zassert_equal(sqlite3_step(st), SQLITE_DONE, "%s", sqlite3_errmsg(c));
		sqlite3_reset(st);
	}
	sqlite3_finalize(st);
	zassert_ok(sqlite3_close(c));
}

static void *suite_setup(void)
{
	smp_dummy_enable();
	setup_rows();
	return NULL;
}

ZTEST(sqnano_remote, test_info)
{
	struct enc e = {.n = 0};
	const uint8_t *r;
	size_t n;

	head(&e, 5, 1);
	text(&e, "op");
	text(&e, "info");
	r = exchange(&e, &n);
	zassert_true(contains(r, n, "app.db"));
	zassert_true(contains(r, n, "fram.db"));
	zassert_true(contains(r, n, "shadow"));
}

ZTEST(sqnano_remote, test_tables_and_scan)
{
	struct enc e = {.n = 0};
	const uint8_t *r;
	size_t n;
	int total = 0;
	uint32_t after = 0;

	head(&e, 5, 2);
	text(&e, "op");
	text(&e, "tables");
	text(&e, "db");
	head(&e, 0, 0);
	r = exchange(&e, &n);
	zassert_true(contains(r, n, "CREATE TABLE sensor"));

	/* Pages of 16 rows, resumed after the last rowid. */
	for (int page = 0; page < 10; page++) {
		int got;

		e.n = 0;
		head(&e, 5, page ? 5 : 4);
		text(&e, "op");
		text(&e, "scan");
		text(&e, "db");
		head(&e, 0, 0);
		text(&e, "t");
		text(&e, "sensor");
		text(&e, "n");
		head(&e, 0, 16);
		if (page) {
			text(&e, "after");
			head(&e, 0, after);
		}
		r = exchange(&e, &n);
		got = rows_of(r, n);
		zassert_true(got >= 0, "not a scan response");
		total += got;
		after += (uint32_t)got;
		if (contains(r, n, "\x64more\xf4")) {
			break;
		}
	}
	zassert_equal(total, 40);
}

ZTEST(sqnano_remote, test_run)
{
	struct enc e = {.n = 0};
	const uint8_t *r;
	size_t n;

	head(&e, 5, 4);
	text(&e, "op");
	text(&e, "run");
	text(&e, "db");
	head(&e, 0, 0);
	text(&e, "q");
	text(&e, "sensor_last");
	text(&e, "p");
	head(&e, 4, 2);
	head(&e, 0, 1);
	head(&e, 0, 3);
	r = exchange(&e, &n);
	zassert_equal(rows_of(r, n), 3);
	zassert_true(contains(r, n, "\x62ts"));

	/* Writes are refused without CONFIG_SQNANO_REMOTE_WRITE. */
	e.n = 0;
	head(&e, 5, 3);
	text(&e, "op");
	text(&e, "run");
	text(&e, "db");
	head(&e, 0, 0);
	text(&e, "q");
	text(&e, "sensor_add");
	r = exchange(&e, &n);
	zassert_true(contains(r, n, "\x63" "err"));
}

ZTEST(sqnano_remote, test_malformed)
{
	struct enc e = {.n = 0};
	const uint8_t *r;
	size_t n;

	head(&e, 4, 0);
	r = exchange(&e, &n);
	zassert_true(contains(r, n, "malformed"));
}

ZTEST_SUITE(sqnano_remote, NULL, suite_setup, NULL, NULL, NULL);
