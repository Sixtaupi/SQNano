/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Canonical form of a statement, used to find its precompiled version.
 *
 * This must stay byte-for-byte identical to normalize() in
 * tools/sqnanoc/lex.py: tokens separated by one space, ASCII lowercased
 * except string literals and parameter names, comments and the final ';'
 * dropped. The output is streamed to a sink so that no buffer is needed.
 */

#include "compat.h"

static bool is_space(uint8_t c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool is_digit(uint8_t c)
{
	return c >= '0' && c <= '9';
}

static bool is_hex(uint8_t c)
{
	return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool ident_start(uint8_t c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

static bool ident_char(uint8_t c)
{
	return ident_start(c) || is_digit(c) || c == '$';
}

static uint8_t lower(uint8_t c)
{
	return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c;
}

static void emit(struct sqnano_norm_sink *k, const uint8_t *p, size_t n, bool fold)
{
	if (k->tokens++) {
		k->put(k, ' ');
	}
	for (size_t i = 0; i < n; i++) {
		k->put(k, fold ? lower(p[i]) : p[i]);
	}
}

size_t sqnano_normalize(const char *sql, size_t n, struct sqnano_norm_sink *k)
{
	const uint8_t *s = (const uint8_t *)sql;
	size_t i = 0;

	k->tokens = 0;
	while (i < n) {
		uint8_t c = s[i];
		size_t start = i;

		if (is_space(c)) {
			i++;
			continue;
		}
		if (c == '-' && i + 1 < n && s[i + 1] == '-') {
			while (i < n && s[i] != '\n') {
				i++;
			}
			continue;
		}
		if (c == '/' && i + 1 < n && s[i + 1] == '*') {
			i += 2;
			while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) {
				i++;
			}
			i = i < n ? i + 2 : n;
			continue;
		}
		if (c == ';') {
			return i + 1;
		}
		if (c == '\'') {
			i++;
			while (i < n) {
				if (s[i] == '\'') {
					if (i + 1 < n && s[i + 1] == '\'') {
						i += 2;
						continue;
					}
					i++;
					break;
				}
				i++;
			}
			emit(k, s + start, i - start, false);
			continue;
		}
		if ((c == 'x' || c == 'X') && i + 1 < n && s[i + 1] == '\'') {
			i += 2;
			while (i < n && s[i] != '\'') {
				i++;
			}
			i = i < n ? i + 1 : n;
			emit(k, s + start, i - start, true);
			continue;
		}
		if (c == '"' || c == '`') {
			i++;
			while (i < n) {
				if (s[i] == c) {
					if (i + 1 < n && s[i + 1] == c) {
						i += 2;
						continue;
					}
					i++;
					break;
				}
				i++;
			}
			emit(k, s + start, i - start, true);
			continue;
		}
		if (c == '[') {
			while (i < n && s[i] != ']') {
				i++;
			}
			i = i < n ? i + 1 : n;
			emit(k, s + start, i - start, true);
			continue;
		}
		if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(s[i + 1]))) {
			if (c == '0' && i + 2 < n && (s[i + 1] == 'x' || s[i + 1] == 'X') &&
			    is_hex(s[i + 2])) {
				i += 2;
				while (i < n && is_hex(s[i])) {
					i++;
				}
			} else {
				while (i < n && is_digit(s[i])) {
					i++;
				}
				if (i < n && s[i] == '.') {
					i++;
					while (i < n && is_digit(s[i])) {
						i++;
					}
				}
				if (i < n && (s[i] == 'e' || s[i] == 'E')) {
					size_t j = i + 1;

					if (j < n && (s[j] == '+' || s[j] == '-')) {
						j++;
					}
					if (j < n && is_digit(s[j])) {
						i = j;
						while (i < n && is_digit(s[i])) {
							i++;
						}
					}
				}
			}
			emit(k, s + start, i - start, true);
			continue;
		}
		if (c == '?') {
			i++;
			while (i < n && is_digit(s[i])) {
				i++;
			}
			emit(k, s + start, i - start, false);
			continue;
		}
		if ((c == ':' || c == '@' || c == '$') && i + 1 < n && ident_char(s[i + 1])) {
			i++;
			while (i < n && ident_char(s[i])) {
				i++;
			}
			emit(k, s + start, i - start, false);
			continue;
		}
		if (ident_start(c)) {
			while (i < n && ident_char(s[i])) {
				i++;
			}
			emit(k, s + start, i - start, true);
			continue;
		}
		{
			static const char *const ops[] = {"->>", "||", "<=", ">=", "==", "!=",
							  "<>",  "<<", ">>", "->"};
			size_t len = 1;

			for (size_t o = 0; o < sizeof(ops) / sizeof(ops[0]); o++) {
				size_t l = strlen(ops[o]);

				if (i + l <= n && memcmp(s + i, ops[o], l) == 0) {
					len = l;
					break;
				}
			}
			i += len;
			emit(k, s + start, len, false);
		}
	}
	return i;
}

/* ---- Sinks ---- */

static void hash_put(struct sqnano_norm_sink *k, uint8_t c)
{
	k->hash = (k->hash ^ c) * 0x01000193u;
	k->len++;
	if (k->word_len < sizeof(k->word) - 1 && k->tokens == 1) {
		k->word[k->word_len++] = (char)c;
		k->word[k->word_len] = 0;
	}
}

void sqnano_norm_hash_init(struct sqnano_norm_sink *k)
{
	memset(k, 0, sizeof(*k));
	k->put = hash_put;
	k->hash = 0x811c9dc5u;
}

static void cmp_put(struct sqnano_norm_sink *k, uint8_t c)
{
	if (k->ref[k->len] != (char)c) {
		k->mismatch = true;
	}
	if (k->ref[k->len]) {
		k->len++;
	} else {
		k->mismatch = true;
	}
}

void sqnano_norm_cmp_init(struct sqnano_norm_sink *k, const char *ref)
{
	memset(k, 0, sizeof(*k));
	k->put = cmp_put;
	k->ref = ref;
}

bool sqnano_norm_cmp_done(const struct sqnano_norm_sink *k)
{
	return !k->mismatch && k->ref[k->len] == 0;
}

static void buf_put(struct sqnano_norm_sink *k, uint8_t c)
{
	if (k->len + 1 < k->cap) {
		k->buf[k->len] = (char)c;
		k->buf[k->len + 1] = 0;
	} else {
		k->mismatch = true;
	}
	k->len++;
}

void sqnano_norm_buf_init(struct sqnano_norm_sink *k, char *buf, size_t cap)
{
	memset(k, 0, sizeof(*k));
	k->put = buf_put;
	k->buf = buf;
	k->cap = cap;
	if (cap) {
		buf[0] = 0;
	}
}
