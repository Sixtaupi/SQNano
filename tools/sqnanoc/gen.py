# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""Planner and C code generator.

Each statement becomes a resumable step function (a switch on x->pc whose
case labels sit inside the scan loops) and a state structure holding its
cursors, sort buffers and accumulators. Expressions are translated into
calls to the runtime of include/sqnano/exec.h, which implements the SQLite
semantics (affinity, NULL logic, conversions).
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field, replace

import catalog as C
import parse as P
from lex import SqlError, fnv1a, normalize

OPS_CMP = {"=": "EQ", "!=": "NE", "<": "LT", "<=": "LE", ">": "GT", ">=": "GE", "is": "IS",
           "isnot": "ISNOT"}
OPS_ARITH = {"+": "ADD", "-": "SUB", "*": "MUL", "/": "DIV", "%": "REM", "&": "BITAND",
             "|": "BITOR", "<<": "SHL", ">>": "SHR"}
SWAP = {"<": ">", "<=": ">=", ">": "<", ">=": "<=", "=": "="}

SCALAR = {
    "abs": ("abs", 1, 1), "length": ("length", 1, 1), "lower": ("lower", 1, 1),
    "upper": ("upper", 1, 1), "substr": ("substr", 2, 3), "substring": ("substr", 2, 3),
    "round": ("round", 1, 2), "min": ("min", 2, 64), "max": ("max", 2, 64),
    "coalesce": ("coalesce", 2, 64), "ifnull": ("coalesce", 2, 2), "nullif": ("nullif", 2, 2),
    "typeof": ("typeof", 1, 1), "instr": ("instr", 2, 2), "trim": ("trim", 1, 2),
    "ltrim": ("ltrim", 1, 2), "rtrim": ("rtrim", 1, 2), "replace": ("replace", 3, 3),
    "hex": ("hex", 1, 1), "sign": ("sign", 1, 1),
}
AGGS = {"count": "COUNT", "sum": "SUM", "total": "TOTAL", "avg": "AVG", "min": "MIN", "max": "MAX"}
COLL_C = {"binary": "SQNANO_COLL_BINARY", "nocase": "SQNANO_COLL_NOCASE", "rtrim": "SQNANO_COLL_RTRIM"}
ROWID_NAMES = ("rowid", "oid", "_rowid_")
RESERVED_FIELDS = {"x", "params", "out", "_s", "_scratch"}
# Fields of the SELECT machinery, prefixed for each part of a compound SELECT.
SEL_FIELDS = {"so1", "so2", "sv1", "sv2", "pk", "has", "ag", "av", "gv", "gk", "first", "final",
              "kb", "gh", "limit", "offset", "once"}


def ekey(e):
    """Structural key of an expression, ignoring positions and qualifiers."""
    if isinstance(e, P.Col):
        return ("Col", getattr(e, "sid", None), e.name)
    if isinstance(e, P.Expr):
        return (type(e).__name__,) + tuple(
            ekey(v) for k, v in vars(e).items() if k not in ("pos", "end"))
    if isinstance(e, (list, tuple)):
        return tuple(ekey(x) for x in e)
    if isinstance(e, float):
        return ("f", e.hex())
    return e


def cstr(s: str | bytes) -> str:
    b = s.encode() if isinstance(s, str) else s
    out = ['"']
    for ch in b:
        if ch == 0x22:
            out.append('\\"')
        elif ch == 0x5C:
            out.append("\\\\")
        elif ch == 0x3F:
            out.append("\\?")
        elif 0x20 <= ch < 0x7F:
            out.append(chr(ch))
        else:
            out.append("\\%03o" % ch)
    out.append('"')
    return "".join(out)


def cident(name: str) -> str:
    s = re.sub(r"[^0-9a-zA-Z_]+", "_", name.lower()).strip("_")
    if not s or s[0].isdigit():
        s = "_" + s
    return s


def is_numeric_aff(a) -> bool:
    return a in (C.AFF_NUMERIC, C.AFF_INTEGER, C.AFF_REAL)


def walk(e):
    yield e
    if isinstance(e, P.Expr):
        for v in vars(e).values():
            items = v if isinstance(v, (list, tuple)) else [v]
            for it in items:
                if isinstance(it, tuple):
                    for x in it:
                        if isinstance(x, P.Expr):
                            yield from walk(x)
                elif isinstance(it, P.Expr):
                    yield from walk(it)


def has_agg(e) -> bool:
    for n in walk(e):
        if isinstance(n, P.Func) and n.name in AGGS and (n.name not in ("min", "max") or len(n.args) == 1):
            return True
    return False


def has_col(e) -> bool:
    return any(isinstance(n, P.Col) for n in walk(e))


SUBQ = (P.Subquery, P.Exists, P.InSelect)


def refs(e) -> set:
    """Sources an expression reads (None stands for a column not bound yet)."""
    out = set()
    for n in walk(e):
        if isinstance(n, P.Col):
            out.add(getattr(n, "sid", None))
        elif isinstance(n, SUBQ):
            out |= getattr(n.select, "free", set())
    return out


def subq_tables(e) -> set:
    """Tables read by the subqueries of an expression."""
    out = set()
    for n in walk(e) if e is not None else []:
        if isinstance(n, SUBQ):
            out |= getattr(n.select, "reads", set())
    return out


def select_exprs(st) -> list:
    """Expressions of a SELECT that are evaluated against its sources."""
    out = [rc.expr for rc in st.cols if rc.expr is not None]
    out += [st.where, st.having] + list(st.group) + [o.expr for o in st.order]
    out += [j.on for j in st.joins]
    return [e for e in out if e is not None]


def split_and(e) -> list:
    if e is None:
        return []
    if isinstance(e, P.Binary) and e.op == "and":
        return split_and(e.l) + split_and(e.r)
    return [e]


class Emitter:
    def __init__(self, indent: int = 1):
        self.lines: list[str] = []
        self.ind = indent

    def __call__(self, s: str):
        self.lines.append("\t" * self.ind + s)

    def label(self, s: str):
        self.lines.append("\t" * max(self.ind - 1, 0) + s)

    def open(self, s: str):
        self(s + " {")
        self.ind += 1

    def close(self, s: str = "}"):
        self.ind -= 1
        self(s)


@dataclass(eq=False)
class Src:
    """A table of a FROM clause, as seen by the generated code."""
    sid: int
    table: C.Table
    alias: str | None = None
    cols: str | None = None  # C array of the decoded columns
    rowid: str | None = None  # C int64_t expression
    null: str | None = None  # C flag set on the NULL row of a LEFT JOIN
    hidden: frozenset = frozenset()  # USING/NATURAL columns that resolve to the left table
    sname: str = "s0"  # state fields
    cname: str = "c0"
    kind: str = "inner"
    on: list = field(default_factory=list)  # LEFT JOIN conditions

    @property
    def name(self) -> str:
        return (self.alias or self.table.name).lower()


@dataclass
class Ctx:
    table: C.Table | None = None
    alias: str | None = None
    cols: str | None = None
    rowid: str | None = None
    excluded: str | None = None
    groups: dict | None = None
    aggs: dict | None = None
    extra: list = field(default_factory=list)  # further sources (joins)
    outer: "Ctx | None" = None  # enclosing query (correlated subqueries)
    src0: Src | None = None

    def __post_init__(self):
        if self.src0 is None and self.table is not None:
            self.src0 = Src(0, self.table, self.alias, self.cols, self.rowid)

    def sources(self) -> list:
        return ([self.src0] if self.src0 is not None else []) + list(self.extra)

    def outer_sids(self) -> set:
        out = set()
        c = self.outer
        while c is not None:
            out |= {s.sid for s in c.sources()}
            c = c.outer
        return out


def ctx_for(srcs: list, outer: Ctx | None = None) -> Ctx:
    if not srcs:
        return Ctx(outer=outer)
    s0 = srcs[0]
    return Ctx(s0.table, s0.alias, s0.cols, s0.rowid, extra=list(srcs[1:]), outer=outer, src0=s0)


def col_in(src: Src, e) -> tuple | None:
    """(column index or 'rowid', Column or None) of @p e in @p src, or None."""
    t = src.table
    ci = t.col(e.name)
    if ci is not None:
        return ci, t.cols[ci]
    if e.name in ROWID_NAMES and not t.without_rowid:
        if t.ipk is not None:
            return t.ipk, t.cols[t.ipk]
        return "rowid", None
    return None


class Unit:
    """State shared by the whole generated file."""

    def __init__(self):
        self.consts: list[str] = []
        self.const_names: dict = {}

    def const(self, lit: P.Lit) -> str:
        key = (lit.kind, ekey(lit.value) if not isinstance(lit.value, bytes) else lit.value)
        if key in self.const_names:
            return self.const_names[key]
        name = f"k{len(self.const_names)}"
        self.const_names[key] = name
        self.consts.append(f"static const sqnano_value {name} = {cvalue(lit, self, name)};")
        return name


def cvalue(lit: P.Lit | None, unit: Unit, hint: str) -> str:
    if lit is None or lit.kind == "null":
        return "{.type = SQNANO_NULL}"
    if lit.kind == "int":
        v = int(lit.value)
        if v == -(2**63):
            return "{.type = SQNANO_INTEGER, .u = {.i = INT64_MIN}}"
        return f"{{.type = SQNANO_INTEGER, .u = {{.i = {v}LL}}}}"
    if lit.kind == "real":
        v = float(lit.value)
        if math.isinf(v):
            return f"{{.type = SQNANO_FLOAT, .u = {{.r = {'-' if v < 0 else ''}HUGE_VAL}}}}"
        return f"{{.type = SQNANO_FLOAT, .u = {{.r = {v.hex()}}}}}"
    if lit.kind == "text":
        b = str(lit.value).encode()
        return f"{{.type = SQNANO_TEXT, .n = {len(b)}, .u = {{.z = {cstr(b)}}}}}"
    b = bytes(lit.value)
    arr = f"{hint}_blob"
    unit.consts.append(
        f"static const uint8_t {arr}[{max(len(b), 1)}] = {{{', '.join(str(x) for x in b) or '0'}}};")
    return f"{{.type = SQNANO_BLOB, .n = {len(b)}, .u = {{.p = {arr}}}}}"


@dataclass
class ScanPlan:
    index: C.Index | None
    eqs: list = field(default_factory=list)  # (expr, aff, coll, desc)
    lo: list = field(default_factory=list)  # (expr, aff, coll, desc)
    hi: list = field(default_factory=list)
    desc: bool = False
    order_ok: bool = False
    describe: str = ""


class ExprGen:
    """Translate expressions into C statements writing temporaries t[]."""

    def __init__(self, unit: Unit, cat: C.Catalog, em: Emitter, state=None):
        self.unit = unit
        self.cat = cat
        self.em = em
        self.ntemps = 0
        self.param_aff: dict[int, str] = {}
        self.state = state
        self.qg = None  # QueryGen generating subqueries

    def tmp(self, n: int = 1) -> int:
        i = self.ntemps
        self.ntemps += n
        return i

    def t(self, i: int) -> str:
        return f"t[{i}]"

    def chk(self, call: str):
        self.em(f"rc = {call};")
        self.em("if (rc) {")
        self.em("\treturn rc;")
        self.em("}")

    # -- type information --
    def resolve(self, e: P.Col, ctx: Ctx):
        """Find the source of a column: (Src, column index or 'rowid', Column or None)."""
        sid = getattr(e, "sid", None)
        c = ctx
        while c is not None:
            srcs = c.sources()
            if sid is not None:
                for src in srcs:
                    if src.sid == sid:
                        r = col_in(src, e)
                        if r is None:
                            break
                        return (src,) + r
            elif e.table is not None:
                for src in srcs:
                    if e.table == src.name:
                        r = col_in(src, e)
                        if r is None:
                            raise SqlError(f"no such column: {e.table}.{e.name}", e.pos)
                        return (src,) + r
            else:
                hits = [(src,) + col_in(src, e) for src in srcs
                        if e.name not in src.hidden and col_in(src, e) is not None]
                if len(hits) > 1:
                    raise SqlError(f"ambiguous column name: {e.name}", e.pos)
                if hits:
                    return hits[0]
            c = c.outer
        if e.table is not None:
            raise SqlError(f"no such column: {e.table}.{e.name}", e.pos)
        raise SqlError(f"no such column: {e.name}", e.pos)

    def col_of(self, e: P.Col, ctx: Ctx):
        """Resolve a column: returns (column index or 'rowid', Column or None)."""
        if e.table == "excluded" and ctx.excluded:
            t = ctx.table
            ci = t.col(e.name)
            if ci is None:
                raise SqlError(f"no such column: excluded.{e.name}", e.pos)
            return ci, t.cols[ci]
        _, ci, col = self.resolve(e, ctx)
        return ci, col

    def aff_of(self, e, ctx: Ctx):
        if isinstance(e, P.Col):
            if ctx.groups is not None and ekey(e) in ctx.groups and ctx.table is None:
                return C.AFF_NONE
            ci, col = self.col_of(e, ctx)
            return C.AFF_INTEGER if col is None else col.aff
        if isinstance(e, P.Cast):
            return C.type_affinity(e.typename)
        if isinstance(e, P.Collate):
            return self.aff_of(e.e, ctx)
        if isinstance(e, P.Subquery) and self.qg is not None:
            return self.qg.subquery_aff(e, ctx)
        return C.AFF_NONE

    def coll_of(self, e, ctx: Ctx):
        if isinstance(e, P.Collate):
            c = e.coll.lower()
            if c not in COLL_C:
                raise SqlError(f"no such collation sequence: {e.coll}", e.pos)
            return c, True
        if isinstance(e, P.Col) and (ctx.sources() or ctx.outer is not None):
            ci, col = self.col_of(e, ctx)
            return (col.coll if col else "binary"), False
        return None, False

    def cmp_coll(self, l, r, ctx: Ctx, rctx: Ctx | None = None) -> str:
        lc, le = self.coll_of(l, ctx)
        rc, re_ = self.coll_of(r, rctx or ctx)
        if le:
            return lc
        if re_:
            return rc
        return lc or rc or "binary"

    def cmp_aff(self, l, r, ctx: Ctx, rctx: Ctx | None = None):
        a1, a2 = self.aff_of(l, ctx), self.aff_of(r, rctx or ctx)
        if a1 and a2:
            return C.AFF_NUMERIC if is_numeric_aff(a1) or is_numeric_aff(a2) else C.AFF_NONE
        a = a1 or a2
        if a == C.AFF_BLOB:
            return C.AFF_NONE
        return C.AFF_NUMERIC if is_numeric_aff(a) else a

    def note_param(self, e, aff):
        if isinstance(e, P.Param) and aff and e.index not in self.param_aff:
            self.param_aff[e.index] = aff

    def with_aff(self, cexpr: str, e, aff, ctx: Ctx) -> str:
        if not aff:
            return cexpr
        a = self.aff_of(e, ctx)
        if aff == C.AFF_NUMERIC and is_numeric_aff(a):
            return cexpr
        if aff == C.AFF_TEXT and a == C.AFF_TEXT:
            return cexpr
        if isinstance(e, P.Lit) and (
            (aff == C.AFF_NUMERIC and e.kind in ("int", "real", "null"))
            or (aff == C.AFF_TEXT and e.kind in ("text", "null"))
        ):
            return cexpr
        i = self.tmp()
        self.em(f"{self.t(i)} = {cexpr};")
        self.chk(f"sqnano_affinity(x, &{self.t(i)}, '{aff}')")
        return self.t(i)

    # -- expressions --
    def ex(self, e, ctx: Ctx) -> str:
        if ctx.groups is not None and ekey(e) in ctx.groups:
            return ctx.groups[ekey(e)]
        if ctx.aggs is not None and isinstance(e, P.Func) and ekey(e) in ctx.aggs:
            return ctx.aggs[ekey(e)]
        em = self.em
        if isinstance(e, P.Lit):
            return self.unit.const(e)
        if isinstance(e, P.Param):
            return f"x->params[{e.index}]"
        if isinstance(e, P.Col):
            if ctx.groups is not None:
                raise SqlError(
                    f"column {e.name} must appear in the GROUP BY clause or be used in an "
                    "aggregate function", e.pos)
            if e.table == "excluded" and ctx.excluded:
                ci = ctx.table.col(e.name)
                if ci is None:
                    raise SqlError(f"no such column: excluded.{e.name}", e.pos)
                return f"{ctx.excluded}[{ci}]"
            src, ci, col = self.resolve(e, ctx)
            if src.cols is None:
                raise SqlError(f"no such column: {e.name}", e.pos)
            if ci == "rowid":
                i = self.tmp()
                if src.null:
                    em(f"if ({src.null}) {{")
                    em(f"\tsqnano_v_null(&{self.t(i)});")
                    em("} else {")
                    em(f"\tsqnano_v_int(&{self.t(i)}, {src.rowid});")
                    em("}")
                else:
                    em(f"sqnano_v_int(&{self.t(i)}, {src.rowid});")
                return self.t(i)
            return f"{src.cols}[{ci}]"
        if isinstance(e, P.Collate):
            self.coll_of(e, ctx)
            return self.ex(e.e, ctx)
        if isinstance(e, P.Unary):
            a = self.ex(e.e, ctx)
            i = self.tmp()
            if e.op == "not":
                em(f"(void)sqnano_op_not(&{self.t(i)}, &{a});")
            else:
                fn = {"-": "neg", "+": "pos", "~": "bitnot"}[e.op]
                self.chk(f"sqnano_op_{fn}(x, &{self.t(i)}, &{a})")
            return self.t(i)
        if isinstance(e, P.Binary):
            if e.op in ("and", "or"):
                return self.logic(e, ctx)
            if e.op in OPS_CMP:
                return self.compare(e.op, e.l, e.r, ctx)
            a = self.ex(e.l, ctx)
            b = self.ex(e.r, ctx)
            i = self.tmp()
            if e.op == "||":
                self.chk(f"sqnano_op_concat(x, &{self.t(i)}, &{a}, &{b})")
            else:
                self.chk(f"sqnano_op_arith(x, &{self.t(i)}, &{a}, &{b}, SQNANO_OP_{OPS_ARITH[e.op]})")
            return self.t(i)
        if isinstance(e, P.IsNull):
            a = self.ex(e.e, ctx)
            i = self.tmp()
            em(f"sqnano_v_int(&{self.t(i)}, ({a}.type == SQNANO_NULL) {'!=' if e.neg else '=='} 1);")
            return self.t(i)
        if isinstance(e, P.Between):
            v = self.ex(e.e, ctx)
            i = self.tmp()
            em("{")
            em.ind += 1
            lo = self.compare(">=", e.e, e.lo, ctx, left_c=v)
            em(f"int ta = sqnano_truth(&{lo});")
            em("if (ta == 0) {")
            em(f"\tsqnano_v_int(&{self.t(i)}, 0);")
            em("} else {")
            em.ind += 1
            hi = self.compare("<=", e.e, e.hi, ctx, left_c=v)
            em(f"int tb = sqnano_truth(&{hi});")
            em(f"sqnano_v_bool(&{self.t(i)}, tb == 0 ? 0 : ((ta < 0 || tb < 0) ? -1 : 1));")
            em.close()
            em.close()
            if e.neg:
                em(f"(void)sqnano_op_not(&{self.t(i)}, &{self.t(i)});")
            return self.t(i)
        if isinstance(e, P.InList):
            v = self.ex(e.e, ctx)
            i = self.tmp()
            em("{")
            em.ind += 1
            em(f"int f = 0, hn = {v}.type == SQNANO_NULL;")
            for it in e.items:
                c = self.compare("=", e.e, it, ctx, left_c=v)
                em(f"if (sqnano_truth(&{c}) == 1) {{")
                em("\tf = 1;")
                em(f"}} else if ({c}.type == SQNANO_NULL) {{")
                em("\thn = 1;")
                em("}")
            em(f"sqnano_v_bool(&{self.t(i)}, f ? 1 : (hn ? -1 : 0));")
            em.close()
            if e.neg:
                em(f"(void)sqnano_op_not(&{self.t(i)}, &{self.t(i)});")
            return self.t(i)
        if isinstance(e, P.Like):
            s = self.ex(e.e, ctx)
            p = self.ex(e.pat, ctx)
            esc = self.ex(e.esc, ctx) if e.esc is not None else None
            i = self.tmp()
            self.chk(f"sqnano_op_like(x, &{self.t(i)}, &{s}, &{p}, {('&' + esc) if esc else 'NULL'}, "
                     f"{'true' if e.op == 'glob' else 'false'})")
            if e.neg:
                em(f"(void)sqnano_op_not(&{self.t(i)}, &{self.t(i)});")
            return self.t(i)
        if isinstance(e, P.Case):
            return self.case(e, ctx)
        if isinstance(e, P.Cast):
            a = self.ex(e.e, ctx)
            i = self.tmp()
            aff = C.type_affinity(e.typename)
            self.chk(f"sqnano_cast(x, &{self.t(i)}, &{a}, '{aff}')")
            return self.t(i)
        if isinstance(e, P.Func):
            return self.func(e, ctx)
        if isinstance(e, SUBQ):
            if self.qg is None:
                raise SqlError("subqueries are not allowed here", e.pos)
            return self.qg.subquery(e, ctx)
        raise SqlError(f"unsupported expression {type(e).__name__}", getattr(e, "pos", None))

    def compare(self, op, l, r, ctx: Ctx, left_c: str | None = None, rctx: Ctx | None = None,
                right_c: str | None = None) -> str:
        rctx = rctx or ctx
        aff = self.cmp_aff(l, r, ctx, rctx)
        coll = self.cmp_coll(l, r, ctx, rctx)
        if isinstance(l, P.Col) and not isinstance(r, P.Col):
            self.note_param(r, self.aff_of(l, ctx))
        if isinstance(r, P.Col) and not isinstance(l, P.Col):
            self.note_param(l, self.aff_of(r, rctx))
        a = left_c if left_c is not None else self.ex(l, ctx)
        b = right_c if right_c is not None else self.ex(r, rctx)
        a = self.with_aff(a, l, aff, ctx)
        b = self.with_aff(b, r, aff, rctx)
        i = self.tmp()
        self.em(f"(void)sqnano_op_cmp(&{self.t(i)}, &{a}, &{b}, SQNANO_OP_{OPS_CMP[op]}, "
                f"{COLL_C[coll]});")
        return self.t(i)

    def logic(self, e: P.Binary, ctx: Ctx) -> str:
        em = self.em
        i = self.tmp()
        a = self.ex(e.l, ctx)
        em("{")
        em.ind += 1
        em(f"int ta = sqnano_truth(&{a});")
        stop = "0" if e.op == "and" else "1"
        em(f"if (ta == {stop}) {{")
        em(f"\tsqnano_v_int(&{self.t(i)}, {stop});")
        em("} else {")
        em.ind += 1
        b = self.ex(e.r, ctx)
        em(f"int tb = sqnano_truth(&{b});")
        if e.op == "and":
            em(f"sqnano_v_bool(&{self.t(i)}, tb == 0 ? 0 : ((ta < 0 || tb < 0) ? -1 : 1));")
        else:
            em(f"sqnano_v_bool(&{self.t(i)}, tb == 1 ? 1 : ((ta < 0 || tb < 0) ? -1 : 0));")
        em.close()
        em.close()
        return self.t(i)

    def case(self, e: P.Case, ctx: Ctx) -> str:
        em = self.em
        i = self.tmp()
        base = self.ex(e.base, ctx) if e.base is not None else None
        depth = 0
        for cond, val in e.whens:
            if base is not None:
                c = self.compare("=", e.base, cond, ctx, left_c=base)
            else:
                c = self.ex(cond, ctx)
            em(f"if (sqnano_truth(&{c}) == 1) {{")
            em.ind += 1
            v = self.ex(val, ctx)
            em(f"{self.t(i)} = {v};")
            em.ind -= 1
            em("} else {")
            em.ind += 1
            depth += 1
        if e.other is not None:
            v = self.ex(e.other, ctx)
            em(f"{self.t(i)} = {v};")
        else:
            em(f"sqnano_v_null(&{self.t(i)});")
        for _ in range(depth):
            em.close()
        return self.t(i)

    def func(self, e: P.Func, ctx: Ctx) -> str:
        name = e.name
        if name in AGGS and (name not in ("min", "max") or len(e.args) <= 1):
            raise SqlError(f"misuse of aggregate function {name}()", e.pos)
        if e.distinct or e.star:
            raise SqlError(f"wrong use of DISTINCT or * in {name}()", e.pos)
        if name in ("likely", "unlikely") and len(e.args) == 1:
            return self.ex(e.args[0], ctx)
        if name == "likelihood" and len(e.args) == 2:
            return self.ex(e.args[0], ctx)
        if name == "iif" and len(e.args) == 3:
            return self.case(P.Case(None, [(e.args[0], e.args[1])], e.args[2]), ctx)
        if name not in SCALAR:
            raise SqlError(f"no such function: {name} (not supported in version 1)", e.pos)
        fn, lo, hi = SCALAR[name]
        if not lo <= len(e.args) <= hi:
            raise SqlError(f"wrong number of arguments to function {name}()", e.pos)
        vals = [self.ex(a, ctx) for a in e.args]
        base = self.tmp(len(vals))
        for k, v in enumerate(vals):
            self.em(f"{self.t(base + k)} = {v};")
        i = self.tmp()
        self.chk(f"sqnano_fn_{fn}(x, &{self.t(i)}, {len(vals)}, &{self.t(base)})")
        return self.t(i)


