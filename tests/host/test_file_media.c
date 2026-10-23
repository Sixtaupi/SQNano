/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * File media: chunk files with atomic replacement, as on LittleFS.
 *
 * A simulated file system cuts the power during every file update (the
 * update happens or not, then everything fails), and the database must
 * remount to the state before or after the last commit. Then the same
 * database runs on real files through POSIX, written to a temporary file
 * and renamed into place.
 */

#define _XOPEN_SOURCE 700 /* mkdtemp */
#define _DARWIN_C_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kv.h"

#define CHUNK  2048
#define NCHUNK 96
#define TXNS   12

/* ---- Simulated file system ---- */

struct sim_fs {
	struct {
		uint8_t data[CHUNK];
		uint32_t len;
	} f[NCHUNK];
	int32_t cut;
	uint32_t rng;
	bool off;
	uint32_t updates;
};

static struct sim_fs fs, fs_image;

static int sim_read(void *ctx, uint32_t index, uint32_t off, void *buf, size_t len)
{
	struct sim_fs *s = ctx;
	uint8_t *out = buf;

	if (s->off) {
		return -EIO;
	}
	if (index >= NCHUNK) {
		return -EINVAL;
	}
	for (size_t i = 0; i < len; i++) {
		out[i] = off + i < s->f[index].len ? s->f[index].data[off + i] : 0;
	}
	return 0;
}

static int sim_write(void *ctx, uint32_t index, const void *buf, size_t len)
{
	struct sim_fs *s = ctx;

	if (s->off) {
		return -EIO;
	}
	if (index >= NCHUNK || len > CHUNK) {
		return -EINVAL;
	}
	s->updates++;
	if (s->cut >= 0 && s->cut-- == 0) {
		/* Power lost during the update: it is atomic, so all or nothing. */
		s->rng = s->rng * 1103515245u + 12345u;
		if (s->rng & 0x10000) {
			memcpy(s->f[index].data, buf, len);
			s->f[index].len = (uint32_t)len;
		}
		s->off = true;
		return -EIO;
	}
	memcpy(s->f[index].data, buf, len);
	s->f[index].len = (uint32_t)len;
	return 0;
}

static const struct sqnano_file_ops sim_ops = {.read = sim_read, .write = sim_write};

static struct sqnano_db db;
static struct sqnano_file_media fm;
static uint8_t cache[CHUNK];
static struct kv_model states[TXNS + 1];

static int open_db(const struct sqnano_file_ops *ops, void *ctx)
{
	struct sqnano_db_config cfg = {.page_size = 512, .create = true};

	sqnano_file_media_init(&fm, ops, ctx, NCHUNK * CHUNK, CHUNK, cache);
	return sqnano_db_open(&db, &fm.media, &cfg, NULL);
}

static void create_tree(struct kv_model *m)
{
	struct sqnano_txn t;
	uint32_t seed = 5;

	memset(m, 0, sizeof(*m));
	CHECK_RC(sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1), SQNANO_OK);
	db_lock(&db);
	CHECK_RC(bt_tree_create(&t, KV_TREE), SQNANO_OK);
	db_unlock(&db);
	for (uint32_t k = 0; k < 400; k += 3) {
		m->present[k] = 1;
		m->val[k] = test_rand(&seed);
		CHECK_RC(kv_put(&t, k, m->val[k], 120), SQNANO_OK);
	}
	CHECK_RC(sqnano_txn_commit(&t), SQNANO_OK);
}

/* Returns the number of transactions committed. */
static int workload(bool record)
{
	uint32_t seed = 0xfeed;
	struct kv_model m = states[0];
	int committed = 0;

	for (int i = 0; i < TXNS; i++) {
		struct sqnano_txn t;
		int rc = sqnano_txn_begin(&db, &t, SQNANO_TXN_WRITE, -1);

		for (int k = 0; k < 20 && rc == SQNANO_OK; k++) {
			uint32_t key = test_rand(&seed) % 400;

			if (test_rand(&seed) % 3 == 0) {
				bool found;

				rc = kv_del(&t, key, &found);
				m.present[key] = 0;
			} else {
				uint32_t v = test_rand(&seed);

				rc = kv_put(&t, key, v, 120);
				m.present[key] = 1;
				m.val[key] = v;
			}
		}
		rc = rc == SQNANO_OK ? sqnano_txn_commit(&t) : (sqnano_txn_rollback(&t), rc);
		if (rc) {
			if (record) {
				fprintf(stderr, "transaction %d failed (%d)\n", i, rc);
			}
			CHECK(!record);
			return committed;
		}
		committed++;
		if (record) {
			states[committed] = m;
		}
	}
	return committed;
}

