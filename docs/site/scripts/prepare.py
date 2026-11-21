#!/usr/bin/env python3
"""Prepare the generated inputs of the docs site. Standard library only.

    python3 scripts/prepare.py snippets   build src/generated/snippets.json from ../../examples/<topic>/<lang>/main.<ext>
    python3 scripts/prepare.py check      fail when an example exists in one language only or has no README.md
    python3 scripts/prepare.py bench      build src/generated/bench.json from benchmarks/results/full/
    python3 scripts/prepare.py all        snippets + check + bench

`bench` takes benchmarks/results/full/ (fixed names, see benchmarks/suite.py: system.json and one file per runtime; a new run overwrites
them and git keeps the history) and puts the files together as one run. No run is fine: the page then says there is no data yet.
HTTPP_BENCH_SMOKE=1 also takes benchmarks/results/quick/ (the test run of `run.sh quick`) to preview the page with it; the page marks it.

ponytail: no `symbols` yet. httpp has no Doxygen or Sphinx reference, so there is nothing to feed it.
"""

import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent.parent
EXAMPLES = REPO / "examples"
GITHUB = "https://github.com/mohammadraziei/httpp"
BRANCH = "master"
LANGS = json.loads((ROOT / "languages.json").read_text(encoding="utf-8"))
OUT = ROOT / "src" / "generated"
RESULTS = ROOT.parent.parent / "benchmarks" / "results"
BENCH_SCHEMA = 1


def collect():
    """{topic: {language id: file path}} from examples/<topic>/<lang>/main.<ext>."""
    found = {}
    for lang in LANGS:
        for f in sorted(EXAMPLES.glob(f"*/{lang['id']}/main.{lang['ext']}")):
            found.setdefault(f.parent.parent.name, {})[lang["id"]] = f
    return found


def cmd_snippets():
    data = {}
    for name, per_lang in collect().items():
        topic = f"examples/{name}"
        data[name] = {
            "readme": f"{GITHUB}/tree/{BRANCH}/{topic}",
            **{
                lang: {
                    "code": f.read_text(encoding="utf-8").rstrip("\n"),
                    "path": f.relative_to(REPO).as_posix(),
                    "url": f"{GITHUB}/blob/{BRANCH}/{f.relative_to(REPO).as_posix()}",
                }
                for lang, f in per_lang.items()
            },
        }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "snippets.json").write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"snippets.json: {len(data)} examples")
    if not data:
        print("warning: no example files found; pages that use <Lang> will show nothing", file=sys.stderr)


def load_run(directory):
    """benchmarks/results/<kind>/: system.json plus one file per runtime (cpp.json, py314.json, ...) put back together as one run."""
    sys_file = directory / "system.json"
    if not sys_file.exists():
        return None
    try:
        system = json.loads(sys_file.read_text(encoding="utf-8"))
        run = {"id": directory.name, "results": [], "rated": [], "idle": []}
        pythons = []
        for rt in system["runtimes"]:
            doc = json.loads((directory / rt["file"]).read_text(encoding="utf-8"))
            if doc.get("schema") != BENCH_SCHEMA:
                raise ValueError(f"{rt['file']}: schema {doc.get('schema')}")
            for key in ("results", "rated", "idle"):
                run[key] += doc.get(key, [])
            if "python" in doc:
                pythons.append(doc["python"])
    except (ValueError, KeyError, OSError) as e:
        print(f"warning: {directory.name}/ is not a usable benchmark run ({e}); skipped", file=sys.stderr)
        return None
    if system.get("schema") != BENCH_SCHEMA:
        return None
    run["meta"] = {**{k: v for k, v in system.items() if k not in ("runtimes", "schema")}, "pythons": pythons}
    return run


def cmd_bench():
    # fixed names (benchmarks/suite.py): results/full/ is a real run, results/quick/ a test of the machinery (only with HTTPP_BENCH_SMOKE=1)
    kinds = ["full"] + (["quick"] if os.environ.get("HTTPP_BENCH_SMOKE") == "1" else [])
    runs = [r for r in (load_run(RESULTS / k) for k in kinds) if r]
    runs.sort(key=lambda r: r["meta"].get("date", ""), reverse=True)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "bench.json").write_text(json.dumps({"runs": runs}, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"bench.json: {len(runs)} run(s) ({', '.join(r['id'] for r in runs) or 'none'})")


def cmd_check():
    ids = [lang["id"] for lang in LANGS]
    bad = []
    for name, per_lang in sorted(collect().items()):
        missing = [i for i in ids if i not in per_lang]
        if missing:
            bad.append(f"  {name}: missing in {', '.join(missing)}")
        if not (EXAMPLES / name / "README.md").is_file():
            bad.append(f"  {name}: no README.md")
    if bad:
        print("examples check failed:\n" + "\n".join(bad), file=sys.stderr)
        return 1
    print("examples check: ok")
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