@dataclass
class QueryInfo:
    sql: str
    norm: str
    hash: int
    name: str | None
    source: str
    stmt: object = None
    params: list = field(default_factory=list)
    kind: str = "SELECT"
    cname: str = ""
    cols: list = field(default_factory=list)  # (name, decltype, ctype)
    code: list = field(default_factory=list)
    state_fields: list = field(default_factory=list)
    param_types: list = field(default_factory=list)
    report: list = field(default_factory=list)
    typed: bool = False


class QueryGen:
    def __init__(self, unit: Unit, cat: C.Catalog, q: QueryInfo, tprefix: str):
        self.unit = unit
        self.cat = cat
        self.q = q
        self.tprefix = tprefix
        self.em = Emitter(1)
        self.x = ExprGen(unit, cat, self.em)
        self.x.qg = self
        self.fields: dict[str, str] = {}
        self.pc = 0
        self.closers: list[str] = []
        self.nsid = 0  # source ids
        self.nsub = 0  # subqueries (state field prefixes)
        self.nemit = 0  # subquery emissions (C labels)
        self.prologue: list[str] = []  # run at the start of each execution
        self.case0 = 0
        self.row_sink = None  # where SELECT rows go (default: x->out and yield)
        self.fp = ""  # field prefix of the current compound part
        self.sorter_names: set = set()

    def field(self, name: str, decl: str):
        if self.fp and name in SEL_FIELDS:
            decl = re.sub(rf"\b{name}\b", self.fp + name, decl, count=1)
            name = self.fp + name
        if name not in self.fields:
            self.fields[name] = decl
        return f"q->_s.{name}"

    def f(self, name: str) -> str:
        """State field of the SELECT being generated (prefixed in compound parts)."""
        return f"q->_s.{self.fp if name in SEL_FIELDS else ''}{name}"

    def tref(self, t: C.Table) -> str:
        return f"&{self.tprefix}{cident(t.name)}"

    def iref(self, i: C.Index) -> str:
        return f"&{self.tprefix}ix_{cident(i.name)}"

    def yield_row(self):
        self.pc += 1
        self.em(f"x->pc = {self.pc};")
        self.em("return SQNANO_ROW;")
        self.em.label(f"case {self.pc}:;")

    def chk(self, call: str):
        self.x.chk(call)

    # -- name resolution --
    def case0_label(self):
        self.em.label("case 0:")
        self.case0 = len(self.em.lines)

    def bind(self, st: P.Select, outer: list, prefix: str):
        """Resolve the FROM clause of @p st and bind its columns (and its subqueries)."""
        srcs = []
        if st.table:
            items = [(st.table, st.alias, None)] + [(j.table, j.alias, j) for j in st.joins]
            for k, (tn, alias, j) in enumerate(items):
                t = self.cat.table(tn)
                sid = self.nsid
                self.nsid += 1
                src = Src(sid, t, alias, f"q->_s.{prefix}c{k}", f"q->_s.{prefix}s{k}.rowid",
                          sname=f"{prefix}s{k}", cname=f"{prefix}c{k}")
                if j is not None and j.kind == "left":
                    src.kind = "left"
                    src.null = f"q->_s.{prefix}n{k}"
                srcs.append(src)
        st.srcs = srcs
        scopes = [srcs] + outer
        for k, j in enumerate(st.joins, start=1):
            right = srcs[k]
            names = j.using
            if j.natural:
                names = [c.name for c in right.table.cols
                         if any(left.table.col(c.name) is not None for left in srcs[:k])]
            j.conds = []
            if names:
                hidden = set()
                for n in names:
                    left = next((x for x in srcs[:k] if x.table.col(n) is not None), None)
                    if left is None or right.table.col(n) is None:
                        raise SqlError(f"cannot join using column {n} - column not present in "
                                       "both tables")
                    lc, rc_ = P.Col(None, n.lower()), P.Col(None, n.lower())
                    lc.sid, rc_.sid = left.sid, right.sid
                    j.conds.append(P.Binary("=", lc, rc_))
                    hidden.add(n.lower())
                right.hidden = frozenset(hidden)
        aliases = {rc.alias.lower() for rc in st.cols if rc.alias}
        for e in select_exprs(st):
            top_alias = any(o.expr is e for o in st.order) or any(g is e for g in st.group)
            if top_alias and isinstance(e, P.Col) and e.table is None and e.name in aliases:
                continue
            self.bind_expr(e, scopes, lenient=top_alias)
        if st.limit is not None:
            self.bind_expr(st.limit, outer)
        if st.offset is not None:
            self.bind_expr(st.offset, outer)
        own = {x.sid for x in srcs}
        free = set()
        reads = {x.table.name.lower() for x in srcs}
        for e in select_exprs(st) + [j for jn in st.joins for j in jn.conds]:
            free |= refs(e)
            reads |= subq_tables(e)
        st.free = {x for x in free if x is not None} - own
        st.reads = reads

    def lookup(self, e: P.Col, scopes: list):
        for scope in scopes:
            if e.table is not None:
                m = [x for x in scope if x.name == e.table.lower()]
                if m:
                    return m[0] if col_in(m[0], e) is not None else None
                continue
            hits = [x for x in scope if e.name not in x.hidden and col_in(x, e) is not None]
            if len(hits) > 1:
                raise SqlError(f"ambiguous column name: {e.name}", e.pos)
            if hits:
                return hits[0]
        return None

    def bind_expr(self, e, scopes: list, lenient: bool = False):
        for n in walk(e):
            if isinstance(n, P.Col):
                if n.table == "excluded" or getattr(n, "sid", None) is not None:
                    continue
                try:
                    src = self.lookup(n, scopes)
                except SqlError:
                    if lenient:
                        continue
                    raise
                if src is not None:
                    n.sid = src.sid
                elif not lenient:
                    q = f"{n.table}." if n.table else ""
                    raise SqlError(f"no such column: {q}{n.name}", n.pos)
            elif isinstance(n, SUBQ) and not hasattr(n.select, "srcs"):
                self.nsub += 1
                n.select.prefix = f"u{self.nsub}_"
                self.bind(n.select, scopes, n.select.prefix)

    def bind_dml(self, t: C.Table | None, exprs: list):
        """Bind the subqueries of an INSERT, UPDATE or DELETE (table: source 0)."""
        scope = [[Src(0, t)]] if t is not None else []
        self.nsid = max(self.nsid, 1)
        for e in exprs:
            if e is None:
                continue
            for n in walk(e):
                if isinstance(n, SUBQ) and not hasattr(n.select, "srcs"):
                    self.nsub += 1
                    n.select.prefix = f"u{self.nsub}_"
                    self.bind(n.select, scope, n.select.prefix)

    # -- planning --
    def term_info(self, term, t: C.Table, ctx: Ctx, src: Src | None = None,
                  bound: set | None = None):
        """Analyze a WHERE conjunct usable for an index: [(colidx|'rowid', op, expr, cmp)]."""
        out = []
        target = src or ctx.src0
        bound = bound or set()
        if isinstance(term, P.Between) and not term.neg and isinstance(term.e, P.Col):
            for op, v in ((">=", term.lo), ("<=", term.hi)):
                out += self.term_info(P.Binary(op, term.e, v), t, ctx, src, bound)
            return out
        if not isinstance(term, P.Binary) or term.op not in ("=", "<", "<=", ">", ">="):
            return out
        l, r, op = term.l, term.r, term.op
        orig = (term.l, term.r)  # the collation comes from the left operand as written
        for _ in range(2):
            base = l.e if isinstance(l, P.Collate) else l
            if isinstance(base, P.Col):
                try:
                    s_, ci, col = self.x.resolve(base, ctx)
                except SqlError:
                    s_ = None
                if s_ is target and refs(r) <= bound:
                    self.x.note_param(r, col.aff if col is not None else C.AFF_INTEGER)
                    out.append((ci, op, r, l, orig))
                    return out
            l, r, op = r, l, SWAP[op]
        return out

    def aff_ok(self, l, r, ctx: Ctx, t: C.Table, ci) -> bool:
        """Can an index on column @p ci serve the comparison (sqlite3IndexAffinityOk)?"""
        aff = self.x.cmp_aff(l, r, ctx)
        if ci == "rowid" or not aff or aff == C.AFF_BLOB:
            return True
        col_aff = t.cols[ci].aff
        if aff == C.AFF_TEXT:
            return col_aff == C.AFF_TEXT
        return is_numeric_aff(col_aff)

    def plan(self, t: C.Table, where, order: list, ctx: Ctx, avoid: set = frozenset(),
             rowid_only: bool = False, src: Src | None = None, bound: set | None = None) -> ScanPlan:
        terms = []
        target = src or ctx.src0
        conj = where if isinstance(where, list) else split_and(where)
        for c in conj:
            terms += self.term_info(c, t, ctx, src, bound)
        ipk = t.ipk
        # A WITHOUT ROWID table is its PRIMARY KEY index: it replaces the rowid scan.
        prim = t.pk_index if t.without_rowid else None
        cands = [prim] if rowid_only else [prim] + self.cat.table_indexes(t)
        rowkey = ([(t.col(c), k, d) for c, k, d in prim.cols] if prim is not None else
                  [("rowid", "binary", False)])
        best, best_score = None, -1
        for idx in cands:
            if idx is not None and idx is not prim and any(c in avoid for c, _, _ in idx.cols):
                continue
            p = ScanPlan(idx)
            keycols = ([(ipk if ipk is not None else "rowid", "binary", False)] if idx is None else
                       [(t.col(c), k, d) for c, k, d in idx.cols])
            used_eq = set()
            for ci, coll, desc in keycols:
                eq = None
                for tci, op, r, l, oc in terms:
                    if tci == ci and op == "=" and (idx is None or self.x.cmp_coll(*oc, ctx) == coll
                                                    and self.aff_ok(l, r, ctx, t, ci)):
                        eq = (r, l)
                        break
                if eq:
                    aff = self.x.cmp_aff(eq[1], eq[0], ctx) if idx is not None else C.AFF_INTEGER
                    p.eqs.append((eq[0], aff, coll, desc))
                    used_eq.add(ci)
                    continue
                for tci, op, r, l, oc in terms:
                    if tci != ci or op == "=":
                        continue
                    if idx is not None and (self.x.cmp_coll(*oc, ctx) != coll or
                                            not self.aff_ok(l, r, ctx, t, ci)):
                        continue
                    aff = self.x.cmp_aff(l, r, ctx) if idx is not None else C.AFF_INTEGER
                    lower = op in (">", ">=")
                    if desc:
                        lower = not lower
                    (p.lo if lower else p.hi).append((r, aff, coll, desc))
                break
            # Is ORDER BY satisfied by this scan?
            rest = keycols[len(p.eqs):] + (rowkey if idx is not None and idx is not prim else [])
            ok = bool(order)
            rev = None
            k = 0
            for oe, odesc in order:
                base = oe.e if isinstance(oe, P.Collate) else oe
                if not isinstance(base, P.Col):
                    ok = False
                    break
                try:
                    s_, ci, col = self.x.resolve(base, ctx)
                except SqlError:
                    ok = False
                    break
                if s_ is not target:
                    ok = False
                    break
                if ci in used_eq:
                    continue
                if k >= len(rest):
                    ok = False
                    break
                rci, rcoll, rdesc = rest[k]
                if ci == ipk and rci == "rowid":
                    ci = "rowid"
                ocoll = self.x.coll_of(oe, ctx)[0] or "binary"
                if rci != ci or (col is not None and ocoll != rcoll and col.aff == C.AFF_TEXT):
                    ok = False
                    break
                r_ = odesc != rdesc
                if rev is None:
                    rev = r_
                elif rev != r_:
                    ok = False
                    break
                k += 1
            p.order_ok = ok
            p.desc = bool(ok and rev)
            unique_full = (idx is not None and idx.unique and len(p.eqs) == len(idx.cols)) or (
                idx is None and p.eqs)
            score = (len(p.eqs) * 10 + (5 if (p.lo or p.hi) else 0) + (3 if ok else 0)
                     + (20 if unique_full else 0))
            if idx is None or idx is prim:
                score += 1 if (p.eqs or p.lo or p.hi) else 0
            if score > best_score:
                best, best_score = p, score
        p = best
        if p.index is not None and p.index is prim:
            cols = [c for c, _, _ in prim.cols]
            conds = [f"{c}=?" for c in cols[: len(p.eqs)]]
            if p.lo or p.hi:
                conds.append(f"{cols[len(p.eqs)]} range")
            p.describe = (f"SEARCH {t.name} USING PRIMARY KEY ({' AND '.join(conds)})" if conds
                          else f"SCAN {t.name}")
            # The generated code scans the table itself (NULL index).
            p.index = None
        elif p.index is None:
            if p.eqs:
                p.describe = f"SEARCH {t.name} USING ROWID (rowid=?)"
            elif p.lo or p.hi:
                p.describe = f"SEARCH {t.name} USING ROWID RANGE"
            else:
                p.describe = f"SCAN {t.name}"
        else:
            cols = [c for c, _, _ in p.index.cols]
            conds = [f"{c}=?" for c in cols[: len(p.eqs)]]
            if p.lo or p.hi:
                conds.append(f"{cols[len(p.eqs)]} range")
            what = "SEARCH" if conds else "SCAN"
            p.describe = f"{what} {t.name} USING INDEX {p.index.name}" + (
                f" ({' AND '.join(conds)})" if conds else "")
        return p

    def open_scan(self, t: C.Table, p: ScanPlan, ctx_const: Ctx, src: Src | None = None):
        sn, cn = (src.sname, src.cname) if src is not None else ("s0", "c0")
        s = self.field(sn, f"struct sqnano_scan {sn};")
        cols = self.field(cn, f"sqnano_value {cn}[{len(t.cols)}];")
        idx = self.iref(p.index) if p.index else "NULL"
        self.em(f"sqnano_scan_init(&{s}, x, {self.tref(t)}, {idx}, {cols}, "
                f"{'SQNANO_SCAN_DESC' if p.desc else '0'});")
        for e, aff, coll, desc in p.eqs:
            v = self.x.ex(e, ctx_const)
            affc = f"'{aff}'" if aff else "0"
            self.chk(f"sqnano_scan_eq(&{s}, &{v}, {affc}, {COLL_C[coll]}, "
                     f"{'true' if desc else 'false'})")
        for which, lst in (("SQNANO_SCAN_LO", p.lo), ("SQNANO_SCAN_HI", p.hi)):
            if lst:
                e, aff, coll, desc = lst[0]
                v = self.x.ex(e, ctx_const)
                affc = f"'{aff}'" if aff else "0"
                self.chk(f"sqnano_scan_range(&{s}, {which}, &{v}, {affc}, {COLL_C[coll]}, "
                         f"{'true' if desc else 'false'})")
        if p.describe not in self.q.report:
            self.q.report.append(p.describe)

    # -- nested loops over the FROM clause --
    def conjuncts(self, st: P.Select) -> list:
        """WHERE and inner-join conditions; LEFT JOIN conditions go to their source."""
        conj = split_and(st.where)
        for k, j in enumerate(st.joins, start=1):
            terms = split_and(j.on) + list(getattr(j, "conds", []))
            if j.kind == "left":
                st.srcs[k].on = terms
            else:
                conj += terms
        return conj

    def plan_from(self, st: P.Select, ctx: Ctx, order: list) -> list:
        conj = self.conjuncts(st)
        plans = []
        bound = ctx.outer_sids()
        for k, src in enumerate(st.srcs):
            terms = src.on if src.kind == "left" else conj
            plans.append(self.plan(src.table, terms, order if k == 0 else [], ctx, src=src,
                                   bound=set(bound)))
            bound.add(src.sid)
        return plans

    def open_loops(self, st: P.Select, ctx: Ctx, plans: list, inline: str | None = None) -> list:
        """Open one loop per source and filter the rows; returns the lines closing them.

        @p inline names a C variable holding the scratch mark of a subquery
        evaluated inside an expression: its loops never yield.
        """
        em = self.em
        conj = self.conjuncts(st)
        for c in conj:
            if has_agg(c):
                raise SqlError("misuse of aggregate function in WHERE", c.pos)
        srcs = st.srcs
        index = {x.sid: k for k, x in enumerate(srcs)}
        placed: dict[int, list] = {}
        for c in conj:
            ks = [index[r] for r in refs(c) if r in index]
            depth = len(srcs) - 1 if None in refs(c) else max(ks, default=0)
            placed.setdefault(depth, []).append(c)
        if not srcs:
            if inline:
                em.open("do")
            else:
                once = self.field("once", "int32_t once;")
                em.open(f"for ({once} = 0; {once} < 1; {once}++)")
            for c in placed.get(0, []):
                self.where_filter(c, ctx)
            return ["} while (0);" if inline else "}"]
        for k, (src, p) in enumerate(zip(srcs, plans)):
            self.open_scan(src.table, p, ctx, src)
            s = f"q->_s.{src.sname}"
            if src.kind == "left":
                m = self.field(f"{src.sname}m", f"uint8_t {src.sname}m;")
                n = self.field(src.null[len("q->_s."):], f"uint8_t {src.null[len('q->_s.'):]};")
                em(f"{m} = 0;")
                em(f"{n} = 0;")
            em.open("for (;;)")
            if src.kind == "left":
                em(f"if ({src.null}) {{")
                em("\tbreak;")
                em("}")
            em(f"rc = sqnano_scan_next(&{s});")
            if src.kind == "left":
                em("if (rc == SQNANO_DONE) {")
                em(f"\tif (q->_s.{src.sname}m) {{")
                em("\t\tbreak;")
                em("\t}")
                em(f"\t{src.null} = 1;")
                em(f"\tfor (int k = 0; k < {len(src.table.cols)}; k++) {{")
                em(f"\t\tsqnano_v_null(&{src.cols}[k]);")
                em("\t}")
                em("} else if (rc) {")
                em("\treturn rc;")
                em("}")
            else:
                em("if (rc == SQNANO_DONE) {")
                em("\tbreak;")
                em("}")
                em("if (rc) {")
                em("\treturn rc;")
                em("}")
            if inline:
                em(f"x->scratch.used = {inline};")
            else:
                em("sqnano_scratch_reset(x);")
            if src.kind == "left":
                em.open(f"if (!{src.null})")
                for c in src.on:
                    self.where_filter(c, ctx)
                em(f"q->_s.{src.sname}m = 1;")
                em.close()
            for c in placed.get(k, []):
                self.where_filter(c, ctx)
        return ["}"] * len(srcs)

    def close_loops(self, closers: list):
        for c in reversed(closers):
            self.em.close(c)

    def scan_loop_head(self):
        s = "q->_s.s0"
        self.em.open("for (;;)")
        self.em(f"rc = sqnano_scan_next(&{s});")
        self.em("if (rc == SQNANO_DONE) {")
        self.em("\tbreak;")
        self.em("}")
        self.em("if (rc) {")
        self.em("\treturn rc;")
        self.em("}")
        self.em("sqnano_scratch_reset(x);")

    def where_filter(self, where, ctx: Ctx):
        if where is None:
            return
        if has_agg(where):
            raise SqlError("misuse of aggregate function in WHERE", where.pos)
        c = self.x.ex(where, ctx)
        self.em(f"if (sqnano_truth(&{c}) != 1) {{")
        self.em("\tcontinue;")
        self.em("}")

    def limits(self, st: P.Select, cconst: Ctx):
        if st.limit is None:
            return False
        lim = self.field("limit", "int64_t limit;")
        off = self.field("offset", "int64_t offset;")
        v = self.x.ex(st.limit, cconst)
        self.x.note_param(st.limit, C.AFF_INTEGER)
        self.chk(f"sqnano_limit_value(x, &{v}, &{lim})")
        self.em(f"{off} = 0;")
        if st.offset is not None:
            v = self.x.ex(st.offset, cconst)
            self.x.note_param(st.offset, C.AFF_INTEGER)
            self.chk(f"sqnano_limit_value(x, &{v}, &{off})")
        return True

    def limit_gate(self, has_limit: bool, in_loop: bool = True, brk: str = "break;"):
        """Apply OFFSET/LIMIT before emitting a row (inside a loop)."""
        if not has_limit:
            return
        self.em(f"if ({self.f('offset')} > 0) {{")
        self.em(f"\t{self.f('offset')}--;")
        self.em("\tcontinue;")
        self.em("}")
        self.em(f"if ({self.f('limit')} == 0) {{")
        self.em(f"\t{brk}")
        self.em("}")
        self.em(f"if ({self.f('limit')} > 0) {{")
        self.em(f"\t{self.f('limit')}--;")
        self.em("}")

    def emit_out(self, vals: list):
        """Deliver one result row (C values) to the current sink."""
        if self.row_sink is not None:
            self.row_sink(vals)
            return
        for i, v in enumerate(vals):
            self.em(f"x->out[{i}] = {v};")
        self.yield_row()

    def output(self, exprs: list, ctx: Ctx):
        vals = [self.x.ex(e, ctx) for e in exprs]
        for i, v in enumerate(vals):
            self.em(f"x->out[{i}] = {v};")

    # -- result columns --
    def result_columns(self, cols: list[P.ResultCol], t: C.Table | None, ctx: Ctx):
        outs = []
        srcs = ctx.sources()
        for rc in cols:
            if rc.expr is None:
                if not srcs:
                    raise SqlError("no tables specified")
                pick = srcs
                if rc.star_table:
                    pick = [x for x in srcs if x.name == rc.star_table.lower()] or [
                        x for x in srcs if x.table.name.lower() == rc.star_table.lower()
                        and x.alias is None]
                    if not pick:
                        raise SqlError(f"no such table: {rc.star_table}")
                for src in pick:
                    for c in src.table.cols:
                        if not rc.star_table and c.name.lower() in src.hidden:
                            continue
                        e = P.Col(None, c.key)
                        e.sid = src.sid
                        outs.append((e, c.name, c.decltype or None, c.aff))
                continue
            e = rc.expr
            base = e
            decl = None
            aff = None
            if isinstance(base, P.Col) and srcs and not (base.table == "excluded"):
                ci, col = self.x.col_of(base, ctx)
                name = col.name if col is not None else base.name
                decl = (col.decltype or None) if col is not None else "INTEGER"
                aff = col.aff if col is not None else C.AFF_INTEGER
            else:
                name = rc.text
                if isinstance(e, P.Func) and e.name == "count":
                    aff = C.AFF_INTEGER
                elif isinstance(e, P.Func) and e.name in ("avg", "total"):
                    aff = C.AFF_REAL
                elif (isinstance(e, P.Func) and e.name in ("min", "max", "sum")
                      and len(e.args) == 1 and isinstance(e.args[0], P.Col) and srcs):
                    try:
                        _, col = self.x.col_of(e.args[0], ctx)
                        aff = col.aff if col is not None else C.AFF_INTEGER
                    except SqlError:
                        aff = None
                    if aff not in (C.AFF_INTEGER, C.AFF_REAL, C.AFF_TEXT):
                        aff = None
            outs.append((e, rc.alias or name, decl, aff))
        return outs

    def set_cols(self, outs):
        self.q.cols = [(n, d, a) for _, n, d, a in outs]

    # -- SELECT --
    def gen_select(self, st: P.Select):
        prep = self.query_prepare(st)
        em = self.em
        em.open("switch (x->pc)")
        self.case0_label()
        self.query_rows(st, prep)
        em("return SQNANO_DONE;")
        em("default:")
        em("\tbreak;")
        em.close()
        em("return SQNANO_MISUSE;")

    def query_prepare(self, st: P.Select, set_cols: bool = True, prefix: str = "") -> dict:
        return (self.compound_prepare(st, set_cols, prefix) if st.compound else
                self.select_prepare(st, set_cols, prefix))

    def query_rows(self, st: P.Select, prep: dict):
        if st.compound:
            self.compound_rows(st, prep)
        else:
            self.select_rows(st, prep)

    def compound_prepare(self, st: P.Select, set_cols: bool, prefix: str) -> dict:
        parts = [st] + [p for _, p in st.compound]
        preps = [self.select_prepare(part, set_cols and i == 0, f"{prefix}k{i}_")
                 for i, part in enumerate(parts)]
        nc = len(preps[0]["exprs"])
        for (op, _), pr in zip(st.compound, preps[1:]):
            if len(pr["exprs"]) != nc:
                raise SqlError(f"SELECTs to the left and right of {op.upper()} do not have the same "
                               "number of result columns")
        # Collation of each column: the first part that gives one.
        colls = []
        for k in range(nc):
            coll = "binary"
            for pr in preps:
                c, _ = self.x.coll_of(pr["exprs"][k], pr["ctx"])
                if c:
                    coll = c
                    break
            colls.append(coll)
        names = [o[1].lower() for o in preps[0]["outs"]]
        order = []
        for i, term in enumerate(st.c_order):
            e, coll = term.expr, None
            if isinstance(e, P.Collate):
                coll = e.coll.lower()
                if coll not in COLL_C:
                    raise SqlError(f"no such collation sequence: {e.coll}", e.pos)
                e = e.e
            if isinstance(e, P.Lit) and e.kind == "int":
                if not 1 <= e.value <= nc:
                    raise SqlError(f"{i + 1}st ORDER BY term out of range - should be between 1 "
                                   f"and {nc}", e.pos)
                k = e.value - 1
            elif isinstance(e, P.Col) and e.table is None and e.name in names:
                k = names.index(e.name)
            else:
                raise SqlError(f"{i + 1}st ORDER BY term does not match any column in the result "
                               "set", e.pos)
            order.append((k, term.desc, coll or colls[k]))
        reads = set()
        for part in parts:
            reads |= part.reads
        st.reads_all = reads
        return {"parts": parts, "preps": preps, "order": order, "nc": nc, "colls": colls,
                "prefix": prefix}

    def compound_rows(self, st: P.Select, prep: dict):
        """Rows of a compound SELECT into the current sink."""
        em = self.em
        parts, preps, order, nc, colls = (prep["parts"], prep["preps"], prep["order"], prep["nc"],
                                          prep["colls"])
        ops = [op for op, _ in st.compound]
        saved = st.limit, st.offset
        st.limit, st.offset = st.c_limit, st.c_offset
        has_limit = self.limits(st, Ctx())
        st.limit, st.offset = saved
        sink0 = self.row_sink
        base = self.fp
        off, lim = self.f("offset"), self.f("limit")
        done = f"{base}c_done"

        def gated(vals):
            """OFFSET/LIMIT without continue/break (inside the loops of a part)."""
            if has_limit:
                em.open(f"if ({off} > 0)")
                em(f"{off}--;")
                em.close("} else {")
                em.ind += 1
                em(f"if ({lim} == 0) {{")
                em(f"\tgoto {done};")
                em("}")
                em(f"if ({lim} > 0) {{")
                em(f"\t{lim}--;")
                em("}")
            sink = self.row_sink
            self.row_sink = sink0
            self.emit_out(vals)
            self.row_sink = sink
            if has_limit:
                em.close()

        def run_part(i, sink):
            self.fp = f"{base}k{i}_"
            self.row_sink = sink
            self.select_rows(parts[i], preps[i])
            self.row_sink = sink0
            self.fp = base

        if all(op == "union all" for op in ops) and not order:
            for i in range(len(parts)):
                run_part(i, gated)
            if has_limit:
                em.label(f"{done}:;")
            return
        so = [self.sorter(f"{base}soc0"), self.sorter(f"{base}soc1")]
        cv = self.field(f"{base}cv", f"sqnano_value {base}cv[{nc}];")
        held = self.field(f"{base}chd", f"sqnano_value {base}chd[{nc}];")
        first = self.field(f"{base}cf", f"uint8_t {base}cf;")
        end = self.field(f"{base}ce", f"uint8_t {base}ce;")
        flags = self.field(f"{base}cg", f"uint8_t {base}cg;")
        pk = self.field(f"{base}cpk", f"const uint8_t *{base}cpk;")
        pl = self.field(f"{base}cpl", f"uint16_t {base}cpl;")
        self.q.report.append("compound SELECT: rows sorted in RAM (CONFIG_SQNANO_SORT_RAM_SIZE, then runs in free pages)")

        def add(target, tag):
            def sink(vals):
                em.open("")
                em("struct sqnano_keybuf kb;")
                em("kb.len = 0;")
                for k, v in enumerate(vals):
                    self.key_append("kb", v, colls[k], False)
                em(f"rc = sqnano_key_append_rowid(&kb, {tag});")
                em("if (rc) {")
                em("\treturn sqnano_exec_error(x, rc, \"sort key too large\");")
                em("}")
                b = self.x.tmp(nc)
                for k, v in enumerate(vals):
                    em(f"t[{b + k}] = {v};")
                self.chk(f"sqnano_sorter_add(&{target}, x, &kb, &t[{b}], {nc})")
                em.close()
            return sink

        def read(src, op, on_row):
            """Rows of a sorted step; set operators keep one row per group of equal rows."""
            if op == "union all":
                em.open("for (;;)")
                em(f"rc = sqnano_sorter_next(&{src}, x, {cv}, {nc});")
                em("if (rc == SQNANO_DONE) {")
                em("\tbreak;")
                em("}")
                em("if (rc) {")
                em("\treturn rc;")
                em("}")
                on_row([f"{cv}[{k}]" for k in range(nc)])
                em.close()
                return
            em(f"{first} = 1;")
            em(f"{end} = 0;")
            em.open("for (;;)")
            em(f"rc = sqnano_sorter_next(&{src}, x, {cv}, {nc});")
            em("if (rc == SQNANO_DONE) {")
            em(f"\t{end} = 1;")
            em("} else if (rc) {")
            em("\treturn rc;")
            em(f"}} else if (!{first} && {src}.klen == {pl} && "
               f"memcmp({src}.key, {pk}, {src}.klen - 8u) == 0) {{")
            if op == "union":
                # As SQLite's merge: among equal rows, the first one of the right side wins
                # (rows of each side keep their order: the sort is stable).
                em(f"\tif (!({flags} & 2) && sqnano_key_get_rowid({src}.key + {src}.klen - 8)) {{")
                em(f"\t\trc = sqnano_sorter_hold(&{src}, x, {held}, {nc});")
                em("\t\tif (rc) {")
                em("\t\t\treturn rc;")
                em("\t\t}")
                em("\t}")
            em(f"\t{flags} |= (uint8_t)(1u << sqnano_key_get_rowid({src}.key + {src}.klen - 8));")
            em("\tcontinue;")
            em("}")
            keep = {"union": "1", "intersect": f"{flags} == 3", "except": f"{flags} == 1"}[op]
            em.open(f"if (!{first} && {keep})")
            on_row([f"{held}[{k}]" for k in range(nc)])
            em.close()
            em(f"if ({end}) {{")
            em("\tbreak;")
            em("}")
            em(f"{first} = 0;")
            # The group's row and key must outlive the following entries.
            self.chk(f"sqnano_sorter_hold(&{src}, x, {held}, {nc})")
            em(f"{pk} = {src}.key;")
            em(f"{pl} = {src}.klen;")
            em(f"{flags} = (uint8_t)(1u << sqnano_key_get_rowid({src}.key + {src}.klen - 8));")
            em.close()

        cur = 0
        self.chk(f"sqnano_sorter_open(&{so[0]}, x)")
        run_part(0, add(so[0], 0))
        self.chk(f"sqnano_sorter_sort(&{so[0]}, x)")
        how = "union all"
        for i in range(1, len(parts)):
            nxt = 1 - cur
            self.chk(f"sqnano_sorter_open(&{so[nxt]}, x)")
            read(so[cur], how, add(so[nxt], 0))
            em(f"sqnano_sorter_close(&{so[cur]}, x);")
            run_part(i, add(so[nxt], 1))
            self.chk(f"sqnano_sorter_sort(&{so[nxt]}, x)")
            how, cur = ops[i - 1], nxt
        if not order:
            read(so[cur], how, gated)
            if has_limit:
                em.label(f"{done}:;")
            return
        nxt = 1 - cur
        self.chk(f"sqnano_sorter_open(&{so[nxt]}, x)")
        if has_limit:
            self.top_n(so[nxt])

        def ordered(vals):
            em.open("")
            em("struct sqnano_keybuf kb;")
            em("kb.len = 0;")
            for k, desc, coll in order:
                self.key_append("kb", vals[k], coll, desc)
            b = self.x.tmp(nc)
            for k, v in enumerate(vals):
                em(f"t[{b + k}] = {v};")
            self.chk(f"sqnano_sorter_add(&{so[nxt]}, x, &kb, &t[{b}], {nc})")
            em.close()

        read(so[cur], how, ordered)
        em(f"sqnano_sorter_close(&{so[cur]}, x);")
        self.chk(f"sqnano_sorter_sort(&{so[nxt]}, x)")
        em.open("for (;;)")
        em(f"rc = sqnano_sorter_next(&{so[nxt]}, x, {cv}, {nc});")
        em("if (rc == SQNANO_DONE) {")
        em("\tbreak;")
        em("}")
        em("if (rc) {")
        em("\treturn rc;")
        em("}")
        self.limit_gate(has_limit)
        self.emit_out([f"{cv}[{k}]" for k in range(nc)])
        em.close()

    def select_prepare(self, st: P.Select, set_cols: bool = True, prefix: str = "") -> dict:
        """Resolve a SELECT: result columns, ORDER BY and GROUP BY terms."""
        self.bind(st, [], prefix)
        ctx = ctx_for(st.srcs)
        t = ctx.table
        outs = self.result_columns(st.cols, t, ctx)
        if set_cols:
            self.set_cols(outs)
        exprs = [o[0] for o in outs]
        aliases = {o[1].lower(): o[0] for o in outs}
        order = []
        for term in st.order:
            e = term.expr
            if isinstance(e, P.Lit) and e.kind == "int":
                if not 1 <= e.value <= len(outs):
                    raise SqlError(f"ORDER BY term out of range - should be between 1 and {len(outs)}",
                                   e.pos)
                e = outs[e.value - 1][0]
            elif isinstance(e, P.Col) and e.table is None and e.name in aliases:
                # As SQLite: a result alias wins over a column of the same name.
                e = aliases[e.name]
            order.append((e, term.desc))
        group = []
        for g in st.group:
            if isinstance(g, P.Lit) and g.kind == "int":
                if not 1 <= g.value <= len(outs):
                    raise SqlError("GROUP BY term out of range", g.pos)
                g = outs[g.value - 1][0]
            elif isinstance(g, P.Col) and g.table is None and g.name in aliases and getattr(
                    g, "sid", None) is None:
                g = aliases[g.name]
            if has_agg(g):
                raise SqlError("aggregate functions are not allowed in the GROUP BY clause", g.pos)
            group.append(g)
        is_agg = bool(group) or any(has_agg(e) for e in exprs) or (
            st.having is not None and has_agg(st.having))
        if st.having is not None and not is_agg:
            raise SqlError("HAVING clause on a non-aggregate query", st.having.pos)
        if is_agg and st.distinct:
            raise SqlError("DISTINCT with aggregates is not supported in version 1")
        return {"ctx": ctx, "outs": outs, "exprs": exprs, "order": order, "group": group,
                "is_agg": is_agg}

    def select_rows(self, st: P.Select, prep: dict):
        """Emit the loops producing the rows of a SELECT into the current sink."""
        em = self.em
        ctx, exprs, order, group = prep["ctx"], prep["exprs"], prep["order"], prep["group"]
        has_limit = self.limits(st, Ctx())
        if prep["is_agg"]:
            self.gen_agg(st, ctx, exprs, group, order, has_limit)
            return
        plans = self.plan_from(st, ctx, order)
        need_sort = bool(order) and not (plans and plans[0].order_ok) or st.distinct
        if need_sort:
            self.gen_sorted(st, ctx, plans, exprs, order, has_limit)
            return
        n = self.open_loops(st, ctx, plans)
        self.limit_gate(has_limit, brk=f"goto {self.fp}rows_done;" if len(n) > 1 else "break;")
        self.emit_out([self.x.ex(e, ctx) for e in exprs])
        self.close_loops(n)
        if len(n) > 1 and has_limit:
            em.label(f"{self.fp}rows_done:;")

    def sorter(self, name: str) -> str:
        f = self.field(name, f"struct sqnano_sorter {name};")
        self.sorter_names.add(f[len("q->_s."):])
        self.closers.append(f"sqnano_sorter_close(&{f}, x);")
        return f

    def key_append(self, kb: str, v: str, coll: str, desc: bool):
        self.em(f"rc = sqnano_key_append(&{kb}, &{v}, {COLL_C[coll]}, {'true' if desc else 'false'}, "
                "false);")
        self.em("if (rc) {")
        self.em(f"\treturn sqnano_exec_error(x, rc, \"sort key too large\");")
        self.em("}")

    def top_n(self, so: str):
        """ORDER BY ... LIMIT: the sorter only keeps the rows that can be returned."""
        self.em(f"sqnano_sorter_limit(&{so}, {self.f('limit')}, {self.f('offset')});")
        self.q.report.append("LIMIT: the sort keeps the first LIMIT + OFFSET rows only")

    def gen_sorted(self, st, ctx, plans, exprs, order, has_limit):
        em = self.em
        so = self.sorter("so1")
        n = len(exprs)
        sv = self.field("sv1", f"sqnano_value sv1[{n}];")
        self.chk(f"sqnano_sorter_open(&{so}, x)")
        if has_limit and not st.distinct:
            self.top_n(so)
        if st.distinct:
            out_keys = {ekey(e) for e in exprs}
            for oe, _ in order:
                if ekey(oe) not in out_keys:
                    raise SqlError("with DISTINCT, ORDER BY terms must be result columns in version 1")
        nl = self.open_loops(st, ctx, plans)
        em.open("")
        em("struct sqnano_keybuf kb;")
        em("kb.len = 0;")
        for oe, desc in order:
            v = self.x.ex(oe, ctx)
            self.key_append("kb", v, self.x.coll_of(oe, ctx)[0] or "binary", desc)
        vals = [self.x.ex(e, ctx) for e in exprs]
        if st.distinct:
            for e, v in zip(exprs, vals):
                self.key_append("kb", v, self.x.coll_of(e, ctx)[0] or "binary", False)
        base = self.x.tmp(n)
        for i, v in enumerate(vals):
            em(f"t[{base + i}] = {v};")
        self.chk(f"sqnano_sorter_add(&{so}, x, &kb, &t[{base}], {n})")
        em.close()
        self.close_loops(nl)
        self.q.report.append("ORDER BY/DISTINCT: sort in RAM (CONFIG_SQNANO_SORT_RAM_SIZE, then runs in free pages)")
        self.chk(f"sqnano_sorter_sort(&{so}, x)")
        if st.distinct:
            self.field("pk", "struct sqnano_keybuf pk;")
            self.field("has", "uint8_t has;")
            em(f"{self.f('has')} = 0;")
        em.open("for (;;)")
        em(f"rc = sqnano_sorter_next(&{so}, x, {sv}, {n});")
        em("if (rc == SQNANO_DONE) {")
        em("\tbreak;")
        em("}")
        em("if (rc) {")
        em("\treturn rc;")
        em("}")
        if st.distinct:
            em(f"if ({self.f('has')} && {so}.klen == {self.f('pk')}.len && memcmp({so}.key, {self.f('pk')}.b, "
               f"{self.f('pk')}.len) == 0) {{")
            em("\tcontinue;")
            em("}")
            em(f"{self.f('has')} = 1;")
            em(f"{self.f('pk')}.len = {so}.klen;")
            em(f"memcpy({self.f('pk')}.b, {so}.key, {so}.klen);")
        self.limit_gate(has_limit)
        self.emit_out([f"{sv}[{i}]" for i in range(n)])
        em.close()

    def collect_aggs(self, exprs: list) -> list:
        found = []
        keys = set()
        for e in exprs:
            if e is None:
                continue
            for n in walk(e):
                if isinstance(n, P.Func) and n.name in AGGS and (
                        n.name not in ("min", "max") or len(n.args) == 1):
                    k = ekey(n)
                    if k in keys:
                        continue
                    if n.distinct:
                        raise SqlError(f"{n.name}(DISTINCT ...) is not supported in version 1", n.pos)
                    if n.star and n.name != "count":
                        raise SqlError(f"wrong use of * in {n.name}()", n.pos)
                    if n.name == "count" and n.star:
                        kind = "COUNT_STAR"
                    else:
                        if len(n.args) != 1:
                            raise SqlError(f"wrong number of arguments to function {n.name}()",
                                           n.pos)
                        if has_agg(n.args[0]):
                            raise SqlError("misuse of aggregate function", n.pos)
                        kind = AGGS[n.name]
                    keys.add(k)
                    found.append((k, kind, None if n.star else n.args[0]))
        return found

    def gen_agg(self, st, ctx, exprs, group, order, has_limit):
        em = self.em
        aggs = self.collect_aggs(exprs + [st.having] + [o for o, _ in order])
        na = max(len(aggs), 1)
        ag = self.field("ag", f"struct sqnano_agg ag[{na}];")
        av = self.field("av", f"sqnano_value av[{na}];")
        agg_map = {k: f"{av}[{j}]" for j, (k, _, _) in enumerate(aggs)}

        def agg_coll(arg):
            return COLL_C[self.x.coll_of(arg, ctx)[0] or "binary"] if arg is not None else COLL_C["binary"]

        plans = self.plan_from(st, ctx, [(g, False) for g in group])
        if group and plans and plans[0].order_ok:
            self.gen_group_stream(st, ctx, plans, exprs, group, order, has_limit, aggs, agg_map,
                                  agg_coll)
            return
        if not group:
            for j in range(len(aggs)):
                em(f"sqnano_agg_reset(&{ag}[{j}]);")
            mm = self.minmax(st, ctx, exprs, aggs)
            if mm is not None:
                plans = mm
            nl = self.open_loops(st, ctx, plans)
            for j, (_, kind, arg) in enumerate(aggs):
                if arg is None:
                    v = self.unit.const(P.Lit("null", None))
                else:
                    v = self.x.ex(arg, ctx)
                if mm is not None:
                    em(f"if ({v}.type == SQNANO_NULL) {{")
                    em("\tcontinue;")
                    em("}")
                self.chk(f"sqnano_agg_step(x, &{ag}[{j}], SQNANO_AGG_{kind}, &{v}, {agg_coll(arg)})")
            if mm is not None:
                em(f"goto {self.fp}mm_done;")
            self.close_loops(nl)
            if mm is not None:
                em.label(f"{self.fp}mm_done:;")
            for j, (_, kind, _) in enumerate(aggs):
                self.chk(f"sqnano_agg_final(x, &{ag}[{j}], SQNANO_AGG_{kind}, &{av}[{j}])")
            gctx = replace(ctx, groups={}, aggs=agg_map)
            if st.having is not None:
                c = self.x.ex(st.having, gctx)
                em(f"if (sqnano_truth(&{c}) != 1) {{")
                em("\treturn SQNANO_DONE;")
                em("}")
            if has_limit:
                em(f"if ({self.f('offset')} > 0 || {self.f('limit')} == 0) {{")
                em("\treturn SQNANO_DONE;")
                em("}")
            self.emit_out([self.x.ex(e, gctx) for e in exprs])
            return
        # GROUP BY: sort the rows on the group key, then aggregate each run.
        ng = len(group)
        argpos = {}
        nv = ng
        for j, (_, _, arg) in enumerate(aggs):
            if arg is not None:
                argpos[j] = nv
                nv += 1
        so1 = self.sorter("so1")
        sv1 = self.field("sv1", f"sqnano_value sv1[{nv}];")
        gv = self.field("gv", f"sqnano_value gv[{ng}];")
        self.field("gk", "struct sqnano_keybuf gk;")
        self.field("first", "uint8_t first;")
        self.field("final", "uint8_t final;")
        self.chk(f"sqnano_sorter_open(&{so1}, x)")
        nl = self.open_loops(st, ctx, plans)
        em.open("")
        em("struct sqnano_keybuf kb;")
        em("kb.len = 0;")
        gvals = []
        for g in group:
            v = self.x.ex(g, ctx)
            gvals.append(v)
            self.key_append("kb", v, self.x.coll_of(g, ctx)[0] or "binary", False)
        avals = [self.x.ex(arg, ctx) for _, _, arg in aggs if arg is not None]
        base = self.x.tmp(nv)
        for i, v in enumerate(gvals + avals):
            em(f"t[{base + i}] = {v};")
        self.chk(f"sqnano_sorter_add(&{so1}, x, &kb, &t[{base}], {nv})")
        em.close()
        self.close_loops(nl)
        self.q.report.append("GROUP BY: sort in RAM (CONFIG_SQNANO_SORT_RAM_SIZE, then runs in free pages)")
        self.chk(f"sqnano_sorter_sort(&{so1}, x)")
        em(f"{self.f('first')} = 1;")
        em(f"{self.f('final')} = 0;")
        gmap = {ekey(g): f"{gv}[{i}]" for i, g in enumerate(group)}
        gctx = replace(ctx, groups=gmap, aggs=agg_map)
        # Groups come out sorted on the group key: ORDER BY the same terms needs no sort.
        same = len(order) <= len(group) and all(
            not d and ekey(o) == ekey(g) for (o, d), g in zip(order, group))
        ordered = bool(order) and not same
        if ordered:
            so2 = self.sorter("so2")
            n = len(exprs)
            sv2 = self.field("sv2", f"sqnano_value sv2[{n}];")
            self.chk(f"sqnano_sorter_open(&{so2}, x)")
            if has_limit:
                self.top_n(so2)

        def steps():
            for j, (_, kind, arg) in enumerate(aggs):
                v = f"{sv1}[{argpos[j]}]" if arg is not None else self.unit.const(P.Lit("null", None))
                self.chk(f"sqnano_agg_step(x, &{ag}[{j}], SQNANO_AGG_{kind}, &{v}, {agg_coll(arg)})")

        em.open("for (;;)")
        em(f"rc = sqnano_sorter_next(&{so1}, x, {sv1}, {nv});")
        em("if (rc == SQNANO_DONE) {")
        em(f"\tif ({self.f('first')}) {{")
        em("\t\tbreak;")
        em("\t}")
        em(f"\t{self.f('final')} = 1;")
        em("} else if (rc) {")
        em("\treturn rc;")
        em(f"}} else if (!{self.f('first')} && {so1}.klen == {self.f('gk')}.len && "
           f"memcmp({so1}.key, {self.f('gk')}.b, {self.f('gk')}.len) == 0) {{")
        em.ind += 1
        steps()
        em("continue;")
        em.close()
        em.open(f"if (!{self.f('first')})")
        for j, (_, kind, _) in enumerate(aggs):
            self.chk(f"sqnano_agg_final(x, &{ag}[{j}], SQNANO_AGG_{kind}, &{av}[{j}])")
        hv = None
        if st.having is not None:
            hv = self.x.ex(st.having, gctx)
        em.open(f"if ({'sqnano_truth(&' + hv + ') == 1' if hv else '1'})")
        if ordered:
            em.open("")
            em("struct sqnano_keybuf kb;")
            em("kb.len = 0;")
            for oe, desc in order:
                v = self.x.ex(oe, gctx)
                self.key_append("kb", v, self.x.coll_of(oe, ctx)[0] or "binary", desc)
            vals = [self.x.ex(e, gctx) for e in exprs]
            base = self.x.tmp(len(vals))
            for i, v in enumerate(vals):
                em(f"t[{base + i}] = {v};")
            self.chk(f"sqnano_sorter_add(&{so2}, x, &kb, &t[{base}], {len(vals)})")
            em.close()
        else:
            if has_limit:
                em(f"if ({self.f('offset')} > 0) {{")
                em(f"\t{self.f('offset')}--;")
                em(f"}} else if ({self.f('limit')} != 0) {{")
                em.ind += 1
                em(f"if ({self.f('limit')} > 0) {{")
                em(f"\t{self.f('limit')}--;")
                em("}")
            self.emit_out([self.x.ex(e, gctx) for e in exprs])
            if has_limit:
                em.close()
        em.close()
        em.close()
        em(f"if ({self.f('final')}) {{")
        em("\tbreak;")
        em("}")
        em(f"{self.f('first')} = 0;")
        em(f"{self.f('gk')}.len = {so1}.klen;")
        em(f"memcpy({self.f('gk')}.b, {so1}.key, {so1}.klen);")
        # Group values outlive the following entries of the sorter.
        self.chk(f"sqnano_sorter_hold(&{so1}, x, {gv}, {ng})")
        for j in range(len(aggs)):
            em(f"sqnano_agg_reset(&{ag}[{j}]);")
        steps()
        em.close()
        if ordered:
            n = len(exprs)
            self.chk(f"sqnano_sorter_sort(&{so2}, x)")
            em.open("for (;;)")
            em(f"rc = sqnano_sorter_next(&{so2}, x, {sv2}, {n});")
            em("if (rc == SQNANO_DONE) {")
            em("\tbreak;")
            em("}")
            em("if (rc) {")
            em("\treturn rc;")
            em("}")
            self.limit_gate(has_limit)
            self.emit_out([f"{sv2}[{i}]" for i in range(n)])
            em.close()

    def minmax(self, st: P.Select, ctx: Ctx, exprs: list, aggs: list):
        """SELECT min(col) or max(col) alone: read the first row in index order.

        As SQLite (minMaxQuery): with an index on the column, the result is the
        first non-NULL entry, which also decides between equal values.
        Returns the plans to use, or None.
        """
        if (len(exprs) != 1 or len(aggs) != 1 or st.having is not None or
                aggs[0][1] not in ("MIN", "MAX") or not isinstance(exprs[0], P.Func) or
                not isinstance(aggs[0][2], P.Col)):
            return None
        plans = self.plan_from(st, ctx, [(aggs[0][2], aggs[0][1] == "MAX")])
        if not plans or not plans[0].order_ok:
            return None
        self.q.report.append(f"{aggs[0][1].lower()}(): first row in index order")
        return plans

    def gen_group_stream(self, st, ctx, plans, exprs, group, order, has_limit, aggs, agg_map,
                         agg_coll):
        """GROUP BY on rows already delivered in group order by the scan: no sort."""
        em = self.em
        ng = len(group)
        ag, av = f"{self.f('ag')}", f"{self.f('av')}"
        gh = self.field("gh", f"struct sqnano_agg gh[{ng}];")
        gv = self.field("gv", f"sqnano_value gv[{ng}];")
        self.field("gk", "struct sqnano_keybuf gk;")
        self.field("kb", "struct sqnano_keybuf kb;")
        self.field("first", "uint8_t first;")
        self.q.report.append("GROUP BY: rows arrive in group order, no sort")
        gmap = {ekey(g): f"{gv}[{i}]" for i, g in enumerate(group)}
        gctx = replace(ctx, groups=gmap, aggs=agg_map)
        same = len(order) <= len(group) and all(
            not d and ekey(o) == ekey(g) for (o, d), g in zip(order, group))
        ordered = bool(order) and not same
        if ordered:
            so2 = self.sorter("so2")
            n = len(exprs)
            sv2 = self.field("sv2", f"sqnano_value sv2[{n}];")
            self.chk(f"sqnano_sorter_open(&{so2}, x)")
            if has_limit:
                self.top_n(so2)

        def steps():
            for j, (_, kind, arg) in enumerate(aggs):
                if arg is None:
                    v = self.unit.const(P.Lit("null", None))
                else:
                    v = self.x.ex(arg, ctx)
                self.chk(f"sqnano_agg_step(x, &{ag}[{j}], SQNANO_AGG_{kind}, &{v}, {agg_coll(arg)})")

        def emit_group():
            """Finish the current group and emit it (or queue it for ORDER BY)."""
            for j, (_, kind, _) in enumerate(aggs):
                self.chk(f"sqnano_agg_final(x, &{ag}[{j}], SQNANO_AGG_{kind}, &{av}[{j}])")
            for i in range(ng):
                self.chk(f"sqnano_agg_final(x, &{gh}[{i}], SQNANO_AGG_MIN, &{gv}[{i}])")
            hv = self.x.ex(st.having, gctx) if st.having is not None else None
            em.open(f"if ({'sqnano_truth(&' + hv + ') == 1' if hv else '1'})")
            if ordered:
                em.open("")
                em("struct sqnano_keybuf okb;")
                em("okb.len = 0;")
                for oe, desc in order:
                    v = self.x.ex(oe, gctx)
                    self.key_append("okb", v, self.x.coll_of(oe, ctx)[0] or "binary", desc)
                vals = [self.x.ex(e, gctx) for e in exprs]
                base = self.x.tmp(len(vals))
                for i, v in enumerate(vals):
                    em(f"t[{base + i}] = {v};")
                self.chk(f"sqnano_sorter_add(&{so2}, x, &okb, &t[{base}], {len(vals)})")
                em.close()
            else:
                if has_limit:
                    em(f"if ({self.f('offset')} > 0) {{")
                    em(f"\t{self.f('offset')}--;")
                    em(f"}} else if ({self.f('limit')} != 0) {{")
                    em.ind += 1
                    em(f"if ({self.f('limit')} > 0) {{")
                    em(f"\t{self.f('limit')}--;")
                    em("}")
                self.emit_out([self.x.ex(e, gctx) for e in exprs])
                if has_limit:
                    em.close()
            em.close()

        em(f"{self.f('first')} = 1;")
        nl = self.open_loops(st, ctx, plans)
        em(f"{self.f('kb')}.len = 0;")
        for g in group:
            v = self.x.ex(g, ctx)
            self.key_append(f"{self.f('kb')}", v, self.x.coll_of(g, ctx)[0] or "binary", False)
        em(f"if (!{self.f('first')} && {self.f('kb')}.len == {self.f('gk')}.len && "
           f"memcmp({self.f('kb')}.b, {self.f('gk')}.b, {self.f('kb')}.len) == 0) {{")
        em.ind += 1
        steps()
        em("continue;")
        em.close()
        em.open(f"if (!{self.f('first')})")
        emit_group()
        em.close()
        em(f"{self.f('first')} = 0;")
        em(f"{self.f('gk')} = {self.f('kb')};")
        for j in range(len(aggs)):
            em(f"sqnano_agg_reset(&{ag}[{j}]);")
        for i, g in enumerate(group):
            em(f"sqnano_agg_reset(&{gh}[{i}]);")
            v = self.x.ex(g, ctx)
            self.chk(f"sqnano_agg_step(x, &{gh}[{i}], SQNANO_AGG_MIN, &{v}, SQNANO_COLL_BINARY)")
        steps()
        self.close_loops(nl)
        em.open(f"if (!{self.f('first')})")
        em(f"{self.f('first')} = 1;")
        emit_group()
        em.close()
        if ordered:
            n = len(exprs)
            self.chk(f"sqnano_sorter_sort(&{so2}, x)")
            em.open("for (;;)")
            em(f"rc = sqnano_sorter_next(&{so2}, x, {sv2}, {n});")
            em("if (rc == SQNANO_DONE) {")
            em("\tbreak;")
            em("}")
            em("if (rc) {")
            em("\treturn rc;")
            em("}")
            self.limit_gate(has_limit)
            self.emit_out([f"{sv2}[{i}]" for i in range(n)])
            em.close()

    # -- subqueries --
    def subquery_aff(self, e: P.Subquery, ctx: Ctx):
        sel = e.select
        if not hasattr(sel, "srcs") or len(sel.cols) != 1 or sel.cols[0].expr is None:
            return C.AFF_NONE
        return self.x.aff_of(sel.cols[0].expr, ctx_for(sel.srcs, ctx))

    def subquery(self, node, ctx: Ctx) -> str:
        """Evaluate a subquery inline (it never yields); returns the temporary holding it."""
        em, xg = self.em, self.x
        sel = node.select
        if not hasattr(sel, "srcs"):
            raise SqlError("subqueries are not allowed here", node.pos)
        p = sel.prefix
        n = self.nemit
        self.nemit += 1
        if sel.group or sel.having is not None:
            raise SqlError("GROUP BY and HAVING in a subquery are not supported in version 1",
                           node.pos)
        if sel.compound:
            raise SqlError("compound SELECT in a subquery is not supported in version 1", node.pos)
        sctx = ctx_for(sel.srcs, ctx)
        exists = isinstance(node, P.Exists)
        rexpr = None
        if not exists:
            outs = self.result_columns(sel.cols, sctx.table, sctx)
            if len(outs) != 1:
                raise SqlError(f"sub-select returns {len(outs)} columns - expected 1", node.pos)
            rexpr = outs[0][0]
        agg = rexpr is not None and has_agg(rexpr)
        if exists and any(rc.expr is not None and has_agg(rc.expr) for rc in sel.cols):
            agg = True
        order = []
        for term in sel.order:
            e = term.expr
            if isinstance(e, P.Lit) and e.kind == "int" and rexpr is not None and e.value == 1:
                e = rexpr
            order.append((e, term.desc))
        res = xg.tmp()
        cached = not sel.free and not isinstance(node, P.InSelect)
        if cached:
            ok = self.field(f"{p}ok", f"uint8_t {p}ok;")
            val = self.field(f"{p}val", f"sqnano_value {p}val;")
            buf = self.field(f"{p}buf", f"uint8_t {p}buf[SQNANO_AGG_TEXT_SIZE];")
            self.prologue.append(f"{ok} = 0;")
            em.open(f"if (!{ok})")
        em.open("")
        mark = f"sqm{n}"
        end = f"sq{n}_end"
        em(f"uint32_t {mark} = x->scratch.used;")
        em(f"(void){mark};")
        if sel.limit is not None:
            em(f"int64_t lim{n}, off{n} = 0;")
            em(f"(void)off{n};")
            v = xg.ex(sel.limit, ctx)
            xg.note_param(sel.limit, C.AFF_INTEGER)
            self.chk(f"sqnano_limit_value(x, &{v}, &lim{n})")
            if sel.offset is not None:
                v = xg.ex(sel.offset, ctx)
                xg.note_param(sel.offset, C.AFF_INTEGER)
                self.chk(f"sqnano_limit_value(x, &{v}, &off{n})")
        lhs = None
        if isinstance(node, P.InSelect):
            lhs = xg.ex(node.e, ctx)
            k = xg.tmp()
            em(f"t[{k}] = {lhs};")
            lhs = f"t[{k}]"
            em(f"int f{n} = 0, nr{n} = 0, hn{n} = {lhs}.type == SQNANO_NULL;")
        if exists:
            em(f"sqnano_v_int(&t[{res}], {1 if node.neg else 0});")
        else:
            em(f"sqnano_v_null(&t[{res}]);")
        plans = self.plan_from(sel, sctx, order if (order and not agg) else [])
        order_ok = not order or agg or (plans and plans[0].order_ok)
        top1 = False
        if not order_ok:
            if exists or (isinstance(node, P.InSelect) and sel.limit is None):
                pass  # the order does not change the result
            elif isinstance(node, P.Subquery) and sel.offset is None:
                top1 = True
            else:
                raise SqlError("ORDER BY with LIMIT in a subquery needs a matching index in "
                               "version 1", node.pos)

        def limit_gate():
            if sel.limit is None:
                return
            em(f"if (off{n} > 0) {{")
            em(f"\toff{n}--;")
            em("\tcontinue;")
            em("}")
            em(f"if (lim{n} == 0) {{")
            em(f"\tgoto {end};")
            em("}")
            em(f"if (lim{n} > 0) {{")
            em(f"\tlim{n}--;")
            em("}")

        if agg:
            aggs = self.collect_aggs([rexpr] if rexpr is not None else [])
            na = max(len(aggs), 1)
            ag = self.field(f"{p}ag", f"struct sqnano_agg {p}ag[{na}];")
            av = self.field(f"{p}av", f"sqnano_value {p}av[{na}];")
            agg_map = {kk: f"{av}[{j}]" for j, (kk, _, _) in enumerate(aggs)}
            for j in range(len(aggs)):
                em(f"sqnano_agg_reset(&{ag}[{j}]);")
            mm = (self.minmax(sel, sctx, [rexpr], aggs)
                  if rexpr is not None and sel.limit is None else None)
            if mm is not None:
                plans = mm
            nl = self.open_loops(sel, sctx, plans, inline=mark)
            for j, (_, kind, arg) in enumerate(aggs):
                v = self.unit.const(P.Lit("null", None)) if arg is None else xg.ex(arg, sctx)
                coll = (COLL_C[xg.coll_of(arg, sctx)[0] or "binary"] if arg is not None
                        else COLL_C["binary"])
                if mm is not None:
                    em(f"if ({v}.type == SQNANO_NULL) {{")
                    em("\tcontinue;")
                    em("}")
                self.chk(f"sqnano_agg_step(x, &{ag}[{j}], SQNANO_AGG_{kind}, &{v}, {coll})")
            if mm is not None:
                em(f"goto sq{n}_mm;")
            self.close_loops(nl)
            if mm is not None:
                em.label(f"sq{n}_mm:;")
            for j, (_, kind, _) in enumerate(aggs):
                self.chk(f"sqnano_agg_final(x, &{ag}[{j}], SQNANO_AGG_{kind}, &{av}[{j}])")
            gctx = replace(sctx, groups={}, aggs=agg_map)
            gate = f"lim{n} != 0 && off{n} == 0" if sel.limit is not None else "1"
            em.open(f"if ({gate})")
            if exists:
                em(f"sqnano_v_int(&t[{res}], {0 if node.neg else 1});")
            elif isinstance(node, P.InSelect):
                y = xg.ex(rexpr, gctx)
                c = xg.compare("=", node.e, rexpr, ctx, left_c=lhs, rctx=gctx, right_c=y)
                em(f"nr{n} = 1;")
                em(f"if (sqnano_truth(&{c}) == 1) {{")
                em(f"\tf{n} = 1;")
                em(f"}} else if ({c}.type == SQNANO_NULL) {{")
                em(f"\thn{n} = 1;")
                em("}")
            else:
                v = xg.ex(rexpr, gctx)
                em(f"t[{res}] = {v};")
            em.close()
        elif top1:
            bk = self.field(f"{p}bk", f"struct sqnano_keybuf {p}bk;")
            bv = self.field(f"{p}bv", f"sqnano_value {p}bv;")
            bb = self.field(f"{p}bb", f"uint8_t {p}bb[SQNANO_AGG_TEXT_SIZE];")
            em(f"{bk}.len = 0;")
            em(f"int has{n} = 0;")
            self.q.report.append("subquery ORDER BY: best row kept while scanning")
            nl = self.open_loops(sel, sctx, plans, inline=mark)
            em.open("")
            em("struct sqnano_keybuf kb;")
            em("kb.len = 0;")
            for oe, desc in order:
                v = xg.ex(oe, sctx)
                self.key_append("kb", v, xg.coll_of(oe, sctx)[0] or "binary", desc)
            em(f"int c = memcmp(kb.b, {bk}.b, kb.len < {bk}.len ? kb.len : {bk}.len);")
            em("if (c == 0) {")
            em(f"\tc = (int)kb.len - (int)({bk}.len);")
            em("}")
            em.open(f"if (!has{n} || c < 0)")
            v = xg.ex(rexpr, sctx)
            self.chk(f"sqnano_value_save(x, &{bv}, &{v}, {bb}, sizeof({bb}))")
            em(f"{bk} = kb;")
            em(f"has{n} = 1;")
            em.close()
            em.close()
            self.close_loops(nl)
            em(f"if (has{n}) {{")
            em(f"\tt[{res}] = {bv};")
            em("}")
        else:
            nl = self.open_loops(sel, sctx, plans, inline=mark)
            limit_gate()
            if exists:
                em(f"sqnano_v_int(&t[{res}], {0 if node.neg else 1});")
                em(f"goto {end};")
            elif isinstance(node, P.InSelect):
                y = xg.ex(rexpr, sctx)
                c = xg.compare("=", node.e, rexpr, ctx, left_c=lhs, rctx=sctx, right_c=y)
                em(f"nr{n} = 1;")
                em(f"if (sqnano_truth(&{c}) == 1) {{")
                em(f"\tf{n} = 1;")
                em(f"\tgoto {end};")
                em(f"}} else if ({c}.type == SQNANO_NULL) {{")
                em(f"\thn{n} = 1;")
                em("}")
            else:
                v = xg.ex(rexpr, sctx)
                em(f"t[{res}] = {v};")
                em(f"goto {end};")
            self.close_loops(nl)
        if any(f"goto {end};" in ln for ln in em.lines):
            em.label(f"{end}:;")
        if isinstance(node, P.InSelect):
            em(f"sqnano_v_bool(&t[{res}], f{n} ? 1 : (nr{n} && hn{n} ? -1 : 0));")
            if node.neg:
                em(f"(void)sqnano_op_not(&t[{res}], &t[{res}]);")
        if cached:
            self.chk(f"sqnano_value_save(x, &{val}, &t[{res}], {buf}, sizeof({buf}))")
            em(f"{ok} = 1;")
            em.close()
            em.close()
            em(f"t[{res}] = {val};")
        else:
            em.close()
        return f"t[{res}]"

    # -- INSERT --
    def gen_insert(self, st: P.Insert):
        t = self.cat.table(st.table)
        em = self.em
        n = len(t.cols)
        r = self.field("r", f"sqnano_value r[{n}];")
        self.field("ch", "int64_t ch;")
        if st.columns is not None:
            colidx = []
            for c in st.columns:
                ci = t.col(c)
                if ci is None:
                    if c.lower() in ROWID_NAMES and t.ipk is None and not t.without_rowid:
                        raise SqlError("inserting an explicit rowid is not supported without an "
                                       "INTEGER PRIMARY KEY column")
                    ci = t.ipk if c.lower() in ROWID_NAMES else None
                    if ci is None:
                        raise SqlError(f"table {t.name} has no column named {c}")
                if ci in colidx:
                    raise SqlError(f"column {c} specified more than once")
                colidx.append(ci)
        else:
            colidx = list(range(n))
        rows = st.rows or [[]]
        for row in st.rows:
            if len(row) != len(colidx):
                raise SqlError(f"{len(row)} values for {len(colidx)} columns" if st.columns else
                               f"table {t.name} has {n} columns but {len(row)} values were supplied")
        oe = {"abort": "ABORT", "fail": "FAIL", "ignore": "IGNORE", "replace": "REPLACE",
              "rollback": "ROLLBACK"}[st.oe]
        target = "NULL"
        up = None
        if st.upsert:
            if len(st.upsert) > 1:
                raise SqlError("multiple ON CONFLICT clauses are not supported in version 1")
            up = st.upsert[0]
            if up.target_where is not None:
                raise SqlError("ON CONFLICT (...) WHERE is not supported in version 1")
            if up.target:
                want = sorted(c.lower() for c in up.target)
                if len(want) == 1 and t.col(want[0]) is not None and t.col(want[0]) == t.ipk:
                    target = "&sqnano_rowid_target"
                elif t.without_rowid and sorted(c for c, _, _ in t.pk_index.cols) == want:
                    target = self.iref(t.pk_index)
                else:
                    for idx in self.cat.table_indexes(t):
                        if idx.unique and sorted(c for c, _, _ in idx.cols) == want:
                            target = self.iref(idx)
                            break
                    else:
                        raise SqlError("ON CONFLICT clause does not match any PRIMARY KEY or UNIQUE "
                                       "constraint")
            elif up.action == "update":
                raise SqlError("ON CONFLICT DO UPDATE requires a conflict target")
            oe = "UPSERT"
        self.bind_dml(None, [e for row in rows for e in row])
        self.bind_dml(t, [rc.expr for rc in st.returning] + (
            [up.where] + [e for _, e in up.sets] if up is not None else []))
        if len(rows) > 1 and any(t.name.lower() in subq_tables(e) for row in rows for e in row):
            raise SqlError("a multi-row INSERT reading its own table in a subquery is not "
                           "supported in version 1")
        prep = None
        if st.select is not None:
            prep = self.query_prepare(st.select, set_cols=False, prefix="i_")
            nv = prep["nc"] if st.select.compound else len(prep["exprs"])
            if nv != len(colidx):
                raise SqlError(f"table {t.name} has {len(colidx)} columns but {nv} values were "
                               "supplied" if st.columns is None else
                               f"{nv} values for {len(colidx)} columns")
        ret_ctx = Ctx(t, None, r, "x->last_rowid")
        ret = self.result_columns(st.returning, t, ret_ctx) if st.returning else []
        self.set_cols(ret)

        def fill(vals):
            for ci in range(n):
                if ci not in colidx:
                    em(f"{r}[{ci}] = {self.unit.const(t.cols[ci].default or P.Lit('null', None))};")
            for ci, v in zip(colidx, vals):
                em(f"{r}[{ci}] = {v};")

        def insert():
            em("q->_s.ch = x->changes;")
            em(f"rc = sqnano_row_insert(x, {self.tref(t)}, {r}, SQNANO_OE_{oe}, {target});")
            if up is not None:
                em.open("if (rc == SQNANO_CONFLICT)")
                em("rc = SQNANO_OK;")
                if up.action == "update":
                    self.upsert_update(t, up, r)
                em.close()
            em("if (rc) {")
            em("\treturn rc;")
            em("}")
            if ret:
                em.open("if (x->changes != q->_s.ch)")
                if up is not None and up.action == "update":
                    em("if (q->_s.upd) {")
                    em.ind += 1
                    self.output([o[0] for o in ret], Ctx(t, None, "q->_s.nr", "q->_s.s0.rowid"))
                    em.ind -= 1
                    em("} else {")
                    em.ind += 1
                    self.output([o[0] for o in ret], ret_ctx)
                    em.close()
                else:
                    self.output([o[0] for o in ret], ret_ctx)
                self.yield_row()
                em.close()

        cconst = Ctx()
        em.open("switch (x->pc)")
        self.case0_label()
        if prep is None:
            self.field("i", "int32_t i;")
            em.open(f"for (q->_s.i = 0; q->_s.i < {len(rows)}; q->_s.i++)")
            em("sqnano_scratch_reset(x);")
            for k, row in enumerate(rows):
                em.open(f"if (q->_s.i == {k})")
                vals = []
                for ci, e in zip(colidx, row):
                    vals.append(self.x.ex(e, cconst))
                    self.x.note_param(e, t.cols[ci].aff)
                fill(vals)
                em.close()
            insert()
            em.close()
        elif t.name.lower() in getattr(st.select, "reads_all", st.select.reads) and not (
                st.select.compound and (prep["order"] or any(
                    op != "union all" for op, _ in st.select.compound))):
            # (A sorted compound has read everything before its first row comes out.)
            # The SELECT reads the table it fills: compute every row first, in order.
            m = len(colidx)
            so = self.sorter("som")
            sv = self.field("svm", f"sqnano_value svm[{m}];")
            seq = self.field("seqm", "int64_t seqm;")
            self.chk(f"sqnano_sorter_open(&{so}, x)")
            em(f"{seq} = 0;")

            def keep(vals):
                em.open("")
                em("struct sqnano_keybuf kb;")
                em("kb.len = 0;")
                em(f"(void)sqnano_key_append_rowid(&kb, {seq}++);")
                base = self.x.tmp(m)
                for i, v in enumerate(vals):
                    em(f"t[{base + i}] = {v};")
                self.chk(f"sqnano_sorter_add(&{so}, x, &kb, &t[{base}], {m})")
                em.close()

            self.row_sink = keep
            self.query_rows(st.select, prep)
            self.row_sink = None
            self.q.report.append("INSERT ... SELECT reading its own table: rows buffered in RAM "
                                 "first")
            self.chk(f"sqnano_sorter_sort(&{so}, x)")
            em.open("for (;;)")
            em(f"rc = sqnano_sorter_next(&{so}, x, {sv}, {m});")
            em("if (rc == SQNANO_DONE) {")
            em("\tbreak;")
            em("}")
            em("if (rc) {")
            em("\treturn rc;")
            em("}")
            fill([f"{sv}[{i}]" for i in range(m)])
            insert()
            em.close()
        else:
            def put(vals):
                fill(vals)
                insert()

            self.row_sink = put
            self.query_rows(st.select, prep)
            self.row_sink = None
        em("return SQNANO_DONE;")
        em("default:")
        em("\tbreak;")
        em.close()
        em("return SQNANO_MISUSE;")

    def upsert_update(self, t: C.Table, up: P.Upsert, r: str):
        em = self.em
        n = len(t.cols)
        s0 = self.field("s0", "struct sqnano_scan s0;")
        c0 = self.field("c0", f"sqnano_value c0[{n}];")
        nr = self.field("nr", f"sqnano_value nr[{n}];")
        self.field("upd", "uint8_t upd;")
        em("q->_s.upd = 0;")
        em(f"sqnano_scan_init(&{s0}, x, {self.tref(t)}, NULL, {c0}, 0);")
        self.chk(f"sqnano_row_fetch_conflict(&{s0})")
        ctx = Ctx(t, None, c0, f"{s0}.rowid", excluded=r)
        go = "1"
        if up.where is not None:
            go = "sqnano_truth(&" + self.x.ex(up.where, ctx) + ") == 1"
        em.open(f"if ({go})")
        self.assign_sets(t, up.sets, ctx, c0, nr)
        em("q->_s.upd = 1;")
        self.chk(f"sqnano_row_update(x, {self.tref(t)}, {s0}.rowid, {c0}, {nr}, {s0}.row, "
                 f"{s0}.rowlen, SQNANO_OE_ABORT)")
        em.close()

    def assign_sets(self, t: C.Table, sets, ctx: Ctx, c0: str, nr: str):
        em = self.em
        targets = []
        for names, e in sets:
            for name in names:
                ci = t.col(name)
                if ci is None:
                    if name.lower() in ROWID_NAMES and t.ipk is not None:
                        ci = t.ipk
                    else:
                        raise SqlError(f"no such column: {name}")
                targets.append((ci, e))
        vals = []
        for ci, e in targets:
            v = self.x.ex(e, ctx)
            self.x.note_param(e, t.cols[ci].aff)
            k = self.x.tmp()
            em(f"t[{k}] = {v};")
            vals.append((ci, k))
        em(f"memcpy({nr}, {c0}, sizeof({nr}));")
        for ci, k in vals:
            em(f"{nr}[{ci}] = t[{k}];")
        return [ci for ci, _ in targets]

    def buffer_keys(self, t: C.Table, ctx: Ctx, where):
        """Scan the rows that @p where selects into a sorter: rowids, or PRIMARY KEY values."""
        em = self.em
        nk = len(t.pk_index.cols) if t.without_rowid else 1
        so = self.sorter("so1")
        sv = self.field("sv1", f"sqnano_value sv1[{nk}];")
        self.chk(f"sqnano_sorter_open(&{so}, x)")
        self.scan_loop_head()
        self.where_filter(where, ctx)
        k = self.x.tmp(nk)
        if t.without_rowid:
            for i, (c, _, _) in enumerate(t.pk_index.cols):
                em(f"t[{k + i}] = q->_s.c0[{t.col(c)}];")
        else:
            em(f"sqnano_v_int(&t[{k}], q->_s.s0.rowid);")
        self.chk(f"sqnano_sorter_add(&{so}, x, NULL, &t[{k}], {nk})")
        em.close()
        self.chk(f"sqnano_sorter_sort(&{so}, x)")
        return so, sv

    def fetch_buffered(self, t: C.Table, so: str, sv: str):
        """Loop over the buffered rows, each loaded into q->_s.s0 (left open)."""
        em = self.em
        nk = len(t.pk_index.cols) if t.without_rowid else 1
        em.open("for (;;)")
        em(f"rc = sqnano_sorter_next(&{so}, x, {sv}, {nk});")
        em("if (rc == SQNANO_DONE) {")
        em("\tbreak;")
        em("}")
        em("if (rc) {")
        em("\treturn rc;")
        em("}")
        em("sqnano_scratch_reset(x);")
        if t.without_rowid:
            em(f"rc = sqnano_row_fetch_pk(&q->_s.s0, {sv});")
        else:
            em(f"rc = sqnano_row_fetch(&q->_s.s0, {sv}[0].u.i);")

    # -- UPDATE --
    def gen_update(self, st: P.Update):
        t = self.cat.table(st.table)
        em = self.em
        n = len(t.cols)
        ctx = Ctx(t, None, "q->_s.c0", "q->_s.s0.rowid")
        changed = set()
        for names, _ in st.sets:
            for name in names:
                ci = t.col(name)
                if ci is None and name.lower() in ROWID_NAMES and not t.without_rowid:
                    ci = t.ipk if t.ipk is not None else "rowid"
                if ci is None:
                    raise SqlError(f"no such column: {name}")
                if ci == "rowid":
                    raise SqlError("updating the rowid of a table without INTEGER PRIMARY KEY is "
                                   "not supported")
                changed.add(t.cols[ci].key)
        self.bind_dml(t, [st.where] + [e for _, e in st.sets] + [rc.expr for rc in st.returning])
        # A subquery reading the table must see it before the update: rowids first.
        reads_self = t.name.lower() in subq_tables(st.where) or any(
            t.name.lower() in subq_tables(e) for _, e in st.sets)
        # Rows whose key changes move in the table tree: keys first, then the updates.
        two_phase = (t.ipk is not None and t.cols[t.ipk].key in changed) or reads_self or (
            t.without_rowid and any(c in changed for c, _, _ in t.pk_index.cols))
        plan = self.plan(t, st.where, [], ctx, avoid=changed, rowid_only=False)
        nr = self.field("nr", f"sqnano_value nr[{n}];")
        self.field("ch", "int64_t ch;")
        oe = {"abort": "ABORT", "fail": "FAIL", "ignore": "IGNORE", "replace": "REPLACE",
              "rollback": "ROLLBACK"}[st.oe]
        ret = self.result_columns(st.returning, t, Ctx(t, None, nr, "q->_s.s0.rowid")) if st.returning else []
        self.set_cols(ret)
        em.open("switch (x->pc)")
        self.case0_label()
        self.open_scan(t, plan, Ctx())

        def body():
            self.assign_sets(t, st.sets, ctx, "q->_s.c0", nr)
            em("q->_s.ch = x->changes;")
            self.chk(f"sqnano_row_update(x, {self.tref(t)}, q->_s.s0.rowid, q->_s.c0, {nr}, "
                     f"q->_s.s0.row, q->_s.s0.rowlen, SQNANO_OE_{oe})")
            if ret:
                em.open("if (x->changes != q->_s.ch)")
                self.output([o[0] for o in ret], Ctx(t, None, nr, "q->_s.s0.rowid"))
                self.yield_row()
                em.close()

        if not two_phase:
            self.scan_loop_head()
            self.where_filter(st.where, ctx)
            body()
            em.close()
        else:
            so, sv = self.buffer_keys(t, ctx, st.where)
            self.q.report.append("UPDATE " + ("reading its own table" if reads_self else
                                              "of the row key") + ": " +
                                 ("keys" if t.without_rowid else "rowids") + " buffered in RAM first")
            self.fetch_buffered(t, so, sv)
            em("if (rc == SQNANO_NOTFOUND) {")
            em("\tcontinue;")
            em("}")
            em("if (rc) {")
            em("\treturn rc;")
            em("}")
            body()
            em.close()
        em("return SQNANO_DONE;")
        em("default:")
        em("\tbreak;")
        em.close()
        em("return SQNANO_MISUSE;")

    # -- DELETE --
    def gen_delete(self, st: P.Delete):
        t = self.cat.table(st.table)
        em = self.em
        ctx = Ctx(t, None, "q->_s.c0", "q->_s.s0.rowid")
        self.bind_dml(t, [st.where] + [rc.expr for rc in st.returning])
        two_phase = t.name.lower() in subq_tables(st.where)
        plan = self.plan(t, st.where, [], ctx)
        ret = self.result_columns(st.returning, t, ctx) if st.returning else []
        self.set_cols(ret)
        em.open("switch (x->pc)")
        self.case0_label()
        self.open_scan(t, plan, Ctx())
        if two_phase:
            # The WHERE clause reads the table: decide on every row before deleting any.
            so, sv = self.buffer_keys(t, ctx, st.where)
            self.q.report.append("DELETE reading its own table: " +
                                 ("keys" if t.without_rowid else "rowids") + " buffered in RAM first")
            self.fetch_buffered(t, so, sv)
            em("if (rc == SQNANO_NOTFOUND) {")
            em("\tcontinue;")
            em("}")
            em("if (rc) {")
            em("\treturn rc;")
            em("}")
        else:
            self.scan_loop_head()
            self.where_filter(st.where, ctx)
        if ret:
            self.output([o[0] for o in ret], ctx)
        self.chk(f"sqnano_row_delete(x, {self.tref(t)}, q->_s.s0.rowid, q->_s.c0)")
        if ret:
            self.yield_row()
        em.close()
        em("return SQNANO_DONE;")
        em("default:")
        em("\tbreak;")
        em.close()
        em("return SQNANO_MISUSE;")

    def generate(self):
        st = self.q.stmt
        if isinstance(st, P.Select):
            self.q.kind = "SELECT"
            self.gen_select(st)
        elif isinstance(st, P.Insert):
            self.q.kind = "WRITE"
            self.gen_insert(st)
        elif isinstance(st, P.Update):
            self.q.kind = "WRITE"
            self.gen_update(st)
        elif isinstance(st, P.Delete):
            self.q.kind = "WRITE"
            self.gen_delete(st)
        else:
            raise SqlError("unsupported statement")
        if self.prologue:
            self.em.lines[self.case0:self.case0] = ["\t\t" + ln for ln in dict.fromkeys(self.prologue)]


