"""Every public C++ name (HTTPP_API in include/httpp/*.hpp) must have a Python counterpart, or an entry below
that says why not (ADR-0016). It compares names, not behavior.

ponytail: the headers are read with regexes, not a C++ parser. It handles the plain declarations this project
writes (`HTTPP_API ret name(...)` inside a struct/class or at namespace level). Upgrade path: libclang.
"""

import re
from pathlib import Path

import pytest

import httpp

INCLUDE = Path(__file__).resolve().parents[2] / "include" / "httpp"
pytestmark = pytest.mark.skipif(not INCLUDE.is_dir(), reason="the C++ headers are not next to the tests")


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def cpp_api():
    """{(header, owner): {names}} for every HTTPP_API declaration; owner '' = namespace-level function."""
    out = {}
    for path in sorted(INCLUDE.glob("*.hpp")):
        text = _strip_comments(path.read_text(encoding="utf-8"))
        spans = []
        for m in re.finditer(r"\b(?:struct|class)\s+(?:HTTPP_API\s+)?(\w+)\s*(?:final\s*)?\{", text):
            depth, j = 1, m.end()
            while depth and j < len(text):
                depth += (text[j] == "{") - (text[j] == "}")
                j += 1
            spans.append((m.start(), j, m.group(1)))
        for m in re.finditer(r"HTTPP_API\s+([^;{]*?)\b(operator\s*\S+|~?\w+)\s*\(", text):
            name = m.group(2)
            owner = ""
            for a, b, n in spans:
                if a < m.start() < b:
                    owner = n  # the innermost block starts last
            if name.startswith(("~", "operator", "__")) or name == owner:
                continue  # destructors, operators, export macros, constructors
            out.setdefault((path.name, owner), set()).add(name)
    return out


# (header, C++ owner) -> the Python object that carries it. None = not meant to be reachable from Python.
TARGETS = {
    ("client.hpp", "client"): httpp.Client,
    ("client.hpp", "async_client"): httpp.AsyncClient,
    ("client.hpp", "request"): httpp.Request,
    ("curl.hpp", "request"): httpp.Request,  # httpp::curl::request: Python's Request has the same builder names
    ("download.hpp", "download"): httpp.Download,
    ("progress.hpp", "bar"): httpp.progress.bar,
    ("server.hpp", "server"): httpp.Server,
    ("url.hpp", "url"): httpp.URL,
    ("log.hpp", ""): httpp,
    ("sha256.hpp", ""): httpp,
    ("swagger_ui.hpp", ""): httpp,
    ("server.hpp", "responder"): None,  # handed to route_async handlers; Python handlers just return or await
}

# C++ name -> Python name, per owner, for names that differ on purpose.
ALIASES = {
    "client": {"del": "delete", "send": "request"},
    "async_client": {"del": "delete", "send": "request"},
    "bar": {"is_finished": "finished"},  # a property in Python, like total and current
    "server": {
        "del": "delete", "before": "before_request", "after": "after_request",
        "set_error_handler": "error_handler", "set_exception_handler": "exception_handler",
        # an `async def` handler goes through the ordinary decorator in Python
        "route_async": "route", "get_async": "get", "post_async": "post", "put_async": "put",
        "patch_async": "patch", "del_async": "delete",
    },
    "": {"mount_swagger_ui": None},  # lives on Server: Server.mount_swagger_ui (checked below)
}

# C++ setters that are constructor arguments in Python: name -> keyword, with a value that must be accepted.
CTOR_ARGS = {
    "client": {
        "set_timeout": ("timeout", 5), "set_follow_redirects": ("follow_redirects", True),
        "set_basic_auth": ("auth", ("u", "p")), "set_bearer_token": ("token", "t"),
        "set_verify": ("verify", False), "set_ca_file": ("ca_file", "ca.pem"),
        "set_proxy": ("proxy", ("127.0.0.1", 3128)), "set_keep_alive": ("keep_alive", False),
    },
    "async_client": {
        "set_timeout": ("timeout", 5), "set_follow_redirects": ("follow_redirects", True),
        "set_basic_auth": ("auth", ("u", "p")), "set_bearer_token": ("token", "t"),
        "set_verify": ("verify", False), "set_ca_file": ("ca_file", "ca.pem"),
        "set_proxy": ("proxy", ("127.0.0.1", 3128)), "set_header": ("headers", {"X": "1"}),
    },
}

