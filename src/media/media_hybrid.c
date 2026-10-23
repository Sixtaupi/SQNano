/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Hybrid media: a NOR flash with a FRAM in front of it.
 *
 * FRAM layout:
 *   [0, 16)           header: magic, block size, slot and erase counts, CRC
 *   [16, 16 + 16 E)   erase records: magic, unit, seq, CRC
 *   [..., ...)        slots: header (magic, block, seq, CRC) then the block
 *
 * Every operation gets a sequence number. The content of block b is the
 * valid slot of b if any; otherwise 0xFF if its unit has an erase record;
 * otherwise the flash. Entries are freed by clearing their magic and filled
 * magic last. A write stages the block in a free slot (data first, header
 * last) and then frees the slot of the previous version; an erase
 * writes a new erase record, then frees the older record and the slots of
 * the unit. A power cut between the two steps leaves both versions valid,
 * and the mount keeps the one with the highest sequence number.
 *
 * When fewer than (blocks per unit + 1) slots are free, the oldest slot is
 * evicted: its unit is physically erased first if it has an erase record,
 * the block is programmed, then the slot is freed. A block found neither
 * erased nor equal on the flash (an eviction cut halfway) makes its unit be
 * refreshed: its other flash blocks are staged, then the unit is erased.
 */

#include <errno.h>
#include <string.h>

#include <sqnano/media.h>

