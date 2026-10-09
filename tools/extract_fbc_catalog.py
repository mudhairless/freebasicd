#!/usr/bin/env python3
"""Extract the fbc message catalog into tools/fbc_catalog.tsv.

The catalog is data: error.bas holds two brace-delimited arrays whose 1-based
position is the number fbc prints (FB_ERRMSG_OK = 0; error number = enum
position - 1). This script reads the pinned tag's error.bas and writes the
five columns the server needs.

    kind  number  name  text  level

`kind` is E or W. `level` is a warning's -w gate level (0 means off by default);
errors carry 0 because the level model does not apply to them.

This step needs the fbc source, so it is a manual, rarely-run tool. The
checked-in tools/fbc_catalog.tsv is the durable snapshot; the C++ emitter
(tools/fbc_catalog.cpp) and tests/fbc_diagnostics_checks.cpp work from it and
never touch the compiler tree. See AGENTS.md ("A finished milestone goes in
CHANGELOG.md") for why the catalog is imported rather than built from source.

Usage:
    FBC_SRC=/tmp/opencode/fbc-1.10.2/src/compiler \
        tools/extract_fbc_catalog.py > tools/fbc_catalog.tsv
"""

import os
import re
import sys

FBC_SRC = os.environ.get(
    "FBC_SRC",
    "/tmp/opencode/fbc-1.10.2/src/compiler",
)

ERRLINE = re.compile(r"FB_ERRMSG_([A-Z0-9_]+)\s*'/\s*@\"(.*)\"")
WARNLINE = re.compile(r"FB_WARNINGMSG_([A-Z0-9_]+)\s*'/\s*(\d+),\s*@\"(.*)\"")


def main():
    path = os.path.join(FBC_SRC, "error.bas")
    try:
        with open(path, encoding="utf-8", errors="replace") as source:
            lines = source.read().splitlines()
    except OSError as exc:
        sys.stderr.write(f"extract_fbc_catalog: cannot read {path}: {exc}\n")
        return 1

    entries = []
    for line in lines:
        match = ERRLINE.search(line)
        if match:
            entries.append(("E", match.group(1), match.group(2), 0))
            continue
        match = WARNLINE.search(line)
        if match:
            entries.append(("W", match.group(1), match.group(3), int(match.group(2))))

    errors = sum(1 for entry in entries if entry[0] == "E")
    warnings = sum(1 for entry in entries if entry[0] == "W")
    sys.stderr.write(f"extract_fbc_catalog: {errors} errors, {warnings} warnings\n")

    print("# fbc message catalog snapshot (pinned tag; see AGENTS.md).")
    print("# kind\tnumber\tname\ttext\t-w level")

    error_no = warning_no = 0
    for kind, name, text, level in entries:
        if kind == "E":
            error_no += 1
            number = error_no
        else:
            warning_no += 1
            number = warning_no
        text = text.replace("\t", " ")
        print(f"{kind}\t{number}\t{name}\t{text}\t{level}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
