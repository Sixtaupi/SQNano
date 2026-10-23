# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""Versioned catalog: tables, columns, indexes and migrations.

Schema files are applied in order (v1.sql, v2.sql, ...). Each version
records the structural steps that lead to it from the previous one, and the
oldest version whose firmware can still use a database of this version
(expand/contract compatibility).

Tree ids and record slots are allocated once and never reused, so that a
record written by any firmware version can be decoded by any other one.
"""

from __future__ import annotations

import copy
import os
import re
from dataclasses import dataclass, field

import parse as P
from lex import SqlError, fnv1a, normalize, split_statements

AFF_NONE = 0
AFF_BLOB = "A"
AFF_TEXT = "B"
AFF_NUMERIC = "C"
AFF_INTEGER = "D"
AFF_REAL = "E"

COLLATIONS = {"binary": 0, "nocase": 1, "rtrim": 2}


def type_affinity(typename: str) -> str:
    """SQLite column affinity rules (section 3.1 of the datatype documentation)."""
    t = typename.upper()
    if "INT" in t:
        return AFF_INTEGER
    if "CHAR" in t or "CLOB" in t or "TEXT" in t:
        return AFF_TEXT
    if "BLOB" in t or not t:
        return AFF_BLOB
    if "REAL" in t or "FLOA" in t or "DOUB" in t:
        return AFF_REAL
    return AFF_NUMERIC


@dataclass
class Column:
    name: str
    decltype: str
    aff: str
    slot: int
    notnull: bool = False
    default: P.Lit | None = None
    coll: str = "binary"
    ipk: bool = False

    @property
    def key(self) -> str:
        return self.name.lower()


@dataclass
class Index:
    name: str
    tree: int
    table: str
    cols: list[tuple[str, str, bool]]  # column name, collation, desc
    unique: bool
    auto: bool
    sql: str


@dataclass
class Check:
    name: str | None
    expr: P.Expr
    text: str


@dataclass
class ForeignKey:
    cols: list[str]  # child columns
    parent: str
    parent_cols: list[str]  # empty: the parent's primary key
    on_delete: str = "no action"
    on_update: str = "no action"
    deferred: bool = False


@dataclass
class Table:
    name: str
    tree: int
    cols: list[Column]
    checks: list[Check]
    sql: str
    next_slot: int
    autoinc: bool = False
    nauto: int = 0
    fks: list = field(default_factory=list)
    pk: list = field(default_factory=list)  # PRIMARY KEY column names
    without_rowid: bool = False
    pk_index: "Index | None" = None  # WITHOUT ROWID: the key of the table tree

    def col(self, name: str) -> int | None:
        name = name.lower()
        for i, c in enumerate(self.cols):
            if c.key == name:
                return i
        return None

    @property
    def ipk(self) -> int | None:
        for i, c in enumerate(self.cols):
            if c.ipk:
                return i
        return None


@dataclass
class Step:
    op: str  # create_tree, drop_tree, build_index
    tree: int
    table: str | None = None
    index: str | None = None


@dataclass
class Catalog:
    version: int = 0
    tables: dict[str, Table] = field(default_factory=dict)
    indexes: dict[str, Index] = field(default_factory=dict)
    next_tree: int = 1
    compat: int = 0
    steps: list[Step] = field(default_factory=list)
    up: list[tuple[str, str, int]] = field(default_factory=list)  # (sql, file, offset)
    lazy: list[tuple[str, str, int]] = field(default_factory=list)  # vN_lazy.sql
    expand_only: bool = True
    ddl_hashes: dict[int, str] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)

    def table(self, name: str) -> Table:
        t = self.tables.get(name.lower())
        if t is None:
            raise SqlError(f"no such table: {name}")
        return t

    def table_indexes(self, t: Table) -> list[Index]:
        return sorted((i for i in self.indexes.values() if i.table == t.name.lower()),
                      key=lambda i: i.tree)

    def canonical(self) -> str:
        out = []
        for t in sorted(self.tables.values(), key=lambda t: t.tree):
            cols = ",".join(
                f"{c.key}:{c.slot}:{c.aff}:{int(c.notnull)}:{c.coll}:{int(c.ipk)}:"
                f"{None if c.default is None else (c.default.kind, repr(c.default.value))}"
                for c in t.cols)
            checks = ";".join(normalize(c.text)[0] for c in t.checks)
            wr = ""
            if t.without_rowid:
                wr = " W[" + ",".join(f"{c}:{k}:{int(d)}" for c, k, d in t.pk_index.cols) + "]"
            out.append(f"T {t.name.lower()} {t.tree} {int(t.autoinc)} [{cols}] [{checks}]{wr}")
        for i in sorted(self.indexes.values(), key=lambda i: i.tree):
            cols = ",".join(f"{c}:{k}:{int(d)}" for c, k, d in i.cols)
            out.append(f"I {i.name.lower()} {i.tree} {i.table} {int(i.unique)} [{cols}]")
        return "\n".join(out)

    @property
    def hash(self) -> int:
        return fnv1a(self.canonical()) or 1


def strip_if_not_exists(norm: str) -> str:
    for prefix in ("create table if not exists ", "create index if not exists ",
                   "create unique index if not exists "):
        if norm.startswith(prefix):
            return prefix[: -len("if not exists ")] + norm[len(prefix):]
    return norm


def ddl_hash(sql: str) -> int:
    return fnv1a(strip_if_not_exists(normalize(sql)[0]))


def const_value(e: P.Expr) -> P.Lit:
    """Fold a constant DEFAULT expression."""
    if isinstance(e, P.Lit):
        return e
    if isinstance(e, P.Unary) and e.op in "-+":
        v = const_value(e.e)
        if v.kind in ("int", "real"):
            return P.Lit(v.kind, -v.value if e.op == "-" else v.value)
    if isinstance(e, P.Binary) and e.op in ("+", "-", "*"):
        a, b = const_value(e.l), const_value(e.r)
        if a.kind in ("int", "real") and b.kind in ("int", "real"):
            r = {"+": a.value + b.value, "-": a.value - b.value, "*": a.value * b.value}[e.op]
            kind = "int" if a.kind == b.kind == "int" else "real"
            return P.Lit(kind, r)
    raise SqlError("DEFAULT must be a constant", e.pos)


def apply_default_affinity(lit: P.Lit, aff: str) -> P.Lit:
    """Defaults are stored with the column affinity applied, as SQLite does."""
    if lit.kind == "null" or lit.kind == "blob":
        return lit
    if aff in (AFF_INTEGER, AFF_NUMERIC, AFF_REAL):
        v = lit.value
        if lit.kind == "text":
            try:
                txt = str(v).strip()
                v = int(txt) if re.fullmatch(r"[+-]?\d+", txt) else float(txt)
            except ValueError:
                return lit
        if aff == AFF_REAL:
            return P.Lit("real", float(v))
        if isinstance(v, float) and v.is_integer() and -(2**63) <= v < 2**63:
            return P.Lit("int", int(v))
        return P.Lit("int" if isinstance(v, int) else "real", v)
    if aff == AFF_TEXT and lit.kind in ("int", "real"):
        v = lit.value
        if isinstance(v, float):
            txt = repr(v)
            if "e" not in txt and "." not in txt:
                txt += ".0"
        else:
            txt = str(v)
        return P.Lit("text", txt)
    return lit


def expr_columns(e, acc: set[str]):
    if isinstance(e, P.Col):
        acc.add(e.name)
    for v in vars(e).values() if isinstance(e, P.Expr) else ():
        if isinstance(v, P.Expr):
            expr_columns(v, acc)
        elif isinstance(v, list):
            for it in v:
                if isinstance(it, P.Expr):
                    expr_columns(it, acc)
                elif isinstance(it, tuple):
                    for x in it:
                        if isinstance(x, P.Expr):
                            expr_columns(x, acc)
        elif isinstance(v, tuple):
            for x in v:
                if isinstance(x, P.Expr):
                    expr_columns(x, acc)


def rename_columns(e, old: str, new: str):
    if isinstance(e, P.Col) and e.name == old:
        e.name = new
    if isinstance(e, P.Expr):
        for v in vars(e).values():
            items = v if isinstance(v, list) else [v]
            for it in items:
                if isinstance(it, tuple):
                    for x in it:
                        rename_columns(x, old, new)
                else:
                    rename_columns(it, old, new)


class CatalogBuilder:
    def __init__(self, cat: Catalog):
        self.cat = cat

    def coll(self, name: str | None, pos=None) -> str:
        if name is None:
            return "binary"
        if name.lower() not in COLLATIONS:
            raise SqlError(f"no such collation sequence: {name}", pos)
        return name.lower()

    def add_index(self, t: Table, name: str, cols, unique: bool, auto: bool, sql: str,
                  existing: bool) -> Index:
        cat = self.cat
        if name.lower() in cat.indexes:
            raise SqlError(f"index {name} already exists")
        if name.lower() in cat.tables:
            raise SqlError(f"there is already a table named {name}")
        resolved = []
        for cname, coll, desc in cols:
            ci = t.col(cname)
            if ci is None:
                if cname.lower() in ("rowid", "oid", "_rowid_"):
                    raise SqlError("indexes on rowid are not supported")
                raise SqlError(f"no such column: {cname}")
            c = t.cols[ci]
            resolved.append((c.key, self.coll(coll) if coll else c.coll, desc))
        idx = Index(name, cat.next_tree, t.name.lower(), resolved, unique, auto, sql)
        cat.next_tree += 1
        cat.indexes[name.lower()] = idx
        cat.steps.append(Step("create_tree", idx.tree))
        if existing:
            cat.steps.append(Step("build_index", idx.tree, t.name.lower(), name.lower()))
            cat.expand_only = False
        return idx

    def create_table(self, st: P.CreateTable, sql: str):
        cat = self.cat
        key = st.name.lower()
        if key in cat.tables:
            if st.if_not_exists:
                return
            raise SqlError(f"table {st.name} already exists")
        if key in cat.indexes:
            raise SqlError(f"there is already an index named {st.name}")
        if key.startswith("sqlite_"):
            raise SqlError(f"object name reserved for internal use: {st.name}")
        if not st.cols:
            raise SqlError("a table needs at least one column")
        t = Table(st.name, cat.next_tree, [], [], sql.strip(), 0)
        cat.next_tree += 1
        pk_cols = None
        autoinc = False
        uniques = []
        for cd in st.cols:
            if t.col(cd.name) is not None:
                raise SqlError(f"duplicate column name: {cd.name}")
            aff = type_affinity(cd.typename)
            c = Column(cd.name, cd.typename, aff, t.next_slot, cd.notnull,
                       None, self.coll(cd.collate))
            t.next_slot += 1
            if cd.default is not None:
                c.default = apply_default_affinity(const_value(cd.default), aff)
            if cd.pk:
                if pk_cols is not None:
                    raise SqlError(f'table "{st.name}" has more than one primary key')
                pk_cols = [(cd.name, None, cd.pk_desc)]
                autoinc = cd.autoinc
            if cd.unique:
                uniques.append([(cd.name, None, False)])
            for name, e, text in cd.check:
                t.checks.append(Check(name, e, text))
            if cd.fk:
                ref, rcols, acts = cd.fk
                t.fks.append(ForeignKey([cd.name], ref, rcols, acts["delete"], acts["update"],
                                        acts["deferred"]))
            t.cols.append(c)
        for tc in st.constraints:
            if tc.kind == "pk":
                if pk_cols is not None:
                    raise SqlError(f'table "{st.name}" has more than one primary key')
                pk_cols = tc.cols
                autoinc = tc.autoinc
            elif tc.kind == "unique":
                uniques.append(tc.cols)
            elif tc.kind == "check":
                t.checks.append(Check(tc.name, tc.check, tc.check_text))
            else:
                if tc.ref_cols and len(tc.ref_cols) != len(tc.cols):
                    raise SqlError(f"foreign key on {tc.cols[0][0]} should reference only one "
                                   f"column of table {tc.ref}" if len(tc.cols) == 1 else
                                   "number of columns in foreign key does not match the number of "
                                   "columns in the referenced table")
                t.fks.append(ForeignKey([n for n, _, _ in tc.cols], tc.ref, list(tc.ref_cols),
                                        tc.actions["delete"], tc.actions["update"],
                                        tc.actions["deferred"]))
        for ch in t.checks:
            used: set[str] = set()
            expr_columns(ch.expr, used)
            for u in used:
                if t.col(u) is None:
                    raise SqlError(f"no such column: {u}", ch.expr.pos)
        ipk_done = False
        if pk_cols is not None:
            for n, _, _ in pk_cols:
                if t.col(n) is None:
                    raise SqlError(f"no such column: {n}")
            t.pk = [n for n, _, _ in pk_cols]
            if len(pk_cols) == 1 and not st.without_rowid:
                c = t.cols[t.col(pk_cols[0][0])]
                if c.decltype.upper() == "INTEGER" and not pk_cols[0][2]:
                    c.ipk = True
                    c.aff = AFF_INTEGER
                    ipk_done = True
            if autoinc and not st.without_rowid:
                if not ipk_done:
                    raise SqlError("AUTOINCREMENT is only allowed on an INTEGER PRIMARY KEY")
                t.autoinc = True
        if st.without_rowid:
            if pk_cols is None:
                raise SqlError(f"PRIMARY KEY missing on table {st.name}")
            if autoinc:
                raise SqlError("AUTOINCREMENT not allowed on WITHOUT ROWID tables")
            t.without_rowid = True
            spec = []
            for n, coll, desc in pk_cols:
                c = t.cols[t.col(n)]
                # As SQLite: the PRIMARY KEY of a WITHOUT ROWID table is NOT NULL.
                c.notnull = True
                if any(k == c.key for k, _, _ in spec):
                    continue
                spec.append((c.key, self.coll(coll) if coll else c.coll, bool(desc)))
            t.nauto += 1
            t.pk_index = Index(f"sqlite_autoindex_{st.name}_{t.nauto}", t.tree, key, spec, True,
                               True, "")
        cat.tables[key] = t
        cat.steps.append(Step("create_tree", t.tree))
        if pk_cols is not None and not ipk_done and not st.without_rowid:
            t.nauto += 1
            self.add_index(t, f"sqlite_autoindex_{st.name}_{t.nauto}", pk_cols, True, True, "",
                           False)
        for u in uniques:
            t.nauto += 1
            self.add_index(t, f"sqlite_autoindex_{st.name}_{t.nauto}", u, True, True, "", False)
        cat.ddl_hashes[ddl_hash(sql)] = sql.strip()

    def create_index(self, st: P.CreateIndex, sql: str):
        cat = self.cat
        if st.name.lower() in cat.indexes:
            if st.if_not_exists:
                return
            raise SqlError(f"index {st.name} already exists")
        t = cat.table(st.table)
        existing = t.tree not in [s.tree for s in cat.steps if s.op == "create_tree"]
        self.add_index(t, st.name, st.cols, st.unique, False, sql.strip(), existing)
        cat.ddl_hashes[ddl_hash(sql)] = sql.strip()

    def alter(self, st: P.AlterTable):
        cat = self.cat
        t = cat.table(st.table)
        if st.action == "add":
            cd = st.coldef
            if t.col(cd.name) is not None:
                raise SqlError(f"duplicate column name: {cd.name}")
            if cd.pk:
                raise SqlError("Cannot add a PRIMARY KEY column")
            if cd.unique:
                raise SqlError("Cannot add a UNIQUE column")
            aff = type_affinity(cd.typename)
            c = Column(cd.name, cd.typename, aff, t.next_slot, cd.notnull, None,
                       self.coll(cd.collate))
            t.next_slot += 1
            if cd.default is not None:
                c.default = apply_default_affinity(const_value(cd.default), aff)
            if cd.notnull and (c.default is None or c.default.kind == "null"):
                raise SqlError("Cannot add a NOT NULL column with default value NULL")
            for name, e, text in cd.check:
                t.checks.append(Check(name, e, text))
            if cd.fk:
                if c.default is not None and c.default.kind != "null":
                    raise SqlError("Cannot add a REFERENCES column with non-NULL default value")
                ref, rcols, acts = cd.fk
                t.fks.append(ForeignKey([cd.name], ref, rcols, acts["delete"], acts["update"],
                                        acts["deferred"]))
            t.cols.append(c)
        elif st.action == "rename_col":
            ci = t.col(st.old)
            if ci is None:
                raise SqlError(f"no such column: \"{st.old}\"")
            if t.col(st.new) is not None and st.new.lower() != st.old.lower():
                raise SqlError(f"duplicate column name: {st.new}")
            old = t.cols[ci].key
            t.cols[ci].name = st.new
            for idx in cat.table_indexes(t) + ([t.pk_index] if t.pk_index else []):
                idx.cols = [(st.new.lower() if c == old else c, k, d) for c, k, d in idx.cols]
            t.pk = [st.new if n.lower() == old else n for n in t.pk]
            for ch in t.checks:
                rename_columns(ch.expr, old, st.new.lower())
            for fk in t.fks:
                fk.cols = [st.new if n.lower() == old else n for n in fk.cols]
            for other in cat.tables.values():
                for fk in other.fks:
                    if fk.parent.lower() == t.name.lower():
                        fk.parent_cols = [st.new if n.lower() == old else n for n in fk.parent_cols]
        elif st.action == "rename_table":
            new = st.new.lower()
            if new in cat.tables or new in cat.indexes:
                raise SqlError(f"there is already another table or index with this name: {st.new}")
            old = t.name.lower()
            del cat.tables[old]
            t.name = st.new
            cat.tables[new] = t
            for idx in cat.indexes.values():
                if idx.table == old:
                    idx.table = new
            if t.pk_index:
                t.pk_index.table = new
            for other in cat.tables.values():
                for fk in other.fks:
                    if fk.parent.lower() == old:
                        fk.parent = st.new
        elif st.action == "drop_col":
            ci = t.col(st.old)
            if ci is None:
                raise SqlError(f"no such column: \"{st.old}\"")
            c = t.cols[ci]
            if c.ipk or (t.without_rowid and c.key in (n.lower() for n in t.pk)):
                raise SqlError(f'cannot drop PRIMARY KEY column: "{c.name}"')
            for idx in cat.table_indexes(t):
                if any(n == c.key for n, _, _ in idx.cols):
                    what = "UNIQUE" if idx.auto else "indexed"
                    raise SqlError(f'cannot drop {what} column: "{c.name}"')
            for ch in t.checks:
                used: set[str] = set()
                expr_columns(ch.expr, used)
                if c.key in used:
                    raise SqlError(f'error in table {t.name} after drop column: no such column: {c.name}')
            if any(c.key in (n.lower() for n in fk.cols) for fk in t.fks) or any(
                    fk.parent.lower() == t.name.lower() and c.key in (n.lower() for n in fk.parent_cols)
                    for other in cat.tables.values() for fk in other.fks):
                raise SqlError(f'cannot drop column "{c.name}": used in a foreign key')
            if len(t.cols) == 1:
                raise SqlError(f'cannot drop column "{c.name}": no other columns exist')
            del t.cols[ci]
            cat.expand_only = False
        t.sql = ""

    def drop(self, st: P.Drop):
        cat = self.cat
        cat.expand_only = False
        if st.kind == "table":
            key = st.name.lower()
            if key not in cat.tables:
                if st.if_exists:
                    return
                raise SqlError(f"no such table: {st.name}")
            t = cat.tables.pop(key)
            for idx in cat.table_indexes(t):
                del cat.indexes[idx.name.lower()]
                cat.steps.append(Step("drop_tree", idx.tree))
            cat.steps.append(Step("drop_tree", t.tree))
        else:
            key = st.name.lower()
            idx = cat.indexes.get(key)
            if idx is None:
                if st.if_exists:
                    return
                raise SqlError(f"no such index: {st.name}")
            if idx.auto:
                raise SqlError("index associated with UNIQUE or PRIMARY KEY constraint cannot be "
                               "dropped")
            del cat.indexes[key]
            cat.steps.append(Step("drop_tree", idx.tree))

    def apply(self, sql: str, where: str):
        try:
            st, params = P.parse(sql)
        except SqlError as e:
            raise SqlError(f"{where}: {e.msg}", e.pos) from None
        if params:
            raise SqlError(f"{where}: parameters are not allowed in schema statements")
        try:
            if isinstance(st, P.CreateTable):
                self.create_table(st, sql)
            elif isinstance(st, P.CreateIndex):
                self.create_index(st, sql)
            elif isinstance(st, P.AlterTable):
                self.alter(st)
            elif isinstance(st, P.Drop):
                self.drop(st)
            elif isinstance(st, P.Pragma) and st.name in ("foreign_keys", "journal_mode",
                                                           "synchronous"):
                pass
            else:
                raise SqlError("only CREATE, ALTER and DROP statements are allowed in schema "
                               "files; put data changes in vN_up.sql")
        except SqlError as e:
            raise SqlError(f"{where}: {e.msg}", e.pos) from None


@dataclass
class SchemaHistory:
    versions: list[Catalog]

    @property
    def current(self) -> Catalog:
        return self.versions[-1]


def _version_files(path: str) -> list[tuple[int, str, str | None]]:
    if os.path.isfile(path):
        return [(1, path, None, None)]
    found: dict[int, list] = {}
    for f in sorted(os.listdir(path)):
        m = re.fullmatch(r"v(\d+)(_up|_lazy)?\.sql", f)
        if not m:
            continue
        v = int(m.group(1))
        ent = found.setdefault(v, [None, None, None])
        ent[{None: 0, "_up": 1, "_lazy": 2}[m.group(2)]] = os.path.join(path, f)
    if not found:
        raise SqlError(f"{path}: no schema file (expected v1.sql, v2.sql, ...)")
    res = []
    for i, v in enumerate(sorted(found)):
        if v != i + 1:
            raise SqlError(f"{path}: schema versions must be consecutive from v1 (missing v{i + 1})")
        ddl, up, lazy = found[v]
        if ddl is None:
            raise SqlError(f"{path}: v{v}_up.sql or v{v}_lazy.sql without v{v}.sql")
        res.append((v, ddl, up, lazy))
    return res


def load_schema(path: str, upto: int | None = None) -> SchemaHistory:
    """Load schema versions 1..upto (all of them by default)."""
    versions: list[Catalog] = []
    cat = Catalog()
    for v, ddl, up, lazy in _version_files(path):
        if upto is not None and v > upto:
            break
        cat = copy.deepcopy(cat)
        cat.version = v
        cat.steps = []
        cat.up = []
        cat.lazy = []
        cat.expand_only = True
        cat.warnings = []
        b = CatalogBuilder(cat)
        with open(ddl, encoding="utf-8") as fh:
            text = fh.read()
        for sql, off in split_statements(text):
            line = text.encode()[:off].count(b"\n") + 1
            b.apply(sql, f"{ddl}:{line}")
        if up:
            with open(up, encoding="utf-8") as fh:
                utext = fh.read()
            for sql, off in split_statements(utext):
                cat.up.append((sql, up, utext.encode()[:off].count(b"\n") + 1))
            if cat.up:
                cat.expand_only = False
        if lazy:
            with open(lazy, encoding="utf-8") as fh:
                ltext = fh.read()
            for sql, off in split_statements(ltext):
                cat.lazy.append((sql, lazy, ltext.encode()[:off].count(b"\n") + 1))
            if cat.lazy:
                cat.expand_only = False
        if v == 1:
            cat.compat = 1
        else:
            cat.compat = versions[-1].compat if cat.expand_only else v
        versions.append(cat)
    return SchemaHistory(versions)


def table_signature(t: Table, cat: Catalog) -> tuple:
    cols = tuple((c.key, c.decltype.upper(), c.notnull, c.coll, c.ipk,
                  None if c.default is None else (c.default.kind, c.default.value))
                 for c in t.cols)
    idx = tuple(sorted((tuple(i.cols), i.unique) for i in cat.table_indexes(t) if i.auto))
    checks = tuple(sorted(normalize(c.text)[0] for c in t.checks))
    wr = tuple(t.pk_index.cols) if t.without_rowid else None
    return cols, idx, checks, wr


def check_ddl_matches(sql: str, cat: Catalog) -> str | None:
    """Check a CREATE statement found in application code against the catalog.

    Returns None when it describes exactly the current schema object, or an
    explanation of the difference.
    """
    st, _ = P.parse(sql)
    if isinstance(st, P.CreateTable):
        key = st.name.lower()
        if key not in cat.tables:
            return f"table {st.name} is not part of the schema; add it with a migration"
        tmp = Catalog()
        CatalogBuilder(tmp).create_table(st, sql)
        mine = table_signature(tmp.tables[key], tmp)
        theirs = table_signature(cat.tables[key], cat)
        if mine != theirs:
            return (f"CREATE TABLE {st.name} differs from the schema (columns, types, defaults or "
                    "constraints); schema changes must go through versioned migrations")
        return None
    if isinstance(st, P.CreateIndex):
        idx = cat.indexes.get(st.name.lower())
        if idx is None:
            return f"index {st.name} is not part of the schema; add it with a migration"
        t = cat.table(st.table)
        want = []
        for n, coll, desc in st.cols:
            ci = t.col(n)
            if ci is None:
                return f"no such column: {n}"
            want.append((t.cols[ci].key, coll.lower() if coll else t.cols[ci].coll, desc))
        if idx.table != t.name.lower() or idx.cols != want or idx.unique != st.unique:
            return f"CREATE INDEX {st.name} differs from the schema"
        return None
    return "not a CREATE TABLE or CREATE INDEX statement"
