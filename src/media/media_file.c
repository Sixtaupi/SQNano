/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * File media: chunk files on a power-safe file system, one chunk cached in
 * RAM with write-back. Writes reach the file system when another chunk is
 * needed or at a sync barrier, in the order they were made per chunk; the
 * engine syncs before and after each commit record, which is all shadow
 * paging needs: the pages of a commit are durable before its superblock,
 * and the superblock before the commit returns.
 */

#include <errno.h>
#include <string.h>

#include <sqnano/media.h>

#define NONE UINT32_MAX

static struct sqnano_file_media *fm_of(const struct sqnano_media *m)
{
	return (struct sqnano_file_media *)m->ctx;
}

static int fm_flush(struct sqnano_file_media *f)
{
	int err;

	if (!f->dirty) {
		return 0;
	}
	err = f->ops->write(f->ctx, f->cached, f->cache, f->chunk);
	f->writes++;
	if (err) {
		/* The chunk may or may not have been replaced: forget it. */
		f->cached = NONE;
		f->dirty = false;
		return err;
	}
	f->dirty = false;
	return 0;
}

static int fm_load(struct sqnano_file_media *f, uint32_t index)
{
	int err;

	if (f->cached == index) {
		return 0;
	}
	err = fm_flush(f);
	if (err) {
		return err;
	}
	err = f->ops->read(f->ctx, index, 0, f->cache, f->chunk);
	f->cached = err ? NONE : index;
	return err;
}

static int fm_init(struct sqnano_media *m)
{
	struct sqnano_file_media *f = fm_of(m);

	if (f->chunk == 0 || f->size % f->chunk) {
		return -EINVAL;
	}
	memset(&m->caps, 0, sizeof(m->caps));
	m->size = f->size;
	m->caps.write_granularity = 1;
	m->caps.endurance_cycles = 0xffffffffu;
	m->caps.prefer_shadow = true;
	f->cached = NONE;
	f->dirty = false;
	return 0;
}

static int fm_read(const struct sqnano_media *m, uint32_t off, void *buf, size_t len)
{
	struct sqnano_file_media *f = fm_of(m);
	uint8_t *out = buf;

	if (off > f->size || len > f->size - off) {
		return -EINVAL;
	}
	while (len) {
		uint32_t index = off / f->chunk;
		uint32_t in = off % f->chunk;
		size_t n = f->chunk - in < len ? f->chunk - in : len;
		int err;

		if (f->cached == index) {
			memcpy(out, f->cache + in, n);
		} else {
			err = f->ops->read(f->ctx, index, in, out, n);
			if (err) {
				return err;
			}
		}
		off += (uint32_t)n;
		out += n;
		len -= n;
	}
	return 0;
}

static int fm_write(const struct sqnano_media *m, uint32_t off, const void *buf, size_t len)
{
	struct sqnano_file_media *f = fm_of(m);
	const uint8_t *src = buf;

	if (off > f->size || len > f->size - off) {
		return -EINVAL;
	}
	while (len) {
		uint32_t index = off / f->chunk;
		uint32_t in = off % f->chunk;
		size_t n = f->chunk - in < len ? f->chunk - in : len;
		int err = fm_load(f, index);

		if (err) {
			return err;
		}
		memcpy(f->cache + in, src, n);
		f->dirty = true;
		off += (uint32_t)n;
		src += n;
		len -= n;
	}
	return 0;
}

static int fm_erase(const struct sqnano_media *m, uint32_t off, size_t len)
{
	(void)m;
	(void)off;
	(void)len;
	return -ENOTSUP;
}

static int fm_sync(const struct sqnano_media *m)
{
	return fm_flush(fm_of(m));
}

static const struct sqnano_media_api file_api = {
	.init = fm_init,
	.read = fm_read,
	.write = fm_write,
	.erase = fm_erase,
	.sync = fm_sync,
};

void sqnano_file_media_init(struct sqnano_file_media *m, const struct sqnano_file_ops *ops,
			    void *ctx, uint32_t size, uint32_t chunk, uint8_t *cache)
{
	memset(m, 0, sizeof(*m));
	m->media.api = &file_api;
	m->media.ctx = m;
	m->ops = ops;
	m->ctx = ctx;
	m->size = size;
	m->chunk = chunk;
	m->cache = cache;
	m->cached = NONE;
}
