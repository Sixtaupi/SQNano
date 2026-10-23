/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * NAND media: a NOR-like flash over raw NAND.
 *
 * Logical erase unit u (spb logical pages) lives in physical block phys[u].
 * The engine writes the logical pages of a unit in any order; each goes to
 * the next physical page of the block, followed by a trailer:
 *
 *   [slot data][magic u16][unit u16][slot u8][flags u8][gen u16][count u16]
 *   [0 u16][crc32 of data and trailer][soft ECC: u16 per 512 bytes][0xff..]
 *
 * Init reads every page and rebuilds map[] (logical page -> physical page)
 * from the trailers. A page that does not validate (interrupted program,
 * uncorrectable) only wastes its physical page; the spare pages of a block
 * absorb them, and a full block is compacted into a fresh one.
 *
 * Moving a unit (program or erase failure, ECC refresh, full block) copies
 * its pages to a free block with generation gen + 1 and flag COPY, then
 * programs a DONE marker page, and only then marks the old block bad
 * (RETIRE) or erases it. If init finds two blocks for a unit, a copy
 * without its marker loses; otherwise the newer generation wins and the
 * other block is erased, or marked bad when the winner says RETIRE.
 */

#include <errno.h>
#include <string.h>

#include <sqnano/media.h>

uint32_t sqnano_crc32(uint32_t crc, const void *buf, size_t len);

#define TAG_MAGIC  0x4e53u /* "SN" */
#define TAG_SIZE   16u
#define ECC_CHUNK  512u
#define NONE16     0xffffu
#define NONE8      0xffu
#define DONE_SLOT  0xffu

#define F_COPY   0x01
#define F_DONE   0x02
#define F_RETIRE 0x04

/* Physical block states. */
#define BS_FREE  0 /* erased */
#define BS_DIRTY 1 /* to erase before use */
#define BS_USED  2 /* backs a logical unit */
#define BS_BAD   3

/* Unit flags; the C_* ones only during init, for the block found so far. */
#define UF_REFRESH 0x01
#define UC_COPY    0x02
#define UC_DONE    0x04
#define UC_RETIRE  0x08

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t get32(const uint8_t *p)
{
	return get16(p) | (uint32_t)get16(p + 2) << 16;
}

static bool newer(uint16_t a, uint16_t b)
{
	return (int16_t)(uint16_t)(a - b) > 0;
}

/* ---- Software ECC: extended Hamming code (SEC-DED) per 512 bytes ---- */

/*
 * Data bit d sits at the (d + 1)th position >= 1 that is not a power of two;
 * check bit j at position 2^j. The 13 check bits are the XOR of the
 * positions of the data bits set, so that a valid codeword has syndrome 0;
 * bit 15 makes the parity of the whole codeword even.
 */
static uint32_t ecc_scan(const uint8_t *p, uint32_t n, uint32_t *par)
{
	uint32_t x = 0, odd = 0, pos = 3;

	for (uint32_t i = 0; i < n; i++) {
		uint8_t v = p[i];

		for (int b = 0; b < 8; b++, pos++) {
			if ((pos & (pos - 1)) == 0) {
				pos++;
			}
			if (v & (1u << b)) {
				x ^= pos;
				odd ^= 1;
			}
		}
	}
	*par = odd;
	return x;
}

static uint16_t ecc_calc(const uint8_t *p, uint32_t n)
{
	uint32_t par;
	uint32_t x = ecc_scan(p, n, &par);

	par ^= (uint32_t)__builtin_popcount(x) & 1u;
	return (uint16_t)(x | par << 15);
}

/* Returns the number of bits corrected (0 or 1), or -EBADMSG. */
static int ecc_fix(uint8_t *p, uint32_t n, uint16_t stored)
{
	uint32_t pd;
	uint32_t c = stored & 0x1fffu;
	uint32_t s = ecc_scan(p, n, &pd) ^ c;
	uint32_t odd = pd ^ ((uint32_t)__builtin_popcount(c) & 1u) ^ (stored >> 15);
	uint32_t d;

	if (!s && !odd) {
		return 0;
	}
	if (!odd) {
		/* Two errors. */
		return -EBADMSG;
	}
	if (s == 0 || (s & (s - 1)) == 0) {
		/* The parity or a check bit was hit: the data is right. */
		return 1;
	}
	d = s - (32u - (uint32_t)__builtin_clz(s)) - 1u;
	if (d >= n * 8u) {
		return -EBADMSG;
	}
	p[d / 8] ^= (uint8_t)(1u << (d % 8));
	return 1;
}

