#!/usr/bin/env python3
# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""sqnano-cli: query a SQNano database on a device with the full SQL of SQLite.

The device runs a small cursor server (CONFIG_SQNANO_REMOTE, an SMP/mcumgr
group). The tables a statement reads are copied from the device into an
in-memory SQLite database, then SQLite runs the statement. Every statement
reads the device again; ".cache on" keeps the copies until ".refresh".

    sqnano-cli --port /dev/ttyACM0            # SMP over the serial console
    sqnano-cli --udp 192.0.2.1                # SMP over UDP (port 1337)
    sqnano-cli --exec ./remote_server         # a program speaking SMP on stdio

Only the Python standard library is needed.
"""

import argparse
import base64
import csv
import io
import json
import os
import re
import select
import socket
import sqlite3
import struct
import subprocess
import sys
import time

GROUP = 64          # MGMT_GROUP_ID_PERUSER, CONFIG_SQNANO_REMOTE_GROUP
CMD_REQUEST = 0


class RemoteError(Exception):
    def __init__(self, code, msg):
        super().__init__(f"{msg} (error {code})")
        self.code = code


# ---- CBOR (the subset SMP uses) ----

def cbor_encode(v) -> bytes:
    def head(major, n):
        if n < 24:
            return bytes([major << 5 | n])
        for ai, size in ((24, 1), (25, 2), (26, 4), (27, 8)):
            if n < 1 << (8 * size):
                return bytes([major << 5 | ai]) + n.to_bytes(size, "big")
        raise ValueError("integer too large")

    if v is None:
        return b"\xf6"
    if v is True:
        return b"\xf5"
    if v is False:
        return b"\xf4"
    if isinstance(v, int):
        return head(0, v) if v >= 0 else head(1, -1 - v)
    if isinstance(v, float):
        return b"\xfb" + struct.pack(">d", v)
    if isinstance(v, str):
        b = v.encode("utf-8")
        return head(3, len(b)) + b
    if isinstance(v, (bytes, bytearray, memoryview)):
        return head(2, len(v)) + bytes(v)
    if isinstance(v, (list, tuple)):
        return head(4, len(v)) + b"".join(cbor_encode(x) for x in v)
    if isinstance(v, dict):
        return head(5, len(v)) + b"".join(cbor_encode(k) + cbor_encode(x) for k, x in v.items())
    raise TypeError(f"cannot encode {type(v).__name__}")


def cbor_decode(b: bytes):
    pos = 0

    def item():
        nonlocal pos
        ib = b[pos]
        pos += 1
        major, ai = ib >> 5, ib & 31
        if major == 7:
            if ai == 20:
                return False
            if ai == 21:
                return True
            if ai in (22, 23):
                return None
            if ai == 25:
                pos += 2
                return struct.unpack(">e", b[pos - 2:pos])[0]
            if ai == 26:
                pos += 4
                return struct.unpack(">f", b[pos - 4:pos])[0]
            if ai == 27:
                pos += 8
                return struct.unpack(">d", b[pos - 8:pos])[0]
            raise ValueError("unsupported CBOR simple value")
        if ai < 24:
            n = ai
        elif ai <= 27:
            size = 1 << (ai - 24)
            n = int.from_bytes(b[pos:pos + size], "big")
            pos += size
        elif ai == 31 and major in (4, 5):
            n = None
        else:
            raise ValueError("unsupported CBOR length")
        if major == 0:
            return n
        if major == 1:
            return -1 - n
        if major in (2, 3):
            s = b[pos:pos + n]
            pos += n
            return bytes(s) if major == 2 else s.decode("utf-8", "surrogateescape")
        if major == 4:
            out = []
            while (n is None and b[pos] != 0xff) or (n is not None and len(out) < n):
                out.append(item())
            if n is None:
                pos += 1
            return out
        if major == 5:
            out = {}
            while (n is None and b[pos] != 0xff) or (n is not None and len(out) < n):
                k = item()
                out[k] = item()
            if n is None:
                pos += 1
            return out
        if major == 6:
            return item()
        raise ValueError("bad CBOR")

    v = item()
    return v


# ---- SMP transports ----

def smp_packet(seq: int, payload: bytes, op: int = 0) -> bytes:
    return struct.pack(">BBHHBB", op, 0, len(payload), GROUP, seq, CMD_REQUEST) + payload


def crc16_ccitt(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = (crc << 1 ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xffff
    return crc


class ExecTransport:
    """A program reading SMP packets on stdin and answering on stdout."""

    def __init__(self, cmd: str):
        self.p = subprocess.Popen(cmd, shell=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def exchange(self, pkt: bytes) -> bytes:
        self.p.stdin.write(pkt)
        self.p.stdin.flush()
        hdr = self.p.stdout.read(8)
        if len(hdr) < 8:
            raise RemoteError(-1, "the server exited")
        n = struct.unpack(">H", hdr[2:4])[0]
        return hdr + self.p.stdout.read(n)

    def close(self):
        self.p.stdin.close()
        self.p.wait()
        self.p.stdout.close()


class UdpTransport:
    def __init__(self, addr: str, timeout: float):
        host, _, port = addr.partition(":")
        self.dest = (host, int(port or 1337))
        self.s = socket.socket(socket.AF_INET6 if ":" in host and port == "" else socket.AF_INET,
                               socket.SOCK_DGRAM)
        self.s.settimeout(timeout)

    def exchange(self, pkt: bytes) -> bytes:
        self.s.sendto(pkt, self.dest)
        return self.s.recv(65536)

    def close(self):
        self.s.close()


class SerialTransport:
    """SMP over the serial console: base64 frames, CRC16, 0x06 0x09 start."""

    def __init__(self, port: str, baud: int, timeout: float):
        import termios
        import tty

        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        speed = getattr(termios, f"B{baud}")
        attrs[4] = attrs[5] = speed
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.timeout = timeout
        self.buf = b""

    def _line(self, deadline: float) -> bytes:
        while b"\n" not in self.buf:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                raise RemoteError(-1, "no answer from the device")
            self.buf += os.read(self.fd, 4096)
        line, _, self.buf = self.buf.partition(b"\n")
        return line

    def exchange(self, pkt: bytes) -> bytes:
        body = struct.pack(">H", len(pkt) + 2) + pkt + struct.pack(">H", crc16_ccitt(pkt))
        out = b""
        for i in range(0, len(body), 93):
            out += (b"\x06\x09" if i == 0 else b"\x04\x14") + base64.b64encode(body[i:i + 93]) + b"\n"
        os.write(self.fd, out)
        deadline = time.monotonic() + self.timeout
        data = b""
        while True:
            line = self._line(deadline).rstrip(b"\r")
            k = max(line.rfind(b"\x06\x09"), line.rfind(b"\x04\x14"))
            if k < 0:
                continue  # console output
            if line[k:k + 2] == b"\x06\x09":
                data = b""
            data += base64.b64decode(line[k + 2:])
            if len(data) >= 2 and len(data) - 2 >= struct.unpack(">H", data[:2])[0]:
                n = struct.unpack(">H", data[:2])[0]
                pkt_in, crc = data[2:n], data[n:n + 2]
                if struct.pack(">H", crc16_ccitt(pkt_in)) != crc:
                    raise RemoteError(-1, "bad CRC from the device")
                return pkt_in

    def close(self):
        os.close(self.fd)


class Device:
    def __init__(self, transport):
        self.t = transport
        self.seq = 0

    def request(self, **req):
        self.seq = (self.seq + 1) & 0xff
        pkt = self.t.exchange(smp_packet(self.seq, cbor_encode(req)))
        if len(pkt) < 8 or pkt[6] != self.seq or pkt[0] != 1:
            raise RemoteError(-1, "unexpected SMP answer")
        payload = cbor_decode(pkt[8:])
        if "d" not in payload:
            raise RemoteError(payload.get("rc", -1), "the device has no SQNano remote group")
        resp = cbor_decode(payload["d"])
        if "err" in resp:
            raise RemoteError(resp["err"], resp.get("msg", "error"))
        return resp

    def schema(self, db: int):
        """[kind (0 table, 1 index), name, sql]; long SQL texts come in pieces."""
        out, start, off, part = [], 0, 0, b""
        while True:
            r = self.request(op="tables", db=db, **{"from": start, "off": off})
            for kind, name, chunk, done in r["s"]:
                part += chunk
                if done:
                    out.append([kind, name, part.decode("utf-8")])
                    part = b""
            if not r["more"]:
                return r["sv"], out
            start, off = r["from"], r["off"]

    def scan(self, db: int, table: str):
        after = None
        while True:
            req = {"op": "scan", "db": db, "t": table}
            if after is not None:
                req["after"] = after
            r = self.request(**req)
            yield from r["rows"]
            if not r["more"] or not r["rows"]:
                return
            after = r["rows"][-1][0]


class Mirror:
    """The device tables, copied on demand into an in-memory SQLite database."""

    def __init__(self, dev: Device, db: int = 0):
        self.dev = dev
        self.db = db
        self.sql = sqlite3.connect(":memory:")
        self.cache = False
        self.loaded = set()
        self.version, entries = dev.schema(db)
        # Indexes made for PRIMARY KEY and UNIQUE constraints have no SQL.
        entries = [e for e in entries if e[2]]
        self.tables = {}
        self.entries = entries
        for kind, name, sql in entries:
            self.sql.execute(sql)
            if kind == 0:
                self.tables[name.lower()] = name

    def columns(self, table):
        """Column names, and the position of the INTEGER PRIMARY KEY (rowid alias) or None."""
        info = self.sql.execute(f'PRAGMA table_info("{table}")').fetchall()
        names = [r[1] for r in info]
        pks = [i for i, r in enumerate(info) if r[5]]
        ipk = pks[0] if len(pks) == 1 and info[pks[0]][2].upper() == "INTEGER" else None
        return names, ipk

    def without_rowid(self, table):
        sql = next((e[2] for e in self.entries if e[0] == 0 and e[1].lower() == table.lower()), "")
        return re.search(r"\bwithout\s+rowid\s*;?\s*$", sql, re.I) is not None

    def fetch(self, table):
        names, ipk = self.columns(table)
        cols = ", ".join(f'"{c}"' for c in names)
        self.sql.execute(f'DELETE FROM "{table}"')
        if self.without_rowid(table):
            # Rows come with their encoded key first.
            q = f'INSERT INTO "{table}" ({cols}) VALUES ({", ".join("?" * len(names))})'
            rows = (r[1:] for r in self.dev.scan(self.db, table))
        elif ipk is not None:
            q = f'INSERT INTO "{table}" ({cols}) VALUES ({", ".join("?" * len(names))})'

            def rows_ipk():
                for r in self.dev.scan(self.db, table):
                    vals = list(r[1:])
                    vals[ipk] = r[0]  # the rowid is the column
                    yield vals
            rows = rows_ipk()
        else:
            q = f'INSERT INTO "{table}" (rowid, {cols}) VALUES ({", ".join("?" * (len(names) + 1))})'
            rows = self.dev.scan(self.db, table)
        self.sql.executemany(q, rows)
        self.sql.commit()
        self.loaded.add(table)

    def tables_read(self, statement):
        seen = set()

        def auth(action, a1, a2, dbname, trigger):
            if action == sqlite3.SQLITE_READ and a1 and a1.lower() in self.tables:
                seen.add(self.tables[a1.lower()])
            return sqlite3.SQLITE_OK

        self.sql.set_authorizer(auth)
        try:
            self.sql.execute("EXPLAIN " + statement)
        finally:
            self.sql.set_authorizer(None)
        return seen

    def query(self, statement):
        for t in sorted(self.tables_read(statement)):
            if not (self.cache and t in self.loaded):
                self.fetch(t)
        cur = self.sql.execute(statement)
        names = [d[0] for d in cur.description] if cur.description else []
        return names, cur.fetchall()

    def export(self, path):
        for t in self.tables.values():
            self.fetch(t)
        if os.path.exists(path):
            os.remove(path)
        out = sqlite3.connect(path)
        self.sql.backup(out)
        out.execute(f"PRAGMA user_version = {int(self.version)}")
        out.close()


# ---- Output ----

def fmt(v):
    if v is None:
        return "NULL"
    if isinstance(v, bytes):
        return "x'" + v.hex() + "'"
    if isinstance(v, float):
        return repr(v)
    return str(v)


def show(names, rows, mode, out):
    if mode == "json":
        out.write(json.dumps([{n: (v.hex() if isinstance(v, bytes) else v) for n, v in zip(names, r)}
                              for r in rows], ensure_ascii=False) + "\n")
        return
    if mode == "csv":
        w = csv.writer(out, lineterminator="\n")
        w.writerow(names)
        for r in rows:
            w.writerow([fmt(v) if v is None or isinstance(v, bytes) else v for v in r])
        return
    if mode == "line":
        width = max((len(n) for n in names), default=0)
        for r in rows:
            for n, v in zip(names, r):
                out.write(f"{n:>{width}} = {fmt(v)}\n")
            out.write("\n")
        return
    cells = [[fmt(v) for v in r] for r in rows]
    widths = [max([len(n)] + [len(c[i]) for c in cells]) for i, n in enumerate(names)]
    if names:
        out.write("  ".join(n.ljust(w) for n, w in zip(names, widths)).rstrip() + "\n")
        out.write("  ".join("-" * w for w in widths) + "\n")
    for c in cells:
        out.write("  ".join(v.ljust(w) for v, w in zip(c, widths)).rstrip() + "\n")


# ---- Shell ----

def parse_value(tok: str):
    if tok.upper() == "NULL":
        return None
    if len(tok) >= 2 and tok[0] == tok[-1] and tok[0] in "'\"":
        return tok[1:-1]
    if tok[:2].lower() == "x'" and tok.endswith("'"):
        return bytes.fromhex(tok[2:-1])
    for conv in (int, float):
        try:
            return conv(tok)
        except ValueError:
            pass
    return tok


class Shell:
    def __init__(self, dev: Device, out=sys.stdout):
        self.dev = dev
        self.out = out
        self.mode = "table"
        self.db = 0
        self.mirror = Mirror(dev, 0)

    def dot(self, line: str) -> bool:
        words = line.split()
        cmd, args = words[0], words[1:]
        if cmd in (".quit", ".exit"):
            return False
        if cmd == ".tables":
            self.out.write("  ".join(sorted(self.mirror.tables.values())) + "\n")
        elif cmd == ".schema":
            for kind, name, sql in self.mirror.entries:
                if not args or name.lower() in (a.lower() for a in args) or (
                        kind == 1 and any(a.lower() in sql.lower() for a in args)):
                    self.out.write(sql.rstrip(";") + ";\n")
        elif cmd == ".info":
            dbs = self.dev.request(op="info")["dbs"]
            show(["db", "path", "mode", "page_size", "pages", "used", "schema", "seq"],
                 [(i, d["path"], d["mode"], d["ps"], d["total"], d["used"], d["sv"], d["seq"])
                  for i, d in enumerate(dbs)], self.mode, self.out)
        elif cmd == ".open":
            self.db = int(args[0])
            self.mirror = Mirror(self.dev, self.db)
        elif cmd == ".mode":
            self.mode = args[0]
        elif cmd == ".cache":
            self.mirror.cache = args[0] == "on"
        elif cmd == ".refresh":
            self.mirror.loaded.clear()
        elif cmd == ".export":
            self.mirror.export(args[0])
            self.out.write(f"exported {len(self.mirror.tables)} tables to {args[0]}\n")
        elif cmd == ".run":
            r = self.dev.request(op="run", db=self.db, q=args[0], p=[parse_value(a) for a in args[1:]])
            show(r["c"], r["rows"], self.mode, self.out)
            if r["more"]:
                self.out.write("(truncated: the response buffer is full)\n")
            if r["ch"]:
                self.out.write(f"({r['ch']} changes)\n")
        elif cmd == ".help":
            self.out.write(".tables | .schema [table] | .info | .open N | .mode table|csv|json|line\n"
                           ".run <query> [values] | .export file.db | .cache on|off | .refresh | .quit\n"
                           "SQL statements end with ';' and read a fresh copy of their tables.\n")
        else:
            raise RemoteError(-1, f"unknown command {cmd} (see .help)")
        return True

    def execute(self, text: str) -> bool:
        text = text.strip()
        if not text:
            return True
        try:
            if text.startswith("."):
                return self.dot(text)
            names, rows = self.mirror.query(text)
            if names:
                show(names, rows, self.mode, self.out)
        except (RemoteError, sqlite3.Error) as e:
            self.out.write(f"Error: {e}\n")
        return True

    def repl(self, stream):
        interactive = stream.isatty()
        pending = ""
        while True:
            if interactive:
                sys.stdout.write("sqnano> " if not pending else "   ...> ")
                sys.stdout.flush()
            line = stream.readline()
            if not line:
                break
            if not pending and line.lstrip().startswith("."):
                if not self.execute(line):
                    break
                continue
            pending += line
            if sqlite3.complete_statement(pending):
                self.execute(pending)
                pending = ""
        if pending.strip():
            self.execute(pending)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--port", help="serial port (SMP over the console)")
    g.add_argument("--udp", help="device address[:port] (SMP over UDP)")
    g.add_argument("--exec", dest="exec_cmd", help="program speaking SMP on stdin/stdout")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--db", type=int, default=0, help="database index (see .info)")
    ap.add_argument("-c", "--command", action="append", help="run a command or statement and exit")
    args = ap.parse_args(argv)
    if args.port:
        t = SerialTransport(args.port, args.baud, args.timeout)
    elif args.udp:
        t = UdpTransport(args.udp, args.timeout)
    else:
        t = ExecTransport(args.exec_cmd)
    try:
        dev = Device(t)
        sh = Shell(dev)
        if args.db:
            sh.dot(f".open {args.db}")
        if args.command:
            for c in args.command:
                sh.execute(c)
        else:
            sh.repl(sys.stdin)
    except RemoteError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    finally:
        t.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