def check_function(unit: Unit, cat: C.Catalog, t: C.Table, fname: str) -> list[str]:
    """C function evaluating the CHECK constraints of @p t on a row image."""
    em = Emitter(1)
    xg = ExprGen(unit, cat, em)
    ctx = Ctx(t, None, "row", "row[0].u.i")
    for ch in t.checks:
        v = xg.ex(ch.expr, ctx)
        msg = ch.name or ch.text
        em(f"if (sqnano_truth(&{v}) == 0) {{")
        em(f"\treturn sqnano_exec_error(x, SQNANO_CONSTRAINT_CHECK, \"CHECK constraint failed: %s\", "
           f"{cstr(msg)});")
        em("}")
    head = [f"static int {fname}(struct sqnano_exec *x, const sqnano_value *row)", "{",
            f"\tsqnano_value t[{max(xg.ntemps, 1)}];", "\tint rc = SQNANO_OK;", "",
            "\t(void)t;", "\t(void)rc;"]
    return head + em.lines + ["\treturn SQNANO_OK;", "}", ""]


NULL_FREE_FUNCS = {"upper", "lower", "trim", "ltrim", "rtrim", "length", "abs", "hex", "replace",
                   "substr", "round", "sign", "instr"}


def never_null(e, t: C.Table) -> bool:
    """Conservative: True only when @p e cannot evaluate to NULL on a row of @p t."""
    if isinstance(e, P.Lit):
        return e.kind != "null"
    if isinstance(e, P.Col):
        ci = t.col(e.name)
        return ci is not None and t.cols[ci].notnull
    if isinstance(e, (P.Collate, P.Cast)):
        return never_null(e.e, t)
    if isinstance(e, P.Unary):
        return never_null(e.e, t)
    if isinstance(e, P.IsNull):
        return True
    if isinstance(e, P.Binary):
        if e.op in ("is", "is not"):
            return True
        if e.op in ("/", "%"):
            return False
        return never_null(e.l, t) and never_null(e.r, t)
    if isinstance(e, P.Func):
        name = e.name.lower()
        if name == "typeof":
            return True
        if name in ("coalesce", "ifnull"):
            return any(never_null(a, t) for a in e.args)
        if name in NULL_FREE_FUNCS:
            return all(never_null(a, t) for a in e.args)
        return False
    if isinstance(e, P.Case):
        return e.other is not None and never_null(e.other, t) and all(
            never_null(r, t) for _, r in e.whens)
    return False


