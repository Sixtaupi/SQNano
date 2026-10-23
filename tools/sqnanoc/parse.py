# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""Recursive-descent parser for the SQL subset of SQNano.

The grammar and operator precedence follow SQLite's parse.y. Constructs
outside the version 1 subset are rejected with an explicit message rather
than silently ignored.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from lex import SqlError, Tok, tokenize

# ---- AST ----


@dataclass
class Expr:
    pos: int = field(default=0, kw_only=True)
    end: int = field(default=0, kw_only=True)


@dataclass
class Lit(Expr):
    kind: str  # int, real, text, blob, null
    value: object


@dataclass
class Param(Expr):
    name: str  # "?", "?3", ":id"
    index: int = 0  # 0-based slot, set by the parser


@dataclass
class Col(Expr):
    table: str | None
    name: str


@dataclass
class Unary(Expr):
    op: str  # -, +, ~, not
    e: Expr


@dataclass
class Binary(Expr):
    op: str
    l: Expr
    r: Expr


@dataclass
class Like(Expr):
    op: str  # like, glob
    e: Expr
    pat: Expr
    esc: Expr | None
    neg: bool


@dataclass
class Between(Expr):
    e: Expr
    lo: Expr
    hi: Expr
    neg: bool


@dataclass
class InList(Expr):
    e: Expr
    items: list[Expr]
    neg: bool


@dataclass
class IsNull(Expr):
    e: Expr
    neg: bool


@dataclass
class Case(Expr):
    base: Expr | None
    whens: list[tuple[Expr, Expr]]
    other: Expr | None


@dataclass
class Cast(Expr):
    e: Expr
    typename: str


@dataclass
class Func(Expr):
    name: str
    args: list[Expr]
    star: bool = False
    distinct: bool = False


@dataclass
class Collate(Expr):
    e: Expr
    coll: str


@dataclass
class Subquery(Expr):
    """Scalar subquery: (SELECT ...)."""
    select: "Select"


@dataclass
class Exists(Expr):
    select: "Select"
    neg: bool


@dataclass
class InSelect(Expr):
    e: Expr
    select: "Select"
    neg: bool


@dataclass
class ResultCol:
    expr: Expr | None  # None for *
    alias: str | None
    text: str
    star_table: str | None = None


@dataclass
class OrderTerm:
    expr: Expr
    desc: bool


@dataclass
class Select:
    distinct: bool
    cols: list[ResultCol]
    table: str | None
    alias: str | None
    where: Expr | None
    group: list[Expr]
    having: Expr | None
    order: list[OrderTerm]
    limit: Expr | None
    offset: Expr | None
    joins: list["Join"] = field(default_factory=list)
    # Compound SELECT: (operator, part) after this first part; ORDER BY and
    # LIMIT of the whole compound.
    compound: list = field(default_factory=list)
    c_order: list = field(default_factory=list)
    c_limit: "Expr | None" = None
    c_offset: "Expr | None" = None


@dataclass
class Join:
    table: str
    alias: str | None
    kind: str  # inner, left
    on: Expr | None = None
    using: list[str] | None = None
    natural: bool = False


@dataclass
class Upsert:
    target: list[str] | None
    target_where: Expr | None
    action: str  # nothing, update
    sets: list[tuple[list[str], Expr]]
    where: Expr | None


@dataclass
class Insert:
    table: str
    columns: list[str] | None
    rows: list[list[Expr]]  # empty list: DEFAULT VALUES
    oe: str  # abort, replace, ignore, fail, rollback
    upsert: list[Upsert]
    returning: list[ResultCol]
    select: "Select | None" = None


@dataclass
class Update:
    table: str
    sets: list[tuple[list[str], Expr]]
    where: Expr | None
    oe: str
    returning: list[ResultCol]


@dataclass
class Delete:
    table: str
    where: Expr | None
    returning: list[ResultCol]


@dataclass
class TxnStmt:
    kind: str  # begin, commit, rollback, savepoint, release, rollback_to
    name: str | None = None


@dataclass
class ColDef:
    name: str
    typename: str
    pk: bool = False
    pk_desc: bool = False
    autoinc: bool = False
    notnull: bool = False
    unique: bool = False
    check: list[tuple[str | None, Expr, str]] = field(default_factory=list)
    default: Expr | None = None
    collate: str | None = None
    fk: tuple[str, list[str], dict] | None = None


@dataclass
class TableConstraint:
    kind: str  # pk, unique, check, fk
    name: str | None
    cols: list[tuple[str, str | None, bool]]  # name, collate, desc
    check: Expr | None = None
    check_text: str = ""
    autoinc: bool = False
    ref: str | None = None  # FOREIGN KEY: parent table, columns and actions
    ref_cols: list[str] = field(default_factory=list)
    actions: dict = field(default_factory=dict)


