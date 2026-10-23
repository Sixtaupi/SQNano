#!/usr/bin/env python3
# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""Write the seed inputs of the fuzz targets into tests/fuzz/corpus/."""

import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "sqnanoc"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "sqnano-cli"))

from lex import split_statements  # noqa: E402
from sqnano_cli import cbor_encode  # noqa: E402


def write(target, name, data: bytes):
    d = os.path.join(HERE, "corpus", target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as fh:
        fh.write(data)


def main():
    rng = random.Random(1)
    n = 0
    for path in ("host/app/queries.sql", "host/diff/queries.sql"):
        with open(os.path.join(HERE, "..", path), encoding="utf-8") as fh:
            text = fh.read()
        for sql, _ in split_statements(text):
            params = bytes(rng.choice([0, 1, 2, 3 | 5 << 3, 4 | 3 << 3]) for _ in range(6))
            write("fuzz_sql", f"q{n:03d}", sql.strip().encode() + b"\0" + params + b"abcdefgh")
            n += 1
    requests = [
        {"op": "info"},
        {"op": "tables", "db": 0},
        {"op": "tables", "db": 0, "from": 1, "off": 10},
        {"op": "scan", "db": 0, "t": "sensor"},
        {"op": "scan", "db": 0, "t": "sensor", "after": 40, "hi": 90, "n": 5},
        {"op": "scan", "db": 0, "t": "event"},
        {"op": "scan", "db": 0, "t": "event", "after": b"\x30pump\x00\x00\x6a"},
        {"op": "run", "db": 0, "q": "sensor_last", "p": [3, 4]},
        {"op": "run", "db": 0, "q": "sensor_stats", "n": 2},
        {"op": "run", "db": 0, "q": "sensor_add", "p": [9, 42, 1.25]},
        {"op": "run", "db": 0, "q": "config_set", "p": ["k", b"\x00\xff"]},
    ]
    for i, rq in enumerate(requests):
        write("fuzz_remote", f"r{i:02d}", bytes([0x7f | (0x80 if rq.get("q") else 0)]) + cbor_encode(rq))
        write("fuzz_remote", f"s{i:02d}", bytes([3]) + cbor_encode(rq))
    write("fuzz_mount", "none", b"")
    for i in range(8):
        write("fuzz_mount", f"e{i}", bytes(rng.randrange(256) for _ in range(4 * (i + 1))))
    for i in range(8):
        write("fuzz_keys", f"k{i}", bytes(rng.randrange(256) for _ in range(64)))


if __name__ == "__main__":
    main()
