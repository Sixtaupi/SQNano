/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief SQLite source-compatible facade over SQNano.
 *
 * Applications written against SQLite recompile unchanged. Compatibility is
 * at the source level only: the ABI and the file format differ. Every SQL
 * statement must be known at build time; sqlite3_prepare_v2() finds its
 * precompiled version by the hash of its normalized text and fails with
 * "query not precompiled: <hash>" otherwise.
 *
 * No memory is allocated: connections, statements and their state come from
 * static pools sized by CONFIG_SQNANO_MAX_CONNECTIONS and
 * CONFIG_SQNANO_MAX_STMTS. sqlite3_exec() error messages point to a buffer
 * owned by the connection; sqlite3_free() accepts them and does nothing.
 */

#ifndef SQNANO_SQLITE3_H_
#define SQNANO_SQLITE3_H_

#include <stdarg.h>
#include <stdint.h>

#include <sqnano/rc.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SQLITE_VERSION        "3.45.0"
#define SQLITE_VERSION_NUMBER 3045000
#define SQLITE_SOURCE_ID      "sqnano-0.1.0"

typedef int64_t sqlite3_int64;
typedef uint64_t sqlite3_uint64;
typedef int64_t sqlite_int64;
typedef uint64_t sqlite_uint64;

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef void (*sqlite3_destructor_type)(void *);
typedef int (*sqlite3_callback)(void *, int, char **, char **);

#define SQLITE_STATIC    ((sqlite3_destructor_type)0)
#define SQLITE_TRANSIENT ((sqlite3_destructor_type)-1)

#define SQLITE_OK         SQNANO_OK
#define SQLITE_ERROR      SQNANO_ERROR
#define SQLITE_INTERNAL   SQNANO_INTERNAL
#define SQLITE_PERM       SQNANO_PERM
#define SQLITE_ABORT      SQNANO_ABORT
#define SQLITE_BUSY       SQNANO_BUSY
#define SQLITE_LOCKED     SQNANO_LOCKED
#define SQLITE_NOMEM      SQNANO_NOMEM
#define SQLITE_READONLY   SQNANO_READONLY
#define SQLITE_INTERRUPT  SQNANO_INTERRUPT
#define SQLITE_IOERR      SQNANO_IOERR
#define SQLITE_CORRUPT    SQNANO_CORRUPT
#define SQLITE_NOTFOUND   SQNANO_NOTFOUND
#define SQLITE_FULL       SQNANO_FULL
#define SQLITE_CANTOPEN   SQNANO_CANTOPEN
#define SQLITE_PROTOCOL   SQNANO_PROTOCOL
#define SQLITE_EMPTY      SQNANO_EMPTY
#define SQLITE_SCHEMA     SQNANO_SCHEMA
#define SQLITE_TOOBIG     SQNANO_TOOBIG
#define SQLITE_CONSTRAINT SQNANO_CONSTRAINT
#define SQLITE_MISMATCH   SQNANO_MISMATCH
#define SQLITE_MISUSE     SQNANO_MISUSE
#define SQLITE_NOLFS      SQNANO_NOLFS
#define SQLITE_AUTH       SQNANO_AUTH
#define SQLITE_FORMAT     SQNANO_FORMAT
#define SQLITE_RANGE      SQNANO_RANGE
#define SQLITE_NOTADB     SQNANO_NOTADB
#define SQLITE_NOTICE     27
#define SQLITE_WARNING    28
#define SQLITE_ROW        SQNANO_ROW
#define SQLITE_DONE       SQNANO_DONE

#define SQLITE_CONSTRAINT_CHECK      SQNANO_CONSTRAINT_CHECK
#define SQLITE_CONSTRAINT_FOREIGNKEY SQNANO_CONSTRAINT_FOREIGNKEY
#define SQLITE_CONSTRAINT_NOTNULL    SQNANO_CONSTRAINT_NOTNULL
#define SQLITE_CONSTRAINT_PRIMARYKEY SQNANO_CONSTRAINT_PRIMARYKEY
#define SQLITE_CONSTRAINT_UNIQUE     SQNANO_CONSTRAINT_UNIQUE
#define SQLITE_ABORT_ROLLBACK        (SQLITE_ABORT | (2 << 8))
#define SQLITE_CANTOPEN_SCHEMA       SQNANO_CANTOPEN_SCHEMA

#define SQLITE_INTEGER 1
#define SQLITE_FLOAT   2
#define SQLITE_TEXT    3
#define SQLITE3_TEXT   3
#define SQLITE_BLOB    4
#define SQLITE_NULL    5

#define SQLITE_OPEN_READONLY     0x00000001
#define SQLITE_OPEN_READWRITE    0x00000002
#define SQLITE_OPEN_CREATE       0x00000004
#define SQLITE_OPEN_URI          0x00000040
#define SQLITE_OPEN_MEMORY       0x00000080
#define SQLITE_OPEN_NOMUTEX      0x00008000
#define SQLITE_OPEN_FULLMUTEX    0x00010000
#define SQLITE_OPEN_SHAREDCACHE  0x00020000
#define SQLITE_OPEN_PRIVATECACHE 0x00040000