@dataclass
class CreateTable:
    name: str
    if_not_exists: bool
    cols: list[ColDef]
    constraints: list[TableConstraint]
    without_rowid: bool


@dataclass
class CreateIndex:
    name: str
    unique: bool
    if_not_exists: bool
    table: str
    cols: list[tuple[str, str | None, bool]]


@dataclass
class AlterTable:
    table: str
    action: str  # add, rename_col, rename_table, drop_col
    coldef: ColDef | None = None
    old: str | None = None
    new: str | None = None


@dataclass
class Drop:
    kind: str  # table, index
    name: str
    if_exists: bool


@dataclass
class Pragma:
    name: str
    value: object | None


# Words that cannot be used as a bare column name in expressions.
_RESERVED = {
    "select", "from", "where", "group", "having", "order", "limit", "offset", "and", "or",
    "not", "is", "in", "like", "glob", "between", "case", "when", "then", "else", "end",
    "cast", "as", "on", "by", "asc", "desc", "union", "values", "set", "returning", "null",
    "escape", "collate", "distinct", "all", "exists", "join", "inner", "left", "cross",
    "natural", "using", "into", "default", "isnull", "notnull", "do", "conflict", "except",
    "intersect", "window", "over", "filter",
}

_UNSUPPORTED_STMT = {
    "with": "common table expressions (WITH) are not supported in version 1",
    "attach": "ATTACH is not supported",
    "detach": "DETACH is not supported",
    "vacuum": "VACUUM is not supported",
    "analyze": "ANALYZE is not supported",
    "reindex": "REINDEX is not supported",
    "explain": "EXPLAIN is only available in the shell",
}


