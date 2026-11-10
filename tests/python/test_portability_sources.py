"""httpp does not use std::filesystem (ADR-0054). The macOS wheels target 10.13, where <filesystem> is marked
unavailable (it needs 10.15), so one use fails the macOS build and nothing else: Linux CI cannot see it. The file
system goes through src/core/internal/fs.hpp. (The C++ test programs may use it: they never ship.)"""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE_DIRS = ["src/core", "src/cli", "include"]


def test_the_library_and_the_cli_do_not_use_std_filesystem():
    offenders = []
    for d in SOURCE_DIRS:
        base = ROOT / d
        for path in [*base.rglob("*.cpp"), *base.rglob("*.hpp"), *base.rglob("*.h")]:
            if "third_party" in path.parts:
                continue
            for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                code = line.split("//", 1)[0]
                if "<filesystem>" in code or "std::filesystem" in code:
                    offenders.append("%s:%d" % (path.relative_to(ROOT), number))
    assert not offenders, "std::filesystem breaks the macOS 10.13 build, use httpp::fs: " + ", ".join(offenders)
