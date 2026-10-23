/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SQNANO_TESTUTIL_H_
#define SQNANO_TESTUTIL_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define CHECK_RC(expr, want)                                                                       \
	do {                                                                                       \
		int rc_ = (expr);                                                                  \
		if (rc_ != (want)) {                                                               \
			fprintf(stderr, "%s:%d: %s returned %d, expected %d\n", __FILE__, __LINE__, \
				#expr, rc_, (int)(want));                                          \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

static inline uint32_t test_rand(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

#endif /* SQNANO_TESTUTIL_H_ */
