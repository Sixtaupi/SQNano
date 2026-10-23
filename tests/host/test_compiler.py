#!/usr/bin/env python3
# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""Host compiler tests.

Usage: test_compiler.py NORMALIZE_TOOL
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
SQNANOC = os.path.join(ROOT, "tools", "sqnanoc")
sys.path.insert(0, SQNANOC)

import catalog as C  # noqa: E402
import gen as G  # noqa: E402
from extract import extract_file  # noqa: E402
from lex import SqlError, fnv1a, normalize  # noqa: E402

NORMALIZE_TOOL = None

CORPUS = [
    "SELECT ts, value FROM sensor WHERE id = :id ORDER BY ts DESC LIMIT :n;",
    "select  TS,value\nfrom SENSOR -- comment\n where ID=:id order by ts desc limit :n",
    "SELECT 'It''s' || \"Col\" FROM [T] WHERE `a`>=?1 AND b<>? -- x",
    "/* c */ INSERT INTO t VALUES (X'0aFF', 1.5E3, .5, 0x1F, -3, @p, $q);",
    "SELECT a->>'x', b->'y', c||d, e<<2, f>>1, g!=h, i==j FROM t",
    "SELECT 'unterminated",
    "select é, ÉTÉ from été where x = 'ÉTÉ'",
    "  ;  ",
    "SELECT 1; SELECT 2",
    "select 1e, 1e+, 1.e5, 12abc",
]


def c_normalize(texts):
    data = b"\0".join(t.encode() for t in texts) + b"\0"
    out = subprocess.run([NORMALIZE_TOOL], input=data, capture_output=True, check=True).stdout
    res = []
    for line in out.decode().splitlines():
        h, used, norm = (line.split(" ", 2) + [""])[:3]
        res.append((int(h, 16), int(used), norm))
    return res


def compile_text(schema: str, queries: str, version=None, extra=None):
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "schema"))
        with open(os.path.join(d, "schema", "v1.sql"), "w") as fh:
            fh.write(schema)
        for name, text in (extra or {}).items():
            with open(os.path.join(d, "schema", name), "w") as fh:
                fh.write(text)
        hist = C.load_schema(os.path.join(d, "schema"), version)
        g = G.Generator(hist, 1)
        for i, q in enumerate(queries.split(";\n")):
            if q.strip():
                g.add_query(q, f"q{i}", f"test:{i}")
        return g.generate()


SCHEMA = """
CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b TEXT COLLATE NOCASE, c REAL);
CREATE INDEX t_ab ON t(a, b);
"""


class Normalization(unittest.TestCase):
    def test_c_matches_python(self):
        got = c_normalize(CORPUS)
        for text, (h, used, norm) in zip(CORPUS, got):
            pn, pused = normalize(text)
            self.assertEqual(norm, pn, text)
            self.assertEqual(used, pused, text)
            self.assertEqual(h, fnv1a(pn), text)

    def test_spelling_insensitive(self):
        a = normalize(CORPUS[0])[0]
        b = normalize(CORPUS[1])[0]
        self.assertEqual(a, b)

    def test_literals_and_params_kept(self):
        n = normalize("select 'AbC', :Param from T")[0]
        self.assertEqual(n, "select 'AbC' , :Param from t")


