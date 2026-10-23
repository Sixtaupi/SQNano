# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""SQL lexer and the canonical normalization shared with the C runtime.

normalize() must stay byte-for-byte identical to sqnano_normalize() in
src/compat/normalize.c: the runtime finds a precompiled statement by the
hash of its normalized text.
"""

from __future__ import annotations

from dataclasses import dataclass


class SqlError(Exception):
    """A compile error, reported with the position in the SQL text."""

    def __init__(self, msg: str, pos: int | None = None):
        super().__init__(msg)
        self.msg = msg
        self.pos = pos


@dataclass
class Tok:
    kind: str  # id, qid, str, blob, int, real, param, op, eof
    text: str  # source text
    value: object  # decoded value (identifier name, literal value)
    pos: int
    end: int

    def is_kw(self, *words: str) -> bool:
        return self.kind == "id" and self.value in words

    def __repr__(self) -> str:
        return f"{self.kind}:{self.text}"


_OPS3 = ("->>",)
_OPS2 = ("||", "<=", ">=", "==", "!=", "<>", "<<", ">>", "->")


def _ident_start(c: str) -> bool:
    return c.isascii() and (c.isalpha() or c == "_") or ord(c) >= 0x80


def _ident_char(c: str) -> bool:
    return _ident_start(c) or (c.isascii() and c.isdigit()) or c == "$"


def tokenize(sql: str) -> list[Tok]:
    toks: list[Tok] = []
    i, n = 0, len(sql)
    while i < n:
        c = sql[i]
        if c in " \t\n\r\f\v":
            i += 1
            continue
        if sql.startswith("--", i):
            j = sql.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        if sql.startswith("/*", i):
            j = sql.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        start = i
        if c == "'":
            j = i + 1
            buf = []
            while True:
                if j >= n:
                    raise SqlError("unterminated string literal", start)
                if sql[j] == "'":
                    if j + 1 < n and sql[j + 1] == "'":
                        buf.append("'")
                        j += 2
                        continue
                    break
                buf.append(sql[j])
                j += 1
            i = j + 1
            toks.append(Tok("str", sql[start:i], "".join(buf), start, i))
            continue
        if c in "xX" and i + 1 < n and sql[i + 1] == "'":
            j = sql.find("'", i + 2)
            if j < 0:
                raise SqlError("unterminated blob literal", start)
            hexs = sql[i + 2 : j]
            if len(hexs) % 2 or any(ch not in "0123456789abcdefABCDEF" for ch in hexs):
                raise SqlError("malformed blob literal", start)
            i = j + 1
            toks.append(Tok("blob", sql[start:i], bytes.fromhex(hexs), start, i))
            continue
        if c in "\"`[":
            close = "]" if c == "[" else c
            j = i + 1
            buf = []
            while True:
                if j >= n:
                    raise SqlError("unterminated identifier", start)
                if sql[j] == close:
                    if close != "]" and j + 1 < n and sql[j + 1] == close:
                        buf.append(close)
                        j += 2
                        continue
                    break
                buf.append(sql[j])
                j += 1
            i = j + 1
            toks.append(Tok("qid", sql[start:i], "".join(buf), start, i))
            continue
        if c.isdigit() or (c == "." and i + 1 < n and sql[i + 1].isdigit()):
            j = i
            if c == "0" and i + 2 < n and sql[i + 1] in "xX" and sql[i + 2] in "0123456789abcdefABCDEF":
                j = i + 2
                while j < n and sql[j] in "0123456789abcdefABCDEF":
                    j += 1
                v = int(sql[i + 2 : j], 16)
                if v >= 1 << 63:
                    v -= 1 << 64
                if v >= 1 << 63 or v < -(1 << 63):
                    raise SqlError("hex literal too big", start)
                toks.append(Tok("int", sql[i:j], v, start, j))
                i = j
                continue
            real = False
            while j < n and sql[j].isdigit():
                j += 1
            if j < n and sql[j] == ".":
                real = True
                j += 1
                while j < n and sql[j].isdigit():
                    j += 1
            if j < n and sql[j] in "eE":
                k = j + 1
                if k < n and sql[k] in "+-":
                    k += 1
                if k < n and sql[k].isdigit():
                    real = True
                    j = k
                    while j < n and sql[j].isdigit():
                        j += 1
            text = sql[i:j]
            if not real:
                v = int(text)
                if v >= 1 << 63:
                    toks.append(Tok("real", text, float(text), start, j))
                else:
                    toks.append(Tok("int", text, v, start, j))
            else:
                toks.append(Tok("real", text, float(text), start, j))
            i = j
            continue
        if c == "?":
            j = i + 1
            while j < n and sql[j].isdigit():
                j += 1
            toks.append(Tok("param", sql[i:j], sql[i:j], start, j))
            i = j
            continue
        if c in ":@$" and i + 1 < n and _ident_char(sql[i + 1]):
            j = i + 1
            while j < n and _ident_char(sql[j]):
                j += 1
            toks.append(Tok("param", sql[i:j], sql[i:j], start, j))
            i = j
            continue
        if _ident_start(c):
            j = i + 1
            while j < n and _ident_char(sql[j]):
                j += 1
            word = sql[i:j]
            toks.append(Tok("id", word, word.lower(), start, j))
            i = j
            continue
        op = None
        for cand in _OPS3 + _OPS2:
            if sql.startswith(cand, i):
                op = cand
                break
        if op is None:
            op = c
        i += len(op)
        toks.append(Tok("op", op, op, start, i))
    toks.append(Tok("eof", "", None, n, n))
    return toks


def _lower_ascii(b: bytes) -> bytes:
    return bytes(ch + 32 if 65 <= ch <= 90 else ch for ch in b)


def _is_ident_start(b: int) -> bool:
    return 65 <= b <= 90 or 97 <= b <= 122 or b == 95 or b >= 0x80


def _is_ident_char(b: int) -> bool:
    return _is_ident_start(b) or 48 <= b <= 57 or b == 36


def _is_digit(b: int) -> bool:
    return 48 <= b <= 57


def _is_hex(b: int) -> bool:
    return _is_digit(b) or 65 <= b <= 70 or 97 <= b <= 102


def normalize(sql: str | bytes) -> tuple[str, int]:
    """Return the normalized first statement of @p sql and the offset after it.

    Tokens are separated by one space. Everything is lowercased (ASCII)
    except string literals and parameter names. Comments and the trailing
    ';' are dropped.
    """
    s = sql.encode() if isinstance(sql, str) else sql
    n = len(s)
    i = 0
    out: list[bytes] = []
    while i < n:
        c = s[i]
        if c in b" \t\n\r\f\v":
            i += 1
            continue
        if s.startswith(b"--", i):
            while i < n and s[i] != 10:
                i += 1
            continue
        if s.startswith(b"/*", i):
            j = s.find(b"*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == 59:  # ';'
            i += 1
            break
        start = i
        if c == 39:  # string literal
            i += 1
            while i < n:
                if s[i] == 39:
                    if i + 1 < n and s[i + 1] == 39:
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
            out.append(s[start:i])
            continue
        if c in b"xX" and i + 1 < n and s[i + 1] == 39:
            i += 2
            while i < n and s[i] != 39:
                i += 1
            i = min(i + 1, n)
            out.append(_lower_ascii(s[start:i]))
            continue
        if c in b'"`':
            i += 1
            while i < n:
                if s[i] == c:
                    if i + 1 < n and s[i + 1] == c:
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
            out.append(_lower_ascii(s[start:i]))
            continue
        if c == 91:  # [
            while i < n and s[i] != 93:
                i += 1
            i = min(i + 1, n)
            out.append(_lower_ascii(s[start:i]))
            continue
        if _is_digit(c) or (c == 46 and i + 1 < n and _is_digit(s[i + 1])):
            if c == 48 and i + 2 < n and s[i + 1] in b"xX" and _is_hex(s[i + 2]):
                i += 2
                while i < n and _is_hex(s[i]):
                    i += 1
            else:
                while i < n and _is_digit(s[i]):
                    i += 1
                if i < n and s[i] == 46:
                    i += 1
                    while i < n and _is_digit(s[i]):
                        i += 1
                if i < n and s[i] in b"eE":
                    k = i + 1
                    if k < n and s[k] in b"+-":
                        k += 1
                    if k < n and _is_digit(s[k]):
                        i = k
                        while i < n and _is_digit(s[i]):
                            i += 1
            out.append(_lower_ascii(s[start:i]))
            continue
        if c == 63:  # ?
            i += 1
            while i < n and _is_digit(s[i]):
                i += 1
            out.append(s[start:i])
            continue
        if c in b":@$" and i + 1 < n and _is_ident_char(s[i + 1]):
            i += 1
            while i < n and _is_ident_char(s[i]):
                i += 1
            out.append(s[start:i])
            continue
        if _is_ident_start(c):
            while i < n and _is_ident_char(s[i]):
                i += 1
            out.append(_lower_ascii(s[start:i]))
            continue
        for op in (b"->>", b"||", b"<=", b">=", b"==", b"!=", b"<>", b"<<", b">>", b"->"):
            if s.startswith(op, i):
                i += len(op)
                break
        else:
            i += 1
        out.append(s[start:i])
    return b" ".join(out).decode("utf-8", "replace"), i


def fnv1a(text: str) -> int:
    h = 0x811C9DC5
    for b in text.encode():
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def split_statements(sql: str) -> list[tuple[str, int]]:
    """Split a script into statements, keeping each one's source offset."""
    res = []
    pos = 0
    data = sql.encode()
    while pos < len(data):
        norm, used = normalize(data[pos:])
        if norm:
            text = data[pos : pos + used].decode()
            res.append((text.rstrip().rstrip(";"), pos))
        pos += used
        if used == 0:
            break
    return res
