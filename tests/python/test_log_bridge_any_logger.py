"""`httpp.bridge_logger(name)` sends a C++ logger tree into Python's `logging`, under the same names (ADR-0011).

The C++ records here come from `_log_from_cpp`, which logs on any C++ logger from the calling thread (with the GIL held,
as a binding without `nogil` does) and from library threads."""

import logging
import logging.config
import subprocess
import sys

import httpp
import pytest
from httpp import _log
from httpp.httpp_utils_python import _log_from_cpp

ROOT = "acme"
NAMES = (ROOT, ROOT + ".dsp", ROOT + ".dsp.fft", ROOT + ".models", ROOT + ".audio")


def cpp(name, level, message="hello", where="run"):
    _log_from_cpp(name, level, where, message)


def seen(caplog, name=None):
    return [r for r in caplog.records if r.name.startswith(ROOT) and (name is None or r.name == name)]


@pytest.fixture(autouse=True)
def bridged():
    _log._unbridge_all()
    httpp.bridge_logger(ROOT)
    root_level = logging.getLogger().level
    yield
    for name in NAMES:  # while bridged, so the C++ levels go back to "inherit" as well
        logging.getLogger(name).setLevel(logging.NOTSET)
    logging.getLogger().setLevel(root_level)
    _log._unbridge_all()


def test_a_record_reaches_the_python_logger_of_the_same_name(caplog):
    cpp("acme.dsp.fft", logging.WARNING, "bad size", where="fft::run")
    [record] = seen(caplog)
    assert (record.name, record.levelno, record.getMessage(), record.funcName) == (
        "acme.dsp.fft", logging.WARNING, "bad size", "fft::run")


def test_the_root_level_decides_what_cpp_produces(caplog):
    cpp("acme.dsp", logging.INFO, "quiet")
    assert seen(caplog) == []
    logging.getLogger(ROOT).setLevel(logging.INFO)
    cpp("acme.dsp", logging.INFO, "now")
    assert [r.getMessage() for r in seen(caplog)] == ["now"]
    logging.getLogger(ROOT).setLevel(logging.WARNING)
    cpp("acme.dsp", logging.INFO, "quiet again")
    assert [r.getMessage() for r in seen(caplog)] == ["now"]


def test_a_logger_below_the_root_has_its_own_level(caplog):
    logging.getLogger("acme.dsp").setLevel(logging.DEBUG)
    cpp("acme.dsp.fft", logging.DEBUG, "dsp debug")
    cpp("acme.models", logging.DEBUG, "models debug")
    assert [r.name for r in seen(caplog)] == ["acme.dsp.fft"]

    caplog.clear()
    logging.getLogger("acme.models").setLevel(logging.ERROR)
    cpp("acme.models", logging.WARNING, "models warning")
    cpp("acme.audio", logging.WARNING, "audio warning")
    assert [r.name for r in seen(caplog)] == ["acme.audio"]

    caplog.clear()
    logging.getLogger("acme.dsp").setLevel(logging.NOTSET)  # inherits the root's WARNING again
    cpp("acme.dsp.fft", logging.DEBUG, "dsp debug")
    assert seen(caplog) == []


def test_the_applications_root_logger_counts_when_the_bridged_logger_only_inherits(caplog):
    cpp("acme.audio", logging.DEBUG, "before")
    logging.getLogger().setLevel(logging.DEBUG)  # what logging.basicConfig(level=...) does
    cpp("acme.audio", logging.DEBUG, "after")
    assert [r.getMessage() for r in seen(caplog)] == ["after"]


def test_dictconfig_controls_it(caplog):
    logging.config.dictConfig({"version": 1, "disable_existing_loggers": False,
                               "loggers": {"acme.dsp": {"level": "DEBUG"}}})
    cpp("acme.dsp.fft", logging.DEBUG, "configured")
    cpp("acme.audio", logging.DEBUG, "not configured")
    assert [r.getMessage() for r in seen(caplog)] == ["configured"]


def test_levels_set_before_the_bridge_existed_are_honoured(caplog):
    _log._unbridge_all()
    logging.getLogger("acme.dsp").setLevel(logging.DEBUG)
    httpp.bridge_logger(ROOT)
    cpp("acme.dsp.fft", logging.DEBUG, "early")
    assert [r.getMessage() for r in seen(caplog)] == ["early"]


def test_calling_it_again_changes_nothing(caplog):
    httpp.bridge_logger(ROOT)
    httpp.bridge_logger(ROOT)
    cpp("acme.dsp", logging.WARNING, "once")
    assert [r.getMessage() for r in seen(caplog)] == ["once"]


def test_after_unbridging_the_records_are_dropped(caplog):
    _log._unbridge_all()
    cpp("acme.dsp", logging.ERROR, "nobody listens")
    assert seen(caplog) == []


@pytest.mark.parametrize("name", ["", "root", "httpp", "httpp.server"])
def test_names_that_are_not_ours_are_refused(name):
    with pytest.raises(ValueError):
        httpp.bridge_logger(name)


@pytest.mark.parametrize("name", ["acme.dsp", "acme.dsp.fft"])
def test_a_logger_inside_one_already_bridged_is_refused(name):
    with pytest.raises(ValueError):
        httpp.bridge_logger(name)


SCENARIO = """
import logging
import httpp
from httpp.httpp_utils_python import _log_from_cpp

lines = []


class Collect(logging.Handler):
    def emit(self, record):
        lines.append(record.getMessage())


logging.getLogger("acme").addHandler(Collect())
logging.getLogger("acme").setLevel(logging.INFO)
httpp.bridge_logger("acme")
# 4 library threads and this thread, which holds the GIL while it logs, through the same logger
_log_from_cpp("acme.worker", logging.INFO, "run", "line", 500, 4)
assert len(lines) == 2500, len(lines)
print("done")
"""


def test_python_logging_with_the_gil_held_does_not_deadlock_with_library_threads():
    # A library thread waits for the GIL inside the callback while a Python thread that holds the GIL logs through the same logger
    # (ADR-0074). Run in its own process: a deadlock must fail this test, not hang the suite.
    try:
        done = subprocess.run([sys.executable, "-c", SCENARIO], capture_output=True, text=True, timeout=60, check=False)
    except subprocess.TimeoutExpired:
        pytest.fail("deadlock: library threads and a GIL-holding thread logging through one bridged logger")
    assert done.returncode == 0, done.stderr
    assert done.stdout.strip() == "done"