class Planner(unittest.TestCase):
    def test_index_and_order(self):
        _, c, rep = compile_text(SCHEMA, "SELECT id FROM t WHERE a = ? ORDER BY b DESC")
        self.assertIn("SEARCH t USING INDEX t_ab (a=?)", rep)
        self.assertNotIn("sort in RAM", rep)
        self.assertIn("SQNANO_SCAN_DESC", c)

    def test_collation_mismatch_disables_index(self):
        _, _, rep = compile_text(SCHEMA, "SELECT id FROM t WHERE a = 1 AND b = ? COLLATE BINARY")
        self.assertIn("(a=?)", rep)
        self.assertNotIn("b=?", rep)

    def test_rowid_range(self):
        _, _, rep = compile_text(SCHEMA, "SELECT a FROM t WHERE id > ? AND id <= ?")
        self.assertIn("USING ROWID RANGE", rep)

    def test_sort_reported(self):
        _, _, rep = compile_text(SCHEMA, "SELECT id FROM t ORDER BY c")
        self.assertIn("sort in RAM", rep)

    def test_join_plan(self):
        _, _, rep = compile_text(SCHEMA, "SELECT t.id, u.c FROM t LEFT JOIN t AS u ON u.a = t.id "
                                         "WHERE t.id > ? ORDER BY t.id")
        self.assertIn("SEARCH t USING ROWID RANGE", rep)
        self.assertIn("SEARCH t USING INDEX t_ab (a=?)", rep)
        self.assertNotIn("sort in RAM", rep)

    def test_correlated_subquery_uses_index(self):
        _, c, rep = compile_text(SCHEMA, "SELECT id, (SELECT max(c) FROM t AS u WHERE u.a = t.id) "
                                         "FROM t")
        self.assertIn("SEARCH t USING INDEX t_ab (a=?)", rep)
        self.assertNotIn("u1_ok", c)  # correlated: evaluated for every row

    def test_constant_subquery_cached(self):
        _, c, _ = compile_text(SCHEMA, "SELECT id FROM t WHERE c > (SELECT avg(c) FROM t)")
        self.assertIn("u1_ok = 0;", c)

    def test_deterministic(self):
        one = compile_text(SCHEMA, "SELECT * FROM t WHERE b LIKE ?;\nINSERT INTO t (a) VALUES (?)")
        two = compile_text(SCHEMA, "SELECT * FROM t WHERE b LIKE ?;\nINSERT INTO t (a) VALUES (?)")
        self.assertEqual(one, two)


class Errors(unittest.TestCase):
    def check(self, sql, needle, schema=SCHEMA):
        with self.assertRaises(SqlError) as cm:
            compile_text(schema, sql)
        self.assertIn(needle, cm.exception.msg)

    def test_unknown_column(self):
        self.check("SELECT nope FROM t", "no such column: nope")

    def test_unknown_table(self):
        self.check("SELECT * FROM nope", "no such table: nope")

    def test_join(self):
        self.check("SELECT * FROM t RIGHT JOIN t AS u ON t.a = u.a", "RIGHT and FULL")
        self.check("SELECT a FROM t, t AS u", "ambiguous column name: a")
        self.check("SELECT t.a FROM t AS x", "no such column: t.a")
        self.check("SELECT * FROM t JOIN t AS u USING (nope)", "cannot join using column nope")

    def test_subquery(self):
        self.check("SELECT * FROM t WHERE a IN (SELECT a, b FROM t)", "sub-select returns 2 columns")
        self.check("SELECT (SELECT a FROM t GROUP BY a)", "GROUP BY and HAVING in a subquery")
        self.check("SELECT * FROM t WHERE a IN (SELECT a FROM t ORDER BY c LIMIT 2)",
                   "needs a matching index")

    def test_window(self):
        self.check("SELECT count(*) OVER () FROM t", "FILTER/OVER")

    def test_bare_column_in_group(self):
        self.check("SELECT a, c FROM t GROUP BY a", "must appear in the GROUP BY")

    def test_schema_mismatch(self):
        self.check("CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, a TEXT)", "differs")

    def test_without_rowid(self):
        w = "CREATE TABLE w (a INTEGER PRIMARY KEY, b TEXT) WITHOUT ROWID;"
        self.check("SELECT rowid FROM w", "no such column: rowid", schema=w)
        self.check("UPDATE w SET rowid = 1", "no such column: rowid", schema=w)
        self.check("SELECT 1", "PRIMARY KEY missing on table w",
                   schema="CREATE TABLE w (a, b) WITHOUT ROWID;")
        self.check("SELECT 1", "AUTOINCREMENT not allowed",
                   schema="CREATE TABLE w (a INTEGER PRIMARY KEY AUTOINCREMENT) WITHOUT ROWID;")
        self.check("SELECT 1", "cannot drop PRIMARY KEY column",
                   schema=w + "ALTER TABLE w DROP COLUMN a;")

    def test_lazy_migration(self):
        base = ("CREATE TABLE m (id INTEGER PRIMARY KEY, a INTEGER, b TEXT NOT NULL DEFAULT '', "
                "c REAL CHECK (c > 0), r INTEGER REFERENCES m(id));\nCREATE INDEX m_a ON m(a);\n")

        def lazy(sql):
            with self.assertRaises(SqlError) as cm:
                compile_text(base, "SELECT 1", extra={"v2.sql": "-- lazy\n", "v2_lazy.sql": sql})
            return cm.exception.msg

        self.assertIn("indexed column", lazy("UPDATE m SET a = a + 1;"))
        self.assertIn("may be NULL", lazy("UPDATE m SET b = upper(c);"))
        self.assertIn("CHECK constraint", lazy("UPDATE m SET c = c * 2;"))
        self.assertIn("foreign key column", lazy("UPDATE m SET r = NULL;"))
        self.assertIn("no subquery", lazy("UPDATE m SET c = (SELECT max(c) FROM m);"))
        self.assertIn("only UPDATE", lazy("DELETE FROM m;"))
        self.assertIn("INTEGER PRIMARY KEY", lazy("UPDATE m SET b = 'x' || id;"))
        # Accepted: the C code of the conversion is generated with the catalog.
        c = "".join(compile_text(base, "SELECT 1", extra={
            "v2.sql": "ALTER TABLE m ADD COLUMN d TEXT;\n",
            "v2_lazy.sql": "UPDATE m SET b = b || coalesce(a, 0), d = lower(b) WHERE a > 1;\n"}))
        self.assertIn("cat2_lazy0", c)
        self.assertIn(".lazy = 1", c)

    def test_unknown_function(self):
        self.check("SELECT date(a) FROM t", "no such function: date")