static uint32_t ecc_chunks(uint32_t covered)
{
	return (covered + ECC_CHUNK - 1) / ECC_CHUNK;
}

/* ---- Pages ---- */

static struct sqnano_nand_media *nm_of(const struct sqnano_media *m)
{
	return (struct sqnano_nand_media *)m->ctx;
}

static uint32_t ppb(const struct sqnano_nand_media *n)
{
	return n->geo.pages_per_block;
}

static uint8_t *map_of(struct sqnano_nand_media *n, uint16_t u)
{
	return &n->map[(uint32_t)u * n->spb];
}

static bool blank(const uint8_t *p, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		if (p[i] != 0xff) {
			return false;
		}
	}
	return true;
}

/* Program n->buf (slot data already in place) at page @p page of block @p b. */
static int prog_at(struct sqnano_nand_media *n, uint16_t b, uint32_t page, uint16_t u, uint8_t slot,
		   uint8_t flags, uint16_t gen, uint16_t count)
{
	uint8_t *t = n->buf + n->slot;
	uint32_t covered = n->slot + TAG_SIZE;

	put16(t, TAG_MAGIC);
	put16(t + 2, u);
	t[4] = slot;
	t[5] = flags;
	put16(t + 6, gen);
	put16(t + 8, count);
	put16(t + 10, 0);
	put32(t + 12, sqnano_crc32(0, n->buf, n->slot + 12u));
	memset(t + TAG_SIZE, 0xff, n->geo.page_size - covered);
	if (n->geo.soft_ecc) {
		for (uint32_t c = 0; c < ecc_chunks(covered); c++) {
			uint32_t at = c * ECC_CHUNK;
			uint32_t len = covered - at < ECC_CHUNK ? covered - at : ECC_CHUNK;

			put16(t + TAG_SIZE + 2 * c, ecc_calc(n->buf + at, len));
		}
	}
	n->programs++;
	return n->ops->program(n->ctx, (uint32_t)b * ppb(n) + page, n->buf);
}

/*
 * Read a physical page into n->buf and check its trailer. Returns the bit
 * flips corrected (>= 0), -EBADMSG for a page that is not a valid tagged
 * page (erased pages included, then *erased is set), or another error.
 */
static int load(struct sqnano_nand_media *n, uint16_t b, uint32_t page, bool *erased)
{
	uint8_t *t = n->buf + n->slot;
	uint32_t covered = n->slot + TAG_SIZE;
	int flips = n->ops->read(n->ctx, (uint32_t)b * ppb(n) + page, n->buf);

	*erased = false;
	if (flips < 0) {
		return flips;
	}
	if (blank(n->buf, n->geo.page_size)) {
		*erased = true;
		return -EBADMSG;
	}
	if (n->geo.soft_ecc) {
		for (uint32_t c = 0; c < ecc_chunks(covered); c++) {
			uint32_t at = c * ECC_CHUNK;
			uint32_t len = covered - at < ECC_CHUNK ? covered - at : ECC_CHUNK;
			int r = ecc_fix(n->buf + at, len, get16(t + TAG_SIZE + 2 * c));

			if (r < 0) {
				return r;
			}
			flips += r;
		}
	}
	if (get16(t) != TAG_MAGIC || get32(t + 12) != sqnano_crc32(0, n->buf, n->slot + 12u)) {
		return -EBADMSG;
	}
	return flips;
}

/* ---- Blocks ---- */

static void mark_bad(struct sqnano_nand_media *n, uint16_t b)
{
	n->bstate[b] = BS_BAD;
	n->bad_blocks++;
	if (n->ops->mark_bad) {
		/* If this fails, init repeats it (RETIRE) or the block fails again. */
		(void)n->ops->mark_bad(n->ctx, b);
	}
}

