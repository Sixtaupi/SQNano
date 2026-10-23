/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SQNANO_COMPAT_INT_H_
#define SQNANO_COMPAT_INT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** Receives the normalized text one byte at a time. */
struct sqnano_norm_sink {
	void (*put)(struct sqnano_norm_sink *k, uint8_t c);
	uint32_t hash;
	size_t len;
	size_t tokens;
	const char *ref;
	char *buf;
	size_t cap;
	bool mismatch;
	char word[16];
	size_t word_len;
};

/** Normalize the first statement of @p sql; returns the bytes consumed. */
size_t sqnano_normalize(const char *sql, size_t n, struct sqnano_norm_sink *k);
/** FNV-1a hash sink; also records the first token in k->word. */
void sqnano_norm_hash_init(struct sqnano_norm_sink *k);
/** Comparison sink against a normalized reference text. */
void sqnano_norm_cmp_init(struct sqnano_norm_sink *k, const char *ref);
bool sqnano_norm_cmp_done(const struct sqnano_norm_sink *k);
/** Copy sink (truncates, sets mismatch on overflow). */
void sqnano_norm_buf_init(struct sqnano_norm_sink *k, char *buf, size_t cap);

#endif /* SQNANO_COMPAT_INT_H_ */