class Catalog(unittest.TestCase):
    def test_affinity(self):
        self.assertEqual(C.type_affinity("VARCHAR(10)"), C.AFF_TEXT)
        self.assertEqual(C.type_affinity("BIGINT"), C.AFF_INTEGER)
        self.assertEqual(C.type_affinity("DOUBLE PRECISION"), C.AFF_REAL)
        self.assertEqual(C.type_affinity(""), C.AFF_BLOB)
        self.assertEqual(C.type_affinity("DECIMAL(10,2)"), C.AFF_NUMERIC)
        self.assertEqual(C.type_affinity("FLOATING POINT"), C.AFF_INTEGER)  # "INT" wins

    def test_compat(self):
        hist = C.load_schema(os.path.join(HERE, "migrate", "schema"))
        self.assertEqual([c.compat for c in hist.versions], [1, 2, 2, 4, 4])
        steps = [s.op for s in hist.versions[1].steps]
        self.assertEqual(steps, ["create_tree", "build_index", "create_tree"])

    def test_drop_indexed_column(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "v1.sql"), "w") as fh:
                fh.write(SCHEMA)
            with open(os.path.join(d, "v2.sql"), "w") as fh:
                fh.write("ALTER TABLE t DROP COLUMN a;")
            with self.assertRaises(SqlError) as cm:
                C.load_schema(d)
            self.assertIn("cannot drop indexed column", cm.exception.msg)


class Extraction(unittest.TestCase):
    def test_literals_and_dynamic(self):
        with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as fh:
            fh.write('void f(sqlite3 *db, const char *s) {\n'
                     '  sqlite3_prepare_v2(db, "SELECT a "\n   "FROM t", -1, &st, 0);\n'
                     '  sqlite3_exec(db, s, 0, 0, 0);\n'
                     '  my_exec(db, "DELETE FROM t");\n'
                     '  // sqlite3_exec(db, "commented", 0, 0, 0);\n}\n')
        try:
            found = extract_file(fh.name, {"my_exec": 1})
        finally:
            os.unlink(fh.name)
        sqls = [(f.line, f.sql) for f in found]
        self.assertEqual(sqls, [(2, "SELECT a FROM t"), (4, None), (5, "DELETE FROM t")])


if __name__ == "__main__":
    NORMALIZE_TOOL = sys.argv.pop(1)
    unittest.main(verbosity=1)
