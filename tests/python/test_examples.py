"""Examples must keep working. The client ones are self-contained and exit on
their own; the server ones block forever, so those are only compiled."""

import os
import subprocess
import sys
from pathlib import Path

import httpp
import pytest

EXAMPLES = Path(__file__).resolve().parents[2] / "examples" / "python"
ALL = sorted(EXAMPLES.glob("[0-9][0-9]_*.py"))
OFFLINE = [
    "09_client.py", "10_request_builder.py", "11_download.py", "12_hooks_and_settings.py",
    "13_streaming_and_files.py", "14_async_client.py", "15_progress_and_url.py",
]


def test_examples_are_found():
    assert len(ALL) >= 15


@pytest.mark.parametrize("path", ALL, ids=lambda p: p.name)
def test_example_compiles(path):
    compile(path.read_text(), str(path), "exec")


@pytest.mark.parametrize("name", OFFLINE)
def test_offline_example_runs(name):
    # The example runs in a child process, whose sys.path starts at the script's own
    # folder, so point it at the very httpp package this test run is testing.
    package_parent = str(Path(httpp.__file__).resolve().parent.parent)
    env = {**os.environ, "PYTHONPATH": os.pathsep.join(filter(None, [package_parent, os.environ.get("PYTHONPATH")]))}
    # timeout: a hang (e.g. a GIL deadlock) must fail the test, not stall CI
    res = subprocess.run([sys.executable, str(EXAMPLES / name)], capture_output=True, timeout=60, env=env)
    assert res.returncode == 0, res.stderr.decode(errors="replace")
