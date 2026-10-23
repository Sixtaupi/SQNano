/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief SQNano result codes.
 *
 * The numeric values are identical to the SQLite result codes so that the
 * sqlite3.h facade can return engine codes unchanged.
 */

#ifndef SQNANO_RC_H_
#define SQNANO_RC_H_

#define SQNANO_OK         0
#define SQNANO_ERROR      1
#define SQNANO_INTERNAL   2
#define SQNANO_PERM       3
#define SQNANO_ABORT      4
#define SQNANO_BUSY       5
#define SQNANO_LOCKED     6
#define SQNANO_NOMEM      7
#define SQNANO_READONLY   8
#define SQNANO_INTERRUPT  9
#define SQNANO_IOERR      10
#define SQNANO_CORRUPT    11
#define SQNANO_NOTFOUND   12
#define SQNANO_FULL       13
#define SQNANO_CANTOPEN   14
#define SQNANO_PROTOCOL   15
#define SQNANO_EMPTY      16
#define SQNANO_SCHEMA     17
#define SQNANO_TOOBIG     18
#define SQNANO_CONSTRAINT 19
#define SQNANO_MISMATCH   20
#define SQNANO_MISUSE     21
#define SQNANO_NOLFS      22
#define SQNANO_AUTH       23
#define SQNANO_FORMAT     24
#define SQNANO_RANGE      25
#define SQNANO_NOTADB     26
#define SQNANO_ROW        100
#define SQNANO_DONE       101

#define SQNANO_CONSTRAINT_CHECK      (SQNANO_CONSTRAINT | (1 << 8))
#define SQNANO_CONSTRAINT_FOREIGNKEY (SQNANO_CONSTRAINT | (3 << 8))
#define SQNANO_CONSTRAINT_NOTNULL    (SQNANO_CONSTRAINT | (5 << 8))
#define SQNANO_CONSTRAINT_PRIMARYKEY (SQNANO_CONSTRAINT | (6 << 8))
#define SQNANO_CONSTRAINT_UNIQUE     (SQNANO_CONSTRAINT | (8 << 8))
#define SQNANO_CANTOPEN_SCHEMA       (SQNANO_CANTOPEN | (16 << 8))

/** Primary result code of an extended code. */
#define SQNANO_RC_PRIMARY(rc) ((rc) & 0xff)

#endif /* SQNANO_RC_H_ */