static int erase_block(struct sqnano_nand_media *n, uint16_t b)
{
	int rc = n->ops->erase(n->ctx, b);

	n->erases++;
	if (rc == -EIO) {
		mark_bad(n, b);
		return rc;
	}
	if (rc == 0) {
		n->bstate[b] = BS_FREE;
	}
	return rc;
}

/* An erased block, taken in turn so that spare blocks share the wear. */
static int pick(struct sqnano_nand_media *n, uint16_t exclude, uint16_t *out)
{
	for (uint32_t i = 0; i < n->geo.blocks; i++) {
		uint16_t b = (uint16_t)((n->cursor + i) % n->geo.blocks);
		int rc;

		if (b == exclude || (n->bstate[b] != BS_FREE && n->bstate[b] != BS_DIRTY)) {
			continue;
		}
		if (n->bstate[b] == BS_DIRTY) {
			rc = erase_block(n, b);
			if (rc == -EIO) {
				continue;
			}
			if (rc) {
				return rc;
			}
		}
		n->cursor = (uint16_t)((b + 1u) % n->geo.blocks);
		*out = b;
		return 0;
	}
	return -ENOSPC;
}

/* The old block of a unit that moved: bad, or erased for reuse. */
static void dispose(struct sqnano_nand_media *n, uint16_t b, bool retire)
{
	if (retire) {
		mark_bad(n, b);
	} else if (erase_block(n, b) != 0 && n->bstate[b] != BS_BAD) {
		n->bstate[b] = BS_DIRTY;
	}
}

/*
 * Move unit @p u to a fresh block. With @p retire the old block failed and
 * is marked bad, and pages that cannot be read are dropped; otherwise the
 * move is given up when a page cannot be read.
 */
static int relocate(struct sqnano_nand_media *n, uint16_t u, bool retire)
{
	uint8_t *map = map_of(n, u);
	uint8_t pos[255];
	uint16_t old = n->phys[u];
	uint16_t count = 0;
	int rc;

	for (uint16_t s = 0; s < n->spb; s++) {
		count += map[s] != NONE8;
	}
	for (int attempt = 0; attempt < 4; attempt++) {
		uint16_t gen = (uint16_t)(n->gen[u] + 1u + (uint16_t)attempt);
		uint16_t nb;
		uint32_t j = 0;
		bool failed = false;
		bool erased;

		rc = pick(n, old, &nb);
		if (rc) {
			return rc;
		}
		for (uint16_t s = 0; s < n->spb && count && !failed; s++) {
			pos[s] = NONE8;
			if (map[s] == NONE8) {
				continue;
			}
			rc = load(n, old, map[s], &erased);
			if (rc < 0) {
				if (rc != -EBADMSG) {
					return rc;
				}
				n->uncorrectable++;
				if (!retire) {
					n->bstate[nb] = j ? BS_DIRTY : BS_FREE;
					return rc;
				}
				continue;
			}
			rc = prog_at(n, nb, j, u, (uint8_t)s, F_COPY, gen, 0);
			if (rc == -EIO) {
				failed = true;
			} else if (rc) {
				return rc;
			}
			pos[s] = (uint8_t)j++;
		}
		if (!failed && count) {
			/* The copy counts only once this marker is there. */
			memset(n->buf, 0, n->slot);
			rc = prog_at(n, nb, j, u, DONE_SLOT, F_COPY | F_DONE | (retire ? F_RETIRE : 0),
				     gen, (uint16_t)j);
			if (rc == -EIO) {
				failed = true;
			} else if (rc) {
				return rc;
			}
			j++;
		}
		if (failed) {
			mark_bad(n, nb);
			continue;
		}
		for (uint16_t s = 0; s < n->spb; s++) {
			map[s] = count ? pos[s] : NONE8;
		}
		n->phys[u] = nb;
		n->fill[u] = (uint8_t)j;
		n->gen[u] = gen;
		n->bstate[nb] = BS_USED;
		if (old != NONE16) {
			n->relocations++;
			dispose(n, old, retire);
		}
		return 0;
	}
	return -EIO;
}

