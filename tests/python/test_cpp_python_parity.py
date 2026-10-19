"""Every public C++ name (HTTPP_API in include/httpp/*.hpp) is either bound in Python or listed in NOT_EXPOSED
with the reason (ADR-0017: a binding is added only when it is necessary). It compares names, not behavior.
The Python binding may include only the public headers.

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
    ("sha256.hpp", ""): httpp.utils,  # the free functions live in httpp.utils, not in the main namespace
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

# Public C++ names that Python deliberately does not carry (ADR-0017 rule 3). An entry must still be unbound (a test
# checks), so adding the binding later forces the entry to go.
NOT_EXPOSED = {
    ("server", "route_io"): "not necessary: Python handlers need the GIL anyway, so add_loop_route is the fast path",
    ("server", "route_shared"): "not necessary: add_route already hands Python the request on a pool thread",
    ("", "find_swagger_ui_asset"): "internal lookup behind mount_swagger_ui, not meant for users",
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
            if (owner, name) in NOT_EXPOSED:
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
    assert not missing, "C++ has it, Python does not (bind it only if it is necessary, otherwise add it to NOT_EXPOSED with the reason):\n" + "\n".join(missing)


def test_not_exposed_names_are_still_unbound():
    api = {}
    for (header, owner), names in cpp_api().items():
        api.setdefault(owner, set()).update(names)
    for (owner, name), reason in NOT_EXPOSED.items():
        assert name in api.get(owner, set()), f"{owner}::{name} is no longer in the C++ headers: drop it from NOT_EXPOSED"
        target = {"server": httpp.Server, "": httpp}[owner]
        assert name not in _py_names(target), f"{owner}::{name} now exists in Python ({reason}): drop it from NOT_EXPOSED"


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


def test_the_binding_includes_only_public_headers():
    """ADR-0017 rule 2: the Cython sources may include only include/httpp/*.hpp from this project."""
    public = {f"httpp/{p.name}" for p in INCLUDE.iterdir()}
    src = INCLUDE.parents[1] / "src" / "bindings" / "python"
    seen = 0
    for pyx in src.glob("*.pyx"):
        text = pyx.read_text(encoding="utf-8")
        for target in re.findall(r'(?:#include|cdef extern from)\s+"([^"]+)"', text):
            seen += 1
            if target.startswith(("httpp/", "internal/")) or "/" in target:
                assert target in public, f"{pyx.name} includes {target}, which is not a public header"
    assert seen, "found no includes to check: the pattern is stale"
