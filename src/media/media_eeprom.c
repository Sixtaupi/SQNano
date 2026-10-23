/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/* EEPROM API media (FRAM, MRAM, EEPROM): in-place updates with undo journal. */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/eeprom.h>

#include <sqnano/zephyr.h>

static const struct device *dev_of(const struct sqnano_media *m)
{
	return ((const struct sqnano_eeprom_media *)m->ctx)->dev;
}

static int em_init(struct sqnano_media *m)
{
	const struct device *dev = dev_of(m);

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	memset(&m->caps, 0, sizeof(m->caps));
	m->size = (uint32_t)eeprom_get_size(dev);
	m->caps.write_granularity = 1;
	m->caps.endurance_cycles = 0xffffffffu;
	return 0;
}

static int em_read(const struct sqnano_media *m, uint32_t off, void *buf, size_t len)
{
	return eeprom_read(dev_of(m), off, buf, len);
}

static int em_write(const struct sqnano_media *m, uint32_t off, const void *buf, size_t len)
{
	return eeprom_write(dev_of(m), off, buf, len);
}

static int em_erase(const struct sqnano_media *m, uint32_t off, size_t len)
{
	(void)m;
	(void)off;
	(void)len;
	return -ENOTSUP;
}

static const struct sqnano_media_api eeprom_api = {
	.init = em_init,
	.read = em_read,
	.write = em_write,
	.erase = em_erase,
};

void sqnano_eeprom_media_init(struct sqnano_eeprom_media *m, const struct device *dev)
{
	memset(m, 0, sizeof(*m));
	m->dev = dev;
	m->media.api = &eeprom_api;
	m->media.ctx = m;
}