static int refresh_pending(struct sqnano_nand_media *n)
{
	for (uint16_t u = 0; u < n->units; u++) {
		int rc;

		if (!(n->uflags[u] & UF_REFRESH)) {
			continue;
		}
		n->uflags[u] &= (uint8_t)~UF_REFRESH;
		if (n->phys[u] == NONE16) {
			continue;
		}
		rc = relocate(n, u, false);
		if (rc == 0) {
			n->refreshes++;
		} else if (rc != -EBADMSG && rc != -ENOSPC && rc != -EIO) {
			return rc;
		}
	}
	return 0;
}

/* ---- Init: rebuild the mapping ---- */

struct scan {
	uint16_t unit;
	uint16_t gen;
	uint8_t fill;
	uint8_t cflags;
};

static uint8_t cflags_of(uint8_t f)
{
	return (uint8_t)(((f & F_COPY) ? UC_COPY : 0) | ((f & F_DONE) ? UC_DONE : 0) |
			 ((f & F_RETIRE) ? UC_RETIRE : 0));
}

/* Read block @p b; with @p fill_map, record its pages in the map of its unit. */
static int scan_block(struct sqnano_nand_media *n, uint16_t b, bool fill_map, struct scan *sc)
{
	sc->unit = NONE16;
	sc->gen = 0;
	sc->fill = 0;
	sc->cflags = 0;
	for (uint32_t p = 0; p < ppb(n); p++) {
		uint8_t *t = n->buf + n->slot;
		bool erased;
		int rc = load(n, b, p, &erased);

		if (rc == -EBADMSG) {
			if (!erased) {
				sc->fill = (uint8_t)(p + 1);
			}
			continue;
		}
		if (rc < 0) {
			return rc;
		}
		sc->fill = (uint8_t)(p + 1);
		if (get16(t + 2) >= n->units || (sc->unit != NONE16 && get16(t + 2) != sc->unit) ||
		    (t[4] != DONE_SLOT && t[4] >= n->spb)) {
			continue;
		}
		if (sc->unit == NONE16) {
			sc->unit = get16(t + 2);
			sc->gen = get16(t + 6);
		}
		sc->cflags |= cflags_of(t[5]);
		if (fill_map && t[4] != DONE_SLOT) {
			map_of(n, sc->unit)[t[4]] = (uint8_t)p;
		}
	}
	return 0;
}

static bool complete(uint8_t cf)
{
	return !(cf & UC_COPY) || (cf & UC_DONE);
}

