"""The library's C++ log goes through Python's `logging` (logger "httpp"), and that logger's level decides what C++
produces (ADR-0018):

    logging.getLogger("httpp").setLevel(logging.INFO)    # C++ now also emits the access log and "listening on ..."
    httpp.set_log_level("info")                           # the same thing

C++ only builds a log line when its own level lets it through, so the C++ level follows the logger's effective level.
`logger.setLevel` is wrapped to keep them in step (that covers `logging.config.dictConfig` too); a change of the root
level that the logger merely inherits is noticed the next time a server starts. An explicit `set_log_callback(...)` or
`set_log_file(...)` replaces the bridge.
"""

import logging
import types

from .httpp_cy import set_log_callback, set_log_level as _set_cpp_level

LOGGER = logging.getLogger("httpp")

_PY_LEVEL = {"trace": 5, "debug": logging.DEBUG, "info": logging.INFO, "warning": logging.WARNING, "error": logging.ERROR}
_LIMITS = (("trace", 5), ("debug", logging.DEBUG), ("info", logging.INFO), ("warning", logging.WARNING),
           ("error", logging.ERROR))


def cpp_level_name(number):
    """The C++ level that lets exactly the lines through that a Python logger at `number` would keep."""
    for name, limit in _LIMITS:
        if number <= limit:
            return name
    return "off"  # CRITICAL and above: C++ has nothing that severe


def sync():
    """Make the C++ level follow the logger's effective level."""
    _set_cpp_level(cpp_level_name(LOGGER.getEffectiveLevel()))


def _emit(level, where, message):
    LOGGER.log(_PY_LEVEL[level], "%s", message)


def _set_level(self, level):
    logging.Logger.setLevel(self, level)
    sync()


def install():
    """Route the C++ log into `logging` and keep the levels in step. Safe to call again."""
    if "setLevel" not in LOGGER.__dict__:
        LOGGER.setLevel = types.MethodType(_set_level, LOGGER)
    set_log_callback(_emit)
    sync()


def set_log_level(level):
    """"trace", "debug", "info", "warning" (the default), "error" or "off": sets the level of the "httpp" logger."""
    if level == "off":
        LOGGER.setLevel(logging.CRITICAL + 1)
    elif level in _PY_LEVEL:
        LOGGER.setLevel(_PY_LEVEL[level])
    else:
        raise ValueError("log level must be one of %s, not %r" % (", ".join([*_PY_LEVEL, "off"]), level))


def ensure_handler():
    """For `httpp run`: if the application configured no logging at all, print the log to stderr."""
    if LOGGER.handlers or logging.getLogger().handlers:
        return
    handler = logging.StreamHandler()
    handler.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] (httpp) %(message)s"))
    LOGGER.addHandler(handler)
