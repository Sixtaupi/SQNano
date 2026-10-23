/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * RAM media simulating FRAM or NOR flash, with power-cut injection.
 *
 * A cut interrupts a write or an erase after a random number of bytes. On
 * NOR the interrupted byte may also be partially programmed (some bits
 * cleared), as happens on real flash.
 */

#include <errno.h>
#include <string.h>

#include <sqnano/media.h>

/* Power state and cut countdown: the group leader's, so media cut together. */
static struct sqnano_ram_media *ctl(struct sqnano_ram_media *r)
{
	return r->group ? r->group : r;
}

static uint32_t ram_rand(struct sqnano_ram_media *r)
{
	/* xorshift32 */
	uint32_t x;

	r = ctl(r);
	x = r->rng ? r->rng : 0x2545f491u;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	r->rng = x;
	return x;
}

static struct sqnano_ram_media *ram_of(const struct sqnano_media *m)
{
	return (struct sqnano_ram_media *)m->ctx;
}

static int ram_init(struct sqnano_media *m)
{
	struct sqnano_ram_media *r = ram_of(m);

	m->size = r->mem_size;
	memset(&m->caps, 0, sizeof(m->caps));
	if (r->kind == SQNANO_RAM_NOR) {
		m->caps.write_granularity = r->write_granularity;
		m->caps.erase_size = r->erase_size;
		m->caps.endurance_cycles = 100000;
	} else {
		m->caps.write_granularity = 1;
		m->caps.endurance_cycles = 0xffffffffu;
	}
	return 0;
}

static int ram_read(const struct sqnano_media *m, uint32_t off, void *buf, size_t len)
{
	struct sqnano_ram_media *r = ram_of(m);

	if (ctl(r)->powered_off) {
		return -EIO;
	}
	if (off > r->mem_size || len > r->mem_size - off) {
		return -EINVAL;
	}
	memcpy(buf, r->mem + off, len);
	return 0;
}

/* Returns true when the current operation must be cut. */
static bool ram_cut_now(struct sqnano_ram_media *r)
{
	r->ops++;
	r = ctl(r);
	r->gops++;
	if (r->cut_countdown < 0) {
		return false;
	}
	if (r->cut_countdown == 0) {
		r->powered_off = true;
		r->cut_countdown = -1;
		return true;
	}
	r->cut_countdown--;
	return false;
}

static void ram_program(struct sqnano_ram_media *r, uint32_t off, const uint8_t *src, size_t len)
{
	if (r->kind == SQNANO_RAM_NOR) {
		for (size_t i = 0; i < len; i++) {
			if (r->mem[off + i] != 0xff) {
				/* Programming over data that was not erased (forbidden with ECC). */
				r->overwrites++;
			}
			r->mem[off + i] &= src[i];
		}
	} else {
		memcpy(r->mem + off, src, len);
	}
}

static int ram_write(const struct sqnano_media *m, uint32_t off, const void *buf, size_t len)
{
	struct sqnano_ram_media *r = ram_of(m);
	const uint8_t *src = buf;

	if (ctl(r)->powered_off) {
		return -EIO;
	}
	if (off > r->mem_size || len > r->mem_size - off) {
		return -EINVAL;
	}
	if (r->kind == SQNANO_RAM_NOR &&
	    ((off % r->write_granularity) != 0 || (len % r->write_granularity) != 0)) {
		return -EINVAL;
	}
	if (ram_cut_now(r)) {
		size_t done = len ? ram_rand(r) % len : 0;

		ram_program(r, off, src, done);
		if (done < len) {
			/* The byte being written ends up in an undefined state. */
			uint8_t junk = (uint8_t)ram_rand(r);

			if (r->kind == SQNANO_RAM_NOR) {
				r->mem[off + done] &= (uint8_t)(src[done] | junk);
			} else {
				r->mem[off + done] = junk;
			}
		}
		return -EIO;
	}
	ram_program(r, off, src, len);
	return 0;
}

static int ram_erase(const struct sqnano_media *m, uint32_t off, size_t len)
{
	struct sqnano_ram_media *r = ram_of(m);

	if (ctl(r)->powered_off) {
		return -EIO;
	}
	if (r->kind != SQNANO_RAM_NOR) {
		return -ENOTSUP;
	}
	if ((off % r->erase_size) != 0 || (len % r->erase_size) != 0 || off > r->mem_size ||
	    len > r->mem_size - off) {
		return -EINVAL;
	}
	r->erases++;
	if (ram_cut_now(r)) {
		size_t done = len ? ram_rand(r) % len : 0;

		memset(r->mem + off, 0xff, done);
		return -EIO;
	}
	memset(r->mem + off, 0xff, len);
	return 0;
}

static const struct sqnano_media_api ram_api = {
	.init = ram_init,
	.read = ram_read,
	.write = ram_write,
	.erase = ram_erase,
};

void sqnano_ram_media_init(struct sqnano_ram_media *r, uint8_t *mem, uint32_t size,
			   enum sqnano_ram_kind kind, uint32_t erase_size)
{
	memset(r, 0, sizeof(*r));
	r->media.api = &ram_api;
	r->media.ctx = r;
	r->mem = mem;
	r->mem_size = size;
	r->kind = kind;
	r->write_granularity = kind == SQNANO_RAM_NOR ? 4 : 1;
	r->erase_size = kind == SQNANO_RAM_NOR ? erase_size : 0;
	r->cut_countdown = -1;
	r->rng = 0x12345678u;
}

void sqnano_ram_media_blank(struct sqnano_ram_media *r)
{
	memset(r->mem, r->kind == SQNANO_RAM_NOR ? 0xff : 0x00, r->mem_size);
}

void sqnano_ram_media_cut_after(struct sqnano_ram_media *r, uint32_t ops, uint32_t seed)
{
	r->cut_countdown = (int32_t)ops;
	r->rng = seed | 1u;
	r->powered_off = false;
}

void sqnano_ram_media_power_on(struct sqnano_ram_media *r)
{
	r->powered_off = false;
	r->cut_countdown = -1;
}
