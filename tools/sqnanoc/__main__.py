# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""sqnanoc: SQNano host SQL compiler.

  sqnanoc compile --schema DIR [--queries FILE]... [--extract SRC]... --out DIR
  sqnanoc check   --schema DIR [--queries FILE]... SRC...
  sqnanoc normalize SQL
"""

from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import catalog as C  # noqa: E402
import gen as G  # noqa: E402
import parse as P  # noqa: E402
from extract import extract_file  # noqa: E402
from lex import SqlError, fnv1a, normalize, split_statements  # noqa: E402

_NAME = re.compile(r"--\s*name:\s*([A-Za-z_][A-Za-z0-9_]*)")


def read_queries(path: str):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    data = text.encode()
    for sql, off in split_statements(text):
        line = data[:off].count(b"\n") + 1
        m = _NAME.search(sql)
        name = m.group(1) if m else None
        # Point at the first line of the statement itself, not its comments.
        lead = len(sql) - len(sql.lstrip())
        body = sql.lstrip()
        while body.startswith("--"):
            nl = body.find("\n")
            lead += nl + 1 if nl >= 0 else len(body)
            body = body[nl + 1 :].lstrip() if nl >= 0 else ""
        line += sql[:lead].count("\n")
        yield sql, name, f"{path}:{line}"


def gather(gen: G.Generator, args, report_dynamic: list) -> None:
    for path in args.queries or []:
        for sql, name, where in read_queries(path):
            try:
                gen.add_query(sql, name, where)
            except SqlError as e:
                raise SqlError(f"{where}: {e.msg}") from None
    for path in args.manifest or []:
        for sql, _, where in read_queries(path):
            gen.add_query(sql, None, where)
    wrappers = {}
    for w in args.wrapper or []:
        name, _, pos = w.partition(":")
        wrappers[name] = int(pos or "2") - 1
    for path in args.extract or []:
        for f in extract_file(path, wrappers):
            where = f"{f.file}:{f.line}"
            if f.sql is None:
                report_dynamic.append((where, f.func, f.arg))
                continue
            for sql, _ in split_statements(f.sql):
                gen.add_query(sql, None, where)


def write_if_changed(path: str, content: str):
    try:
        with open(path, encoding="utf-8") as fh:
            if fh.read() == content:
                return
    except OSError:
        pass
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(content)


def cmd_compile(args) -> int:
    hist = C.load_schema(args.schema, args.version)
    gen = G.Generator(hist, args.migrate_from, args.prefix)
    dynamic: list = []
    gather(gen, args, dynamic)
    h, c, rep = gen.generate()
    os.makedirs(args.out, exist_ok=True)
    write_if_changed(os.path.join(args.out, f"{args.prefix}.h"), h)
    write_if_changed(os.path.join(args.out, f"{args.prefix}.c"), c)
    write_if_changed(os.path.join(args.out, f"{args.prefix}_report.txt"), rep)
    for w in hist.current.warnings:
        print(f"sqnanoc: warning: {w}", file=sys.stderr)
    if args.verbose:
        for w in gen.warnings:
            print(f"sqnanoc: note: {w}", file=sys.stderr)
    for where, func, arg in dynamic:
        print(f"{where}: warning: dynamic SQL passed to {func}() cannot be precompiled: "
              f"{arg}", file=sys.stderr)
    return 0


def infer_schema(args) -> tuple[C.SchemaHistory, list[str], list[str]]:
    """Build a v1 schema from the CREATE statements found in the sources."""
    cat = C.Catalog(version=1, compat=1)
    b = C.CatalogBuilder(cat)
    creates, later = [], []
    wrappers = {}
    for w in args.wrapper or []:
        name, _, pos = w.partition(":")
        wrappers[name] = int(pos or "2") - 1
    for path in args.extract or []:
        for f in extract_file(path, wrappers):
            if f.sql is None:
                continue
            for sql, _ in split_statements(f.sql):
                head = normalize(sql)[0]
                if head.startswith(("create table", "create index", "create unique index")):
                    try:
                        b.apply(sql, f"{f.file}:{f.line}")
                        creates.append(sql.strip())
                    except SqlError as e:
                        later.append(f"{f.file}:{f.line}: {e.msg}")
                elif head.startswith(("alter table", "drop table", "drop index")):
                    later.append(f"{f.file}:{f.line}: {sql.strip()}")
    return C.SchemaHistory([cat]), creates, later


def cmd_check(args) -> int:
    args.extract = (args.extract or []) + args.sources
    inferred = None
    if args.schema:
        hist = C.load_schema(args.schema, args.version)
    else:
        hist, creates, later = infer_schema(args)
        inferred = (creates, later)
    gen = G.Generator(hist, args.migrate_from, args.prefix)
    dynamic: list = []
    accepted, refused = [], []
    try:
        gather(gen, args, dynamic)
    except SqlError as e:
        refused.append((str(e.msg), ""))
    for q in gen.queries:
        cname = f"q_{G.cident(q.name)}" if q.name else f"qa_{q.hash:08x}"
        try:
            g = gen.compile_query(q, hist.current, cname)
            if q.kind == "FAIL":
                refused.append((q.source, "schema change outside migrations: move it to "
                                "schema/vN.sql" + f"\n      {q.norm}"))
            else:
                accepted.append((q, g))
        except SqlError as e:
            refused.append((q.source, e.msg + f"\n      {q.norm}"))
    gen.warnings = [w for w in gen.warnings if "schema change outside migrations" not in w]
    print("SQNano compatibility report")
    if inferred is not None:
        creates, later = inferred
        print(f"  schema inferred from {len(creates)} CREATE statements found in the sources")
        if later:
            print("  schema changes to move into versioned migrations (schema/v2.sql, ...):")
            for item in later:
                print(f"    {item}")
        if args.emit_schema and creates:
            os.makedirs(args.emit_schema, exist_ok=True)
            out = os.path.join(args.emit_schema, "v1.sql")
            with open(out, "w", encoding="utf-8") as fh:
                fh.write("-- Inferred by sqnanoc check; review before use.\n")
                for c in creates:
                    fh.write(c.rstrip(";") + ";\n\n")
            print(f"  wrote {out}")
    print(f"  schema version {hist.current.version}, {len(hist.current.tables)} tables")
    print(f"  accepted statements: {len(accepted)}")
    for q, g in accepted:
        plan = "; ".join(q.report) if q.report else q.kind.lower()
        print(f"    {q.source}: {plan}")
    print(f"  refused statements: {len(refused)}")
    for where, why in refused:
        print(f"    {where}: {why}")
    print(f"  dynamic SQL to rewrite as parameterized queries: {len(dynamic)}")
    for where, func, arg in dynamic:
        print(f"    {where}: {func}(..., {arg}, ...)")
    for w in hist.current.warnings + gen.warnings:
        print(f"  warning: {w}")
    return 1 if refused or dynamic else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="sqnanoc", description="SQNano host SQL compiler")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, schema_required=True):
        p.add_argument("--schema", required=schema_required,
                       help="schema directory (v1.sql, v2.sql, ...)")
        p.add_argument("--queries", action="append", help="queries.sql file ('-- name: x')")
        p.add_argument("--manifest", action="append", help="captured statements manifest")
        p.add_argument("--extract", action="append", help="C source to scan for SQL literals")
        p.add_argument("--wrapper", action="append",
                       help="application function taking SQL, as NAME:ARG (1-based position)")
        p.add_argument("--version", type=int,
                       help="build for this schema version (default: the latest file)")
        p.add_argument("--migrate-from", type=int, default=1,
                       help="oldest schema version the firmware can migrate from")
        p.add_argument("--prefix", default="sqnano_gen", help="output file prefix")

    pc = sub.add_parser("compile", help="generate C code")
    common(pc)
    pc.add_argument("--out", required=True, help="output directory")
    pc.add_argument("-v", "--verbose", action="store_true")
    pk = sub.add_parser("check", help="porting report for an application")
    common(pk, schema_required=False)
    pk.add_argument("--emit-schema", help="without --schema: write the inferred v1.sql here")
    pk.add_argument("sources", nargs="*", help="application C sources")
    pn = sub.add_parser("normalize", help="print the normalized form and hash of a statement")
    pn.add_argument("sql")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "compile":
            return cmd_compile(args)
        if args.cmd == "check":
            return cmd_check(args)
        norm, _ = normalize(args.sql)
        print(norm)
        print(f"0x{fnv1a(norm):08x}")
        return 0
    except SqlError as e:
        print(f"sqnanoc: error: {e.msg}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"sqnanoc: error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