# Gaps that exist today and are tracked in checklist.md. An entry here must still be a real gap (a test checks),
# so fixing the gap forces the entry to go.
KNOWN_GAPS = {
    ("server", "route_io"): "only the internal add_loop_route reaches it",
    ("server", "route_shared"): "only the internal add_route reaches it",
    ("", "find_swagger_ui_asset"): "C++ lookup helper behind mount_swagger_ui, not meant for users",
}


def _py_names(obj):
    return {n for n in dir(obj) if not n.startswith("_")}


def _missing():
    missing, unmapped = [], []
    for (header, owner), names in cpp_api().items():
        if (header, owner) not in TARGETS:
            unmapped.append((header, owner))
            continue
        target = TARGETS[(header, owner)]
        if target is None:
            continue
        have = _py_names(target)
        for name in sorted(names):
            if (owner, name) in KNOWN_GAPS:
                continue
            if name in CTOR_ARGS.get(owner, {}):
                continue
            py = ALIASES.get(owner, {}).get(name, name)
            if py is None:  # alias that lives on another object, checked in its own test
                continue
            if py not in have:
                missing.append(f"{header}: {owner or 'httpp'}::{name} (expected Python name: {py})")
    return missing, unmapped


def test_new_cpp_classes_must_be_added_to_TARGETS():
    _, unmapped = _missing()
    assert not unmapped, f"C++ owners with no entry in TARGETS (map them, or map to None with a reason): {unmapped}"


def test_every_cpp_public_name_has_a_python_counterpart():
    missing, _ = _missing()
    assert not missing, "C++ has it, Python does not (add the binding, or an entry with a reason):\n" + "\n".join(missing)


def test_known_gaps_are_still_gaps():
    api = {}
    for (header, owner), names in cpp_api().items():
        api.setdefault(owner, set()).update(names)
    for (owner, name), reason in KNOWN_GAPS.items():
        assert name in api.get(owner, set()), f"{owner}::{name} is no longer in the C++ headers: drop it from KNOWN_GAPS"
        target = {"server": httpp.Server, "": httpp}[owner]
        assert name not in _py_names(target), f"{owner}::{name} now exists in Python ({reason}): drop it from KNOWN_GAPS"


def test_aliases_point_at_real_python_names():
    for owner, aliases in ALIASES.items():
        target = next(t for (h, o), t in TARGETS.items() if o == owner and t is not None)
        for cpp, py in aliases.items():
            if py is not None:
                assert py in _py_names(target), f"alias {owner}::{cpp} -> {py} does not exist"
    assert "mount_swagger_ui" in _py_names(httpp.Server)


def test_cpp_setters_are_accepted_as_constructor_arguments():
    for owner, cls in (("client", httpp.Client), ("async_client", httpp.AsyncClient)):
        for setter, (keyword, value) in CTOR_ARGS[owner].items():
            try:
                cls("http://127.0.0.1:1", **{keyword: value})
            except TypeError as e:
                pytest.fail(f"{owner}::{setter} should be {cls.__name__}({keyword}=...): {e}")
            except Exception:
                pass  # any other error (such as an unreadable ca_file) means the keyword itself was accepted


def test_every_cpp_setter_is_covered_by_ctor_args_or_a_python_method():
    for (header, owner), names in cpp_api().items():
        if owner not in CTOR_ARGS:
            continue
        for name in names:
            if name.startswith("set_"):
                py = ALIASES.get(owner, {}).get(name, name)
                assert name in CTOR_ARGS[owner] or py in _py_names(TARGETS[(header, owner)]), f"{owner}::{name}"
