# SQNano

SQNano is an embedded SQL database for Zephyr. It stores tables and indexes
in a flash partition or on FRAM/EEPROM, survives power cuts at any point,
and speaks the SQLite dialect. Your SQL is compiled on the host: the firmware
contains plain C functions instead of an SQL interpreter, and an
application written against `sqlite3.h` can be ported mostly unchanged.

```c
sqlite3 *db;
sqlite3_stmt *st;

sqlite3_open("/data/app.db", &db);
sqlite3_prepare_v2(db, "SELECT ts, value FROM sensor WHERE id = :id ORDER BY ts DESC LIMIT :n",
                   -1, &st, NULL);
sqlite3_bind_int(st, 1, 3);
sqlite3_bind_int(st, 2, 10);
while (sqlite3_step(st) == SQLITE_ROW) {
	printk("%lld %f\n", sqlite3_column_int64(st, 0), sqlite3_column_double(st, 1));
}
sqlite3_finalize(st);
```

The design is described in [ARCHITECTURE.md](ARCHITECTURE.md) (in French).

## How it works

- **Storage.** Flash partitions use shadow paging: modified pages are
  written to erased pages, then a superblock record switches to the new
  state atomically. Records are appended to a log of a few sectors, so a
  sector is erased only once every few hundred commits. Pages can be smaller than the
  erase sector: a sector is erased once all its pages are free, and
  compaction moves the live pages out of mostly-free sectors. FRAM and MRAM
  are updated in place behind an undo journal. An EEPROM should use shadow
  paging too (`mode = "shadow"`), since in-place updates rewrite the same
  pages at every commit. A FRAM can sit in front of a flash partition
  (hybrid mode, `journal = <&fram0>`): page writes and erases go to the
  FRAM, and a page reaches the flash only when the FRAM is full, so the
  versions that die within a few commits never wear it. On a LittleFS volume, the database is a directory
  of chunk files, each one replaced atomically by LittleFS, with one chunk
  cached in RAM; it uses copy-on-write pages too, and LittleFS spreads the
  wear. On a NAND partition (`nand;`), a translation layer writes the pages
  of each block in order and tags each one with the logical page it holds
  (the Zephyr NAND API has no spare-area access), replaces blocks that fail
  a program or an erase, and moves blocks whose pages needed ECC
  corrections; a software ECC (`nand-soft-ecc;`) covers chips without one.
  Every page carries a CRC.
- **Engine.** Copy-on-write B+trees with byte-comparable keys (a rowid, or
  the `PRIMARY KEY` of a `WITHOUT ROWID` table), one writer and
  snapshot readers, savepoints, wear-spreading page allocation, external
  merge sort through temporary pages. Leaves left underfull by deletes
  merge with a sibling. Long rows
  continue on overflow pages. Nothing is allocated at run time: every buffer
  is sized by Kconfig.
- **SQL.** `sqnanoc` (Python, run by CMake) checks every query against the
  schema, plans it and generates a resumable C step function for each
  query, plus a typed API (`q_sensor_last_open/bind/step`, fields named
  after the result columns). The `sqlite3.h` facade finds the compiled query
  by a hash of the normalized SQL text.
- **Schema changes** ship with the firmware as versioned migrations
  (`schema/v1.sql`, `v2.sql`, `v2_up.sql` ...). They run at boot in a single
  transaction, and the pre-migration state stays on the media until the
  image is confirmed, so an MCUboot revert finds its database intact. A
  migration too large for the free space can be lazy (`v2_lazy.sql`):
  its `UPDATE`s are compiled into conversions applied to each row written
  before it, when the row is read; rows are stored converted when written,
  and `sqnano_migration_step()` converts the rest in small transactions.
  `sqnanoc` refuses a lazy change that an eager migration could make fail
  or that would make an index stale (indexed, `CHECK`, foreign key or
  primary key columns, `NOT NULL` columns given a value that may be NULL),
  and a firmware that no longer has a conversion refuses a database that
  still needs it.

## Getting started

