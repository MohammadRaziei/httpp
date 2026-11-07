"""The `httpp` command (ADR-0041): finds the native executable inside the package and runs it.

The commands without Python (server, browse, download, curl, --version) are the executable `httpp/bin/httpp` (`httpp.exe` on Windows),
built from src/cli/httpp_cli.cpp. On POSIX this launcher replaces itself with it (`exec`), so nothing of Python is left running.
`httpp py ...` is the Python half (`httpp.__main__`) and runs in this process. Stdlib only.

This module sits next to the `httpp` package in site-packages, not inside it, on purpose: a module inside the package would make
Python import `httpp/__init__` (the compiled extension, about 80 ms) before this code runs, to start a program that does not need it.
Do not import `httpp` at the top of this file."""

import os
import stat
import subprocess
import sys
from pathlib import Path

NATIVE_COMMANDS = ("server", "browse", "download", "curl")
_PY_ONLY_COMMANDS = ("run", "path")
_EXE_NAME = "httpp.exe" if os.name == "nt" else "httpp"


def binary_path():
    """Where the native executable is in this installation."""
    return Path(__file__).resolve().parent / "httpp" / "bin" / _EXE_NAME


def ensure_executable(path):
    """Give the file its execute bits if an installer dropped them (a wheel does not always keep file modes). Raises OSError if it cannot."""
    if os.name == "nt" or os.access(path, os.X_OK):
        return
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def run_native(args):
    """Run `httpp/bin/httpp ARGS...`. On POSIX it does not return unless the program could not be started; on Windows it returns its exit code."""
    exe = binary_path()
    if not exe.is_file():
        print(f"httpp: error: the native executable is missing ({exe}); reinstall httpp, or build it without -DHTTPP_BUILD_CLI=OFF", file=sys.stderr)
        return 1
    try:
        ensure_executable(exe)
    except OSError as e:
        print(f"httpp: error: {exe} is not executable and cannot be made so ({e})", file=sys.stderr)
        return 1

    env = dict(os.environ)
    if sys.executable:
        env.setdefault("HTTPP_PYTHON", sys.executable)  # `httpp py ...` run by the executable itself uses this same interpreter
    if os.name == "nt":
        # Windows finds libhttpp_core.dll next to the executable or on PATH; it is in ../lib
        env["PATH"] = str(exe.parent.parent / "lib") + os.pathsep + env.get("PATH", "")

    sys.stdout.flush()
    sys.stderr.flush()
    argv = [str(exe), *args]
    if os.name == "nt":
        return subprocess.call(argv, env=env)
    try:
        os.execve(str(exe), argv, env)
    except OSError as e:
        print(f"httpp: error: cannot start {exe} ({e})", file=sys.stderr)
        return 1


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv and argv[0] == "py":
        if len(argv) == 1:
            print("usage: httpp py COMMAND [ARGS...]\nPython commands: run, path. Try: httpp py --help", file=sys.stderr)
            return 2
        from httpp.__main__ import main as python_main

        return python_main(argv[1:])
    if argv and argv[0] in _PY_ONLY_COMMANDS:
        print(f"httpp: '{argv[0]}' is a Python command: use `httpp py {argv[0]}`", file=sys.stderr)
        return 2
    return run_native(argv)


if __name__ == "__main__":
    sys.exit(main())
