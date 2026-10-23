/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * File system media for Zephyr: chunk files "<dir>/<index>" through the fs
 * API. LittleFS commits a file when it is closed, so rewriting a chunk in
 * place (same length) is atomic on power loss.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>

#include <sqnano/zephyr.h>

static void chunk_path(const struct sqnano_fs_media *m, uint32_t index, char *out, size_t n)
{
	(void)snprintf(out, n, "%s/%u", m->dir, (unsigned int)index);
}

static int fsm_read(void *ctx, uint32_t index, uint32_t off, void *buf, size_t len)
{
	const struct sqnano_fs_media *m = ctx;
	struct fs_file_t f;
	char path[CONFIG_SQNANO_FS_PATH_MAX];
	ssize_t got = 0;
	int err;

	memset(buf, 0, len);
	chunk_path(m, index, path, sizeof(path));
	fs_file_t_init(&f);
	err = fs_open(&f, path, FS_O_READ);
	if (err == -ENOENT) {
		return 0;
	}
	if (err) {
		return err;
	}
	err = fs_seek(&f, off, FS_SEEK_SET);
	if (err == 0) {
		got = fs_read(&f, buf, len);
	}
	(void)fs_close(&f);
	return err ? err : got < 0 ? (int)got : 0;
}

static int fsm_write(void *ctx, uint32_t index, const void *buf, size_t len)
{
	const struct sqnano_fs_media *m = ctx;
	struct fs_file_t f;
	char path[CONFIG_SQNANO_FS_PATH_MAX];
	ssize_t put;
	int err;

	chunk_path(m, index, path, sizeof(path));
	fs_file_t_init(&f);
	err = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE);
	if (err == -ENOENT) {
		/* First write of the database: create its directory. */
		err = fs_mkdir(m->dir);
		if (err == 0 || err == -EEXIST) {
			err = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE);
		}
	}
	if (err) {
		return err;
	}
	/* Every chunk file has the same length: no truncation needed. */
	put = fs_write(&f, buf, len);
	err = fs_close(&f);
	if (put != (ssize_t)len) {
		return put < 0 ? (int)put : -ENOSPC;
	}
	return err;
}

static const struct sqnano_file_ops fs_ops = {.read = fsm_read, .write = fsm_write};

void sqnano_fs_media_init(struct sqnano_fs_media *m, const char *dir, uint32_t size,
			  uint32_t chunk, uint8_t *cache)
{
	m->dir = dir;
	sqnano_file_media_init(&m->fm, &fs_ops, m, size, chunk, cache);
}
