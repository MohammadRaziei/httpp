"""The abi3 wheel targets Python 3.9 (pyproject.toml, py-api = "cp39"), where the stable ABI has no Py_buffer.
A typed memoryview in a .pyx makes Cython emit Py_buffer code and the wheel build fails (CI-only: a normal
local build does not use the limited API). This is a cheap guard; the real check is the wheels workflow."""
import re
from pathlib import Path

PYX_DIR = Path(__file__).resolve().parents[2] / "src" / "bindings" / "python"
TYPED_MEMORYVIEW = re.compile(r"\b(?:unsigned\s+)?(?:char|int|long|short|float|double|uint8_t|size_t)\s*\[\s*(?:::1|:)(?:\s*,\s*(?:::1|:))*\s*\]")


def test_no_typed_memoryviews_in_the_cython_sources():
    files = sorted(PYX_DIR.glob("*.pyx"))
    assert files, f"no .pyx found in {PYX_DIR}"
    for f in files:
        for n, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
            code = line.split("#", 1)[0]
            assert not TYPED_MEMORYVIEW.search(code), f"{f.name}:{n} uses a typed memoryview (breaks the abi3 build): {line.strip()}"
