#!/usr/bin/env python3
# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""sqnano-cli against the host remote server (SMP packets on a pipe).

The server fills the test application database; the same rows go into a
reference SQLite database, and every statement must give the same result
through the device mirror.
"""

import io
import os
import sqlite3
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "sqnano-cli"))

import sqnano_cli as cli  # noqa: E402

SERVER = None


def reference():
    db = sqlite3.connect(":memory:")
    with open(os.path.join(HERE, "app", "schema", "v1.sql"), encoding="utf-8") as fh:
        db.executescript(fh.read())
    for i in range(300):
        db.execute("INSERT INTO sensor (id, ts, value) VALUES (?, ?, ?)",
                   (i % 5, 1760780000 + i, None if i % 17 == 0 else 20.0 + (i % 13) / 4.0))
    up = "INSERT INTO config (key, val) VALUES (?, ?) ON CONFLICT (key) DO UPDATE SET val = excluded.val"
    for k, v in (("name", 'probe "A" été'), ("raw", b"\x00\x01\x02\xff"), ("gain", -1.5e-3),
                 ("big", -(2 ** 63))):
        db.execute(up, (k, v))
    for i in range(40):
        db.execute("INSERT INTO event (dev, seq, kind) VALUES (?, ?, ?)",
                   (("pump", "valve", "fan")[i % 3], i, "on" if i % 2 else "off"))
    return db


QUERIES = [
    "SELECT * FROM sensor ORDER BY rid",
    "SELECT rowid, * FROM config ORDER BY key",
    "SELECT key, typeof(val), length(val) FROM config ORDER BY 1",
    "SELECT id, count(*), sum(value IS NULL), round(avg(value), 6) FROM sensor GROUP BY id",
    "SELECT s.id, c.key FROM sensor s JOIN config c ON length(c.key) = s.id + 1 WHERE s.ts % 50 = 0 "
    "ORDER BY 1, 2",
    "WITH w AS (SELECT id, max(ts) AS m FROM sensor GROUP BY id) SELECT * FROM w ORDER BY m DESC",
    "SELECT ts, value, rank() OVER (PARTITION BY id ORDER BY value DESC) FROM sensor "
    "WHERE id = 2 ORDER BY ts LIMIT 12",
    # WITHOUT ROWID: paged by key.
    "SELECT * FROM event ORDER BY dev, seq DESC",
    "SELECT dev, count(*), max(seq) FROM event WHERE kind = 'on' GROUP BY dev ORDER BY 1",
]


class RemoteCli(unittest.TestCase):
    def shell(self, cap=2048):
        t = cli.ExecTransport(f"{SERVER} {cap}")
        self.addCleanup(t.close)
        out = io.StringIO()
        return cli.Shell(cli.Device(t), out), out

    def test_cbor_roundtrip(self):
        for v in (0, 23, 24, 255, 256, 65535, 65536, 2 ** 32, 2 ** 63 - 1, -1, -24, -25, -(2 ** 63),
                  1.5, -0.0, "", "été", b"", b"\x00\xff", None, True, False, [1, [2, "x"]],
                  {"a": 1, "b": [None, 2.5]}):
            self.assertEqual(cli.cbor_decode(cli.cbor_encode(v)), v)

    def test_queries_match_sqlite(self):
        ref = reference()
        for cap in (2048, 200, 90):
            sh, _ = self.shell(cap)
            for q in QUERIES:
                names, rows = sh.mirror.query(q)
                want = ref.execute(q)
                self.assertEqual(names, [d[0] for d in want.description], q)
                self.assertEqual(rows, want.fetchall(), f"{q} (cap {cap})")

    def test_tables_and_schema(self):
        sh, out = self.shell()
        sh.execute(".tables")
        sh.execute(".schema sensor")
        text = out.getvalue()
        self.assertIn("config  event  sensor", text)
        self.assertIn("CREATE TABLE sensor", text)
        self.assertIn("CREATE INDEX sensor_pk", text)
        self.assertNotIn("CREATE TABLE config", text)

    def test_info(self):
        sh, _ = self.shell()
        dbs = sh.dev.request(op="info")["dbs"]
        self.assertEqual(dbs[0]["path"], "app.db")
        self.assertEqual(dbs[0]["ps"], 512)
        self.assertEqual(dbs[0]["mode"], "shadow")
        self.assertGreater(dbs[0]["used"], 0)

    def test_run_precompiled(self):
        ref = reference()
        sh, out = self.shell()
        r = sh.dev.request(op="run", db=0, q="sensor_last", p=[3, 4])
        self.assertEqual(r["c"], ["ts", "value"])
        self.assertEqual(r["rows"], [list(x) for x in ref.execute(
            "SELECT ts, value FROM sensor WHERE id = 3 ORDER BY ts DESC LIMIT 4")])
        # A write, then a statement sees it: tables are read again.
        sh.execute(".run sensor_add 9 42 1.25")
        self.assertIn("(1 changes)", out.getvalue())
        _, rows = sh.mirror.query("SELECT id, ts, value FROM sensor WHERE id = 9")
        self.assertEqual(rows, [(9, 42, 1.25)])
        # A truncated result says so.
        r = sh.dev.request(op="run", db=0, q="sensor_stats", n=2)
        self.assertEqual(len(r["rows"]), 2)
        self.assertTrue(r["more"])

    def test_cache(self):
        sh, _ = self.shell()
        sh.execute(".cache on")
        sh.mirror.query("SELECT count(*) FROM sensor")
        sh.dev.request(op="run", db=0, q="sensor_add", p=[1, 2, 3.0])
        self.assertEqual(sh.mirror.query("SELECT count(*) FROM sensor")[1], [(300,)])
        sh.execute(".refresh")
        self.assertEqual(sh.mirror.query("SELECT count(*) FROM sensor")[1], [(301,)])

    def test_export(self):
        ref = reference()
        sh, _ = self.shell()
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "copy.db")
            sh.execute(f".export {path}")
            copy = sqlite3.connect(path)
            for q in QUERIES[:3]:
                self.assertEqual(copy.execute(q).fetchall(), ref.execute(q).fetchall())
            self.assertEqual(copy.execute("PRAGMA integrity_check").fetchall(), [("ok",)])
            copy.close()

    def test_errors(self):
        sh, out = self.shell()
        sh.execute(".run no_such_query")
        sh.execute("SELECT * FROM missing;")
        text = out.getvalue()
        self.assertIn("no precompiled query", text)
        self.assertIn("no such table", text)
        with self.assertRaises(cli.RemoteError):
            sh.dev.request(op="scan", db=0, t="nope")
        with self.assertRaises(cli.RemoteError):
            sh.dev.request(op="scan", db=5, t="sensor")
        with self.assertRaises(cli.RemoteError):
            sh.dev.request(op="bogus", db=0)
        # A response buffer too small for one row is an error, not a truncation.
        devs = {}
        for cap in (34, 40):
            t = cli.ExecTransport(f"{SERVER} {cap}")
            self.addCleanup(t.close)
            devs[cap] = cli.Device(t)
        with self.assertRaises(cli.RemoteError) as e:
            devs[34].request(op="scan", db=0, t="sensor", after=1)  # a row with a REAL
        self.assertIn("row larger", str(e.exception))
        # One row per response still reads the whole table.
        self.assertEqual(len(list(devs[40].scan(0, "sensor"))), 300)

    def test_serial_transport(self):
        """SMP over a console: frames decoded and answered by a fake device on a pty."""
        import base64
        import struct
        import threading

        master, slave = os.openpty()
        backend = cli.ExecTransport(f"{SERVER} 1500")
        self.addCleanup(backend.close)
        stop = threading.Event()

        def device():
            buf, data = b"", b""
            while not stop.is_set():
                try:
                    chunk = os.read(master, 4096)
                except OSError:
                    return
                buf += chunk
                while b"\n" in buf:
                    line, _, buf = buf.partition(b"\n")
                    if line[:2] == b"\x06\x09":
                        data = b""
                    elif line[:2] != b"\x04\x14":
                        continue
                    data += base64.b64decode(line[2:])
                    n = struct.unpack(">H", data[:2])[0]
                    if len(data) - 2 < n:
                        continue
                    pkt = data[2:n]
                    assert struct.pack(">H", cli.crc16_ccitt(pkt)) == data[n:n + 2]
                    rsp = backend.exchange(pkt)
                    body = struct.pack(">H", len(rsp) + 2) + rsp + struct.pack(">H", cli.crc16_ccitt(rsp))
                    out = b"uart:~$ some console output\r\n"
                    for i in range(0, len(body), 90):
                        out += (b"\x06\x09" if i == 0 else b"\x04\x14") + \
                            base64.b64encode(body[i:i + 90]) + b"\n"
                    os.write(master, out)

        th = threading.Thread(target=device, daemon=True)
        th.start()
        t = cli.SerialTransport(os.ttyname(slave), 115200, 5.0)
        try:
            dev = cli.Device(t)
            self.assertEqual(dev.request(op="info")["dbs"][0]["path"], "app.db")
            self.assertEqual(len(list(dev.scan(0, "sensor"))), 300)
        finally:
            stop.set()
            t.close()
            os.close(slave)
            os.close(master)

    def test_serial_framing(self):
        pkt = cli.smp_packet(7, cli.cbor_encode({"op": "info"}))
        self.assertEqual(cli.crc16_ccitt(b"123456789"), 0x31C3)
        self.assertEqual(pkt[:8], bytes([0, 0, 0, len(pkt) - 8, 0, 64, 7, 0]))


if __name__ == "__main__":
    SERVER = sys.argv.pop(1)
    unittest.main(verbosity=1)
