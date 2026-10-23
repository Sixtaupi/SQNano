# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""west sqnano: porting report and SQL compiler front-end.

  west sqnano check <app> [--schema DIR] [--queries FILE] [--wrapper NAME:ARG]
                          [--emit-schema DIR]
  west sqnano compile ...         (same options as sqnanoc compile)
  west sqnano normalize "<sql>"
  west sqnano cli --port /dev/ttyACM0 | --udp ADDR | --exec CMD [-c STATEMENT]
"""

import os
import subprocess
import sys
from pathlib import Path

from west.commands import WestCommand

SQNANOC = Path(__file__).resolve().parents[2] / "tools" / "sqnanoc"
SQNANO_CLI = Path(__file__).resolve().parents[2] / "tools" / "sqnano-cli" / "sqnano_cli.py"
SOURCE_SUFFIXES = (".c", ".h", ".cc", ".cpp", ".cxx")


class Sqnano(WestCommand):
    def __init__(self):
        super().__init__(
            "sqnano",
            "SQNano tools",
            "Porting report for an SQLite application, the host SQL compiler, and\n"
            "sqnano-cli (full SQLite on a device database, over SMP).",
            accepts_unknown_args=True,
        )

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(self.name, help=self.help, description=self.description)
        parser.add_argument("subcommand", choices=["check", "compile", "normalize", "cli"])
        parser.add_argument("target", nargs="?", help="application directory (check)")
        return parser

    def do_run(self, args, unknown):
        if args.subcommand == "cli":
            cmd = [sys.executable, str(SQNANO_CLI)] + ([args.target] if args.target else []) + unknown
            sys.exit(subprocess.run(cmd, env=os.environ).returncode)
        cmd = [sys.executable, str(SQNANOC), args.subcommand]
        if args.subcommand == "check":
            if not args.target:
                self.die("usage: west sqnano check <app directory or source> [options]")
            app = Path(args.target)
            sources = [app] if app.is_file() else sorted(
                p for p in app.rglob("*") if p.suffix in SOURCE_SUFFIXES and "build" not in p.parts)
            if "--schema" not in unknown and app.is_dir() and (app / "schema").is_dir():
                cmd += ["--schema", str(app / "schema")]
            if "--queries" not in unknown and app.is_dir() and (app / "queries.sql").exists():
                cmd += ["--queries", str(app / "queries.sql")]
            cmd += unknown + [str(s) for s in sources]
        else:
            if args.target:
                cmd.append(args.target)
            cmd += unknown
        self.dbg("running", " ".join(cmd))
        sys.exit(subprocess.run(cmd, env=os.environ).returncode)
