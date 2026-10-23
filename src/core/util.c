/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../sqnano_int.h"

/* CRC-32 (IEEE 802.3), nibble table: 64 bytes of flash. */
static const uint32_t crc_nibble[16] = {
	0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
	0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
	0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

uint32_t sqnano_crc32(uint32_t crc, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		crc = (crc >> 4) ^ crc_nibble[crc & 0x0f];
		crc = (crc >> 4) ^ crc_nibble[crc & 0x0f];
	}
	return ~crc;
}

int varint_put(uint8_t *p, uint64_t v)
{
	int n = 0;

	do {
		uint8_t b = (uint8_t)(v & 0x7f);

		v >>= 7;
		p[n++] = v ? (uint8_t)(b | 0x80) : b;
	} while (v);
	return n;
}

int varint_get(const uint8_t *p, const uint8_t *end, uint64_t *v)
{
	uint64_t r = 0;
	int n = 0;

	while (p + n < end && n < 10) {
		uint8_t b = p[n];

		r |= (uint64_t)(b & 0x7f) << (7 * n);
		n++;
		if (!(b & 0x80)) {
			*v = r;
			return n;
		}
	}
	return -1;
}

int varint_len(uint64_t v)
{
	int n = 1;

	while (v >>= 7) {
		n++;
	}
	return n;
}
