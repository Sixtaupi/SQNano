#!/usr/bin/env python3
# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""Differential test: SQNano against SQLite.

The same random sequence of statements (tests/host/diff/queries.sql, random
parameters) runs on SQLite and on the SQNano runner. Every result set,
change count and error class must match. Queries named o_* must return rows
in the same order; the others are compared as multisets.

Usage: difftest.py RUNNER [--seed N] [--rounds N] [--media nor|fram]
"""

from __future__ import annotations

import argparse
import math
import os
import random
import sqlite3
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "sqnanoc"))

from lex import split_statements  # noqa: E402
from parse import parse  # noqa: E402

TEXTS = ["a", "b", "abc", "ABC", "Abc", "12", "007", " 5", "5 ", "1e3", "3.0", "x%y", "hello",
         "", "-4", "zz", "a_b", "0x10", "é", "Hello World", "9223372036854775807", "k1", "k2",
         "K1", "k4"]
PATTERNS_LIKE = ["a%", "%b%", "_b_", "A%", "%", "%\\%%", "h_llo", "1%", ""]
PATTERNS_GLOB = ["a*", "*[0-9]*", "?b?", "[A-Z]*", "*", "a[^b]*", "h?llo"]


def gen_value(rng: random.Random, name: str):
    r = rng.random()
    if name in ("id",):
        return rng.choice([None, rng.randint(1, 60), rng.randint(1, 60), rng.randint(1, 1100)])
    if name in ("lo", "hi", "x"):
        return rng.choice([rng.randint(-25, 70), rng.randint(-25, 70), 2.5, "7", None])
    if name in ("n", "o", "cnt"):
        return rng.choice([0, 1, 2, 3, 5, 10, -1, "2"])
    if name == "m":
        return rng.randint(2, 4)
    if name in ("s", "l"):
        return rng.randint(-4, 6)
    if name == "p":
        return rng.choice(PATTERNS_LIKE + PATTERNS_GLOB)
    if name == "k":
        return rng.choice(["k1", "k2", "K1", "k3", "", "k4"])
    if name in ("pk", "pid"):
        return rng.choice([None, rng.randint(1, 12), rng.randint(1, 12), rng.randint(1, 40), "3", 2.0])
    if name == "grp":
        return rng.randint(0, 4)
    if name == "name" and r < 0.8:
        return rng.choice(["n1", "n2", "n3", "N1", "n4", None])
    if name == "v" and r < 0.7:
        return rng.randint(-5, 30)
    if name == "w":
        return rng.choice([None, 1.5, 0, -2.0, 10, "3"])
    if name == "d" and r < 0.2:
        return rng.choice([b"\x00\x01", b"", b"ab", rng.randint(-3, 3)])
    if name in ("d", "note") and r < 0.35:
        # Longer than a cell of a 512-byte page: overflow pages.
        n = rng.randint(150, 330)
        return rng.choice([("long " * 80)[:n], (bytes(range(256)) * 2)[:n]])
    if r < 0.12:
        return None
    if r < 0.45:
        return rng.randint(-20, 20)
    if r < 0.6:
        return rng.choice([0.5, 2.0, -3.25, 1e10, 7.0, 0.1, 123.456, -0.0])
    if r < 0.95:
        return rng.choice(TEXTS)
    return rng.choice([2**62, -(2**63), 2**63 - 1, 1 << 53])


def enc(v) -> str:
    if v is None:
        return "n"
    if isinstance(v, bool):
        return f"i:{int(v)}"
    if isinstance(v, int):
        return f"i:{v}"
    if isinstance(v, float):
        return f"r:{v.hex()}"
    if isinstance(v, str):
        return "t:" + v.encode().hex()
    return "b:" + bytes(v).hex()


def dec(tok: str):
    if tok == "n":
        return None
    kind, _, val = tok.partition(":")
    if kind == "i":
        return int(val)
    if kind == "r":
        return float.fromhex(val)
    if kind == "t":
        return bytes.fromhex(val).decode("utf-8", "replace")
    return bytes.fromhex(val)


def same_value(a, b) -> bool:
    if isinstance(a, float) and isinstance(b, float):
        if math.isnan(a) and math.isnan(b):
            return True
        return a == b or abs(a - b) <= 1e-9 * max(abs(a), abs(b))
    if type(a) is not type(b):
        return False
    return a == b


def same_rows(a, b, ordered: bool) -> bool:
    if len(a) != len(b):
        return False
    if not ordered:
        key = lambda row: tuple((type(v).__name__, repr(v)) for v in row)  # noqa: E731
        a = sorted(a, key=key)
        b = sorted(b, key=key)
    return all(len(x) == len(y) and all(same_value(p, q) for p, q in zip(x, y))
               for x, y in zip(a, b))


class Runner:
    def __init__(self, path: str, media: str):
        self.p = subprocess.Popen([path, media], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True)

    def run(self, line: str):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        rows = []
        while True:
            out = self.p.stdout.readline()
            if not out:
                raise RuntimeError("runner died")
            parts = out.rstrip("\n").split(" ")
            if parts[0] == "R":
                rows.append(tuple(dec(t) for t in parts[1:]))
            elif parts[0] == "D":
                return rows, ("ok", int(parts[1]))
            else:
                return rows, ("err", int(parts[1]), " ".join(parts[2:]))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("runner")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rounds", type=int, default=3000)
    ap.add_argument("--media", default="nor")
    ap.add_argument("--dir", default=os.path.join(HERE, "diff"))
    ap.add_argument("--check-every", type=int, default=0, help="integrity check period")
    ap.add_argument("--trace", action="store_true")
    ap.add_argument("--stats", action="store_true", help="rows returned per query")
    ap.add_argument("--tolerate-full", action="store_true",
                    help="accept SQLITE_FULL from SQNano (small flash): skip the statement")
    ap.add_argument("--tolerate-sort-full", action="store_true",
                    help="accept a sort larger than the free pages (long runs, documented limit)")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    ref = sqlite3.connect(":memory:", isolation_level=None)
    ref.execute("PRAGMA foreign_keys = ON")
    with open(os.path.join(args.dir, "schema", "v1.sql"), encoding="utf-8") as fh:
        ref.executescript(fh.read())
    queries = {}
    with open(os.path.join(args.dir, "queries.sql"), encoding="utf-8") as fh:
        for sql, _ in split_statements(fh.read()):
            name = None
            for line in sql.splitlines():
                if "-- name:" in line:
                    name = line.split("-- name:")[1].strip()
            if name:
                _, params = parse(sql)
                queries[name] = (sql, params)
    names = sorted(queries)
    weights = [6 if n.startswith(("ins", "u_put", "rep", "ign")) else 4 if n.endswith("_ins") else
               2 if n.startswith(("upd", "del", "u_")) else 1 for n in names]
    runner = Runner(args.runner, args.media)
    stats = {"ok": 0, "err": 0}
    per_query: dict[str, int] = {}
    sps: list[str] = []
    nsp = 0

    def txn(cmd: str, sql: str) -> bool:
        _, res = runner.run(cmd)
        if args.trace:
            print(f"   {sql} -> {res}")
        try:
            ref.execute(sql)
            want = ("ok", 0)
        except sqlite3.Error as e:
            want = ("err", str(e))
        if (res == ("ok", 0)) != (want[0] == "ok"):
            print(f"MISMATCH on {sql}: sqlite {want}, sqnano {res}")
            return False
        return True

    def end_txn() -> bool:
        """COMMIT, or ROLLBACK when deferred constraints refuse the commit."""
        if not txn("M", "COMMIT"):
            return False
        return not ref.in_transaction or txn("K", "ROLLBACK")

    for rnd in range(args.rounds):
        if not ref.in_transaction:
            sps.clear()
        r = rng.random()
        if rnd % 500 == 499:
            if ref.in_transaction and not end_txn():
                return 1
            sps.clear()
            runner.run("X")
            _, res = runner.run("C")
            if res != ("ok", 0):
                print(f"round {rnd}: integrity check failed: {res}")
                return 1
        elif not ref.in_transaction and r < 0.02:
            if not txn("B", "BEGIN"):
                return 1
        elif ref.in_transaction and r < 0.10:
            if r < 0.015:
                ok = txn("M", "COMMIT")
                if not ref.in_transaction:
                    sps.clear()
            elif r < 0.025:
                ok = txn("K", "ROLLBACK")
                sps.clear()
            elif r < 0.06 or not sps:
                if len(sps) < 6:
                    nsp += 1
                    name = f"s{nsp % 4}"
                    sps.append(name)
                    ok = txn("S " + name, "SAVEPOINT " + name)
                else:
                    ok = True
            elif r < 0.085:
                name = rng.choice(sps)
                i = len(sps) - 1 - sps[::-1].index(name)
                del sps[i + 1:]
                ok = txn("T " + name, "ROLLBACK TO " + name)
            else:
                name = rng.choice(sps)
                i = len(sps) - 1 - sps[::-1].index(name)
                del sps[i:]
                ok = txn("L " + name, "RELEASE " + name)
            if not ok:
                return 1
            continue
        if args.check_every and rnd % args.check_every == 0:
            _, res = runner.run("C")
            if res != ("ok", 0):
                print(f"round {rnd}: integrity check failed: {res}")
                return 1
        name = rng.choices(names, weights)[0]
        sql, params = queries[name]
        values = [gen_value(rng, (p or "?").lstrip(":@$?")) for p in params]
        line = "Q " + name + "".join(" " + enc(v) for v in values)
        got_rows, got = runner.run(line)
        if args.trace:
            print(f"{rnd} {'T' if ref.in_transaction else ' '}{len(sps)} {line} -> {got}")
        bind = {p[1:]: v for p, v in zip(params, values)}
        if args.tolerate_full and got[0] == "err" and got[1] == 13:
            # Out of erased flash: the statement was undone, as if SQLite never ran it.
            stats["full"] = stats.get("full", 0) + 1
            continue
        if args.tolerate_sort_full and got[0] == "err" and got[1] == 13 and "sort" in got[2]:
            # The table outgrew what a cross join can sort: the statement did nothing.
            stats["sortfull"] = stats.get("sortfull", 0) + 1
            continue
        try:
            cur = ref.execute(sql, bind)
            want_rows = [tuple(r) for r in cur.fetchall()]
            want = ("ok", cur.rowcount if cur.rowcount >= 0 else 0)
        except sqlite3.IntegrityError as e:
            want_rows, want = [], ("err", 19, str(e))
            if not ref.in_transaction:
                sps.clear()
        except (sqlite3.OperationalError, sqlite3.DataError) as e:
            msg = str(e)
            want_rows, want = [], ("err", 20 if "mismatch" in msg else 1, msg)
        problem = None
        if got[0] != want[0]:
            problem = "status"
        elif got[0] == "err":
            if got[1] != want[1]:
                problem = "error code"
        else:
            returning = "returning" in sql.lower()
            is_write = sql.lstrip().split()[0].lower() in ("insert", "update", "delete") or \
                "\nINSERT" in sql or "\nUPDATE" in sql or "\nDELETE" in sql
            if name.endswith("_ci"):
                # Which of the rows equal under NOCASE is kept depends on the scan order.
                def low(rows):
                    return [tuple(v.lower() if isinstance(v, str) else v for v in r) for r in rows]
                got_rows, want_rows = low(got_rows), low(want_rows)
            if not same_rows(got_rows, want_rows, name.startswith("o_")):
                problem = "rows"
                if os.environ.get("DIFF_DEBUG"):
                    print("got ", sorted(map(repr, got_rows)))
                    print("want", sorted(map(repr, want_rows)))
            elif is_write and not returning and got[1] != want[1]:
                problem = "changes"
        if problem:
            print(f"MISMATCH ({problem}) at round {rnd}: {name}")
            print(f"  sql:    {' '.join(sql.split())}")
            print(f"  params: {values}")
            k = 0
            while k < min(len(want_rows), len(got_rows)) and want_rows[k] == got_rows[k]:
                k += 1
            print(f"  rows: sqlite {len(want_rows)}, sqnano {len(got_rows)}, first difference at {k}")
            print(f"  sqlite: {want} {want_rows[max(0, k - 2):k + 8]}")
            print(f"  sqnano: {got} {got_rows[max(0, k - 2):k + 8]}")
            if os.environ.get("DIFF_DEBUG"):
                ws, gs = [repr(r) for r in want_rows], [repr(r) for r in got_rows]
                print(f"  only sqlite: {[r for r in ws if r not in gs]}")
                print(f"  only sqnano: {[r for r in gs if r not in ws]}")
            return 1
        stats[got[0]] += 1
        per_query[name] = per_query.get(name, 0) + len(got_rows)
        if os.environ.get("DIFF_EACH"):
            # Locate the first statement after which the contents differ.
            g, _ = runner.run("Q o_all")
            w = [tuple(r) for r in ref.execute(queries["o_all"][0]).fetchall()]
            if not same_rows(g, w, True):
                print(f"contents differ after round {rnd}: {line}")
                for a_, b_ in zip(g, w):
                    if not same_rows([a_], [b_], True):
                        print("  sqnano", a_)
                        print("  sqlite", b_)
                        break
                return 1
    if args.stats:
        for k in sorted(per_query):
            print(f"  {k}: {per_query[k]} rows")
    if ref.in_transaction and not end_txn():
        return 1
    _, res = runner.run("C")
    if res != ("ok", 0):
        print(f"final integrity check failed: {res}")
        return 1
    final_rows, _ = runner.run("Q o_all")
    want_rows = [tuple(r) for r in ref.execute(queries["o_all"][0]).fetchall()]
    if not same_rows(final_rows, want_rows, True):
        print("final content differs")
        return 1
    full = f", {stats['full']} out of flash space" if stats.get("full") else ""
    if stats.get("sortfull"):
        full += f", {stats['sortfull']} sorts larger than the free pages"
    print(f"difftest seed {args.seed} media {args.media}: {args.rounds} statements, "
          f"{stats['ok']} ok, {stats['err']} errors{full}, {len(want_rows)} final rows: identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
