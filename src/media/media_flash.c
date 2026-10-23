/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/* Flash partition media (flash_area API): shadow paging. */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <sqnano/zephyr.h>

static const struct flash_area *fa_of(const struct sqnano_media *m)
{
	return ((const struct sqnano_flash_media *)m->ctx)->fa;
}

static int fm_init(struct sqnano_media *m)
{
	const struct flash_area *fa = fa_of(m);
	const struct device *dev = flash_area_get_device(fa);
	struct flash_pages_info info;
	int rc;

	if (!dev || !device_is_ready(dev)) {
		return -ENODEV;
	}
	memset(&m->caps, 0, sizeof(m->caps));
	m->size = (uint32_t)fa->fa_size;
	m->caps.write_granularity = flash_area_align(fa);
	if (m->caps.write_granularity == 0) {
		m->caps.write_granularity = 1;
	}
	m->caps.endurance_cycles = 100000;
	if (flash_params_get_erase_cap(flash_get_parameters(dev)) & FLASH_ERASE_C_EXPLICIT) {
		rc = flash_get_page_info_by_offs(dev, fa->fa_off, &info);
		if (rc) {
			return rc;
		}
		m->caps.erase_size = (uint32_t)info.size;
	}
	return 0;
}

static int fm_read(const struct sqnano_media *m, uint32_t off, void *buf, size_t len)
{
	return flash_area_read(fa_of(m), off, buf, len);
}

static int fm_write(const struct sqnano_media *m, uint32_t off, const void *buf, size_t len)
{
	return flash_area_write(fa_of(m), off, buf, len);
}

static int fm_erase(const struct sqnano_media *m, uint32_t off, size_t len)
{
	return flash_area_erase(fa_of(m), off, len);
}

static const struct sqnano_media_api flash_api = {
	.init = fm_init,
	.read = fm_read,
	.write = fm_write,
	.erase = fm_erase,
};

void sqnano_flash_media_init(struct sqnano_flash_media *m, const struct flash_area *fa)
{
	memset(m, 0, sizeof(*m));
	m->fa = fa;
	m->media.api = &flash_api;
	m->media.ctx = m;
}
