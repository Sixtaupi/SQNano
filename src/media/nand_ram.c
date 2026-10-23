/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/* RAM NAND for tests: power cuts, worn blocks, bit flips, rule checks. */

#include <errno.h>
#include <string.h>

#include <sqnano/media.h>

#define M_PROG  0x80
#define M_TORN  0x40
#define M_FLIPS 0x3f

/* Bit flips the simulated on-die ECC corrects per page. */
#define CHIP_ECC_BITS 4

static uint32_t rnd(struct sqnano_ram_nand *r)
{
	uint32_t x = r->rng ? r->rng : 0x2545f491u;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	r->rng = x;
	return x;
}

static uint8_t *page_mem(struct sqnano_ram_nand *r, uint32_t page)
{
	return r->mem + (size_t)page * r->page_size;
}

/* Returns true when the current operation is the one cut. */
static bool cut_now(struct sqnano_ram_nand *r)
{
	r->ops_count++;
	if (r->cut_countdown < 0) {
		return false;
	}
	if (r->cut_countdown-- == 0) {
		r->powered_off = true;
		r->cut_countdown = -1;
		return true;
	}
	return false;
}

static int rn_read(void *ctx, uint32_t page, void *buf)
{
	struct sqnano_ram_nand *r = ctx;
	uint8_t m;

	if (r->powered_off) {
		return -ENXIO;
	}
	if (page >= r->blocks * r->ppb) {
		return -EINVAL;
	}
	m = r->meta[page];
	memcpy(buf, page_mem(r, page), r->page_size);
	if (!r->chip_ecc) {
		return 0;
	}
	if ((m & M_TORN) || (m & M_FLIPS) > CHIP_ECC_BITS) {
		return -EBADMSG;
	}
	return m & M_FLIPS;
}

static int rn_program(void *ctx, uint32_t page, const void *buf)
{
	struct sqnano_ram_nand *r = ctx;
	uint32_t b = page / r->ppb;
	uint32_t in = page % r->ppb;
	uint8_t *p;

	if (r->powered_off) {
		return -ENXIO;
	}
	if (page >= r->blocks * r->ppb) {
		return -EINVAL;
	}
	p = page_mem(r, page);
	r->programs++;
	if ((r->meta[page] & M_PROG) || in < r->next[b]) {
		/* Programmed twice, or below a page already programmed. */
		r->violations++;
	}
	if (cut_now(r)) {
		size_t done = rnd(r) % r->page_size;

		if (done == 0) {
			/* Cut before the cells were touched. */
			return -ENXIO;
		}
		r->next[b] = (uint8_t)(in + 1);
		memcpy(p, buf, done);
		for (size_t i = done; i < r->page_size; i++) {
			p[i] &= (uint8_t)rnd(r);
		}
		r->meta[page] = M_PROG | M_TORN;
		return -ENXIO;
	}
	if (r->fail_program_after >= 0 && r->fail_program_after-- == 0) {
		r->worn[b] = 1;
	}
	if (r->worn[b]) {
		/* The program fails: the page holds noise. */
		for (size_t i = 0; i < r->page_size; i++) {
			p[i] &= (uint8_t)rnd(r);
		}
		r->next[b] = (uint8_t)(in + 1);
		r->meta[page] = M_PROG | M_TORN;
		return -EIO;
	}
	r->next[b] = (uint8_t)(in + 1);
	memcpy(p, buf, r->page_size);
	r->meta[page] = M_PROG;
	return 0;
}

static int rn_erase(void *ctx, uint32_t block)
{
	struct sqnano_ram_nand *r = ctx;
	uint32_t first = block * r->ppb;

	if (r->powered_off) {
		return -ENXIO;
	}
	if (block >= r->blocks) {
		return -EINVAL;
	}
	r->erases++;
	if (cut_now(r)) {
		bool all = true;

		/* Interrupted: each page is erased, half erased, or left. */
		for (uint32_t q = first; q < first + r->ppb; q++) {
			uint32_t x = rnd(r) % 3;

			if (x == 0) {
				memset(page_mem(r, q), 0xff, r->page_size);
				r->meta[q] = 0;
			} else if (x == 1 && (r->meta[q] & M_PROG)) {
				uint8_t *p = page_mem(r, q);

				for (size_t i = 0; i < r->page_size; i++) {
					p[i] |= (uint8_t)rnd(r);
				}
				r->meta[q] |= M_TORN;
			}
			all = all && r->meta[q] == 0;
		}
		/* A block not wholly erased must be erased again before a program. */
		r->next[block] = all ? 0 : (uint8_t)r->ppb;
		return -ENXIO;
	}
	if (r->fail_erase_after >= 0 && r->fail_erase_after-- == 0) {
		r->worn[block] = 1;
	}
	if (r->worn[block]) {
		return -EIO;
	}
	memset(page_mem(r, first), 0xff, (size_t)r->ppb * r->page_size);
	memset(r->meta + first, 0, r->ppb);
	r->next[block] = 0;
	return 0;
}

static int rn_is_bad(void *ctx, uint32_t block)
{
	struct sqnano_ram_nand *r = ctx;

	if (r->powered_off) {
		return -ENXIO;
	}
	return r->bad[block];
}

static int rn_mark_bad(void *ctx, uint32_t block)
{
	struct sqnano_ram_nand *r = ctx;

	if (r->powered_off) {
		return -ENXIO;
	}
	if (cut_now(r)) {
		return -ENXIO;
	}
	r->bad[block] = 1;
	return 0;
}

void sqnano_ram_nand_init(struct sqnano_ram_nand *r, uint8_t *mem, uint8_t *meta,
			  uint32_t page_size, uint32_t ppb, uint32_t blocks, bool chip_ecc)
{
	memset(r, 0, sizeof(*r));
	r->ops.read = rn_read;
	r->ops.program = rn_program;
	r->ops.erase = rn_erase;
	r->ops.is_bad = rn_is_bad;
	r->ops.mark_bad = rn_mark_bad;
	r->mem = mem;
	r->meta = meta;
	r->page_size = page_size;
	r->ppb = ppb;
	r->blocks = blocks;
	r->chip_ecc = chip_ecc;
	r->cut_countdown = -1;
	r->fail_program_after = -1;
	r->fail_erase_after = -1;
	r->rng = 0x9e3779b9u;
	memset(mem, 0xff, (size_t)page_size * ppb * blocks);
	memset(meta, 0, (size_t)ppb * blocks);
}

void sqnano_ram_nand_cut_after(struct sqnano_ram_nand *r, uint32_t ops, uint32_t seed)
{
	r->cut_countdown = (int32_t)ops;
	r->rng = seed | 1u;
	r->powered_off = false;
}

void sqnano_ram_nand_power_on(struct sqnano_ram_nand *r)
{
	r->powered_off = false;
	r->cut_countdown = -1;
}

void sqnano_ram_nand_flip(struct sqnano_ram_nand *r, uint32_t page, uint32_t bit)
{
	if (r->chip_ecc) {
		if ((r->meta[page] & M_FLIPS) < M_FLIPS) {
			r->meta[page]++;
		}
		return;
	}
	page_mem(r, page)[bit / 8 % r->page_size] ^= (uint8_t)(1u << (bit % 8));
}