def lazy_function(unit: Unit, cat: C.Catalog, st: P.Update, fname: str) -> tuple[C.Table, list[str]]:
    """C function applying one UPDATE of vN_lazy.sql to the slots of a record."""
    t = cat.table(st.table)
    if st.oe != "abort" or st.returning:
        raise SqlError("a lazy migration is a plain UPDATE (no OR clause, no RETURNING)")
    exprs = [st.where] + [e for _, e in st.sets]
    for e in exprs:
        for n in walk(e):
            if isinstance(n, SUBQ):
                raise SqlError("a lazy migration only reads the row it converts (no subquery)")
            if isinstance(n, P.Param):
                raise SqlError("parameters are not allowed in migrations")
            if isinstance(n, P.Func) and (has_agg(n) or getattr(n, "over", None) is not None):
                raise SqlError("aggregates are not allowed in a lazy migration")
            if isinstance(n, P.Col):
                if n.table is not None and n.table.lower() != t.name.lower():
                    raise SqlError(f"no such column: {n.table}.{n.name}", n.pos)
                ci = t.col(n.name)
                if ci is None:
                    raise SqlError(f"no such column: {n.name}", n.pos)
                if t.cols[ci].ipk:
                    raise SqlError("a lazy migration cannot read the INTEGER PRIMARY KEY")
    indexed = {c for i in cat.table_indexes(t) for c, _, _ in i.cols}
    if t.without_rowid:
        indexed |= {c for c, _, _ in t.pk_index.cols}
    fkcols = {n.lower() for fk in t.fks for n in fk.cols}
    for other in cat.tables.values():
        for fk in other.fks:
            if fk.parent.lower() == t.name.lower():
                fkcols |= {n.lower() for n in (fk.parent_cols or t.pk)}
    checked: set[str] = set()
    for ch in t.checks:
        C.expr_columns(ch.expr, checked)
    targets = []
    for names, e in st.sets:
        if len(names) != 1:
            raise SqlError("a lazy migration sets one column at a time")
        ci = t.col(names[0])
        if ci is None:
            raise SqlError(f"no such column: {names[0]}")
        c = t.cols[ci]
        why = ("the INTEGER PRIMARY KEY" if c.ipk else
               "an indexed column" if c.key in indexed else
               "a foreign key column" if c.key in fkcols else
               "a column used by a CHECK constraint" if c.key in checked else
               None)
        if c.notnull and not never_null(e, t):
            why = "a NOT NULL column with a value that may be NULL"
        if why:
            raise SqlError(f"a lazy migration cannot set {c.name}: it is {why}; "
                           "use vN_up.sql (eager migration) for it")
        targets.append((c, e))
    # The record slots, seen as the columns of a table (dropped ones never named).
    by_slot = {c.slot: c for c in t.cols}
    cols = [by_slot.get(k) or C.Column(f"\x01{k}", "", C.AFF_NONE, k) for k in range(t.next_slot)]
    syn = C.Table(t.name, t.tree, cols, [], "", t.next_slot, without_rowid=True)
    em = Emitter(1)
    xg = ExprGen(unit, cat, em)
    ctx = Ctx(syn, None, "s", None)
    used = set()
    for e in exprs:
        for n in walk(e):
            if isinstance(n, P.Col):
                used.add(t.cols[t.col(n.name)].key)
    for c in t.cols:
        if c.key not in used:
            continue
        em(f"if (!have[{c.slot}]) {{")
        em(f"\ts[{c.slot}] = {unit.const(c.default or P.Lit('null', None))};")
        em(f"\thave[{c.slot}] = 1;")
        if c.aff == C.AFF_REAL:
            em(f"}} else if (s[{c.slot}].type == SQNANO_INTEGER) {{")
            em(f"\tsqnano_v_real(&s[{c.slot}], (double)s[{c.slot}].u.i);")
        em("}")
    if st.where is not None:
        v = xg.ex(st.where, ctx)
        em(f"if (sqnano_truth(&{v}) != 1) {{")
        em("\treturn SQNANO_OK;")
        em("}")
    temps = []
    for c, e in targets:
        v = xg.ex(e, ctx)
        k = xg.tmp()
        em(f"t[{k}] = {v};")
        temps.append((c, k))
    for c, k in temps:
        em(f"s[{c.slot}] = t[{k}];")
        em(f"have[{c.slot}] = 1;")
        if c.aff:
            xg.chk(f"sqnano_affinity(x, &s[{c.slot}], '{c.aff}')")
    head = [f"static int {fname}(struct sqnano_exec *x, sqnano_value *s, uint8_t *have)", "{",
            f"\tsqnano_value t[{max(xg.ntemps, 1)}];", "\tint rc = SQNANO_OK;", "",
            "\t(void)t;", "\t(void)rc;"]
    return t, head + em.lines + ["\treturn SQNANO_OK;", "}", ""]


