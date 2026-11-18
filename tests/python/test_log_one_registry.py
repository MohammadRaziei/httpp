"""One registry across wheels (ADR-0086, acceptance 2).

A shared library that links only libhttpp_utils (the probe built by tests/utils, the stand-in for a second wheel such as libspeech's)
logs through get_logger("x.y"); a record from it must reach a handler installed through httpp.bridge_logger("x"). Two copies of
libhttpp_utils in the process would be two registries and the record would be lost.

Set HTTPP_UTILS_PROBE_LIB to the path of the probe (libutils_registry_probe.so, .dylib or .dll); without it the test is skipped.
"""
import ctypes
import logging
import os

import pytest

import httpp
from httpp import _log

PROBE = os.environ.get("HTTPP_UTILS_PROBE_LIB")

pytestmark = pytest.mark.skipif(not PROBE or not os.path.exists(PROBE or ""), reason="HTTPP_UTILS_PROBE_LIB is not set")


def test_record_from_a_library_that_links_only_utils_reaches_python(caplog):
    probe = ctypes.CDLL(PROBE)
    probe.utils_probe_log.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p]
    probe.utils_probe_log.restype = None

    httpp.bridge_logger("x")
    try:
        logging.getLogger("x").setLevel(logging.DEBUG)
        with caplog.at_level(logging.DEBUG, logger="x"):
            probe.utils_probe_log(b"x.y", 20, b"from the probe")
        assert [(r.name, r.getMessage()) for r in caplog.records if r.name.startswith("x")] == [("x.y", "from the probe")]
    finally:
        _log._unbridge_logger("x")
        _log._BRIDGED.discard("x")