#define HDR_MAGIC  0x59485153u /* "SQHY" */
#define SLOT_MAGIC 0x534cu
#define ER_MAGIC   0x4552u
#define ENTRY      16u

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n)
{
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

static struct sqnano_hybrid_media *hm_of(const struct sqnano_media *m)
{
	return (struct sqnano_hybrid_media *)m->ctx;
}

static int fram_read(struct sqnano_hybrid_media *h, uint32_t off, void *buf, size_t len)
{
	return h->fram->api->read(h->fram, off, buf, len);
}

static int fram_write(struct sqnano_hybrid_media *h, uint32_t off, const void *buf, size_t len)
{
	return h->fram->api->write(h->fram, off, buf, len);
}

static uint32_t er_off(uint32_t i)
{
	return ENTRY + i * ENTRY;
}

static uint32_t slot_hdr(const struct sqnano_hybrid_media *h, uint32_t i)
{
	return h->slot_off + i * (ENTRY + h->blk);
}

static int invalidate(struct sqnano_hybrid_media *h, uint32_t off)
{
	static const uint8_t zero[2];

	return fram_write(h, off, zero, sizeof(zero));
}

/*
 * Write an entry over a free one: body first, magic last. A cut before the
 * magic leaves the entry free; written at once, a cut right after the magic
 * would revive the former content of the entry with its valid CRC.
 */
static int entry_write(struct sqnano_hybrid_media *h, uint32_t off, const uint8_t *e)
{
	int err = fram_write(h, off + 2, e + 2, ENTRY - 2);

	return err ? err : fram_write(h, off, e, 2);
}

static int find_slot(const struct sqnano_hybrid_media *h, uint32_t block)
{
	int best = -1;

	for (uint32_t i = 0; i < h->nslots; i++) {
		if (h->slot[i].seq && h->slot[i].block == block &&
		    (best < 0 || h->slot[i].seq > h->slot[best].seq)) {
			best = (int)i;
		}
	}
	return best;
}

static int find_er(const struct sqnano_hybrid_media *h, uint32_t unit)
{
	for (uint32_t i = 0; i < h->nerase; i++) {
		if (h->er[i].seq && h->er[i].unit == unit) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t free_slots(const struct sqnano_hybrid_media *h)
{
	uint32_t n = 0;

	for (uint32_t i = 0; i < h->nslots; i++) {
		n += h->slot[i].seq == 0;
	}
	return n;
}

static bool blank(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (p[i] != 0xff) {
			return false;
		}
	}
	return true;
}

/* Logical content of [off, off + len) inside one block. */
static int block_read(struct sqnano_hybrid_media *h, uint32_t block, uint32_t in, uint8_t *out,
		      uint32_t len)
{
	int s = find_slot(h, block);

	if (s >= 0) {
		return fram_read(h, slot_hdr(h, (uint32_t)s) + ENTRY + in, out, len);
	}
	if (find_er(h, block * h->blk / h->esize) >= 0) {
		memset(out, 0xff, len);
		return 0;
	}
	return h->flash->api->read(h->flash, block * h->blk + in, out, len);
}

static int evict_one(struct sqnano_hybrid_media *h);

/* Keep a free slot for a write plus one per block of a unit, for a refresh. */
static int make_room(struct sqnano_hybrid_media *h)
{
	int err = 0;

	while (!err && free_slots(h) < h->esize / h->blk + 1) {
		err = evict_one(h);
	}
	return err;
}

/* Write @p data (h->blk bytes) as the new version of @p block; no eviction. */
static int stage(struct sqnano_hybrid_media *h, uint32_t block, const uint8_t *data)
{
	uint8_t hdr[ENTRY] = {0};
	int old = find_slot(h, block);
	int i = -1;
	int err;

	for (uint32_t k = 0; k < h->nslots && i < 0; k++) {
		if (h->slot[k].seq == 0) {
			i = (int)k;
		}
	}
	if (i < 0) {
		return -ENOSPC;
	}
	hdr[0] = (uint8_t)SLOT_MAGIC;
	hdr[1] = (uint8_t)(SLOT_MAGIC >> 8);
	put32(hdr + 4, block);
	put32(hdr + 8, h->gen);
	put32(hdr + 12, crc32(crc32(0, hdr + 4, 8), data, h->blk));
	err = fram_write(h, slot_hdr(h, (uint32_t)i) + ENTRY, data, h->blk);
	if (!err) {
		err = entry_write(h, slot_hdr(h, (uint32_t)i), hdr);
	}
	if (err) {
		return err;
	}
	h->slot[i].block = block;
	h->slot[i].seq = h->gen++;
	if (old >= 0) {
		err = invalidate(h, slot_hdr(h, (uint32_t)old));
		h->slot[old].seq = 0;
	}
	return err;
}

/* Erase the flash unit of record @p r, then drop the record. */
static int physical_erase(struct sqnano_hybrid_media *h, int r)
{
	int err = h->flash->api->erase(h->flash, h->er[r].unit * h->esize, h->esize);

	h->flash_erases++;
	if (!err) {
		err = invalidate(h, er_off((uint32_t)r));
		h->er[r].seq = 0;
	}
	return err;
}

/* Stage the flash blocks of @p unit that hold data, then erase it. */
static int refresh(struct sqnano_hybrid_media *h, uint32_t unit)
{
	uint32_t ppe = h->esize / h->blk;
	int err = 0;

	h->refreshes++;
	for (uint32_t c = unit * ppe; c < (unit + 1) * ppe && !err; c++) {
		if (find_slot(h, c) >= 0) {
			continue;
		}
		err = h->flash->api->read(h->flash, c * h->blk, h->tmp, h->blk);
		if (!err && !blank(h->tmp, h->blk)) {
			err = stage(h, c, h->tmp);
		}
	}
	if (!err) {
		err = h->flash->api->erase(h->flash, unit * h->esize, h->esize);
		h->flash_erases++;
	}
	return err;
}

/*
 * Can the flash block @p cur become @p want by programming? Each program
 * unit must be equal or still erased: blocks of the superblock log receive
 * records one after the other without erase, as on the flash itself.
 */
static bool programmable(const struct sqnano_hybrid_media *h, const uint8_t *cur,
			 const uint8_t *want)
{
	uint32_t g = h->flash->caps.write_granularity;

	for (uint32_t o = 0; o < h->blk; o += g) {
		if (memcmp(cur + o, want + o, g) != 0 && !blank(cur + o, g)) {
			return false;
		}
	}
	return true;
}

/* Program the runs of program units that differ. */
static int program_diff(struct sqnano_hybrid_media *h, uint32_t block, const uint8_t *cur,
			const uint8_t *want)
{
	uint32_t g = h->flash->caps.write_granularity;
	uint32_t o = 0;

	while (o < h->blk) {
		uint32_t end;
		int err;

		if (memcmp(cur + o, want + o, g) == 0) {
			o += g;
			continue;
		}
		end = o + g;
		while (end < h->blk && memcmp(cur + end, want + end, g) != 0) {
			end += g;
		}
		err = h->flash->api->write(h->flash, block * h->blk + o, want + o, end - o);
		h->flash_programs++;
		if (err) {
			return err;
		}
		o = end;
	}
	return 0;
}

static int evict_one(struct sqnano_hybrid_media *h)
{
	int v = -1;
	uint32_t block, unit;
	int r;
	int err;

	for (uint32_t i = 0; i < h->nslots; i++) {
		if (h->slot[i].seq && (v < 0 || h->slot[i].seq < h->slot[v].seq)) {
			v = (int)i;
		}
	}
	if (v < 0) {
		return -ENOSPC;
	}
	block = h->slot[v].block;
	unit = block * h->blk / h->esize;
	h->evictions++;
	r = find_er(h, unit);
	if (r >= 0) {
		err = physical_erase(h, r);
		if (err) {
			return err;
		}
	}
	err = fram_read(h, slot_hdr(h, (uint32_t)v) + ENTRY, h->buf, h->blk);
	if (!err) {
		err = h->flash->api->read(h->flash, block * h->blk, h->tmp, h->blk);
	}
	if (err) {
		return err;
	}
	if (!programmable(h, h->tmp, h->buf)) {
		/* Programmed halfway before a power cut: erase the unit first. */
		err = refresh(h, unit);
		if (err) {
			return err;
		}
		memset(h->tmp, 0xff, h->blk);
	}
	err = program_diff(h, block, h->tmp, h->buf);
	if (err) {
		return err;
	}
	err = invalidate(h, slot_hdr(h, (uint32_t)v));
	h->slot[v].seq = 0;
	return err;
}

static int hm_erase(const struct sqnano_media *m, uint32_t off, size_t len)
{
	struct sqnano_hybrid_media *h = hm_of(m);
	uint32_t ppe = h->esize / h->blk;

	if (off % h->esize || len % h->esize || off > m->size || len > m->size - off) {
		return -EINVAL;
	}
	for (uint32_t u = off / h->esize; u < (off + len) / h->esize; u++) {
		uint8_t rec[ENTRY] = {0};
		int old = find_er(h, u);
		int i = -1;
		int err;

		for (uint32_t k = 0; k < h->nerase && i < 0; k++) {
			if (h->er[k].seq == 0) {
				i = (int)k;
			}
		}
		if (i < 0) {
			/* Table full: really erase the oldest recorded unit. */
			int o = 0;

			for (uint32_t k = 1; k < h->nerase; k++) {
				o = h->er[k].seq < h->er[o].seq ? (int)k : o;
			}
			err = physical_erase(h, o);
			if (err) {
				return err;
			}
			i = o;
			old = find_er(h, u);
		}
		rec[0] = (uint8_t)ER_MAGIC;
		rec[1] = (uint8_t)(ER_MAGIC >> 8);
		put32(rec + 4, u);
		put32(rec + 8, h->gen);
		put32(rec + 12, crc32(0, rec + 4, 8));
		err = entry_write(h, er_off((uint32_t)i), rec);
		if (err) {
			return err;
		}
		h->er[i].unit = u;
		h->er[i].seq = h->gen++;
		h->virtual_erases++;
		if (old >= 0) {
			err = invalidate(h, er_off((uint32_t)old));
			h->er[old].seq = 0;
		}
		for (uint32_t k = 0; k < h->nslots && !err; k++) {
			if (h->slot[k].seq && h->slot[k].block / ppe == u) {
				err = invalidate(h, slot_hdr(h, k));
				h->slot[k].seq = 0;
			}
		}
		if (err) {
			return err;
		}
	}
	return 0;
}

static int hm_read(const struct sqnano_media *m, uint32_t off, void *buf, size_t len)
{
	struct sqnano_hybrid_media *h = hm_of(m);
	uint8_t *out = buf;

	if (off > m->size || len > m->size - off) {
		return -EINVAL;
	}
	while (len) {
		uint32_t in = off % h->blk;
		uint32_t n = h->blk - in < len ? h->blk - in : (uint32_t)len;
		int err = block_read(h, off / h->blk, in, out, n);

		if (err) {
			return err;
		}
		off += n;
		out += n;
		len -= n;
	}
	return 0;
}

static int hm_write(const struct sqnano_media *m, uint32_t off, const void *buf, size_t len)
{
	struct sqnano_hybrid_media *h = hm_of(m);
	const uint8_t *src = buf;

	if (off > m->size || len > m->size - off) {
		return -EINVAL;
	}
	if (off % m->caps.write_granularity || len % m->caps.write_granularity) {
		return -EINVAL;
	}
	while (len) {
		uint32_t block = off / h->blk;
		uint32_t in = off % h->blk;
		uint32_t n = h->blk - in < len ? h->blk - in : (uint32_t)len;
		/* Evictions use h->buf: make room before reading the block into it. */
		int err = make_room(h);

		if (!err) {
			err = block_read(h, block, 0, h->buf, h->blk);
		}
		if (err) {
			return err;
		}
		/* NOR programming only clears bits. */
		for (uint32_t k = 0; k < n; k++) {
			h->buf[in + k] &= src[k];
		}
		err = stage(h, block, h->buf);
		if (err) {
			return err;
		}
		off += n;
		src += n;
		len -= n;
	}
	return 0;
}

static int format_fram(struct sqnano_hybrid_media *h, const uint8_t *hdr)
{
	int err = 0;

	for (uint32_t i = 0; i < h->nerase && !err; i++) {
		err = invalidate(h, er_off(i));
	}
	for (uint32_t i = 0; i < h->nslots && !err; i++) {
		err = invalidate(h, slot_hdr(h, i));
	}
	/* Header last: a cut before it formats again. */
	return err ? err : fram_write(h, 0, hdr, ENTRY);
}

/* Load the FRAM tables and settle what a power cut left. */
static int recover(struct sqnano_hybrid_media *h)
{
	uint32_t ppe = h->esize / h->blk;
	uint8_t e[ENTRY];
	int err = 0;

	h->gen = 1;
	for (uint32_t i = 0; i < h->nerase && !err; i++) {
		h->er[i].seq = 0;
		err = fram_read(h, er_off(i), e, ENTRY);
		if (!err && (e[0] | e[1] << 8) == ER_MAGIC && get32(e + 12) == crc32(0, e + 4, 8) &&
		    get32(e + 8) != 0) {
			h->er[i].unit = get32(e + 4);
			h->er[i].seq = get32(e + 8);
		}
	}
	for (uint32_t i = 0; i < h->nslots && !err; i++) {
		h->slot[i].seq = 0;
		err = fram_read(h, slot_hdr(h, i), e, ENTRY);
		if (err || (e[0] | e[1] << 8) != SLOT_MAGIC || get32(e + 8) == 0) {
			continue;
		}
		err = fram_read(h, slot_hdr(h, i) + ENTRY, h->buf, h->blk);
		if (!err && get32(e + 12) == crc32(crc32(0, e + 4, 8), h->buf, h->blk)) {
			h->slot[i].block = get32(e + 4);
			h->slot[i].seq = get32(e + 8);
		}
	}
	/* Older erase records of a unit, older versions of a block. */
	for (uint32_t i = 0; i < h->nerase && !err; i++) {
		for (uint32_t k = i + 1; h->er[i].seq && k < h->nerase && !err; k++) {
			if (h->er[k].seq && h->er[k].unit == h->er[i].unit) {
				uint32_t drop = h->er[k].seq < h->er[i].seq ? k : i;

				err = invalidate(h, er_off(drop));
				h->er[drop].seq = 0;
			}
		}
	}
	for (uint32_t i = 0; i < h->nslots && !err; i++) {
		int r;

		if (!h->slot[i].seq) {
			continue;
		}
		r = find_er(h, h->slot[i].block / ppe);
		if ((r >= 0 && h->er[r].seq > h->slot[i].seq) ||
		    find_slot(h, h->slot[i].block) != (int)i) {
			err = invalidate(h, slot_hdr(h, i));
			h->slot[i].seq = 0;
		}
	}
	for (uint32_t i = 0; i < h->nerase; i++) {
		h->gen = h->er[i].seq >= h->gen ? h->er[i].seq + 1 : h->gen;
	}
	for (uint32_t i = 0; i < h->nslots; i++) {
		h->gen = h->slot[i].seq >= h->gen ? h->slot[i].seq + 1 : h->gen;
	}
	/* Room for a refresh, as during operation. */
	return err ? err : make_room(h);
}

static int hm_init(struct sqnano_media *m)
{
	struct sqnano_hybrid_media *h = hm_of(m);
	uint8_t hdr[ENTRY], want[ENTRY] = {0};
	uint32_t room;
	int err;

	err = h->flash->api->init ? h->flash->api->init(h->flash) : 0;
	if (!err && h->fram->api->init) {
		err = h->fram->api->init(h->fram);
	}
	if (err) {
		return err;
	}
	h->esize = h->flash->caps.erase_size;
	if (h->blk == 0) {
		/* The default page of the engine: the erase unit, within the limit. */
		h->blk = h->esize < SQNANO_HYBRID_MAX_BLOCK ? h->esize : SQNANO_HYBRID_MAX_BLOCK;
	}
	if (!h->esize || !h->blk || h->blk > SQNANO_HYBRID_MAX_BLOCK || h->esize % h->blk ||
	    h->blk % h->flash->caps.write_granularity) {
		return -EINVAL;
	}
	m->caps = h->flash->caps;
	m->size = h->flash->size;
	h->nerase = m->size / h->esize;
	h->nerase = h->nerase > SQNANO_HYBRID_MAX_ERASES ? SQNANO_HYBRID_MAX_ERASES : h->nerase;
	h->slot_off = ENTRY + h->nerase * ENTRY;
	room = h->fram->size > h->slot_off ? h->fram->size - h->slot_off : 0;
	h->nslots = room / (ENTRY + h->blk);
	h->nslots = h->nslots > SQNANO_HYBRID_MAX_SLOTS ? SQNANO_HYBRID_MAX_SLOTS : h->nslots;
	if (h->nslots < 2 * (h->esize / h->blk) + 2) {
		return -ENOSPC;
	}
	put32(want, HDR_MAGIC);
	put32(want + 4, h->blk);
	put32(want + 8, h->nslots | h->nerase << 16);
	put32(want + 12, crc32(0, want, 12));
	err = fram_read(h, 0, hdr, ENTRY);
	if (err) {
		return err;
	}
	if (get32(hdr) != HDR_MAGIC || get32(hdr + 12) != crc32(0, hdr, 12)) {
		err = format_fram(h, want);
	} else if (memcmp(hdr, want, ENTRY) != 0) {
		/* Another geometry: its staged blocks cannot be read back. */
		return -EINVAL;
	}
	return err ? err : recover(h);
}

/* Drop the FRAM copies of whole blocks: they read back from the flash. */
static int hm_discard(const struct sqnano_media *m, uint32_t off, size_t len)
{
	struct sqnano_hybrid_media *h = hm_of(m);
	int err = 0;

	for (uint32_t b = (off + h->blk - 1) / h->blk; (b + 1) * h->blk <= off + len && !err; b++) {
		int s = find_slot(h, b);

		if (s >= 0) {
			err = invalidate(h, slot_hdr(h, (uint32_t)s));
			h->slot[s].seq = 0;
			h->discards++;
		}
	}
	return err;
}

static const struct sqnano_media_api hybrid_api = {
	.init = hm_init,
	.read = hm_read,
	.write = hm_write,
	.erase = hm_erase,
	.discard = hm_discard,
};

void sqnano_hybrid_media_init(struct sqnano_hybrid_media *h, struct sqnano_media *flash,
			      struct sqnano_media *fram, uint32_t block)
{
	memset(h, 0, sizeof(*h));
	h->media.api = &hybrid_api;
	h->media.ctx = h;
	h->flash = flash;
	h->fram = fram;
	h->blk = block;
}