def ctype_of(aff) -> str | None:
    return {C.AFF_INTEGER: "int64_t", C.AFF_REAL: "double", C.AFF_TEXT: "const char *"}.get(aff)


class Generator:
    def __init__(self, hist: C.SchemaHistory, migrate_from: int, prefix: str = "sqnano_gen"):
        self.hist = hist
        self.prefix = prefix
        cur = hist.current.version
        if migrate_from < 1:
            migrate_from = 1
        self.versions = [c for c in hist.versions if c.version >= min(migrate_from, cur)]
        self.unit = Unit()
        self.queries: list[QueryInfo] = []
        self.up_queries: dict[int, list[QueryInfo]] = {}
        self.lazy_fns: list[tuple[int, int, str]] = []  # (table tree, version, function)
        self.out_c: list[str] = []
        self.warnings: list[str] = []

    def add_query(self, sql: str, name: str | None, source: str) -> QueryInfo | None:
        norm, _ = normalize(sql)
        if not norm:
            return None
        h = fnv1a(norm)
        for q in self.queries:
            if q.norm == norm:
                if name and q.name and q.name != name:
                    raise SqlError(f"{source}: same statement as query '{q.name}' ({q.source})")
                q.name = q.name or name
                return q
            if q.hash == h:
                raise SqlError(f"{source}: hash collision with {q.source}; reword one statement")
        q = QueryInfo(sql, norm, h, name, source)
        self.queries.append(q)
        return q

    # -- catalogs --
    def tprefix(self, cat: C.Catalog) -> str:
        return f"cat{cat.version}_"

    def warn(self, msg: str):
        if msg not in self.warnings:
            self.warnings.append(msg)

    def fk_spec(self, cat: C.Catalog, t: C.Table, fk) -> dict | None:
        """Resolve a FOREIGN KEY against its parent table (None: not enforced)."""
        what = f"{t.name}: FOREIGN KEY ({', '.join(fk.cols)}) REFERENCES {fk.parent}"
        parent = cat.tables.get(fk.parent.lower())
        if parent is None:
            self.warn(f"{what}: no such table, not enforced")
            return None
        pnames = [n.lower() for n in fk.parent_cols] or [n.lower() for n in parent.pk]
        cfk = [t.col(n) for n in fk.cols]
        if None in cfk:
            raise SqlError(f"unknown column \"{fk.cols[cfk.index(None)]}\" in foreign key definition")
        if not pnames or len(pnames) != len(cfk) or len(cfk) > 8:
            self.warn(f"{what}: foreign key mismatch, not enforced")
            return None
        if parent.ipk is not None and pnames == [parent.cols[parent.ipk].key]:
            pidx, order, pcols, colls = None, [0], [parent.ipk], ["binary"]
        else:
            pidx = next((i for i in ([parent.pk_index] if parent.without_rowid else []) +
                         cat.table_indexes(parent) if i.unique and
                         sorted(c for c, _, _ in i.cols) == sorted(pnames)), None)
            if pidx is None:
                self.warn(f"{what}: foreign key mismatch (no UNIQUE index on the parent key), "
                          "not enforced")
                return None
            order = [pnames.index(c) for c, _, _ in pidx.cols]
            pcols = [parent.col(c) for c, _, _ in pidx.cols]
            colls = [k for _, k, _ in pidx.cols]
        ccols = [cfk[o] for o in order]
        cidx, cpos = None, None
        for i in ([t.pk_index] if t.without_rowid else []) + cat.table_indexes(t):
            lead = [t.col(c) for c, _, _ in i.cols[:len(ccols)]]
            if len(i.cols) < len(ccols) or sorted(lead) != sorted(ccols):
                continue
            pos = [ccols.index(ci) for ci in lead]
            if all(t.cols[ci].aff == parent.cols[pcols[k]].aff and i.cols[j][1] == colls[k]
                   for j, (ci, k) in enumerate(zip(lead, pos))):
                cidx, cpos = i, pos
                break
        act = {"no action": "NOACTION", "restrict": "RESTRICT", "set null": "SETNULL",
               "set default": "SETDEFAULT", "cascade": "CASCADE"}
        return {"child": t, "parent": parent, "pidx": pidx, "cidx": cidx, "ccols": ccols,
                "pcols": pcols, "cpos": cpos, "on_delete": act[fk.on_delete],
                "on_update": act[fk.on_update], "deferred": fk.deferred}

    def emit_catalog(self, cat: C.Catalog):
        out = self.out_c
        p = self.tprefix(cat)
        out.append(f"/* ---- catalog version {cat.version} (compatible down to {cat.compat}) ---- */")
        out.append("")
        fks = [sp for t in cat.tables.values() for fk in t.fks
               for sp in [self.fk_spec(cat, t, fk)] if sp is not None]
        for n in range(len(fks)):
            out.append(f"static const struct sqnano_fkey {p}fk{n};")
        if fks:
            out.append("")
        idx_names = {}
        for t in cat.tables.values():
            tn = cident(t.name)
            cols = []
            for c in t.cols:
                flags = []
                if c.notnull:
                    flags.append("SQNANO_COL_NOTNULL")
                if c.ipk:
                    flags.append("SQNANO_COL_IPK")
                if c.default is not None and c.default.kind != "null":
                    flags.append("SQNANO_COL_HAS_DEFAULT")
                dname = f"{p}{tn}_d{len(cols)}"
                cols.append(
                    f"\t{{.name = {cstr(c.name)}, .type = {cstr(c.decltype)}, "
                    f".dflt = {cvalue(c.default, self.unit, dname)}, .slot = {c.slot}, "
                    f".aff = '{c.aff}', .coll = {COLL_C[c.coll]}, .flags = {' | '.join(flags) or '0'}}},")
            out.append(f"static const struct sqnano_column {p}{tn}_cols[] = {{")
            out += cols
            out.append("};")
            out.append("")
            tidx = cat.table_indexes(t)
            prim = [t.pk_index] if t.without_rowid else []
            for idx in prim + tidx:
                iname = f"{p}ix_{cident(idx.name)}"
                idx_names[idx.name] = iname
                cn = [str(t.col(c)) for c, _, _ in idx.cols]
                out.append(f"static const uint8_t {iname}_cols[] = {{{', '.join(cn)}}};")
                out.append(f"static const uint8_t {iname}_desc[] = "
                           f"{{{', '.join('1' if d else '0' for _, _, d in idx.cols)}}};")
                out.append(f"static const uint8_t {iname}_coll[] = "
                           f"{{{', '.join(COLL_C[k] for _, k, _ in idx.cols)}}};")
            out.append("")
            for idx in prim + tidx:
                iname = idx_names[idx.name]
                flags = []
                if idx.unique:
                    flags.append("SQNANO_IDX_UNIQUE")
                if idx.auto:
                    flags.append("SQNANO_IDX_AUTO")
                if idx in prim:
                    flags.append("SQNANO_IDX_PRIMARY")
                tables = list(cat.tables.values())
                out.append(
                    f"static const struct sqnano_index {iname} = {{.name = {cstr(idx.name)}, "
                    f".sql = {cstr(idx.sql)}, .cols = {iname}_cols, .desc = {iname}_desc, "
                    f".coll = {iname}_coll, .tree = {idx.tree}, .table = {tables.index(t)}, "
                    f".ncols = {len(idx.cols)}, .flags = {' | '.join(flags) or '0'}}};")
            if tidx:
                out.append(f"static const struct sqnano_index *const {p}{tn}_idx[] = {{"
                           + ", ".join(f"&{idx_names[i.name]}" for i in tidx) + "};")
            chk = "NULL"
            if t.checks:
                chk = f"{p}{tn}_check"
                out += check_function(self.unit, cat, t, chk)
            lz = [(v, fn) for tree, v, fn in self.lazy_fns if tree == t.tree and v <= cat.version]
            if lz:
                out.append(f"static const struct sqnano_lazy {p}{tn}_lazy[] = {{"
                           + ", ".join(f"{{{v}, {fn}}}" for v, fn in lz) + "};")
            tfks = [n for n, sp in enumerate(fks) if sp["child"] is t or sp["parent"] is t]
            if tfks:
                out.append(f"static const struct sqnano_fkey *const {p}{tn}_fks[] = {{"
                           + ", ".join(f"&{p}fk{n}" for n in tfks) + "};")
            out.append(
                f"const struct sqnano_table {p}{tn} = {{.name = {cstr(t.name)}, "
                f".sql = {cstr(t.sql)}, .cols = {p}{tn}_cols, "
                f".indexes = {p + tn + '_idx' if tidx else 'NULL'}, .check = {chk}, "
                f".sql_hash = 0x{C.ddl_hash(t.sql) if t.sql else 0:08x}u, .tree = {t.tree}, "
                f".ncols = {len(t.cols)}, .nslots = {t.next_slot}, "
                f".ipk = {t.ipk if t.ipk is not None else -1}, .nindexes = {len(tidx)}, "
                f".flags = {'SQNANO_TBL_AUTOINC' if t.autoinc else '0'}, "
                f".fks = {p + tn + '_fks' if tfks else 'NULL'}, .nfks = {len(tfks)}, "
                f".pk = {'&' + idx_names[t.pk_index.name] if prim else 'NULL'}, "
                f".lazy = {p + tn + '_lazy' if lz else 'NULL'}, .nlazy = {len(lz)}, "
                f".version = {cat.version}}};")
            out.append("")
        for n, sp in enumerate(fks):
            f = f"{p}fk{n}"
            out.append(f"static const uint8_t {f}_c[] = {{{', '.join(map(str, sp['ccols']))}}};")
            out.append(f"static const uint8_t {f}_p[] = {{{', '.join(map(str, sp['pcols']))}}};")
            if sp["cpos"] is not None:
                out.append(f"static const uint8_t {f}_o[] = {{{', '.join(map(str, sp['cpos']))}}};")
            pidx = f"&{idx_names[sp['pidx'].name]}" if sp["pidx"] else "NULL"
            cidx = f"&{idx_names[sp['cidx'].name]}" if sp["cidx"] else "NULL"
            out.append(
                f"static const struct sqnano_fkey {f} = {{.child = &{p}{cident(sp['child'].name)}, "
                f".parent = &{p}{cident(sp['parent'].name)}, .pidx = {pidx}, .cidx = {cidx}, "
                f".ccols = {f}_c, .pcols = {f}_p, .cpos = {f + '_o' if sp['cpos'] is not None else 'NULL'}, "
                f".ncols = {len(sp['ccols'])}, .on_delete = SQNANO_FK_{sp['on_delete']}, "
                f".on_update = SQNANO_FK_{sp['on_update']}, .deferred = {int(sp['deferred'])}}};")
        if fks:
            out.append("")
        tabs = list(cat.tables.values())
        out.append(f"static const struct sqnano_table *const {p}tables[] = {{"
                   + ", ".join(f"&{p}{cident(t.name)}" for t in tabs) + (" NULL" if not tabs else "") + "};")
        idxs = sorted(cat.indexes.values(), key=lambda i: i.tree)
        out.append(f"static const struct sqnano_index *const {p}indexes[] = {{"
                   + ", ".join(f"&{idx_names[i.name]}" for i in idxs) + (" NULL" if not idxs else "") + "};")
        steps = []
        for s in cat.steps:
            if s.op == "build_index":
                t = cat.tables[s.table]
                steps.append(f"\t{{.op = SQNANO_MIG_BUILD_INDEX, .tree = {s.tree}, "
                             f".table = &{p}{cident(t.name)}, .index = &{idx_names[cat.indexes[s.index].name]}}},")
            else:
                op = "SQNANO_MIG_CREATE_TREE" if s.op == "create_tree" else "SQNANO_MIG_DROP_TREE"
                steps.append(f"\t{{.op = {op}, .tree = {s.tree}}},")
        if steps:
            out.append(f"static const struct sqnano_mig_step {p}steps[] = {{")
            out += steps
            out.append("};")
        up = "NULL"
        if cat.up and cat.version in self.up_queries:
            up = f"{p}up"
        out.append(f"static const struct sqnano_catalog {p}catalog = {{.tables = {p}tables, "
                   f".indexes = {p}indexes, .steps = {p + 'steps' if steps else 'NULL'}, "
                   f".up = {up}, .hash = 0x{cat.hash:08x}u, .version = {cat.version}, "
                   f".compat = {cat.compat}, .ntables = {len(tabs)}, .nindexes = {len(idxs)}, "
                   f".nsteps = {len(steps)}, .lazy = {1 if cat.lazy else 0}}};")
        out.append("")

    # -- queries --
    def compile_query(self, q: QueryInfo, cat: C.Catalog, cname: str):
        q.cname = cname
        st, params = P.parse(q.sql)
        q.stmt = st
        q.params = params
        if isinstance(st, P.TxnStmt):
            q.kind = st.kind.upper()
            return None
        if isinstance(st, (P.CreateTable, P.CreateIndex)) and st.if_not_exists:
            why = C.check_ddl_matches(q.sql, cat)
            if why:
                raise SqlError(why)
            q.kind = "NOOP"
            return None
        if isinstance(st, (P.CreateTable, P.CreateIndex, P.AlterTable, P.Drop)):
            # Kept so that the call fails explicitly if the application ever runs it.
            self.warnings.append(f"{q.source}: schema change outside migrations; it will fail at "
                                 "run time (move it to schema/vN.sql)")
            q.kind = "FAIL"
            return None
        if isinstance(st, P.Pragma):
            q.kind = "PRAGMA"
            return None
        g = QueryGen(self.unit, cat, q, self.tprefix(cat))
        g.generate()
        q.param_types = [ctype_of(g.x.param_aff.get(i)) for i in range(len(params))]
        return g

    def emit_query(self, q: QueryInfo, g: QueryGen | None, out: list[str], hdr: list[str]):
        cname = q.cname
        kinds = {"SELECT": "SQNANO_Q_SELECT", "WRITE": "SQNANO_Q_WRITE", "BEGIN": "SQNANO_Q_BEGIN",
                 "COMMIT": "SQNANO_Q_COMMIT", "ROLLBACK": "SQNANO_Q_ROLLBACK",
                 "NOOP": "SQNANO_Q_NOOP", "FAIL": "SQNANO_Q_FAIL"}
        np_ = max(len(q.params), 1)
        nc = max(len(q.cols), 1)
        typed = q.name is not None and g is not None
        q.typed = typed
        struct = [f"struct {cname} {{", "\tstruct sqnano_exec x;", f"\tsqnano_value params[{np_}];",
                  f"\tsqnano_value out[{nc}];"]
        fields = []
        if typed:
            used = set(RESERVED_FIELDS)
            for name, decl, aff in q.cols:
                f = cident(name)
                base = f
                k = 2
                while f in used:
                    f = f"{base}_{k}"
                    k += 1
                used.add(f)
                ct = ctype_of(aff)
                fields.append((f, ct))
                struct.append(f"\t{ct or 'sqnano_value'} {f};")
        struct.append("\tstruct {")
        if g is not None:
            for decl in g.fields.values():
                struct.append(f"\t\t{decl}")
        struct.append("\t\tuint8_t unused;")
        struct.append("\t} _s;")
        struct.append("\tuint8_t _scratch[SQNANO_SCRATCH_SIZE];")
        struct.append("};")
        (hdr if typed else out).extend(struct + [""])
        # Functions
        out.append(f"static void {cname}_xinit(struct sqnano_exec *x)")
        out.append("{")
        out.append(f"\tstruct {cname} *q = (struct {cname} *)x;")
        out.append("")
        out.append("\tx->params = q->params;")
        out.append("\tx->out = q->out;")
        out.append("\tx->scratch.buf = q->_scratch;")
        out.append("\tx->scratch.cap = SQNANO_SCRATCH_SIZE;")
        out.append(f"\tfor (int i = 0; i < {np_}; i++) {{")
        out.append("\t\tsqnano_v_null(&q->params[i]);")
        out.append("\t}")
        out.append(f"\tfor (int i = 0; i < {nc}; i++) {{")
        out.append("\t\tsqnano_v_null(&q->out[i]);")
        out.append("\t}")
        if g is not None:
            for name in g.fields:
                if name in g.sorter_names:
                    out.append(f"\tq->_s.{name}.slot = -1;")
        out.append("}")
        out.append("")
        out.append(f"static void {cname}_xclose(struct sqnano_exec *x)")
        out.append("{")
        out.append(f"\tstruct {cname} *q = (struct {cname} *)x;")
        out.append("")
        out.append("\t(void)q;")
        if g is not None:
            for c in g.closers:
                out.append(f"\t{c}")
        out.append("}")
        out.append("")
        out.append(f"static int {cname}_xstep(struct sqnano_exec *x)")
        out.append("{")
        if g is not None:
            out.append(f"\tstruct {cname} *q = (struct {cname} *)x;")
            out.append(f"\tsqnano_value t[{max(g.x.ntemps, 1)}];")
            out.append("\tint rc = SQNANO_OK;")
            out.append("")
            out.append("\t(void)q;")
            out.append("\t(void)t;")
            out.append("\t(void)rc;")
            out += g.em.lines
        else:
            out.append("\t(void)x;")
            out.append("\treturn SQNANO_DONE;")
        out.append("}")
        out.append("")
        pnames = ", ".join(cstr(p) if p else "NULL" for p in q.params) or "NULL"
        cnames = ", ".join(cstr(c[0]) for c in q.cols) or "NULL"
        dnames = ", ".join(cstr(c[1]) if c[1] else "NULL" for c in q.cols) or "NULL"
        out.append(f"static const char *const {cname}_params[] = {{{pnames}}};")
        out.append(f"static const char *const {cname}_cols[] = {{{cnames}}};")
        out.append(f"static const char *const {cname}_decltypes[] = {{{dnames}}};")
        storage = "" if typed else "static "
        out.append(
            f"{storage}const struct sqnano_query {cname}_def = {{.name = "
            f"{cstr(q.name) if q.name else 'NULL'}, .sql = {cstr(q.norm)}, .params = {cname}_params, "
            f".cols = {cname}_cols, .decltypes = {cname}_decltypes, .init = {cname}_xinit, "
            f".step = {cname}_xstep, .close = {cname}_xclose, .hash = 0x{q.hash:08x}u, "
            f".state_size = sizeof(struct {cname}), .nparams = {len(q.params)}, "
            f".ncols = {len(q.cols)}, .kind = {kinds.get(q.kind, 'SQNANO_Q_NOOP')}}};")
        out.append("")
        if typed:
            self.emit_typed(q, fields, hdr)

    def emit_typed(self, q: QueryInfo, fields, hdr: list[str]):
        cname = q.cname
        hdr.append(f"extern const struct sqnano_query {cname}_def;")
        hdr.append("")
        hdr.append(f"/** Prepare `{q.name}` (txn NULL: autocommit). */")
        hdr.append(f"static inline int {cname}_open(struct {cname} *q, struct sqnano_db *db, "
                   "struct sqnano_txn *txn)")
        hdr.append("{")
        hdr.append(f"\treturn sqnano_exec_open(&q->x, &{cname}_def, db, txn);")
        hdr.append("}")
        hdr.append("")
        if q.params:
            args = []
            body = []
            for i, (p, ct) in enumerate(zip(q.params, q.param_types)):
                an = cident((p or f"p{i + 1}").lstrip(":@$?")) if p else f"p{i + 1}"
                if an[0].isdigit() or an in ("q",):
                    an = f"p{an}"
                if ct == "int64_t":
                    args.append(f"int64_t {an}")
                    body.append(f"\tsqnano_v_int(&q->params[{i}], {an});")
                elif ct == "double":
                    args.append(f"double {an}")
                    body.append(f"\tsqnano_v_real(&q->params[{i}], {an});")
                elif ct == "const char *":
                    args.append(f"const char *{an}")
                    body.append(f"\tif ({an}) {{")
                    body.append(f"\t\tsqnano_v_text(&q->params[{i}], {an}, (uint32_t)strlen({an}));")
                    body.append("\t} else {")
                    body.append(f"\t\tsqnano_v_null(&q->params[{i}]);")
                    body.append("\t}")
                else:
                    args.append(f"const sqnano_value *{an}")
                    body.append(f"\tq->params[{i}] = *{an};")
            hdr.append(f"/** Bind the parameters of `{q.name}`. Text must stay valid until reset. */")
            hdr.append(f"static inline void {cname}_bind(struct {cname} *q, {', '.join(args)})")
            hdr.append("{")
            hdr += body
            hdr.append("}")
            hdr.append("")
        hdr.append(f"/** Next row of `{q.name}`: SQNANO_ROW, SQNANO_DONE or an error. */")
        hdr.append(f"static inline int {cname}_step(struct {cname} *q)")
        hdr.append("{")
        hdr.append("\tint rc = sqnano_exec_step(&q->x);")
        hdr.append("")
        if fields:
            hdr.append("\tif (rc == SQNANO_ROW) {")
            for i, (f, ct) in enumerate(fields):
                if ct == "int64_t":
                    hdr.append(f"\t\tq->{f} = sqnano_v_as_int(&q->out[{i}]);")
                elif ct == "double":
                    hdr.append(f"\t\tq->{f} = sqnano_v_as_real(&q->out[{i}]);")
                elif ct == "const char *":
                    hdr.append(f"\t\tq->{f} = q->out[{i}].type == SQNANO_TEXT ? q->out[{i}].u.z : NULL;")
                else:
                    hdr.append(f"\t\tq->{f} = q->out[{i}];")
            hdr.append("\t}")
        hdr.append("\treturn rc;")
        hdr.append("}")
        hdr.append("")
        hdr.append(f"static inline int {cname}_reset(struct {cname} *q)")
        hdr.append("{")
        hdr.append("\treturn sqnano_exec_reset(&q->x);")
        hdr.append("}")
        hdr.append("")
        hdr.append(f"/** Run `{q.name}` to completion. */")
        hdr.append(f"static inline int {cname}_run(struct {cname} *q)")
        hdr.append("{")
        hdr.append("\treturn sqnano_exec_run(&q->x);")
        hdr.append("}")
        hdr.append("")

    def generate(self) -> tuple[str, str, str]:
        cur = self.hist.current
        hdr: list[str] = []
        body: list[str] = []
        compiled = []
        for q in self.queries:
            cname = f"q_{cident(q.name)}" if q.name else f"qa_{q.hash:08x}"
            try:
                g = self.compile_query(q, cur, cname)
            except SqlError as e:
                raise SqlError(f"{q.source}: {e.msg}", e.pos) from None
            compiled.append((q, g))
        # The runtime handles transaction statements and pragmas natively.
        compiled = [(q, g) for q, g in compiled
                    if q.kind not in ("BEGIN", "COMMIT", "ROLLBACK", "PRAGMA", "SAVEPOINT",
                                      "RELEASE", "ROLLBACK_TO")]
        up_compiled = {}
        for cat in self.versions:
            if not cat.up:
                continue
            lst = []
            for k, (sql, fname, line) in enumerate(cat.up):
                q = QueryInfo(sql, normalize(sql)[0], fnv1a(normalize(sql)[0]), None, f"{fname}:{line}")
                try:
                    g = self.compile_query(q, cat, f"qm{cat.version}_{k}")
                except SqlError as e:
                    raise SqlError(f"{q.source}: {e.msg}", e.pos) from None
                if q.kind != "WRITE" and q.kind != "SELECT":
                    raise SqlError(f"{q.source}: only INSERT, UPDATE, DELETE are allowed in vN_up.sql")
                if q.params:
                    raise SqlError(f"{q.source}: parameters are not allowed in migrations")
                lst.append((q, g))
            up_compiled[cat.version] = lst
            self.up_queries[cat.version] = [q for q, _ in lst]
        self.compile_lazy()
        for cat in self.versions:
            self.emit_catalog_pre(cat)
        for cat in self.versions:
            self.emit_catalog(cat)
        for q, g in compiled:
            self.emit_query(q, g, body, hdr)
        for v, lst in up_compiled.items():
            for q, g in lst:
                self.emit_query(q, g, body, hdr)
            cat = next(c for c in self.versions if c.version == v)
            p = self.tprefix(cat)
            body.append(f"static int {p}up(struct sqnano_exec *mx)")
            body.append("{")
            for k in range(len(lst)):
                body.append(f"\tstatic struct qm{v}_{k} s{k};")
            body.append("\tint rc = SQNANO_OK;")
            body.append("")
            for k in range(len(lst)):
                body.append(f"\tif (rc == SQNANO_OK) {{")
                body.append(f"\t\trc = sqnano_exec_open(&s{k}.x, &qm{v}_{k}_def, mx->db, mx->txn);")
                body.append("\t}")
                body.append(f"\tif (rc == SQNANO_OK) {{")
                body.append(f"\t\trc = sqnano_exec_run(&s{k}.x);")
                body.append("\t}")
            body.append("\treturn rc;")
            body.append("}")
            body.append("")
        # Query table sorted by hash.
        allq = sorted((q for q, _ in compiled), key=lambda q: q.hash)
        states = [q.cname for q, _ in compiled] + [q.cname for lst in up_compiled.values() for q, _ in lst]
        c = []
        c.append("/* Generated by sqnanoc. Do not edit. */")
        c.append("")
        c.append("#include <math.h>")
        c.append("#include <stdint.h>")
        c.append("#include <string.h>")
        c.append("")
        c.append(f'#include "{self.prefix}.h"')
        c.append("")
        c += self.unit.consts
        c.append("")
        c += self.out_c
        # Forward declarations of migration functions are emitted with the catalogs.
        c += body
        c.append(f"static const struct sqnano_query *const queries[] = {{"
                 + ", ".join(f"&{q.cname}_def" for q in allq) + (" NULL" if not allq else "") + "};")
        c.append(f"static const struct sqnano_catalog *const versions[] = {{"
                 + ", ".join(f"&{self.tprefix(cat)}catalog" for cat in self.versions) + "};")
        c.append("")
        c.append(f"union {self.prefix}_states {{")
        c.append("\tstruct sqnano_exec x;")
        for s in states:
            c.append(f"\tstruct {s} {s};")
        c.append("};")
        c.append("")
        c.append("#if SQNANO_POOL_SLOTS > 0")
        c.append(f"#define STATE_MAX ((sizeof(union {self.prefix}_states) + 7u) & ~7u)")
        c.append("#if SQNANO_STMT_POOL_SIZE > 0")
        c.append("#define POOL_BYTES ((SQNANO_STMT_POOL_SIZE + 7u) & ~7u)")
        c.append("#define POOL_SLOT 0")
        c.append("#else")
        c.append("#define POOL_BYTES (STATE_MAX * SQNANO_POOL_SLOTS)")
        c.append("#define POOL_SLOT STATE_MAX")
        c.append("#endif")
        c.append("static union {")
        c.append("\tuint64_t align;")
        c.append("\tvoid *ptr;")
        c.append("\tuint8_t b[POOL_BYTES];")
        c.append("} pool_mem;")
        c.append("static struct sqnano_pool pool = {")
        c.append("\t.buf = pool_mem.b, .size = POOL_BYTES, .slot = POOL_SLOT, .largest = STATE_MAX,")
        c.append("};")
        c.append("#define STMT_POOL (&pool)")
        c.append("#else")
        c.append("#define STMT_POOL NULL")
        c.append("#endif")
        c.append("")
        c.append("const struct sqnano_schema sqnano_app_schema = {")
        c.append("\t.versions = versions,")
        c.append("\t.queries = queries,")
        c.append("\t.pool = STMT_POOL,")
        c.append(f"\t.nqueries = {len(allq)},")
        c.append(f"\t.nversions = {len(self.versions)},")
        c.append("};")
        c.append("")
        h = []
        guard = cident(self.prefix).upper() + "_H_"
        h.append("/* Generated by sqnanoc. Do not edit. */")
        h.append("")
        h.append(f"#ifndef {guard}")
        h.append(f"#define {guard}")
        h.append("")
        h.append("#include <string.h>")
        h.append("")
        h.append("#include <sqnano/exec.h>")
        h.append("")
        h.append("#ifdef __cplusplus")
        h.append('extern "C" {')
        h.append("#endif")
        h.append("")
        h.append("/** Schema, migrations and precompiled statements of the application. */")
        h.append("extern const struct sqnano_schema sqnano_app_schema;")
        h.append("")
        h.append(f"#define SQNANO_SCHEMA_VERSION {cur.version}")
        h.append(f"#define SQNANO_SCHEMA_HASH 0x{cur.hash:08x}u")
        for t in cur.tables.values():
            h.append(f"#define TREE_{cident(t.name).upper()} {t.tree}")
        for i in cur.indexes.values():
            h.append(f"#define TREE_{cident(i.name).upper()} {i.tree}")
        h.append("")
        for t in cur.tables.values():
            h.append(f"extern const struct sqnano_table {self.tprefix(cur)}{cident(t.name)};")
        h.append("")
        h += hdr
        h.append("#ifdef __cplusplus")
        h.append("}")
        h.append("#endif")
        h.append("")
        h.append(f"#endif /* {guard} */")
        h.append("")
        rep = self.report(compiled)
        return "\n".join(h), "\n".join(c), rep

    def compile_lazy(self):
        for cat in self.versions:
            for k, (sql, fname, line) in enumerate(cat.lazy):
                where = f"{fname}:{line}"
                try:
                    st, params = P.parse(sql)
                    if params:
                        raise SqlError("parameters are not allowed in migrations")
                    if not isinstance(st, P.Update):
                        raise SqlError("only UPDATE statements are allowed in vN_lazy.sql")
                    fn = f"{self.tprefix(cat)}lazy{k}"
                    t, lines = lazy_function(self.unit, cat, st, fn)
                except SqlError as e:
                    raise SqlError(f"{where}: {e.msg}", e.pos) from None
                self.out_c += [f"/* {where}: {normalize(sql)[0]} */"] + lines
                self.lazy_fns.append((t.tree, cat.version, fn))

    def emit_catalog_pre(self, cat: C.Catalog):
        if cat.up:
            self.out_c.append(f"static int {self.tprefix(cat)}up(struct sqnano_exec *mx);")
            self.out_c.append("")

    def report(self, compiled) -> str:
        lines = ["sqnanoc resource report", ""]
        cur = self.hist.current
        lines.append(f"schema version {cur.version} (hash 0x{cur.hash:08x}, compatible down to v{cur.compat})")
        lines.append(f"migrations embedded from v{self.versions[0].version}")
        for w in cur.warnings:
            lines.append(f"warning: {w}")
        lines.append("")
        for q, g in compiled:
            label = q.name or f"<{q.hash:08x}>"
            lines.append(f"{label}  [{q.kind}]  {q.source}")
            lines.append(f"    {q.norm}")
            for r in q.report:
                lines.append(f"    plan: {r}")
                if r.startswith("SCAN ") and "INDEX" not in r:
                    self.warnings.append(f"{q.source}: full table scan ({label})")
                if "sort in RAM" in r:
                    self.warnings.append(f"{q.source}: needs a sort buffer ({label})")
            if g is not None:
                lines.append(f"    temporaries: {g.x.ntemps}, sort buffers: "
                             f"{sum(1 for f in g.fields if f.startswith('so'))}")
        return "\n".join(lines) + "\n"