1. Make the module visible to your build: add it to the west manifest, or
   use `list(APPEND EXTRA_ZEPHYR_MODULES <path>/SQNano)` before
   `find_package(Zephyr)`.

2. Declare the database in the devicetree:

   ```dts
   / {
       appdb: sqnano {
           compatible = "zephyr,sqnano";
           path = "app.db";                  /* name for sqlite3_open() */
           storage = <&storage_partition>;   /* flash partition or EEPROM device */
       };
   };
   ```

   Or, on a mounted LittleFS (declared in a `zephyr,fstab` node):

   ```dts
   logdb: sqnano-lfs {
       compatible = "zephyr,sqnano";
       path = "log.db";
       fs-dir = "/lfs/log.db";           /* directory of the chunk files */
       fs-size = <DT_SIZE_K(128)>;
       fs-chunk = <4096>;                /* the LittleFS block size */
   };
   ```

3. Enable it in `prj.conf`:

   ```
   CONFIG_FLASH=y
   CONFIG_FLASH_MAP=y
   CONFIG_SQNANO=y
   CONFIG_SQNANO_PROFILE_STANDARD=y   # or _TINY, _FULL
   ```

4. Write the schema (`schema/v1.sql`) and, optionally, named queries
   (`queries.sql`, one `-- name: sensor_last` comment per query). Then
   compile them in `CMakeLists.txt`:

   ```cmake
   zephyr_sqnano_queries(queries.sql SCHEMA schema EXTRACT src/main.c)
   ```

   `EXTRACT` also compiles the string literals passed to `sqlite3_prepare*()`
   and `sqlite3_exec()` in the listed sources. SQL built at run time cannot
   be compiled: `sqnanoc` reports each such call with its file and line.

[samples/zephyr/sensor_log](samples/zephyr/sensor_log) is a complete
example. It shows both the `sqlite3.h` API and the typed API, and the MCUboot
confirmation hook.

## SQL support

| Area | Supported |
| --- | --- |
| Queries | `SELECT` with `WHERE`, `ORDER BY`, `LIMIT/OFFSET`, `GROUP BY/HAVING`, `DISTINCT`, aggregates; `INNER`, `LEFT`, `CROSS` and `NATURAL` joins, `USING`; scalar, `IN (SELECT ...)` and `EXISTS` subqueries, correlated or not; `UNION [ALL]`, `INTERSECT`, `EXCEPT` |
| Writes | `INSERT` (`VALUES` or `SELECT`, `OR REPLACE/IGNORE/FAIL/ABORT/ROLLBACK`), UPSERT, `UPDATE`, `DELETE`, `RETURNING` |
| Transactions | `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `RELEASE`, `ROLLBACK TO`; a failing statement only undoes itself |
| Constraints | `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `CHECK`, `DEFAULT`, `AUTOINCREMENT`; `FOREIGN KEY` with `ON DELETE/UPDATE` actions and deferred constraints, active once `PRAGMA foreign_keys = ON` (off by default, as in SQLite) |
| Expressions | SQLite affinity and NULL rules, `LIKE`, `GLOB`, `BETWEEN`, `IN`, `CASE`, `CAST`, collations `BINARY/NOCASE/RTRIM`, common scalar functions |
| Schema | `CREATE TABLE/INDEX` (rowid and `WITHOUT ROWID` tables), `ALTER TABLE ADD/RENAME/DROP COLUMN`, `RENAME TO`, `DROP`, in migrations only (eager `vN_up.sql` or lazy `vN_lazy.sql` data changes) |
| Pragmas | `user_version`, `foreign_keys`, `integrity_check`, `busy_timeout`, `page_size`, `page_count`, `freelist_count`, ... |

Not supported: views, triggers, CTEs (`WITH`), window functions, subqueries in
`FROM`, `RIGHT/FULL` joins, date/time and JSON functions, `ATTACH`,
`VACUUM`.

Known differences with SQLite:

- **Keys are bounded.** Index keys and sort keys are limited by
  `CONFIG_SQNANO_MAX_KEY_SIZE`, and rows by `CONFIG_SQNANO_MAX_RECORD_SIZE`.