static int nm_init(struct sqnano_media *m)
{
	struct sqnano_nand_media *n = nm_of(m);
	const struct sqnano_nand_geometry *g = &n->geo;
	uint32_t reserve;
	int rc;

	if (!g->page_size || g->page_size > SQNANO_NAND_MAX_PAGE || g->pages_per_block < 4 ||
	    g->pages_per_block > 255 || g->blocks < 4 || g->blocks > SQNANO_NAND_MAX_BLOCKS) {
		return -EINVAL;
	}
	n->slot = 0;
	for (uint32_t s = (g->page_size - TAG_SIZE) & ~15u; s >= 128; s -= 16) {
		uint32_t t = TAG_SIZE + (g->soft_ecc ? 2 * ecc_chunks(s + TAG_SIZE) : 0);

		if (s + t <= g->page_size) {
			n->slot = s;
			break;
		}
	}
	n->spb = (uint16_t)(g->pages_per_block - (g->spare_pages ? g->spare_pages : 2));
	reserve = g->reserved_blocks ? g->reserved_blocks : g->blocks / 50 + 2;
	if (!n->slot || n->spb < 2 || reserve + 2 > g->blocks) {
		return -EINVAL;
	}
	n->units = (uint16_t)(g->blocks - reserve);
	if ((uint32_t)n->units * n->spb > SQNANO_NAND_MAX_SLOTS) {
		n->units = (uint16_t)(SQNANO_NAND_MAX_SLOTS / n->spb);
	}
	n->unit_size = n->spb * n->slot;
	memset(n->phys, 0xff, sizeof(n->phys));
	memset(n->fill, 0, sizeof(n->fill));
	memset(n->gen, 0, sizeof(n->gen));
	memset(n->uflags, 0, sizeof(n->uflags));
	memset(n->map, 0xff, sizeof(n->map));
	n->cursor = 0;
	n->bad_blocks = 0;
	n->factory_bad = 0;

	for (uint16_t b = 0; b < g->blocks; b++) {
		struct scan sc;
		uint16_t u, other;
		bool fresh;

		if (n->ops->is_bad && n->ops->is_bad(n->ctx, b) > 0) {
			n->bstate[b] = BS_BAD;
			n->factory_bad++;
			continue;
		}
		/* The map is filled at once for the first block of a unit, the usual case. */
		rc = scan_block(n, b, true, &sc);
		if (rc) {
			return rc;
		}
		u = sc.unit;
		if (u == NONE16) {
			n->bstate[b] = sc.fill ? BS_DIRTY : BS_FREE;
			continue;
		}
		other = n->phys[u];
		fresh = other == NONE16;
		if (!fresh) {
			/* Two blocks for one unit: a move was interrupted. */
			uint8_t ocf = n->uflags[u];
			bool win = complete(sc.cflags) != complete(ocf) ? complete(sc.cflags)
								       : newer(sc.gen, n->gen[u]);
			uint16_t loser = win ? other : b;
			uint8_t wcf = win ? sc.cflags : ocf;
			uint16_t wgen = win ? sc.gen : n->gen[u];
			uint16_t lgen = win ? n->gen[u] : sc.gen;

			/* The scan above mixed the pages of both blocks in the map. */
			memset(map_of(n, u), 0xff, n->spb);
			if (win) {
				n->phys[u] = b;
				n->gen[u] = sc.gen;
				n->fill[u] = sc.fill;
				n->uflags[u] = sc.cflags;
				n->bstate[b] = BS_USED;
			}
			rc = scan_block(n, n->phys[u], true, &sc);
			if (rc) {
				return rc;
			}
			if ((wcf & UC_RETIRE) && newer(wgen, lgen)) {
				mark_bad(n, loser);
			} else {
				rc = erase_block(n, loser);
				if (rc && rc != -EIO) {
					return rc;
				}
			}
			continue;
		}
		n->phys[u] = b;
		n->gen[u] = sc.gen;
		n->fill[u] = sc.fill;
		n->uflags[u] = sc.cflags;
		n->bstate[b] = BS_USED;
	}
	for (uint16_t u = 0; u < n->units; u++) {
		n->uflags[u] = 0;
	}
	memset(&m->caps, 0, sizeof(m->caps));
	m->size = (uint32_t)n->units * n->unit_size;
	m->caps.write_granularity = n->slot;
	m->caps.erase_size = n->unit_size;
	m->caps.page_hint = n->slot;
	m->caps.endurance_cycles = 100000;
	return 0;
}

/* ---- Media operations ---- */

static int nm_read(const struct sqnano_media *m, uint32_t off, void *buf, size_t len)
{
	struct sqnano_nand_media *n = nm_of(m);
	uint8_t *out = buf;

	if (off > m->size || len > m->size - off) {
		return -EINVAL;
	}
	while (len) {
		uint16_t u = (uint16_t)(off / n->unit_size);
		uint32_t s = off % n->unit_size / n->slot;
		uint32_t in = off % n->slot;
		size_t k = n->slot - in < len ? n->slot - in : len;
		uint8_t p = map_of(n, u)[s];

		if (p == NONE8) {
			memset(out, 0xff, k);
		} else {
			bool erased;
			int rc = load(n, n->phys[u], p, &erased);

			if (rc < 0) {
				if (rc == -EBADMSG) {
					n->uncorrectable++;
					return -EIO;
				}
				return rc;
			}
			if (get16(n->buf + n->slot + 2) != u || n->buf[n->slot + 4] != s) {
				return -EIO;
			}
			if (rc) {
				n->corrected += (uint32_t)rc;
				if ((uint32_t)rc >= (n->geo.refresh_threshold ? n->geo.refresh_threshold
									      : 1u)) {
					n->uflags[u] |= UF_REFRESH;
				}
			}
			memcpy(out, n->buf + in, k);
		}
		off += (uint32_t)k;
		out += k;
		len -= k;
	}
	return 0;
}

