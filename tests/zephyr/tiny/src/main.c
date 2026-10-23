/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/* Profile "tiny": 128-byte pages on an EEPROM, typed API only. */

#include <zephyr/ztest.h>

#include <sqnano/zephyr.h>

#include "sqnano_gen.h"

ZTEST(sqnano_tiny, test_counters)
{
	struct sqnano_db *db;
	struct q_counter_bump bump;
	struct q_counter_get get;

	zassert_ok(sqnano_dt_open(sqnano_dt_find("counters.db"), &sqnano_app_schema, &db));
	for (int i = 0; i < 100; i++) {
		zassert_ok(q_counter_bump_open(&bump, db, NULL));
		q_counter_bump_bind(&bump, i % 7);
		zassert_ok(q_counter_bump_run(&bump));
	}
	zassert_ok(q_counter_get_open(&get, db, NULL));
	q_counter_get_bind(&get, 3);
	zassert_equal(q_counter_get_step(&get), SQNANO_ROW);
	zassert_equal(get.value, 14);
	/* Release the snapshot: the tiny profile has a single reader slot. */
	zassert_ok(q_counter_get_reset(&get));
	zassert_ok(sqnano_db_check(db));
}

ZTEST_SUITE(sqnano_tiny, NULL, NULL, NULL, NULL, NULL);
