/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Remote cursor server, used by sqnano-cli from a host.
 *
 * The device only answers simple, stateless requests on read snapshots:
 * list the databases and their schema, read rows of a table by rowid range,
 * run a precompiled query. The host runs the full SQL of SQLite on what it
 * reads. Requests and responses are CBOR maps, carried by any transport
 * (SMP/mcumgr on Zephyr, see CONFIG_SQNANO_REMOTE).
 *
 * Requests (text keys):
 *   {"op": "info"}
 *       -> {"dbs": [{"path", "ps", "total", "used", "mode", "sv", "seq"}]}
 *   {"op": "tables", "db": i, "from": k, "off": byte}
 *       -> {"sv": version, "s": [[kind (0 table, 1 index), name, sql bytes,
 *           done]], "more": bool, "from": k, "off": byte}
 *       Long SQL texts continue in the next response, at "from"/"off".
 *   {"op": "scan", "db": i, "t": name, "after": rowid, "hi": rowid, "n": rows}
 *       -> {"rows": [[rowid, column values in table order]], "more": bool}
 *   {"op": "run", "db": i, "q": name, "p": [values], "n": rows}
 *       -> {"c": [column names], "rows": [[values]], "more": bool, "ch": changes}
 *   Any error -> {"err": code, "msg": text}
 *
 * A scan resumes after the last rowid it returned ("after"); each request
 * reads its own snapshot. Responses stop before they exceed the buffer.
 */

#ifndef SQNANO_REMOTE_H_
#define SQNANO_REMOTE_H_

#include <stddef.h>
#include <stdint.h>

#include <sqnano/sqnano.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A database the server exposes. */
struct sqnano_remote_db {
	const char *path;
	struct sqnano_db *db;
};

/**
 * @brief Answer one request.
 *
 * @param dbs Databases, indexed by the "db" field of requests.
 * @param req CBOR request.
 * @param out Response buffer; @p olen receives the response length.
 * @param allow_write Allow precompiled queries that write.
 * @retval SQNANO_OK A response (possibly an error map) was written.
 * @retval SQNANO_TOOBIG @p cap cannot even hold an error response.
 */
int sqnano_remote_handle(const struct sqnano_remote_db *dbs, uint8_t ndbs, const uint8_t *req,
			 size_t len, uint8_t *out, size_t cap, size_t *olen, bool allow_write);

#ifdef __cplusplus
}
#endif

#endif /* SQNANO_REMOTE_H_ */
