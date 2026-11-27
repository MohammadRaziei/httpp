"""Shared fixtures for the command line (ADR-0041).

Two ways to start the same commands: `httpp` the command (httpp_cli.py, a console script that starts the executable) and
`httpp/sdk/bin/httpp` the native executable itself. `httpp_exe` is both in turn, so every test of the commands runs against each;
`httpp_bin` is the executable only, for what only it does (starting Python itself for `py`)."""

import os
import shutil
import subprocess
import sys
import sysconfig

import pytest

import httpp
import httpp_cli

# Where the commands under test find httpp: pip-installed it is site-packages; the CMake test target sets PYTHONPATH itself
# (tests/python/CMakeLists.txt).


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    """In CI, write the reason of every failure the moment it happens.

    pytest prints the reasons at the end of the run, and a run that dies on the way (an abort() in a C++ thread, a hang that
    pytest-timeout ends) never gets there: the log then says FAILED and nothing more. The report goes to the terminal writer with
    a flush, because the output of a piped process is block-buffered and an abort loses what has not been flushed."""
    outcome = yield
    report = outcome.get_result()
    if not report.failed or not os.environ.get("CI"):
        return
    reporter = item.config.pluginmanager.get_plugin("terminalreporter")
    if reporter is None:
        return
    reporter.ensure_newline()
    reporter.write_line(f"===== {report.when} of {report.nodeid} FAILED =====", red=True)
    reporter.write_line(report.longreprtext)
    flush = getattr(getattr(reporter, "_tw", None), "flush", None)  # a private name of pytest: do without it if it is gone
    if flush is not None:
        flush()


@pytest.fixture(autouse=True)
def _plain_text(monkeypatch):
    """Python 3.14 argparse colours its help when FORCE_COLOR is set; the tests compare text, in this process and in the commands they start."""
    monkeypatch.delenv("FORCE_COLOR", raising=False)
    monkeypatch.setenv("NO_COLOR", "1")
    monkeypatch.setenv("PYTHON_COLORS", "0")


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


@pytest.fixture(autouse=True)
def _no_test_server_left_running(request):
    """A test must not leave a TestClient server thread behind.

    An unclosed server keeps its I/O thread, its worker threads and their stacks for the rest of the process. On the 32-bit
    free-threaded Windows job (2 GiB of address space) that ran the process out of memory after a few hundred tests, and the
    next thread.start() then hung. The client now stops its server when it is collected, so this only fails a test that keeps a
    client alive after it ended (a module level client, say), and says which one."""
    yield
    import gc
    import threading
    import time

    gc.collect()
    deadline = time.monotonic() + 5
    while True:
        left = [t for t in threading.enumerate() if t.name == "httpp-testclient" and t.is_alive()]
        if not left or time.monotonic() >= deadline:
            break
        time.sleep(0.02)
    assert not left, f"{request.node.nodeid} left {len(left)} httpp-testclient server thread(s) running: close the TestClient"


# --- Address-space trace (diagnosis of the cp314t-win32 wheel job) ---------------------------------------------------------
# On the 32-bit free-threaded Windows job the hook tests of test_server_features.py fail after ~750 passed tests with MemoryError
# that Python cannot even print ("lost sys.stderr"). The suspicion is that the process runs out of its 2 GiB of address space;
# nothing proves it. This writes one MEMTRACE line after every test (free address space, address space in use, thread count) so
# one CI run shows whether the numbers fall to zero before the first failure, and in which test they jump.
# It is on by itself in a 32-bit Windows process and with HTTPP_MEM_TRACE=1 anywhere. Take it out when the cause is fixed.
_MEM_TRACE = os.environ.get("HTTPP_MEM_TRACE") == "1" or (sys.platform == "win32" and sys.maxsize <= 2**32)


def _address_space_mib():
    """(free MiB, used MiB) of the address space of this process, or None where it cannot be read."""
    if sys.platform == "win32":
        import ctypes

        class MemoryStatusEx(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        status = MemoryStatusEx()
        status.dwLength = ctypes.sizeof(MemoryStatusEx)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return None
        return status.ullAvailVirtual // 2**20, (status.ullTotalVirtual - status.ullAvailVirtual) // 2**20
    try:
        with open("/proc/self/statm") as f:  # Linux: only so the plumbing can be tried; the number is not an address-space limit
            used = int(f.read().split()[0]) * os.sysconf("SC_PAGE_SIZE") // 2**20
        return 0, used
    except OSError:
        return None


@pytest.fixture(autouse=True)
def _memtrace(request):
    yield
    if not _MEM_TRACE:
        return
    import threading

    numbers = _address_space_mib()
    free, used = numbers if numbers is not None else (None, None)
    if free is None:
        return
    line = f"MEMTRACE free={free}MiB used={used}MiB threads={threading.active_count()} {request.node.nodeid}"
    # pytest's capture would keep the line until the test fails; switch it off for this one line
    capture = request.config.pluginmanager.get_plugin("capturemanager")
    if capture is not None:
        with capture.global_and_fixture_disabled():
            print(line, file=sys.stderr, flush=True)
    else:
        print(line, file=sys.stderr, flush=True)
