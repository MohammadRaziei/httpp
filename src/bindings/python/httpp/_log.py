"""The library's C++ log goes through Python's `logging` (logger "httpp"), and that logger's level decides what C++
produces (ADR-0011):

    logging.getLogger("httpp").setLevel(logging.INFO)    # C++ now also emits the access log and "listening on ..."
    httpp.set_log_level("info")                           # the same thing

C++ only builds a log line when its own level lets it through, so the C++ level follows the logger's effective level.
`logger.setLevel` is wrapped to keep them in step (that covers `logging.config.dictConfig` too); a change of the root
level that the logger merely inherits is noticed the next time a server starts. An explicit `set_log_callback(...)` or
`set_log_file(...)` replaces the bridge.
"""

import atexit
import logging
import types

from .httpp_utils_python import set_log_callback, set_log_level as _set_cpp_level
from .httpp_utils_python import _bridge_logger, _set_logger_level, _unbridge_logger

LOGGER = logging.getLogger("httpp")
_hooked = False

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
    global _hooked
    if not _hooked:
        _hooked = True
        atexit.register(_unhook)


def _unhook():
    """At interpreter exit: stop calling into Python, so a library thread that logs late cannot reach a dying
    interpreter (the C++ log goes back to stderr)."""
    try:
        set_log_callback(None)
    except Exception:
        pass


def set_log_level(level):
    """"trace", "debug", "info", "warning" (the default), "error" or "off": sets the level of the "httpp" logger."""
    if level == "off":
        LOGGER.setLevel(logging.CRITICAL + 1)
    elif level in _PY_LEVEL:
        LOGGER.setLevel(_PY_LEVEL[level])
    else:
        raise ValueError("log level must be one of %s, not %r" % (", ".join([*_PY_LEVEL, "off"]), level))


def ensure_handler():
    """For `httpp py run`: if the application configured no logging at all, print the log to stderr."""
    if LOGGER.handlers or logging.getLogger().handlers:
        return
    handler = logging.StreamHandler()
    handler.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] (httpp) %(message)s"))
    LOGGER.addHandler(handler)


# ---------------------------------------------------------------------------------------------------------------
# Any C++ logger tree (ADR-0011): httpp.bridge_logger("libspeech")
# ---------------------------------------------------------------------------------------------------------------

_BRIDGED = set()
_original_set_level = logging.Logger.setLevel
_level_hook = False


def _emit_record(levelno, name, where, message):
    """A record of a bridged C++ logger goes to the Python logger of the same name, `where` as its funcName."""
    logger = logging.getLogger(name)
    if logger.isEnabledFor(levelno):
        logger.handle(logger.makeRecord(name, levelno, "(C++)", 0, "%s", (message,), None, func=where))


def _sync_name(name):
    """The C++ logger `name` gets the level its Python logger has: a bridged root the effective one (a root that
    only inherits its level must still follow the application's logging setup), a logger below it its own (0
    inherits, in C++ as in Python)."""
    logger = logging.getLogger(name)
    _set_logger_level(name, logger.getEffectiveLevel() if name in _BRIDGED else logger.level)


def _resync(logger):
    for root in _BRIDGED:
        if logger is logging.root or logger.name == root:
            _sync_name(root)
        elif logger.name.startswith(root + "."):
            _sync_name(logger.name)


def _set_level_everywhere(self, level):
    _original_set_level(self, level)
    _resync(self)


def bridge_logger(name):
    """Route the C++ logger `name` and every logger below it into Python's `logging`, under the same names:

        httpp.bridge_logger("libspeech")                              # C++ "libspeech.dsp.stft" -> getLogger("libspeech.dsp.stft")
        logging.getLogger("libspeech").setLevel(logging.INFO)         # what C++ produces follows the Python levels
        logging.getLogger("libspeech.dsp").setLevel(logging.DEBUG)    # ... also for a logger below the root

    A C++ logger's level is that of the Python logger of its name (a bridged root: its effective level, so the
    application's root logger counts), kept in step when `Logger.setLevel` is called on any logger (that covers
    `logging.config.dictConfig` and `logging.basicConfig(level=...)`). C++ only builds a line when its own level lets it
    through. A line is delivered to the Python logger's handlers, with the C++ function as `funcName`. Safe to call
    again. The names "httpp" (and below) are bridged by httpp itself and a name inside or around one already
    bridged is refused (ValueError)."""
    if not name or name == "root":
        raise ValueError("name must be the name of a C++ logger, not %r" % (name,))
    if name == "httpp" or name.startswith("httpp."):
        raise ValueError("the logger %r is bridged by httpp itself (httpp.set_log_level, httpp.set_log_callback)" % (name,))
    for other in _BRIDGED:
        if other != name and (name.startswith(other + ".") or other.startswith(name + ".")):
            raise ValueError("%r overlaps %r, which is already bridged" % (name, other))
    global _level_hook
    if not _level_hook:
        _level_hook = True
        logging.Logger.setLevel = _set_level_everywhere
        atexit.register(_unbridge_all)
    _bridge_logger(name, _emit_record)
    _BRIDGED.add(name)
    _sync_name(name)
    for known, logger in list(logging.root.manager.loggerDict.items()):  # levels set before the bridge existed
        if known.startswith(name + ".") and isinstance(logger, logging.Logger) and logger.level:
            _sync_name(known)


def _unbridge_all():
    """At interpreter exit (and for the tests): stop calling into Python, so a library thread that logs late cannot reach
    a dying interpreter."""
    for name in list(_BRIDGED):
        try:
            _unbridge_logger(name)
        except Exception:
            pass
    _BRIDGED.clear()
