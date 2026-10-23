/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Print the C normalization of each input record (records are separated by
 * NUL bytes): "<hash in hex> <consumed bytes> <normalized text>".
 */

#include <stdio.h>
#include <stdlib.h>

#include "compat/compat.h"

int main(void)
{
	static char buf[1 << 20];
	size_t n = fread(buf, 1, sizeof(buf) - 1, stdin);
	size_t pos = 0;

	buf[n] = 0;
	while (pos < n) {
		const char *rec = buf + pos;
		size_t len = strlen(rec);
		struct sqnano_norm_sink h, b;
		static char norm[1 << 16];
		size_t used;

		sqnano_norm_hash_init(&h);
		used = sqnano_normalize(rec, len, &h);
		sqnano_norm_buf_init(&b, norm, sizeof(norm));
		(void)sqnano_normalize(rec, len, &b);
		printf("%08x %zu %s\n", (unsigned int)h.hash, used, norm);
		pos += len + 1;
	}
	return 0;
}