#define SQLITE_PREPARE_PERSISTENT 0x01

const char *sqlite3_libversion(void);
int sqlite3_libversion_number(void);
const char *sqlite3_sourceid(void);
int sqlite3_threadsafe(void);

int sqlite3_open(const char *filename, sqlite3 **db);
int sqlite3_open_v2(const char *filename, sqlite3 **db, int flags, const char *vfs);
int sqlite3_close(sqlite3 *db);
int sqlite3_close_v2(sqlite3 *db);

int sqlite3_prepare(sqlite3 *db, const char *sql, int nbyte, sqlite3_stmt **stmt,
		    const char **tail);
int sqlite3_prepare_v2(sqlite3 *db, const char *sql, int nbyte, sqlite3_stmt **stmt,
		       const char **tail);
int sqlite3_prepare_v3(sqlite3 *db, const char *sql, int nbyte, unsigned int flags,
		       sqlite3_stmt **stmt, const char **tail);
int sqlite3_step(sqlite3_stmt *stmt);
int sqlite3_reset(sqlite3_stmt *stmt);
int sqlite3_finalize(sqlite3_stmt *stmt);
int sqlite3_clear_bindings(sqlite3_stmt *stmt);
const char *sqlite3_sql(sqlite3_stmt *stmt);
int sqlite3_stmt_readonly(sqlite3_stmt *stmt);
int sqlite3_stmt_busy(sqlite3_stmt *stmt);
sqlite3 *sqlite3_db_handle(sqlite3_stmt *stmt);

int sqlite3_bind_parameter_count(sqlite3_stmt *stmt);
const char *sqlite3_bind_parameter_name(sqlite3_stmt *stmt, int i);
int sqlite3_bind_parameter_index(sqlite3_stmt *stmt, const char *name);
int sqlite3_bind_null(sqlite3_stmt *stmt, int i);
int sqlite3_bind_int(sqlite3_stmt *stmt, int i, int v);
int sqlite3_bind_int64(sqlite3_stmt *stmt, int i, sqlite3_int64 v);
int sqlite3_bind_double(sqlite3_stmt *stmt, int i, double v);
int sqlite3_bind_text(sqlite3_stmt *stmt, int i, const char *v, int n,
		      void (*destructor)(void *));
int sqlite3_bind_blob(sqlite3_stmt *stmt, int i, const void *v, int n,
		      void (*destructor)(void *));
int sqlite3_bind_zeroblob(sqlite3_stmt *stmt, int i, int n);

int sqlite3_column_count(sqlite3_stmt *stmt);
int sqlite3_data_count(sqlite3_stmt *stmt);
const char *sqlite3_column_name(sqlite3_stmt *stmt, int i);
const char *sqlite3_column_decltype(sqlite3_stmt *stmt, int i);
int sqlite3_column_type(sqlite3_stmt *stmt, int i);
int sqlite3_column_int(sqlite3_stmt *stmt, int i);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *stmt, int i);
double sqlite3_column_double(sqlite3_stmt *stmt, int i);
const unsigned char *sqlite3_column_text(sqlite3_stmt *stmt, int i);
const void *sqlite3_column_blob(sqlite3_stmt *stmt, int i);
int sqlite3_column_bytes(sqlite3_stmt *stmt, int i);

int sqlite3_exec(sqlite3 *db, const char *sql, int (*callback)(void *, int, char **, char **),
		 void *arg, char **errmsg);

int sqlite3_errcode(sqlite3 *db);
int sqlite3_extended_errcode(sqlite3 *db);
const char *sqlite3_errmsg(sqlite3 *db);
const char *sqlite3_errstr(int rc);
int sqlite3_changes(sqlite3 *db);
sqlite3_int64 sqlite3_changes64(sqlite3 *db);
int sqlite3_total_changes(sqlite3 *db);
sqlite3_int64 sqlite3_last_insert_rowid(sqlite3 *db);
int sqlite3_busy_timeout(sqlite3 *db, int ms);
int sqlite3_get_autocommit(sqlite3 *db);
int sqlite3_extended_result_codes(sqlite3 *db, int onoff);

/** No heap: always returns NULL. */
void *sqlite3_malloc(int n);
/** Accepts the error messages returned by sqlite3_exec(); does nothing. */
void sqlite3_free(void *p);

struct sqnano_db;
struct sqnano_media;
struct sqnano_db_config;
struct sqnano_schema;

/**
 * @brief Associate a path used by sqlite3_open() with a database.
 *
 * Under Zephyr the databases declared in the devicetree (compatible
 * "zephyr,sqnano") are registered automatically. A path matches when it is
 * identical or when its last component is identical ("/data/app.db" opens
 * "app.db").
 */
int sqnano_sqlite3_register(const char *path, struct sqnano_db *db, struct sqnano_media *media,
			    const struct sqnano_db_config *cfg, const struct sqnano_schema *schema);

/** Engine database behind a connection. */
struct sqnano_db *sqnano_sqlite3_db(sqlite3 *db);

#ifdef __cplusplus
}
#endif

#endif /* SQNANO_SQLITE3_H_ */
