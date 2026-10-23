/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/* Statement state pool: fixed slots and first-fit arena. */

#include "testutil.h"

#include <sqnano/exec.h>

static union {
	uint64_t align;
	uint8_t b[1024];
} mem;

static void slots(void)
{
	struct sqnano_pool p = {.buf = mem.b, .size = 3 * 200, .slot = 200, .largest = 200};
	struct sqnano_schema s = {.pool = &p};
	struct sqnano_pool_stats st;
	void *a, *b, *c, *d;

	a = sqnano_state_alloc(&s, 150);
	b = sqnano_state_alloc(&s, 200);
	c = sqnano_state_alloc(&s, 10);
	CHECK(a == mem.b && b == mem.b + 200 && c == mem.b + 400);
	CHECK(sqnano_state_alloc(&s, 8) == NULL);
	CHECK(sqnano_state_alloc(&s, 201) == NULL);
	sqnano_state_free(&s, b);
	d = sqnano_state_alloc(&s, 200);
	CHECK(d == mem.b + 200);
	CHECK_RC(sqnano_pool_stats(&s, &st), SQNANO_OK);
	/* Bytes really used: 152 + 200 + 16. */
	CHECK(st.used == 368 && st.peak == 368 && st.fails == 2 && st.statements == 3);
	sqnano_state_free(&s, a);
	sqnano_state_free(&s, c);
	sqnano_state_free(&s, d);
	CHECK_RC(sqnano_pool_stats(&s, &st), SQNANO_OK);
	CHECK(st.used == 0 && st.statements == 0);
	printf("pool slots ok\n");
}

static void arena(void)
{
	struct sqnano_pool p = {.buf = mem.b, .size = 512, .largest = 300};
	struct sqnano_schema s = {.pool = &p};
	struct sqnano_pool_stats st;
	uint8_t *a, *b, *c, *d, *e;

	a = sqnano_state_alloc(&s, 100); /* [0, 104) */
	b = sqnano_state_alloc(&s, 200); /* [104, 304) */
	c = sqnano_state_alloc(&s, 150); /* [304, 456) */
	CHECK(a == mem.b && b == mem.b + 104 && c == mem.b + 304);
	CHECK((uintptr_t)b % 8 == 0 && (uintptr_t)c % 8 == 0);
	CHECK(sqnano_state_alloc(&s, 100) == NULL); /* 56 bytes left */
	sqnano_state_free(&s, b);
	/* First fit: the hole left by b. */
	d = sqnano_state_alloc(&s, 120);
	CHECK(d == mem.b + 104);
	e = sqnano_state_alloc(&s, 80);
	CHECK(e == mem.b + 224);
	CHECK(sqnano_state_alloc(&s, 80) == NULL); /* holes of 0 and 56 bytes */
	memset(a, 0xa5, 100);
	memset(d, 0x5a, 120);
	memset(e, 0x33, 80);
	CHECK(a[99] == 0xa5 && d[0] == 0x5a && d[119] == 0x5a && e[0] == 0x33);
	sqnano_state_free(&s, a);
	sqnano_state_free(&s, c);
	CHECK(sqnano_state_alloc(&s, 104) == mem.b);
	CHECK_RC(sqnano_pool_stats(&s, &st), SQNANO_OK);
	CHECK(st.peak == 104 + 200 + 152 && st.fails == 2);
	printf("pool arena ok\n");
}

int main(void)
{
	slots();
	arena();
	return 0;
}
