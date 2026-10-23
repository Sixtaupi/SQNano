# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""Static extraction of the SQL passed to sqlite3_prepare*() and sqlite3_exec().

Only string literals (possibly split into adjacent literals) can be
precompiled. Any other argument is reported as dynamic SQL with its file and
line, so that it can be turned into a parameterized query.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

_CALL = re.compile(r"\bsqlite3_(prepare(?:_v2|_v3)?|exec)\s*\(")
_ESC = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", '"': '"', "'": "'", "a": "\a",
        "b": "\b", "f": "\f", "v": "\v", "?": "?"}


@dataclass
class Found:
    file: str
    line: int
    func: str
    sql: str | None  # None: dynamic SQL
    arg: str = ""


def _skip_ws(s: str, i: int) -> int:
    n = len(s)
    while i < n:
        if s[i].isspace():
            i += 1
        elif s.startswith("//", i):
            j = s.find("\n", i)
            i = n if j < 0 else j + 1
        elif s.startswith("/*", i):
            j = s.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            break
    return i


def _c_string(s: str, i: int) -> tuple[str, int]:
    """Decode the C string literal starting at s[i] == '"'."""
    out = []
    i += 1
    while i < len(s) and s[i] != '"':
        c = s[i]
        if c == "\\":
            i += 1
            e = s[i]
            if e == "x":
                j = i + 1
                while j < len(s) and s[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(chr(int(s[i + 1 : j], 16)))
                i = j
                continue
            if e in "01234567":
                j = i
                while j < len(s) and j < i + 3 and s[j] in "01234567":
                    j += 1
                out.append(chr(int(s[i:j], 8)))
                i = j
                continue
            if e == "\n":
                i += 1
                continue
            out.append(_ESC.get(e, e))
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out), i + 1


def _skip_arg(s: str, i: int) -> int:
    """Skip one call argument; returns the index of the following ',' or ')'."""
    depth = 0
    while i < len(s):
        c = s[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return i
            depth -= 1
        elif c == "," and depth == 0:
            return i
        elif c == '"':
            _, i = _c_string(s, i)
            continue
        elif c == "'":
            j = i + 1
            while j < len(s) and s[j] != "'":
                j += 2 if s[j] == "\\" else 1
            i = j + 1
            continue
        i += 1
    return i


def extract_file(path: str, wrappers: dict[str, int] | None = None) -> list[Found]:
    """Find SQL arguments; @p wrappers maps extra function names to the 0-based
    position of their SQL argument (application helpers around sqlite3_exec)."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    res = []
    pattern = _CALL
    if wrappers:
        names = "|".join(re.escape(w) for w in wrappers)
        pattern = re.compile(r"\b(?:sqlite3_(prepare(?:_v2|_v3)?|exec)|(" + names + r"))\s*\(")
    for m in pattern.finditer(src):
        func = "sqlite3_" + m.group(1) if m.group(1) else m.group(2)
        argpos = wrappers.get(func, 1) if wrappers and m.group(2) else 1
        line = src.count("\n", 0, m.start()) + 1
        # Skip occurrences inside comments or string literals (rough check).
        start_of_line = src.rfind("\n", 0, m.start()) + 1
        prefix = src[start_of_line : m.start()]
        if "//" in prefix or prefix.lstrip().startswith("*") or prefix.count('"') % 2:
            continue
        i = m.end()
        ok = True
        for _ in range(argpos):
            i = _skip_arg(src, i)
            if i >= len(src) or src[i] != ",":
                ok = False
                break
            i += 1
        if not ok:
            continue
        i = _skip_ws(src, i)
        if i < len(src) and src[i] == '"' or src.startswith('u8"', i):
            if src[i] == "u":
                i += 2
            parts = []
            while i < len(src) and src[i] == '"':
                txt, i = _c_string(src, i)
                parts.append(txt)
                i = _skip_ws(src, i)
                if src.startswith('u8"', i):
                    i += 2
            if i < len(src) and src[i] in ",)":
                res.append(Found(path, line, func, "".join(parts)))
                continue
        end = _skip_arg(src, i)
        arg = src[i:end].strip()
        if re.search(r"\b(const|char|struct|unsigned)\b", arg):
            continue  # a declaration of the wrapper, not a call
        res.append(Found(path, line, func, None, arg))
    return res
