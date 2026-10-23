#!/usr/bin/env python3
# Copyright (c) 2026 Romain Pelletant
# SPDX-License-Identifier: Apache-2.0
"""Play the OTA scenario of test_migrate.c with three firmware generations."""

import subprocess
import sys

STEPS = [
    (1, "init"),
    (2, "upgrade"),
    (1, "revert"),
    (2, "cuts"),
    (2, "confirm"),
    (1, "refuse"),
    (3, "upgrade"),
    (2, "compat"),
    (3, "verify"),
    (4, "lazy"),
    (3, "revert"),
    (4, "lazy"),
    (4, "confirm"),
    (3, "refuse"),
    (5, "upgrade"),
    ("5b", "refuse"),
    (5, "finish"),
    ("5b", "verify"),
]


def main() -> int:
    fw = {1: sys.argv[1], 2: sys.argv[2], 3: sys.argv[3], 4: sys.argv[4], 5: sys.argv[5],
          "5b": sys.argv[6]}
    image = sys.argv[7]
    for gen, action in STEPS:
        r = subprocess.run([fw[gen], action, image])
        if r.returncode:
            print(f"step fw{gen} {action} failed")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