class Parser:
    def __init__(self, sql: str):
        self.sql = sql
        self.toks = tokenize(sql)
        self.i = 0
        self.params: list[str | None] = []  # slot -> name (None for a bare "?")
        self._next_pos = 0  # highest ?NNN

    # -- token helpers --
    @property
    def t(self) -> Tok:
        return self.toks[self.i]

    def peek(self, k: int = 1) -> Tok:
        return self.toks[min(self.i + k, len(self.toks) - 1)]

    def next(self) -> Tok:
        t = self.toks[self.i]
        self.i += 1
        return t

    def error(self, msg: str, tok: Tok | None = None):
        raise SqlError(msg, (tok or self.t).pos)

    def kw(self, *words: str) -> bool:
        if self.t.is_kw(*words):
            self.i += 1
            return True
        return False

    def expect_kw(self, word: str):
        if not self.kw(word):
            self.error(f"expected {word.upper()} near '{self.t.text}'")

    def op(self, o: str) -> bool:
        if self.t.kind == "op" and self.t.value == o:
            self.i += 1
            return True
        return False

    def expect_op(self, o: str):
        if not self.op(o):
            self.error(f"expected '{o}' near '{self.t.text or 'end of input'}'")

    def name(self, what: str = "name") -> str:
        t = self.t
        if t.kind == "qid" or t.kind == "str":
            self.i += 1
            return str(t.value)
        if t.kind == "id":
            self.i += 1
            return str(t.value) if t.kind == "id" else t.text
        self.error(f"expected {what} near '{t.text or 'end of input'}'")
        return ""

    def savepoint_name(self) -> str:
        t = self.t
        if t.kind in ("id", "qid", "str"):
            self.i += 1
            return str(t.value)
        self.error(f"expected a savepoint name near '{t.text or 'end of input'}'")
        return ""

    def ident_text(self) -> str:
        """Identifier keeping its original spelling (for declared names)."""
        t = self.t
        if t.kind == "qid":
            self.i += 1
            return str(t.value)
        if t.kind == "id" or t.kind == "str":
            self.i += 1
            return t.text if t.kind == "id" else str(t.value)
        self.error(f"expected a name near '{t.text or 'end of input'}'")
        return ""

    def text(self, a: int, b: int) -> str:
        return self.sql[a:b]

    def at_end(self) -> bool:
        while self.op(";"):
            pass
        return self.t.kind == "eof"

    # -- statements --
    def statement(self):
        t = self.t
        if t.kind == "id" and t.value in _UNSUPPORTED_STMT:
            self.error(_UNSUPPORTED_STMT[t.value])
        if self.kw("select"):
            st = self.select_compound()
        elif self.t.is_kw("insert", "replace"):
            st = self.insert()
        elif self.kw("update"):
            st = self.update()
        elif self.kw("delete"):
            st = self.delete()
        elif self.kw("begin"):
            self.kw("deferred", "immediate", "exclusive")
            self.kw("transaction")
            st = TxnStmt("begin")
        elif self.kw("commit", "end"):
            self.kw("transaction")
            st = TxnStmt("commit")
        elif self.kw("rollback"):
            self.kw("transaction")
            if self.kw("to"):
                self.kw("savepoint")
                st = TxnStmt("rollback_to", self.savepoint_name())
            else:
                st = TxnStmt("rollback")
        elif self.t.kind == "id" and self.t.value == "savepoint":
            self.next()
            st = TxnStmt("savepoint", self.savepoint_name())
        elif self.t.kind == "id" and self.t.value == "release":
            self.next()
            if self.t.kind == "id" and self.t.value == "savepoint":
                self.next()
            st = TxnStmt("release", self.savepoint_name())
        elif self.kw("create"):
            st = self.create()
        elif self.kw("alter"):
            st = self.alter()
        elif self.kw("drop"):
            st = self.drop()
        elif self.kw("pragma"):
            st = self.pragma()
        elif t.kind == "eof":
            self.error("empty statement")
        else:
            self.error(f"near '{t.text}': syntax error")
        if not self.at_end():
            if self.t.is_kw("union", "intersect", "except"):
                self.error("compound SELECT is not supported in version 1")
            self.error(f"near '{self.t.text}': syntax error")
        return st

    def result_cols(self) -> list[ResultCol]:
        cols = []
        while True:
            start = self.t.pos
            if self.op("*"):
                cols.append(ResultCol(None, None, "*"))
            elif (self.t.kind in ("id", "qid") and self.peek().kind == "op" and self.peek().value == "."
                  and self.peek(2).kind == "op" and self.peek(2).value == "*"):
                tbl = self.name()
                self.next()
                self.next()
                cols.append(ResultCol(None, None, "*", star_table=tbl))
            else:
                e = self.expr()
                end = self.toks[self.i - 1].end
                alias = None
                if self.kw("as"):
                    alias = self.ident_text()
                elif self.t.kind in ("qid", "str") or (
                    self.t.kind == "id" and self.t.value not in _RESERVED
                ):
                    alias = self.ident_text()
                cols.append(ResultCol(e, alias, self.text(start, end)))
            if not self.op(","):
                return cols

    def select_body(self) -> Select:
        distinct = False
        if self.kw("distinct"):
            distinct = True
        else:
            self.kw("all")
        cols = self.result_cols()
        table = alias = None
        where = having = limit = offset = None
        group: list[Expr] = []
        order: list[OrderTerm] = []
        joins: list[Join] = []
        if self.kw("from"):
            table, alias = self.table_ref()
            while True:
                natural = False
                kind = "inner"
                if self.op(","):
                    pass
                else:
                    if self.kw("natural"):
                        natural = True
                    if self.t.is_kw("right", "full"):
                        self.error("RIGHT and FULL OUTER JOIN are not supported in version 1")
                    if self.kw("left"):
                        self.kw("outer")
                        kind = "left"
                    elif not self.kw("inner"):
                        self.kw("cross")
                    if not self.kw("join"):
                        if natural or kind != "inner":
                            self.error(f"expected JOIN near '{self.t.text or 'end of input'}'")
                        break
                jt, ja = self.table_ref()
                j = Join(jt, ja, kind, natural=natural)
                if self.kw("on"):
                    j.on = self.expr()
                elif self.kw("using"):
                    self.expect_op("(")
                    j.using = []
                    while True:
                        j.using.append(self.name("column name"))
                        if not self.op(","):
                            break
                    self.expect_op(")")
                if natural and (j.on is not None or j.using is not None):
                    self.error("a NATURAL join may not have an ON or USING clause")
                joins.append(j)
        if self.kw("where"):
            where = self.expr()
        if self.kw("group"):
            self.expect_kw("by")
            group = self.expr_list()
            if self.kw("having"):
                having = self.expr()
        elif self.kw("having"):
            having = self.expr()
        if self.t.is_kw("window"):
            self.error("window functions are not supported")
        if self.kw("order"):
            self.expect_kw("by")
            order = self.order_terms()
        if self.kw("limit"):
            limit = self.expr()
            if self.kw("offset"):
                offset = self.expr()
            elif self.op(","):
                offset, limit = limit, self.expr()
        return Select(distinct, cols, table, alias, where, group, having, order, limit, offset, joins)

    def table_ref(self) -> tuple[str, str | None]:
        if self.t.kind == "op" and self.t.value == "(":
            self.error("subqueries in FROM are not supported in version 1")
        table = self.name("table name")
        alias = None
        if self.op("."):
            table = self.name("table name")
        if self.kw("as"):
            alias = self.name()
        elif self.t.kind in ("id", "qid") and self.t.value not in _RESERVED and self.t.value not in (
            "indexed", "natural", "left", "inner", "cross", "join", "right", "full", "using",
        ):
            alias = self.name()
        if self.kw("indexed"):
            self.expect_kw("by")
            self.name()
        elif self.t.is_kw("not") and self.peek().is_kw("indexed"):
            self.next()
            self.next()
        return table, alias

    def subselect(self) -> "Select":
        """SELECT inside parentheses (the '(' is consumed, the ')' too)."""
        if self.t.is_kw("with"):
            self.error("common table expressions (WITH) are not supported in version 1")
        self.expect_kw("select")
        st = self.select_compound()
        self.expect_op(")")
        return st

    def select_compound(self) -> "Select":
        """SELECT ... [UNION [ALL] | INTERSECT | EXCEPT SELECT ...]* (after 'select')."""
        st = self.select_body()
        last = st
        while self.t.is_kw("union", "intersect", "except"):
            op = self.next().value
            if op == "union" and self.kw("all"):
                op = "union all"
            if last.order:
                self.error(f"ORDER BY clause should come after {op.upper()} not before")
            if last.limit is not None:
                self.error(f"LIMIT clause should come after {op.upper()} not before")
            self.expect_kw("select")
            last = self.select_body()
            st.compound.append((op, last))
        if st.compound:
            st.c_order, st.c_limit, st.c_offset = last.order, last.limit, last.offset
            last.order, last.limit, last.offset = [], None, None
        return st

    def order_terms(self) -> list[OrderTerm]:
        terms = []
        while True:
            e = self.expr()
            desc = False
            if self.kw("desc"):
                desc = True
            else:
                self.kw("asc")
            if self.kw("nulls"):
                self.error("NULLS FIRST/LAST is not supported in version 1")
            terms.append(OrderTerm(e, desc))
            if not self.op(","):
                return terms

    def conflict_clause(self) -> str:
        if self.kw("or"):
            w = self.name()
            if w not in ("rollback", "abort", "fail", "ignore", "replace"):
                self.error(f"unknown conflict resolution '{w}'")
            return w
        return "abort"

    def returning(self) -> list[ResultCol]:
        if self.kw("returning"):
            return self.result_cols()
        return []

    def set_list(self) -> list[tuple[list[str], Expr]]:
        sets = []
        while True:
            if self.op("("):
                names = [self.name()]
                while self.op(","):
                    names.append(self.name())
                self.expect_op(")")
                self.expect_op("=")
                self.error("row-value assignment is not supported in version 1")
            else:
                names = [self.name("column name")]
                self.expect_op("=")
            sets.append((names, self.expr()))
            if not self.op(","):
                return sets

    def insert(self) -> Insert:
        if self.kw("replace"):
            oe = "replace"
        else:
            self.expect_kw("insert")
            oe = self.conflict_clause()
        self.expect_kw("into")
        table = self.name("table name")
        if self.op("."):
            table = self.name("table name")
        if self.kw("as"):
            self.name()
        columns = None
        if self.op("("):
            columns = [self.name("column name")]
            while self.op(","):
                columns.append(self.name("column name"))
            self.expect_op(")")
        rows: list[list[Expr]] = []
        select = None
        if self.kw("default"):
            self.expect_kw("values")
        elif self.t.is_kw("select"):
            self.next()
            select = self.select_compound()
        elif self.kw("values"):
            while True:
                self.expect_op("(")
                rows.append(self.expr_list())
                self.expect_op(")")
                if not self.op(","):
                    break
        elif self.t.is_kw("with"):
            self.error("common table expressions (WITH) are not supported in version 1")
        else:
            self.error(f"near '{self.t.text}': syntax error")
        upserts = []
        while self.kw("on"):
            self.expect_kw("conflict")
            target = None
            twhere = None
            if self.op("("):
                target = [self.name("column name")]
                self.kw("collate") and self.name()
                self.kw("asc", "desc")
                while self.op(","):
                    target.append(self.name("column name"))
                    self.kw("asc", "desc")
                self.expect_op(")")
                if self.kw("where"):
                    twhere = self.expr()
            self.expect_kw("do")
            if self.kw("nothing"):
                upserts.append(Upsert(target, twhere, "nothing", [], None))
            else:
                self.expect_kw("update")
                self.expect_kw("set")
                sets = self.set_list()
                w = self.expr() if self.kw("where") else None
                upserts.append(Upsert(target, twhere, "update", sets, w))
        ret = self.returning()
        return Insert(table, columns, rows, oe, upserts, ret, select)

    def update(self) -> Update:
        oe = self.conflict_clause()
        table = self.name("table name")
        if self.op("."):
            table = self.name("table name")
        self.expect_kw("set")
        sets = self.set_list()
        if self.kw("from"):
            self.error("UPDATE ... FROM is not supported in version 1")
        where = self.expr() if self.kw("where") else None
        ret = self.returning()
        if self.t.is_kw("order", "limit"):
            self.error("UPDATE ... ORDER BY/LIMIT is not supported")
        return Update(table, sets, where, oe, ret)

    def delete(self) -> Delete:
        self.expect_kw("from")
        table = self.name("table name")
        if self.op("."):
            table = self.name("table name")
        where = self.expr() if self.kw("where") else None
        ret = self.returning()
        if self.t.is_kw("order", "limit"):
            self.error("DELETE ... ORDER BY/LIMIT is not supported")
        return Delete(table, where, ret)

    def pragma(self) -> Pragma:
        name = self.name("pragma name")
        if self.op("."):
            name = self.name("pragma name")
        value = None
        if self.op("="):
            value = self.pragma_value()
        elif self.op("("):
            value = self.pragma_value()
            self.expect_op(")")
        return Pragma(name, value)

    def pragma_value(self):
        neg = self.op("-")
        t = self.next()
        if t.kind in ("int", "real"):
            return -t.value if neg else t.value
        if t.kind in ("id", "str", "qid"):
            return t.value
        self.error("bad pragma value", t)
        return None

    # -- DDL --
    def typename(self) -> str:
        words = []
        while self.t.kind in ("id", "qid") and not (
            self.t.kind == "id"
            and self.t.value
            in ("primary", "not", "null", "unique", "check", "default", "collate", "references",
                "constraint", "generated", "as")
        ):
            words.append(self.ident_text())
        if words and self.op("("):
            args = []
            while True:
                neg = "-" if self.op("-") else ""
                self.op("+")
                t = self.next()
                if t.kind not in ("int", "real"):
                    self.error("bad type size", t)
                args.append(neg + t.text)
                if not self.op(","):
                    break
            self.expect_op(")")
            return " ".join(words) + "(" + ",".join(args) + ")"
        return " ".join(words)

    def column_def(self) -> ColDef:
        name = self.ident_text()
        cd = ColDef(name, self.typename())
        while True:
            cname = None
            if self.kw("constraint"):
                cname = self.ident_text()
            if self.kw("primary"):
                self.expect_kw("key")
                cd.pk = True
                if self.kw("desc"):
                    cd.pk_desc = True
                else:
                    self.kw("asc")
                self.on_conflict()
                if self.kw("autoincrement"):
                    cd.autoinc = True
            elif self.kw("not"):
                self.expect_kw("null")
                cd.notnull = True
                self.on_conflict()
            elif self.kw("null"):
                pass
            elif self.kw("unique"):
                cd.unique = True
                self.on_conflict()
            elif self.kw("check"):
                self.expect_op("(")
                a = self.t.pos
                e = self.expr()
                b = self.toks[self.i - 1].end
                self.expect_op(")")
                cd.check.append((cname, e, self.text(a, b)))
            elif self.kw("default"):
                if self.op("("):
                    cd.default = self.expr()
                    self.expect_op(")")
                else:
                    a = self.t.pos
                    neg = self.op("-")
                    if not neg:
                        self.op("+")
                    t = self.next()
                    if t.kind == "int" or t.kind == "real":
                        v = -t.value if neg else t.value
                        cd.default = Lit(t.kind, v, pos=a, end=t.end)
                    elif t.kind == "str" and not neg:
                        cd.default = Lit("text", t.value, pos=a, end=t.end)
                    elif t.kind == "blob" and not neg:
                        cd.default = Lit("blob", t.value, pos=a, end=t.end)
                    elif t.is_kw("null") and not neg:
                        cd.default = Lit("null", None, pos=a, end=t.end)
                    elif t.is_kw("true", "false") and not neg:
                        cd.default = Lit("int", 1 if t.value == "true" else 0, pos=a, end=t.end)
                    elif t.is_kw("current_time", "current_date", "current_timestamp"):
                        self.error("DEFAULT CURRENT_TIME/DATE/TIMESTAMP is not supported", t)
                    elif t.kind in ("id", "qid") and not neg:
                        cd.default = Lit("text", str(t.text if t.kind == "id" else t.value),
                                         pos=a, end=t.end)
                    else:
                        self.error("bad DEFAULT value", t)
            elif self.kw("collate"):
                cd.collate = self.name("collation")
            elif self.kw("references"):
                ref = self.name()
                cols = []
                if self.op("("):
                    cols = [self.name()]
                    while self.op(","):
                        cols.append(self.name())
                    self.expect_op(")")
                cd.fk = (ref, cols, self.fk_actions())
            elif self.kw("generated") or self.kw("as"):
                self.error("generated columns are not supported")
            else:
                return cd

    def on_conflict(self):
        if self.kw("on"):
            self.expect_kw("conflict")
            self.name()
            self.error("ON CONFLICT clauses in constraints are not supported in version 1")

    def fk_actions(self) -> dict:
        """ON DELETE/UPDATE actions and deferral of a foreign key clause."""
        out = {"delete": "no action", "update": "no action", "deferred": False}
        while True:
            if self.kw("on"):
                which = self.name()
                if which not in ("delete", "update"):
                    self.error(f"near '{which}': syntax error")
                if self.kw("set"):
                    act = "set " + self.name()
                    if act not in ("set null", "set default"):
                        self.error(f"near '{act}': syntax error")
                elif self.kw("no"):
                    self.name()
                    act = "no action"
                else:
                    act = self.name()
                    if act not in ("cascade", "restrict"):
                        self.error(f"near '{act}': syntax error")
                out[which] = act
            elif self.kw("match"):
                self.name()
            elif self.kw("not"):
                self.expect_kw("deferrable")
                if self.kw("initially"):
                    self.name()
                out["deferred"] = False
            elif self.kw("deferrable"):
                out["deferred"] = self.kw("initially") and self.name() == "deferred"
            else:
                return out

    def indexed_cols(self) -> list[tuple[str, str | None, bool]]:
        self.expect_op("(")
        cols = []
        while True:
            if self.t.kind not in ("id", "qid") or self.peek().kind == "op" and self.peek().value == "(":
                self.error("indexes on expressions are not supported in version 1")
            n = self.name("column name")
            coll = None
            if self.kw("collate"):
                coll = self.name("collation")
            desc = False
            if self.kw("desc"):
                desc = True
            else:
                self.kw("asc")
            cols.append((n, coll, desc))
            if not self.op(","):
                break
        self.expect_op(")")
        return cols

    def create(self):
        unique = self.kw("unique")
        if self.kw("temp", "temporary"):
            self.error("temporary tables are not supported")
        if self.kw("virtual"):
            self.error("virtual tables are not supported")
        if self.kw("view"):
            self.error("views are not supported in version 1")
        if self.kw("trigger"):
            self.error("triggers are not supported in version 1")
        if self.kw("index"):
            ine = False
            if self.kw("if"):
                self.expect_kw("not")
                self.expect_kw("exists")
                ine = True
            name = self.ident_text()
            self.expect_kw("on")
            table = self.name("table name")
            cols = self.indexed_cols()
            if self.kw("where"):
                self.error("partial indexes are not supported in version 1")
            return CreateIndex(name, unique, ine, table, cols)
        if unique:
            self.error("expected INDEX after UNIQUE")
        self.expect_kw("table")
        ine = False
        if self.kw("if"):
            self.expect_kw("not")
            self.expect_kw("exists")
            ine = True
        name = self.ident_text()
        if self.op("."):
            name = self.ident_text()
        if self.kw("as"):
            self.error("CREATE TABLE ... AS SELECT is not supported")
        self.expect_op("(")
        cols: list[ColDef] = []
        cons: list[TableConstraint] = []
        while True:
            if self.t.is_kw("constraint", "primary", "unique", "check", "foreign"):
                cons.append(self.table_constraint())
            else:
                if cons:
                    self.error("column definitions must precede table constraints")
                cols.append(self.column_def())
            if not self.op(","):
                break
        self.expect_op(")")
        without = False
        while self.t.kind == "id":
            if self.kw("without"):
                w = self.name()
                if w != "rowid":
                    self.error("expected ROWID")
                without = True
            elif self.kw("strict"):
                self.error("STRICT tables are not supported in version 1")
            else:
                break
            self.op(",")
        return CreateTable(name, ine, cols, cons, without)

    def table_constraint(self) -> TableConstraint:
        cname = None
        if self.kw("constraint"):
            cname = self.ident_text()
        if self.kw("primary"):
            self.expect_kw("key")
            cols = self.indexed_cols()
            autoinc = self.kw("autoincrement")
            self.on_conflict()
            return TableConstraint("pk", cname, cols, autoinc=autoinc)
        if self.kw("unique"):
            cols = self.indexed_cols()
            self.on_conflict()
            return TableConstraint("unique", cname, cols)
        if self.kw("check"):
            self.expect_op("(")
            a = self.t.pos
            e = self.expr()
            b = self.toks[self.i - 1].end
            self.expect_op(")")
            return TableConstraint("check", cname, [], e, self.text(a, b))
        self.expect_kw("foreign")
        self.expect_kw("key")
        self.expect_op("(")
        cols = [(self.name(), None, False)]
        while self.op(","):
            cols.append((self.name(), None, False))
        self.expect_op(")")
        self.expect_kw("references")
        ref = self.name()
        ref_cols = []
        if self.op("("):
            ref_cols.append(self.name())
            while self.op(","):
                ref_cols.append(self.name())
            self.expect_op(")")
        return TableConstraint("fk", cname, cols, ref=ref, ref_cols=ref_cols,
                               actions=self.fk_actions())

    def alter(self) -> AlterTable:
        self.expect_kw("table")
        table = self.name("table name")
        if self.kw("rename"):
            if self.kw("to"):
                return AlterTable(table, "rename_table", new=self.ident_text())
            self.kw("column")
            old = self.name("column name")
            self.expect_kw("to")
            return AlterTable(table, "rename_col", old=old, new=self.ident_text())
        if self.kw("add"):
            self.kw("column")
            return AlterTable(table, "add", coldef=self.column_def())
        if self.kw("drop"):
            self.kw("column")
            return AlterTable(table, "drop_col", old=self.name("column name"))
        self.error("unsupported ALTER TABLE")
        return AlterTable(table, "")

    def drop(self) -> Drop:
        if self.kw("table"):
            kind = "table"
        elif self.kw("index"):
            kind = "index"
        else:
            self.error("only DROP TABLE and DROP INDEX are supported")
            kind = ""
        ie = False
        if self.kw("if"):
            self.expect_kw("exists")
            ie = True
        return Drop(kind, self.name(), ie)

    # -- expressions --
    def expr_list(self) -> list[Expr]:
        es = [self.expr()]
        while self.op(","):
            es.append(self.expr())
        return es

    def expr(self) -> Expr:
        return self.or_expr()

    def _mk(self, node: Expr, start: int) -> Expr:
        node.pos = start
        node.end = self.toks[self.i - 1].end
        return node

    def or_expr(self) -> Expr:
        start = self.t.pos
        e = self.and_expr()
        while self.kw("or"):
            e = self._mk(Binary("or", e, self.and_expr()), start)
        return e

    def and_expr(self) -> Expr:
        start = self.t.pos
        e = self.not_expr()
        while self.kw("and"):
            e = self._mk(Binary("and", e, self.not_expr()), start)
        return e

    def not_expr(self) -> Expr:
        start = self.t.pos
        if self.t.is_kw("not") and not self.peek().is_kw("exists"):
            self.next()
            return self._mk(Unary("not", self.not_expr()), start)
        return self.eq_expr()

    def eq_expr(self) -> Expr:
        start = self.t.pos
        e = self.rel_expr()
        while True:
            t = self.t
            if t.kind == "op" and t.value in ("=", "==", "!=", "<>"):
                self.next()
                op = {"==": "=", "<>": "!="}.get(t.value, t.value)
                e = self._mk(Binary(op, e, self.rel_expr()), start)
                continue
            if t.is_kw("is"):
                self.next()
                neg = self.kw("not")
                if self.kw("distinct"):
                    self.expect_kw("from")
                    neg = not neg
                r = self.rel_expr()
                e = self._mk(Binary("isnot" if neg else "is", e, r), start)
                continue
            if t.is_kw("isnull") or t.is_kw("notnull"):
                self.next()
                e = self._mk(IsNull(e, t.value == "notnull"), start)
                continue
            neg = False
            if t.is_kw("not") and self.peek().is_kw("null"):
                self.next()
                self.next()
                e = self._mk(IsNull(e, True), start)
                continue
            if t.is_kw("not") and self.peek().is_kw("in", "like", "glob", "between", "match", "regexp"):
                self.next()
                neg = True
                t = self.t
            if t.is_kw("in"):
                self.next()
                self.expect_op("(")
                if self.t.is_kw("select", "with"):
                    e = self._mk(InSelect(e, self.subselect(), neg), start)
                    continue
                items = [] if self.t.kind == "op" and self.t.value == ")" else self.expr_list()
                self.expect_op(")")
                e = self._mk(InList(e, items, neg), start)
                continue
            if t.is_kw("like", "glob"):
                self.next()
                pat = self.rel_expr()
                esc = None
                if self.kw("escape"):
                    esc = self.rel_expr()
                    if t.value == "glob":
                        self.error("ESCAPE is not allowed with GLOB")
                e = self._mk(Like(t.value, e, pat, esc, neg), start)
                continue
            if t.is_kw("match", "regexp"):
                self.error(f"{t.value.upper()} is not supported")
            if t.is_kw("between"):
                self.next()
                lo = self.rel_expr()
                self.expect_kw("and")
                hi = self.rel_expr()
                e = self._mk(Between(e, lo, hi, neg), start)
                continue
            if neg:
                self.error("syntax error after NOT")
            return e

    def rel_expr(self) -> Expr:
        start = self.t.pos
        e = self.bit_expr()
        while self.t.kind == "op" and self.t.value in ("<", "<=", ">", ">="):
            op = self.next().value
            e = self._mk(Binary(op, e, self.bit_expr()), start)
        return e

    def bit_expr(self) -> Expr:
        start = self.t.pos
        e = self.add_expr()
        while self.t.kind == "op" and self.t.value in ("&", "|", "<<", ">>"):
            op = self.next().value
            e = self._mk(Binary(op, e, self.add_expr()), start)
        return e

    def add_expr(self) -> Expr:
        start = self.t.pos
        e = self.mul_expr()
        while self.t.kind == "op" and self.t.value in ("+", "-"):
            op = self.next().value
            e = self._mk(Binary(op, e, self.mul_expr()), start)
        return e

    def mul_expr(self) -> Expr:
        start = self.t.pos
        e = self.concat_expr()
        while self.t.kind == "op" and self.t.value in ("*", "/", "%"):
            op = self.next().value
            e = self._mk(Binary(op, e, self.concat_expr()), start)
        return e

    def concat_expr(self) -> Expr:
        start = self.t.pos
        e = self.collate_expr()
        while self.t.kind == "op" and self.t.value in ("||", "->", "->>"):
            op = self.next().value
            if op != "||":
                self.error("JSON operators are not supported")
            e = self._mk(Binary("||", e, self.collate_expr()), start)
        return e

    def collate_expr(self) -> Expr:
        start = self.t.pos
        e = self.unary_expr()
        while self.kw("collate"):
            e = self._mk(Collate(e, self.name("collation")), start)
        return e

    def unary_expr(self) -> Expr:
        start = self.t.pos
        if self.t.kind == "op" and self.t.value in ("-", "+", "~"):
            op = self.next().value
            inner = self.unary_expr()
            if op == "-" and isinstance(inner, Lit) and inner.kind in ("int", "real"):
                return self._mk(Lit(inner.kind, -inner.value), start)
            return self._mk(Unary(op, inner), start)
        return self.primary()

    def param(self, t: Tok) -> Param:
        name = t.value
        if name == "?":
            self.params.append(None)
            return Param("?", len(self.params) - 1)
        if name[0] == "?":
            n = int(name[1:])
            if n < 1 or n > 250:
                self.error("parameter index out of range", t)
            while len(self.params) < n:
                self.params.append(None)
            self.params[n - 1] = name
            return Param(name, n - 1)
        if name in self.params:
            return Param(name, self.params.index(name))
        self.params.append(name)
        return Param(name, len(self.params) - 1)

    def primary(self) -> Expr:
        start = self.t.pos
        t = self.next()
        if t.kind == "int":
            return self._mk(Lit("int", t.value), start)
        if t.kind == "real":
            return self._mk(Lit("real", t.value), start)
        if t.kind == "str":
            return self._mk(Lit("text", t.value), start)
        if t.kind == "blob":
            return self._mk(Lit("blob", t.value), start)
        if t.kind == "param":
            return self._mk(self.param(t), start)
        if t.kind == "op" and t.value == "(":
            if self.t.is_kw("select", "with"):
                return self._mk(Subquery(self.subselect()), start)
            e = self.expr()
            if self.op(","):
                self.error("row values are not supported")
            self.expect_op(")")
            return e
        if t.is_kw("null"):
            return self._mk(Lit("null", None), start)
        if t.is_kw("true", "false") and not (self.t.kind == "op" and self.t.value == "("):
            return self._mk(Lit("int", 1 if t.value == "true" else 0), start)
        if t.is_kw("current_time", "current_date", "current_timestamp"):
            self.error("date/time functions are not supported in version 1", t)
        if t.is_kw("exists") or (t.is_kw("not") and self.t.is_kw("exists")):
            neg = t.is_kw("not")
            if neg:
                self.next()
            self.expect_op("(")
            return self._mk(Exists(self.subselect(), neg), start)
        if t.is_kw("raise"):
            self.error("RAISE is only valid in triggers", t)
        if t.is_kw("case"):
            base = None
            if not self.t.is_kw("when"):
                base = self.expr()
            whens = []
            while self.kw("when"):
                c = self.expr()
                self.expect_kw("then")
                whens.append((c, self.expr()))
            if not whens:
                self.error("CASE without WHEN")
            other = self.expr() if self.kw("else") else None
            self.expect_kw("end")
            return self._mk(Case(base, whens, other), start)
        if t.is_kw("cast"):
            self.expect_op("(")
            e = self.expr()
            self.expect_kw("as")
            tn = self.typename()
            self.expect_op(")")
            return self._mk(Cast(e, tn), start)
        if t.kind in ("id", "qid"):
            name = str(t.value) if t.kind == "qid" else t.value
            if t.kind == "id" and t.value in _RESERVED and not (self.t.kind == "op" and self.t.value == "("):
                self.error(f"near '{t.text}': syntax error", t)
            if self.t.kind == "op" and self.t.value == "(" and t.kind == "id":
                self.next()
                distinct = self.kw("distinct")
                star = False
                args: list[Expr] = []
                if self.op("*"):
                    star = True
                elif not (self.t.kind == "op" and self.t.value == ")"):
                    args = self.expr_list()
                if self.t.is_kw("order"):
                    self.error("ORDER BY in aggregate functions is not supported")
                self.expect_op(")")
                if self.t.is_kw("filter", "over"):
                    self.error("FILTER/OVER clauses are not supported")
                return self._mk(Func(t.value, args, star, distinct), start)
            if self.op("."):
                col = self.name("column name")
                return self._mk(Col(name.lower(), col.lower()), start)
            return self._mk(Col(None, name.lower()), start)
        self.error(f"near '{t.text or 'end of input'}': syntax error", t)
        return Lit("null", None)


def parse(sql: str):
    """Parse one statement; returns (ast, parameter names)."""
    p = Parser(sql)
    st = p.statement()
    return st, p.params
