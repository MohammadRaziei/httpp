"""Examples must keep working. The client ones are self-contained and exit on
their own; the server ones block forever, so those are only compiled."""

import subprocess
import sys
from pathlib import Path

import pytest

EXAMPLES = Path(__file__).resolve().parents[2] / "examples" / "python"
ALL = sorted(EXAMPLES.glob("[0-9][0-9]_*.py"))
OFFLINE = ["09_client.py", "10_request_builder.py", "11_download.py"]


def test_examples_are_found():
    assert len(ALL) >= 11


@pytest.mark.parametrize("path", ALL, ids=lambda p: p.name)
def test_example_compiles(path):
    compile(path.read_text(), str(path), "exec")


@pytest.mark.parametrize("name", OFFLINE)
def test_offline_example_runs(name):
    # timeout: a hang (e.g. a GIL deadlock) must fail the test, not stall CI
    res = subprocess.run([sys.executable, str(EXAMPLES / name)], capture_output=True, timeout=60)
    assert res.returncode == 0, res.stderr.decode(errors="replace")
