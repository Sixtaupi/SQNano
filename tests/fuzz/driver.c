/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Stand-in for libFuzzer where it is not available (Apple clang, gcc):
 * runs the inputs given as files or directories, then -runs=N random
 * mutations of them (-seed=S). Under libFuzzer this file is not built.
 *
 *   fuzz_x [-runs=N] [-seed=S] [file|dir ...]
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);

#define MAX_INPUT  4096
#define MAX_CORPUS 256

static uint8_t *corpus[MAX_CORPUS];
static size_t corpus_len[MAX_CORPUS];
static int ncorpus;
static uint32_t rng = 1;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static void add_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	size_t n;

	if (!f) {
		return;
	}
	buf = malloc(MAX_INPUT);
	n = fread(buf, 1, MAX_INPUT, f);
	fclose(f);
	LLVMFuzzerTestOneInput(buf, n);
	if (ncorpus < MAX_CORPUS) {
		corpus[ncorpus] = buf;
		corpus_len[ncorpus++] = n;
	} else {
		free(buf);
	}
}

static void add_path(const char *path)
{
	struct stat st;
	DIR *d;

	if (stat(path, &st) != 0) {
		fprintf(stderr, "%s: not found\n", path);
		exit(1);
	}
	if (!S_ISDIR(st.st_mode)) {
		add_file(path);
		return;
	}
	d = opendir(path);
	for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
		char p[1024];

		if (e->d_name[0] == '.') {
			continue;
		}
		snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
		add_file(p);
	}
	if (d) {
		closedir(d);
	}
}

/* A few byte-level mutations of a corpus entry (or of nothing). */
static size_t mutate(uint8_t *buf)
{
	size_t n = 0;

	if (ncorpus && rnd() % 8) {
		int k = (int)(rnd() % (uint32_t)ncorpus);

		n = corpus_len[k];
		memcpy(buf, corpus[k], n);
	}
	for (int m = 1 + (int)(rnd() % 8); m > 0; m--) {
		size_t at = n ? rnd() % n : 0;

		switch (rnd() % 5) {
		case 0:
			if (n) {
				buf[at] ^= (uint8_t)(1u << (rnd() % 8));
			}
			break;
		case 1:
			if (n) {
				buf[at] = (uint8_t)rnd();
			}
			break;
		case 2:
			if (n < MAX_INPUT) {
				memmove(buf + at + 1, buf + at, n - at);
				buf[at] = (uint8_t)rnd();
				n++;
			}
			break;
		case 3:
			if (n) {
				memmove(buf + at, buf + at + 1, n - at - 1);
				n--;
			}
			break;
		default: {
			size_t len = 1 + rnd() % 16;

			while (len-- && n < MAX_INPUT) {
				buf[n++] = (uint8_t)rnd();
			}
		}
		}
	}
	return n;
}

int main(int argc, char **argv)
{
	static uint8_t buf[MAX_INPUT];
	long runs = 0;

	if (LLVMFuzzerInitialize(&argc, &argv)) {
		return 1;
	}
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "-runs=", 6) == 0) {
			runs = atol(argv[i] + 6);
		} else if (strncmp(argv[i], "-seed=", 6) == 0) {
			rng = (uint32_t)atol(argv[i] + 6) | 1u;
		} else if (argv[i][0] != '-') {
			add_path(argv[i]);
		}
	}
	for (long r = 0; r < runs; r++) {
		size_t n = mutate(buf);

		LLVMFuzzerTestOneInput(buf, n);
	}
	printf("%s: %d inputs, %ld mutations: ok\n", argv[0], ncorpus, runs);
	return 0;
}
