"""Shared fixtures for the command line (ADR-0041).

Two ways to start the same commands: `httpp` the command (httpp_cli.py, a console script that starts the executable) and
`httpp/sdk/bin/httpp` the native executable itself. `httpp_exe` is both in turn, so every test of the commands runs against each;
`httpp_bin` is the executable only, for what only it does (starting Python itself for `py`)."""

import os
import shutil
import subprocess
import sys
import sysconfig
from pathlib import Path

import pytest

import httpp
import httpp_cli

# The CMake test target runs this suite on a staged install directory that is on sys.path only because pytest runs with it as the current
# directory. The commands start other Pythons (`httpp py run`, `python -m httpp`, reload workers), possibly in another directory: give them the
# same place to import httpp and httpp_cli from. With a pip-installed package this is just site-packages again.
_PACKAGE_ROOT = str(Path(httpp.__file__).resolve().parent.parent)
os.environ["PYTHONPATH"] = os.pathsep.join(filter(None, [_PACKAGE_ROOT, os.environ.get("PYTHONPATH")]))


def _find_command():
    name = "httpp.exe" if os.name == "nt" else "httpp"
    explicit = os.environ.get("HTTPP_EXE")
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    scripts = sysconfig.get_path("scripts")  # where pip puts the console script (the venv's bin/ or Scripts/)
    if scripts and os.path.isfile(os.path.join(scripts, name)):
        return os.path.join(scripts, name)
    return shutil.which("httpp")


@pytest.fixture(scope="session")
def httpp_bin():
    """The native executable inside the package (httpp/sdk/bin/httpp), with its execute bit."""
    exe = httpp_cli.binary_path()
    if not exe.is_file():
        pytest.fail(f"the native executable is not in the package ({exe})")
    httpp_cli.ensure_executable(exe)
    return str(exe)


@pytest.fixture
def stop_process():
    """stop_process(proc): stop a command started with Popen together with what it started, and return (stdout, stderr).

    On Windows the pip-generated httpp.exe (and `python -m httpp`) start the native executable as a child process (there is no exec there),
    so terminate() would leave the server running with the pipes open: the whole process tree is killed instead."""

    def stop(proc, timeout=30):
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
        else:
            proc.terminate()
        return proc.communicate(timeout=timeout)

    return stop


@pytest.fixture(scope="session")
def httpp_command():
    """The `httpp` console script that pip generates (httpp_cli.py). Set HTTPP_EXE to test another one.

    A tree that is not pip-installed has none (the CMake test target runs against a staged install directory): then the tests that
    need this very script are skipped (`httpp_exe` falls back to `python -m httpp`, the same program)."""
    exe = _find_command()
    if exe is None:
        if os.environ.get("HTTPP_EXE"):
            pytest.fail(f"HTTPP_EXE is set but is not a file: {os.environ['HTTPP_EXE']}")
        pytest.skip("no `httpp` console script next to this Python (the package is not pip-installed)")
    return exe


@pytest.fixture(params=["command", "binary"])
def httpp_exe(request):
    """The command as an argv prefix (a list): `[console script]` or, when the tree is not pip-installed, `[python, -m, httpp]` (the same
    program, ADR-0041), and `[native executable]` for the other variant. Tests build `[*httpp_exe, ARGS...]`."""
    if request.param == "binary":
        return [request.getfixturevalue("httpp_bin")]
    if os.environ.get("HTTPP_EXE"):  # an explicit choice must exist (the fixture fails when it does not)
        return [request.getfixturevalue("httpp_command")]
    exe = _find_command()
    return [exe] if exe else [sys.executable, "-m", "httpp"]