- **Large sorts use the free pages.** A sort larger than
  `CONFIG_SQNANO_SORT_RAM_SIZE` writes sorted runs to free pages and merges
  them, and fails with `SQLITE_FULL` when those pages run out too. Each
  sort is bounded by the free space, not by the RAM. With `LIMIT`, only
  the rows that can be returned are kept.
- **Transactions on flash use pre-erased space.** With pages smaller than
  the erase sector, a transaction can only write to space that is erased
  when it starts. A very long transaction with open savepoints on a small
  partition can get `SQLITE_FULL` before the partition is full. An
  autocommit statement that fails this way is retried once after a full
  compaction. `sqnano_db_compact()` compacts ahead of time.
- **Inner joins run in `FROM` order**, with nested loops over indexes. There
  are no automatic indexes.
- **Two `UNION` cases** depend on the order in which SQLite reads the table,
  which SQL does not specify: which of two rows equal under `NOCASE` is
  kept, and the order of rows tied in `ORDER BY`.

## Tools

- **`db` shell command** (`CONFIG_SQNANO_SHELL`). It runs restricted SQL on
  the target (`db SELECT ...`, `db EXPLAIN ...`, `db .run <query> id=3`) and
  has dot commands (`.tables`, `.schema`, `.check`, `.stats`).
- **`sqnano-cli`** (`west sqnano cli`, `CONFIG_SQNANO_REMOTE`). It runs the
  full SQL of SQLite from a PC: joins, CTEs, window functions, on the
  device's data. The device only serves rows of its tables by key range
  (rowid, or encoded primary key for `WITHOUT ROWID` tables), and its
  precompiled queries, through an mcumgr (SMP) group over the
  serial console, UDP or any other SMP transport. The tables a statement
  reads are copied into an in-memory SQLite database first. `.export
  copy.db` writes them all to a standard SQLite file. The server takes
  about 5 KB of flash; its RAM is a statement state from the statement
  pool plus the response buffer.

  ```
  $ west sqnano cli --port /dev/ttyACM0
  sqnano> SELECT id, count(*), avg(value) FROM sensor GROUP BY id;
  sqnano> .run sensor_last 3 10
  ```

- **`west sqnano check <app>`** compiles every SQL statement of a Linux
  application against its schema. It lists what SQNano accepts, what it
  refuses and why, and the dynamic SQL to rewrite. The schema can be inferred
  from the application's `CREATE` statements (`--emit-schema`).

## Footprint

Measured on Cortex-M4 (frdm_k64f, `-Os`): code and constants of the engine
objects linked into the image, from the map file (the `standard` line was
measured again after `WITHOUT ROWID` and lazy migrations):

| Configuration | Flash | RAM |
| --- | --- | --- |
| `tiny` profile, typed API only | 23.7 KB + 1.2 KB generated | 3.0 KB |
| `standard` profile with `sqlite3.h`, joins, savepoints, foreign keys, 512-byte pages in 4 KiB sectors | 55 KB + 3.3 KB generated | 14.1 KB per database + 3 KB statement pool |

The statement pool holds the states of the prepared statements. By
default it reserves the largest state for each possible statement
(`CONFIG_SQNANO_MAX_STMTS`, 11.5 KB in this example). With
`CONFIG_SQNANO_STMT_POOL_SIZE`, each statement takes the size of its own
state; the shell `.stats` and `sqnano_pool_stats()` give the peak to size
it from (1.9 KB for the sample). The external sort adds about 2 KB of flash and 0.5 KB of RAM, only when a
query sorts. The page cache dominates the RAM: `CONFIG_SQNANO_PAGE_CACHE_COUNT` pages of
`CONFIG_SQNANO_MAX_PAGE_SIZE` bytes (8 × 4 KiB would be 32 KB).

## Endurance

Page allocation rotates over the whole partition, and so do the superblock
records and the journal slots. Measured with a rolling log (one 40-byte row
inserted and the oldest deleted per commit), until the most-worn sector or
byte reaches its rated cycles:

| Medium | Partition, page | Commits |
| --- | --- | --- |
| NOR, 4 KiB sectors, 100k cycles | 256 KB, 4 KiB pages | 1.4 M |
| | 256 KB, 1 KiB pages | 4.7 M |
| | 1 MB, 1 KiB pages | 11 M |
| NOR + 32 KB FRAM (hybrid) | 256 KB, 512-byte pages | ~90 M (22 times fewer flash erases) |
| EEPROM, 1M cycles, shadow mode | 64 KB, 256-byte pages | 30 M |
| EEPROM, 1M cycles, journal mode | 64 KB, 256-byte pages | 1 M |

On NAND, each logical page is one NAND page and each erase unit one block,
so the NOR figures apply with the block as the sector. A unit whose pages
all died gives its block back to a shared pool, so the spare blocks rotate
too. The NAND layer takes about 8 bytes of RAM per block and 1 byte per
logical page, plus one NAND page of buffer.

Endurance grows with the partition size and falls with the bytes written
per commit: each commit rewrites the modified path of every B+tree it
touches, so grouping rows into transactions divides the wear, and each
index adds to it. FRAM and MRAM endurance (10^12 cycles and more) is not a
concern.

## Testing

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
west twister -T tests/zephyr -T samples/zephyr -p qemu_x86
```

The host suite runs under ASan/UBSan (`-DSQNANO_SANITIZE=ON`, any report
fails the test). GitHub Actions runs it with gcc, clang and on macOS, the
Zephyr tests and samples under Twister, and the fuzzing below
([ci.yml](.github/workflows/ci.yml), [fuzz.yml](.github/workflows/fuzz.yml)).
It includes:

- **Power-cut fuzzing.** The simulated medium is cut at every write and
  erase, including during recovery, on NOR and FRAM, with savepoints,
  overflow pages, small pages in large sectors and compaction. The test
  also checks that no flash byte is ever programmed twice without an erase.
- **A differential test against SQLite.** Random statements run on both
  engines, inside random transactions and savepoints, with foreign keys
  enabled. Every result set, change count and error class must match.
- **sqnano-cli against the remote server.** SMP packets on a pipe and
  on a pseudo-terminal with the serial framing, results compared with
  SQLite.
- **Power cuts on the hybrid media.** One cut for the flash and the FRAM
  together, at every write and erase of either, with a FRAM small enough
  to evict all the time.
- **Power cuts on the file media.** Every chunk file update is cut (it
  happens or not), and POSIX files run the same database.
- **Power cuts on NAND.** A simulated chip cut at every program, erase and
  bad-block marking (and again during the recovery), including during the
  copies made for ECC refreshes and worn-out blocks; blocks wearing out
  during a workload; stored bit flips with the software and the chip ECC.
  The simulator checks that each page is programmed once per erase, in
  order within its block. The Zephyr test runs the layer over the flash
  simulator shaped as a NAND (no double writes).
- **OTA migration scenarios.** Upgrade, revert, cuts during migration and
  confirmation; a lazy migration read before and after conversion, power
  cuts during its background conversion, a revert that drops it, and a
  firmware without the conversion code that refuses the database until it
  is finished.
- **Coverage-guided fuzzing** ([tests/fuzz](tests/fuzz), libFuzzer with
  clang, `-DSQNANO_LIBFUZZER=ON`): database images with corrupted bytes
  (mounted, read, checked and written; the page CRC is skipped so that the
  tree code sees the damage), requests to the remote server, SQL given to
  `sqlite3.h`, and the key and record encodings (keys must sort with
  `memcmp()` as the values sort in SQLite). Without libFuzzer, a small
  driver runs the same targets on random mutations; ctest runs a short
  pass of each. CI fuzzes a few minutes per push and half an hour each
  night, keeping the corpus, and reruns the differential test with fresh
  seeds.
- **`WITHOUT ROWID` tables** in the differential test: composite keys with
  `NOCASE` and `DESC`, secondary and `UNIQUE` indexes, key updates, UPSERT,
  and cascading foreign keys between two such tables.

## License

Apache-2.0. See [LICENSE](LICENSE).
