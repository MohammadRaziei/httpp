"""Shared fixtures for the command line (ADR-0041).

Two ways to start the same commands: `httpp` the command (httpp_cli.py, a console script that starts the executable) and
`httpp/bin/httpp` the native executable itself. `httpp_exe` is both in turn, so every test of the commands runs against each;
`httpp_bin` is the executable only, for what only it does (starting Python itself for `py`)."""

import os
import shutil
import stat
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
    """The native executable inside the package (httpp/bin/httpp), with its execute bit."""
    exe = httpp_cli.binary_path()
    if not exe.is_file():
        pytest.fail(f"the native executable is not in the package ({exe})")
    httpp_cli.ensure_executable(exe)
    return str(exe)


@pytest.fixture(scope="session")
def httpp_command():
    """The `httpp` console script of the installed package. Set HTTPP_EXE to test another one."""
    exe = _find_command()
    if exe is None:
        pytest.fail("the `httpp` command is not installed next to this Python (scripts directory); set HTTPP_EXE to its path")
    return exe


@pytest.fixture(params=["command", "binary"])
def httpp_exe(request, httpp_command, httpp_bin):
    return httpp_command if request.param == "command" else httpp_bin
