/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Statement state pool, shared by the sqlite3.h facade, the shell ".run"
 * and the remote server. The memory is generated with the queries: by
 * default fixed slots of the largest state (no fragmentation possible),
 * or, with CONFIG_SQNANO_STMT_POOL_SIZE, an arena where each statement
 * takes the size of its own state (first fit). Both keep the peak of the
 * bytes really used, to size the arena.
 */

#include <sqnano/exec.h>

#include "../sqnano_int.h"

static SQNANO_MUTEX_DEFINE(pool_lock);

static uint32_t round8(uint32_t n)
{
	return (n + 7u) & ~7u;
}

void *sqnano_state_alloc(const struct sqnano_schema *s, uint32_t n)
{
	struct sqnano_pool *p = s ? s->pool : NULL;
	uint32_t len = round8(n);
	uint32_t at = 0;
	int pos = -1;

	if (!p) {
		return NULL;
	}
	sqnano_mutex_lock(&pool_lock);
	if (p->nblk < SQNANO_POOL_BLOCKS) {
		if (p->slot) {
			/* Fixed slots: the first one no block starts at. */
			if (len <= p->slot) {
				for (at = 0; at + p->slot <= p->size && pos < 0; at += p->slot) {
					int i = 0;

					while (i < p->nblk && p->blk[i].off < at) {
						i++;
					}
					if (i == p->nblk || p->blk[i].off != at) {
						pos = i;
						break;
					}
				}
			}
		} else {
			/* First fit between the blocks, kept sorted by offset. */
			for (int i = 0; i <= p->nblk; i++) {
				uint32_t end = i < p->nblk ? p->blk[i].off : p->size;

				if (end >= at && end - at >= len) {
					pos = i;
					break;
				}
				if (i < p->nblk) {
					at = p->blk[i].off + p->blk[i].len;
				}
			}
		}
	}
	if (pos < 0) {
		p->fails++;
		sqnano_mutex_unlock(&pool_lock);
		return NULL;
	}
	memmove(&p->blk[pos + 1], &p->blk[pos], (size_t)(p->nblk - pos) * sizeof(p->blk[0]));
	p->blk[pos].off = at;
	p->blk[pos].len = len;
	p->nblk++;
	p->used += len;
	p->peak = p->used > p->peak ? p->used : p->peak;
	sqnano_mutex_unlock(&pool_lock);
	return p->buf + at;
}

void sqnano_state_free(const struct sqnano_schema *s, void *mem)
{
	struct sqnano_pool *p = s ? s->pool : NULL;

	if (!p || !mem) {
		return;
	}
	sqnano_mutex_lock(&pool_lock);
	for (int i = 0; i < p->nblk; i++) {
		if (p->buf + p->blk[i].off == (uint8_t *)mem) {
			p->used -= p->blk[i].len;
			p->nblk--;
			memmove(&p->blk[i], &p->blk[i + 1], (size_t)(p->nblk - i) * sizeof(p->blk[0]));
			break;
		}
	}
	sqnano_mutex_unlock(&pool_lock);
}

int sqnano_pool_stats(const struct sqnano_schema *s, struct sqnano_pool_stats *st)
{
	struct sqnano_pool *p = s ? s->pool : NULL;

	if (!p) {
		return SQNANO_NOTFOUND;
	}
	sqnano_mutex_lock(&pool_lock);
	st->size = p->size;
	st->used = p->used;
	st->peak = p->peak;
	st->fails = p->fails;
	st->largest = p->largest;
	st->statements = p->nblk;
	sqnano_mutex_unlock(&pool_lock);
	return SQNANO_OK;
}