static int nm_write(const struct sqnano_media *m, uint32_t off, const void *buf, size_t len)
{
	struct sqnano_nand_media *n = nm_of(m);
	const uint8_t *src = buf;
	int rc;

	if (off > m->size || len > m->size - off || off % n->slot || len % n->slot) {
		return -EINVAL;
	}
	rc = refresh_pending(n);
	if (rc) {
		return rc;
	}
	while (len) {
		uint16_t u = (uint16_t)(off / n->unit_size);
		uint32_t s = off % n->unit_size / n->slot;
		uint8_t *map = map_of(n, u);

		if (map[s] != NONE8) {
			/* Programmed twice without an erase. */
			return -EINVAL;
		}
		for (int tries = 0;; tries++) {
			if (n->phys[u] == NONE16 || n->fill[u] >= ppb(n)) {
				rc = relocate(n, u, false);
				if (rc) {
					return rc;
				}
			}
			memcpy(n->buf, src, n->slot);
			rc = prog_at(n, n->phys[u], n->fill[u], u, (uint8_t)s, 0, n->gen[u], 0);
			n->fill[u]++;
			if (rc == 0) {
				map[s] = (uint8_t)(n->fill[u] - 1u);
				break;
			}
			if (rc != -EIO || tries >= 3) {
				return rc;
			}
			/* The block failed: move what it holds and mark it bad. */
			rc = relocate(n, u, true);
			if (rc) {
				return rc;
			}
		}
		off += n->slot;
		src += n->slot;
		len -= n->slot;
	}
	return 0;
}

static int nm_erase(const struct sqnano_media *m, uint32_t off, size_t len)
{
	struct sqnano_nand_media *n = nm_of(m);
	int rc;

	if (off > m->size || len > m->size - off || off % n->unit_size || len % n->unit_size) {
		return -EINVAL;
	}
	for (uint16_t u = (uint16_t)(off / n->unit_size); len; u++, len -= n->unit_size) {
		uint16_t b = n->phys[u];

		n->uflags[u] &= (uint8_t)~UF_REFRESH;
		if (b == NONE16) {
			continue;
		}
		if (n->fill[u] == 0) {
			/* Nothing programmed since the last erase. */
			n->bstate[b] = BS_FREE;
			n->phys[u] = NONE16;
			continue;
		}
		rc = erase_block(n, b);
		if (rc && rc != -EIO) {
			return rc;
		}
		/* Erased (or bad): the block joins the pool, the unit gets one when written. */
		memset(map_of(n, u), 0xff, n->spb);
		n->fill[u] = 0;
		n->phys[u] = NONE16;
	}
	return refresh_pending(n);
}

/*
 * Pages freed for good: forget them. A unit left without pages gives its
 * block back to the pool, to be erased when taken; until then the block may
 * be found again at init, with content the engine no longer uses.
 */
static int nm_discard(const struct sqnano_media *m, uint32_t off, size_t len)
{
	struct sqnano_nand_media *n = nm_of(m);

	if (off > m->size || len > m->size - off) {
		return -EINVAL;
	}
	for (uint32_t o = (off + n->slot - 1) / n->slot * n->slot; o + n->slot <= off + len;
	     o += n->slot) {
		uint16_t u = (uint16_t)(o / n->unit_size);
		uint8_t *map = map_of(n, u);
		bool empty = true;

		map[o % n->unit_size / n->slot] = NONE8;
		for (uint16_t s = 0; s < n->spb && empty; s++) {
			empty = map[s] == NONE8;
		}
		if (empty && n->phys[u] != NONE16) {
			n->bstate[n->phys[u]] = n->fill[u] ? BS_DIRTY : BS_FREE;
			n->phys[u] = NONE16;
			n->fill[u] = 0;
			n->uflags[u] &= (uint8_t)~UF_REFRESH;
		}
	}
	return 0;
}

static int nm_sync(const struct sqnano_media *m)
{
	return refresh_pending(nm_of(m));
}

static const struct sqnano_media_api nand_api = {
	.init = nm_init,
	.read = nm_read,
	.write = nm_write,
	.erase = nm_erase,
	.sync = nm_sync,
	.discard = nm_discard,
};

void sqnano_nand_media_init(struct sqnano_nand_media *n, const struct sqnano_nand_ops *ops,
			    void *ctx, const struct sqnano_nand_geometry *geo)
{
	memset(n, 0, sizeof(*n));
	n->media.api = &nand_api;
	n->media.ctx = n;
	n->ops = ops;
	n->ctx = ctx;
	n->geo = *geo;
}
