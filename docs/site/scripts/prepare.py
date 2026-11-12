#!/usr/bin/env python3
"""Prepare the generated inputs of the docs site. Standard library only.

    python3 scripts/prepare.py snippets   build src/generated/snippets.json
    python3 scripts/prepare.py check      fail when a snippet exists in one language only
    python3 scripts/prepare.py bench      build src/generated/bench.json from benchmarks/results/*.json
    python3 scripts/prepare.py all        snippets + check + bench

`bench` takes the files with "purpose": "published" (the benchmark suite writes "smoke" for test runs) and the schema this
page understands, newest first. No file is fine: the page then says there is no data yet. HTTPP_BENCH_SMOKE=1 also takes the smoke
files, to preview the page with the numbers of a test run (the page marks them).

ponytail: no `symbols` yet. httpp has no Doxygen or Sphinx reference, so there is nothing to feed it.
"""

import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LANGS = json.loads((ROOT / "languages.json").read_text(encoding="utf-8"))
OUT = ROOT / "src" / "generated"
RESULTS = ROOT.parent.parent / "benchmarks" / "results"
BENCH_SCHEMA = 1


def collect():
    """{snippet name: {language id: file path}} from snippets/<lang>/<name>.<ext>."""
    found = {}
    for lang in LANGS:
        folder = ROOT / "snippets" / lang["id"]
        if not folder.is_dir():
            continue
        for f in sorted(folder.glob("*." + lang["ext"])):
            found.setdefault(f.stem, {})[lang["id"]] = f
    return found


def cmd_snippets():
    data = {}
    for name, per_lang in collect().items():
        data[name] = {
            lang: {"code": f.read_text(encoding="utf-8").rstrip("\n"), "path": f.relative_to(ROOT).as_posix()}
            for lang, f in per_lang.items()
        }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "snippets.json").write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"snippets.json: {len(data)} snippets")
    if not data:
        print("warning: no snippet files found; pages that use <Lang> will show nothing", file=sys.stderr)


def cmd_bench():
    runs = []
    for f in sorted(RESULTS.glob("*.json")):
        try:
            doc = json.loads(f.read_text(encoding="utf-8"))
        except ValueError as e:
            print(f"warning: {f.name} is not valid JSON ({e}); skipped", file=sys.stderr)
            continue
        meta = doc.get("meta", {})
        wanted = ("published", "smoke") if os.environ.get("HTTPP_BENCH_SMOKE") == "1" else ("published",)
        if doc.get("schema") != BENCH_SCHEMA or meta.get("purpose") not in wanted:
            continue
        runs.append({"id": f.stem, **doc})
    runs.sort(key=lambda r: r["meta"].get("date", ""), reverse=True)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "bench.json").write_text(json.dumps({"runs": runs}, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"bench.json: {len(runs)} published run(s)")


def cmd_check():
    ids = [lang["id"] for lang in LANGS]
    bad = []
    for name, per_lang in sorted(collect().items()):
        missing = [i for i in ids if i not in per_lang]
        if missing:
            bad.append(f"  {name}: missing in {', '.join(missing)}")
    if bad:
        print("parity check failed:\n" + "\n".join(bad), file=sys.stderr)
        return 1
    print("parity check: ok")
    return 0


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "all"
    if cmd == "snippets":
        cmd_snippets()
        return 0
    if cmd == "check":
        return cmd_check()
    if cmd == "bench":
        cmd_bench()
        return 0
    if cmd == "all":
        cmd_snippets()
        cmd_bench()
        return cmd_check()
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
