"""Shared fixtures for the command line (ADR-0041).

Two ways to start the same commands: `httpp` the command (httpp_cli.py, a console script that starts the executable) and
`httpp/sdk/bin/httpp` the native executable itself. `httpp_exe` is both in turn, so every test of the commands runs against each;
`httpp_bin` is the executable only, for what only it does (starting Python itself for `py`)."""

import os
import shutil
import sysconfig
from pathlib import Path

import pytest

import httpp
import httpp_cli


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


@pytest.fixture(scope="session")
def httpp_command():
    """The `httpp` console script that pip generates (httpp_cli.py). Set HTTPP_EXE to test another one.

    A tree that is not pip-installed has none (the CMake test target runs against a staged install directory): then the tests that
    need the launcher are skipped, and the same commands still run on the native executable through `httpp_bin`."""
    exe = _find_command()
    if exe is None:
        if os.environ.get("HTTPP_EXE"):
            pytest.fail(f"HTTPP_EXE is set but is not a file: {os.environ['HTTPP_EXE']}")
        pytest.skip("no `httpp` console script next to this Python (the package is not pip-installed)")
    return exe


@pytest.fixture(params=["command", "binary"])
def httpp_exe(request):
    # The fixture is looked up only for the "command" variant, so a missing console script never takes the "binary" variant down with it.
    return request.getfixturevalue("httpp_command") if request.param == "command" else request.getfixturevalue("httpp_bin")
