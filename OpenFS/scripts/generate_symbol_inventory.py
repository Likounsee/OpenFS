#!/usr/bin/env python3
"""Regenerate the lexical Rust symbol index for OpenFS.

Run from any directory inside a checkout:
    python OpenFS/scripts/generate_symbol_inventory.py

This is a standard-library-only lexical index, not a Rust parser. It records
declaration lines for review and does not assert behavioral correctness.
"""
from __future__ import annotations

import argparse
import csv
import re
import subprocess
from pathlib import Path

FUNCTION_RE = re.compile(
    r"^\s*(?:(?:pub(?:\([^)]*\))?)\s+)?(?:async\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\b"
)
TYPE_RE = re.compile(
    r"^\s*(?:(?:pub(?:\([^)]*\))?)\s+)?(struct|enum|trait|type|const|static)\s+([A-Za-z_][A-Za-z0-9_]*)\b"
)
IMPL_RE = re.compile(r"^\s*impl(?:<[^>]+>)?\s+(.+?)\s*\{\s*$")
FIELDS = [
    "source_file", "line", "symbol_kind", "symbol", "declaration",
    "status", "analysis_note",
]


def git(root: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(root), *args], text=True, encoding="utf-8"
    ).strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path,
        help="repository root (defaults to the parent of OpenFS/scripts)",
    )
    args = parser.parse_args()
    script_path = Path(__file__).resolve()
    root = args.root.resolve() if args.root else script_path.parents[2]
    source_revision = git(root, "rev-parse", "HEAD")
    source_root = root / "OpenFS" / "crates"
    rust_files = sorted(source_root.rglob("*.rs"))
    output = root / "OpenFS" / "docs" / "INVENTAIRE_SYMBOLES.csv"
    rows: list[dict[str, str]] = []

    for path in rust_files:
        relative = path.relative_to(root).as_posix()
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            function = FUNCTION_RE.match(line)
            type_match = TYPE_RE.match(line)
            impl_match = IMPL_RE.match(line)
            if function:
                kind, symbol = "function", function.group(1)
            elif type_match:
                kind, symbol = type_match.group(1), type_match.group(2)
            elif impl_match:
                kind, symbol = "impl", impl_match.group(1)
            else:
                continue
            rows.append({
                "source_file": relative,
                "line": str(line_number),
                "symbol_kind": kind,
                "symbol": symbol,
                "declaration": line.strip(),
                "status": "Indexed",
                "analysis_note": (
                    f"Lexical index regenerated from checkout HEAD "
                    f"{source_revision[:12]}; declaration and line detected, "
                    "behavior not validated."
                ),
            })

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    print(f"Wrote {len(rows)} declarations from {len(rust_files)} Rust files to {output}")
    print("Lexical indexing is not a complete Rust AST or behavioral audit.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