static bool matches(const struct kv_model *m)
{
	struct sqnano_txn r;
	int rc;

	CHECK_RC(sqnano_txn_begin(&db, &r, SQNANO_TXN_READ, 0), SQNANO_OK);
	kv_verbose = 0;
	rc = kv_verify(&r, m, 120);
	kv_verbose = 1;
	CHECK_RC(sqnano_txn_commit(&r), SQNANO_OK);
	return rc == SQNANO_OK;
}

static void powercut(void)
{
	uint32_t total, before = 0, after = 0;

	memset(&fs, 0, sizeof(fs));
	fs.cut = -1;
	CHECK_RC(open_db(&sim_ops, &fs), SQNANO_OK);
	CHECK(db.mode == SQNANO_MODE_SHADOW);
	create_tree(&states[0]);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	fs_image = fs;

	CHECK_RC(open_db(&sim_ops, &fs), SQNANO_OK);
	fs.updates = 0;
	CHECK(workload(true) == TXNS);
	total = fs.updates;
	CHECK(matches(&states[TXNS]));
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);

	for (uint32_t cut = 0; cut < total; cut++) {
		int done;

		fs = fs_image;
		fs.cut = (int32_t)cut;
		fs.rng = cut * 2654435761u;
		CHECK_RC(open_db(&sim_ops, &fs), SQNANO_OK);
		done = workload(false);
		/* Reboot: a fresh media instance, nothing cached. */
		fs.off = false;
		fs.cut = -1;
		CHECK_RC(open_db(&sim_ops, &fs), SQNANO_OK);
		CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
		if (matches(&states[done])) {
			before++;
		} else {
			CHECK(done < TXNS && matches(&states[done + 1]));
			after++;
		}
		/* The database keeps working after the cut. */
		CHECK(workload(false) == TXNS);
		CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	}
	printf("file media power cuts: %u cut points (%u file updates for %d commits), "
	       "%u on the old state, %u on the new: ok\n",
	       total, total, TXNS, before, after);
}

/* ---- POSIX files ---- */

static char dir[256];

static void chunk_path(char *out, size_t n, uint32_t index, const char *suffix)
{
	(void)snprintf(out, n, "%s/%u%s", dir, index, suffix);
}

static int posix_read(void *ctx, uint32_t index, uint32_t off, void *buf, size_t len)
{
	char path[300];
	ssize_t got = 0;
	int fd;

	(void)ctx;
	memset(buf, 0, len);
	chunk_path(path, sizeof(path), index, "");
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return errno == ENOENT ? 0 : -errno;
	}
	if (lseek(fd, off, SEEK_SET) >= 0) {
		got = read(fd, buf, len);
	}
	close(fd);
	return got < 0 ? -EIO : 0;
}

static int posix_write(void *ctx, uint32_t index, const void *buf, size_t len)
{
	char tmp[300], path[300];
	int fd;

	(void)ctx;
	chunk_path(tmp, sizeof(tmp), index, ".tmp");
	chunk_path(path, sizeof(path), index, "");
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		return -errno;
	}
	if (write(fd, buf, len) != (ssize_t)len || fsync(fd) != 0) {
		close(fd);
		return -EIO;
	}
	close(fd);
	return rename(tmp, path) == 0 ? 0 : -errno;
}

static const struct sqnano_file_ops posix_ops = {.read = posix_read, .write = posix_write};

static void posix_files(void)
{
	struct kv_model m;
	DIR *d;
	int files = 0;

	(void)snprintf(dir, sizeof(dir), "%s/sqnano-fm-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	CHECK(mkdtemp(dir) != NULL);
	CHECK_RC(open_db(&posix_ops, NULL), SQNANO_OK);
	create_tree(&m);
	states[0] = m;
	CHECK(workload(true) == TXNS);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	CHECK_RC(open_db(&posix_ops, NULL), SQNANO_OK);
	CHECK(matches(&states[TXNS]));
	CHECK_RC(sqnano_db_check(&db), SQNANO_OK);
	CHECK_RC(sqnano_db_close(&db), SQNANO_OK);
	d = opendir(dir);
	CHECK(d != NULL);
	for (struct dirent *e; (e = readdir(d)) != NULL;) {
		char path[600];

		if (e->d_name[0] == '.') {
			continue;
		}
		files++;
		(void)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		unlink(path);
	}
	closedir(d);
	rmdir(dir);
	printf("file media on POSIX files ok (%d chunk files of %d bytes)\n", files, CHUNK);
}

int main(void)
{
	powercut();
	posix_files();
	return 0;
}
