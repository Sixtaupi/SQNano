/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * SMP (mcumgr) group of the remote cursor server, for sqnano-cli.
 *
 * Command 0 carries a request of include/sqnano/remote.h: the request map
 * is the SMP payload itself, and the response is {"d": <CBOR response>}.
 * The databases are those of the devicetree, opened on first use.
 */

#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_encode.h>

#include <sqnano/remote.h>
#include <sqnano/zephyr.h>

#define NDBS_MAX 8

static uint8_t resp[CONFIG_SQNANO_REMOTE_BUF_SIZE];

static int sqnano_smp_request(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	struct sqnano_remote_db dbs[NDBS_MAX];
	int n = sqnano_dt_count();
	size_t room = (size_t)(zse->payload_end - zse->payload);
	size_t cap = sizeof(resp);
	size_t olen = 0;

	n = n > NDBS_MAX ? NDBS_MAX : n;
	for (int i = 0; i < n; i++) {
		const struct sqnano_dt_db *d = sqnano_dt_get(i);

		dbs[i].path = d->path;
		/* NULL schema: the one the application was built with. */
		(void)sqnano_dt_open(d, NULL, &dbs[i].db);
	}
	/* Keep the response within the SMP buffer: "d" and a 3-byte length. */
	if (room < 16) {
		return MGMT_ERR_EMSGSIZE;
	}
	cap = room - 16 < cap ? room - 16 : cap;
	if (sqnano_remote_handle(dbs, (uint8_t)n, zsd->payload,
				 (size_t)(zsd->payload_end - zsd->payload), resp, cap, &olen,
				 IS_ENABLED(CONFIG_SQNANO_REMOTE_WRITE)) != 0) {
		return MGMT_ERR_EMSGSIZE;
	}
	if (!zcbor_tstr_put_lit(zse, "d") || !zcbor_bstr_encode_ptr(zse, (const char *)resp, olen)) {
		return MGMT_ERR_EMSGSIZE;
	}
	return MGMT_ERR_EOK;
}

static const struct mgmt_handler sqnano_smp_handlers[] = {
	[0] = {.mh_read = sqnano_smp_request, .mh_write = sqnano_smp_request},
};

static struct mgmt_group sqnano_smp_group = {
	.mg_handlers = sqnano_smp_handlers,
	.mg_handlers_count = ARRAY_SIZE(sqnano_smp_handlers),
	.mg_group_id = CONFIG_SQNANO_REMOTE_GROUP,
};

static void sqnano_smp_register(void)
{
	mgmt_register_group(&sqnano_smp_group);
}

MCUMGR_HANDLER_DEFINE(sqnano_remote, sqnano_smp_register);
