/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * NAND operations over a flash partition of a Zephyr NAND driver (spi_nand,
 * flexspi_nand): one write block per page, one erase page per block, the
 * on-die ECC result as the read error (-EBADMSG), bad blocks through the
 * flash extended operations when they are enabled.
 */

#include <errno.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <sqnano/zephyr.h>

static int zn_read(void *ctx, uint32_t page, void *buf)
{
	struct sqnano_nand_zmedia *z = ctx;

	return flash_area_read(z->fa, (off_t)page * z->geo.page_size, buf, z->geo.page_size);
}

static int zn_program(void *ctx, uint32_t page, const void *buf)
{
	struct sqnano_nand_zmedia *z = ctx;

	return flash_area_write(z->fa, (off_t)page * z->geo.page_size, buf, z->geo.page_size);
}

static int zn_erase(void *ctx, uint32_t block)
{
	struct sqnano_nand_zmedia *z = ctx;
	size_t bs = (size_t)z->geo.page_size * z->geo.pages_per_block;

	return flash_area_erase(z->fa, (off_t)block * bs, bs);
}

#ifdef CONFIG_FLASH_EX_OP_ENABLED
static off_t block_addr(const struct sqnano_nand_zmedia *z, uint32_t block)
{
	return z->fa->fa_off + (off_t)block * z->geo.page_size * z->geo.pages_per_block;
}

static int zn_is_bad(void *ctx, uint32_t block)
{
	struct sqnano_nand_zmedia *z = ctx;
	enum flash_block_status st = FLASH_BLOCK_GOOD;
	off_t addr = block_addr(z, block);
	int rc = flash_ex_op(flash_area_get_device(z->fa), FLASH_EX_OP_IS_BAD_BLOCK,
			     (uintptr_t)&addr, &st);

	if (rc == -ENOTSUP) {
		return 0;
	}
	return rc ? rc : st == FLASH_BLOCK_BAD;
}

static int zn_mark_bad(void *ctx, uint32_t block)
{
	struct sqnano_nand_zmedia *z = ctx;
	off_t addr = block_addr(z, block);
	int rc = flash_ex_op(flash_area_get_device(z->fa), FLASH_EX_OP_MARK_BAD_BLOCK,
			     (uintptr_t)&addr, NULL);

	return rc == -ENOTSUP ? 0 : rc;
}
#endif

static const struct sqnano_nand_ops zn_ops = {
	.read = zn_read,
	.program = zn_program,
	.erase = zn_erase,
#ifdef CONFIG_FLASH_EX_OP_ENABLED
	.is_bad = zn_is_bad,
	.mark_bad = zn_mark_bad,
#endif
};

int sqnano_nand_zmedia_init(struct sqnano_nand_zmedia *z, const struct flash_area *fa,
			    bool soft_ecc, uint16_t reserved_blocks)
{
	const struct device *dev = flash_area_get_device(fa);
	struct flash_pages_info info;
	int rc;

	z->fa = fa;
	if (!dev || !device_is_ready(dev)) {
		return -ENODEV;
	}
	rc = flash_get_page_info_by_offs(dev, fa->fa_off, &info);
	if (rc) {
		return rc;
	}
	z->geo.page_size = (uint32_t)flash_get_write_block_size(dev);
	z->geo.pages_per_block = (uint32_t)info.size / z->geo.page_size;
	z->geo.blocks = (uint32_t)(fa->fa_size / info.size);
	z->geo.soft_ecc = soft_ecc;
	z->geo.reserved_blocks = reserved_blocks;
	sqnano_nand_media_init(&z->nm, &zn_ops, z, &z->geo);
	return 0;
}
