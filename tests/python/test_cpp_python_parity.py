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
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"^[ \t]*#[^\n]*", "", text, flags=re.M)  # preprocessor lines (the export macros are defined this way) declare no API


def cpp_api():
    """{(header, owner): {names}} for every HTTPP_API or HTTPP_MEMBER declaration; owner '' = namespace-level function."""
    out = {}
    for path in sorted(INCLUDE.glob("*.hpp")):
        text = _strip_comments(path.read_text(encoding="utf-8"))
        spans = []
        for m in re.finditer(r"\b(?:struct|class)\s+(?:HTTPP_API\s+)?(\w+)\s*(?:final\s*)?(?::[^{;]*)?\{", text):
            depth, j = 1, m.end()
            while depth and j < len(text):
                depth += (text[j] == "{") - (text[j] == "}")
                j += 1
            spans.append((m.start(), j, m.group(1)))
        for m in re.finditer(r"(?:HTTPP_API|HTTPP_MEMBER)\s+([^;{]*?)\b(operator\s*\S+|~?\w+)\s*\(", text):
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
    ("client.hpp", "configurable"): None,  # the settings the three classes above share (ADR-0094); _missing() checks each name against all three
    ("client.hpp", "settings_ref"): None,  # what configurable calls; internal
    ("download.hpp", "download"): httpp.Download,
    ("progress.hpp", "bar"): httpp.progress.bar,
    ("server.hpp", "server"): httpp.Server,
    ("router.hpp", "router"): httpp.APIRouter,
    ("server.hpp", "directory"): httpp.Directory,
    ("server.hpp", "server_request"): httpp.ServerRequest,  # httpp::server_request: the handler's request object
    ("proxy.hpp", ""): httpp,  # proxy_from_environment and resolve_proxy (ADR-0067)
    ("url.hpp", ""): httpp.URL,  # the codec helpers and fetch_public_suffix_list are statics of URL (httpp::url is urlparser::url)
    ("banner.hpp", ""): None,  # the terminal banner of the httpp commands (ADR-0078); a Python program prints its own
    ("logging.hpp", ""): httpp,  # set_log_* live in httpp; the module functions (level_name, get_logger, ...) imitate Python's own `logging` (ADR-0057), see NOT_EXPOSED
    ("path.hpp", "path"): None,  # pathlib.Path is the Python side of httpp::path: every path argument takes one (ADR-0055)
    ("cookie.hpp", "cookie"): None,  # httpp.set_cookie() builds a Set-Cookie header and request.cookies reads the Cookie header; both call this class, so the rules live once
    ("sha256.hpp", ""): httpp.utils,  # the free functions live in httpp.utils, not in the main namespace
    ("server.hpp", ""): httpp.utils,  # mime_type_for, the same way
    ("swagger.hpp", ""): httpp,  # find_swagger_ui_asset only (NOT_EXPOSED)
    ("swagger.hpp", "swagger"): httpp.Swagger,
    ("openapi.hpp", "document"): httpp.openapi.Document,
    ("openapi.hpp", ""): httpp.security,  # basic, bearer and api_key are httpp.security's factories (they return dicts)
    ("file_browser.hpp", "file_browser"): httpp.FileBrowser,
    ("file_lock.hpp", "file_lock"): httpp.FileLock,
    ("middleware.hpp", "cors"): httpp.middleware.CORSMiddleware,
    ("middleware.hpp", "trusted_host"): httpp.middleware.TrustedHostMiddleware,
    ("middleware.hpp", "https_redirect"): httpp.middleware.HTTPSRedirectMiddleware,
    ("middleware.hpp", "forwarded_headers"): httpp.middleware.ProxyHeadersMiddleware,
    ("middleware.hpp", "basic_auth"): httpp.middleware.BasicAuthMiddleware,
    ("middleware.hpp", "rate_limit"): httpp.middleware.RateLimitMiddleware,
    ("server.hpp", "server_stopper"): None,  # what a C++ handler captures to stop its server without a cycle (ADR-0081); Python's Server already stops on a signal and owns its handlers
    ("server.hpp", "responder"): None,  # handed to route_async handlers; Python handlers just return or await
    ("websocket.hpp", "websocket"): None,  # Python's WebSocket is its own awaitable API over a mailbox (ADR-0027), not this handle
    ("websocket.hpp", "ws_acceptor"): None,  # Python's handler accepts through `await websocket.accept()`
}

# The classes that derive from configurable<Self> (ADR-0094): its names must exist on each of them.
CONFIGURABLE_USERS = (("client", httpp.Client), ("async_client", httpp.AsyncClient), ("request", httpp.Request),
                      ("download", httpp.Download))

# C++ name -> Python name, per owner, for names that differ on purpose.
ALIASES = {
    "client": {"del": "delete"},  # a keyword in Python; `delete` is one in C++ (ADR-0100)
    "async_client": {"del": "delete"},
    "server": {
        "del": "delete",
        "use": "add_middleware",  # the C++ use() takes raw hooks; Python installs the ready-made classes of httpp.middleware
        # an `async def` handler goes through the ordinary decorator in Python
        "route_async": "route", "get_async": "get", "post_async": "post", "put_async": "put",
        "patch_async": "patch", "del_async": "delete",
    },
    "router": {
        "del": "delete",
        "route_async": "route", "get_async": "get", "post_async": "post", "put_async": "put",
        "patch_async": "patch", "del_async": "delete",
    },
}

# C++ setters that are constructor arguments in Python: name -> keyword, with a value that must be accepted.
CTOR_ARGS = {
    "client": {
        "timeout": ("timeout", 5), "max_redirects": ("max_redirects", 3), "connect_timeout": ("connect_timeout", 2),
        "max_time": ("max_time", 9), "auto_compression": ("auto_compression", False),
        "basic_auth": ("basic_auth", ("u", "p")), "bearer": ("bearer", "t"),
        "verify": ("verify", False), "ca_file": ("ca_file", "ca.pem"),
        "proxy": ("proxy", "http://127.0.0.1:3128"), "keep_alive": ("keep_alive", False),
    },
    "async_client": {
        "timeout": ("timeout", 5), "max_redirects": ("max_redirects", 3), "connect_timeout": ("connect_timeout", 2),
        "max_time": ("max_time", 9), "auto_compression": ("auto_compression", False),
        "basic_auth": ("basic_auth", ("u", "p")), "bearer": ("bearer", "t"),
        "verify": ("verify", False), "ca_file": ("ca_file", "ca.pem"),
        "proxy": ("proxy", "http://127.0.0.1:3128"), "header": ("headers", {"X": "1"}),
    },
}

CTOR_ARGS["cors"] = {
    "allow_origins": ("allow_origins", ["https://a.example"]), "allow_origin_regex": ("allow_origin_regex", "https://.*"),
    "allow_methods": ("allow_methods", ["GET"]), "allow_headers": ("allow_headers", ["X-A"]),
    "allow_credentials": ("allow_credentials", True), "expose_headers": ("expose_headers", ["X-B"]),
    "max_age": ("max_age", 5),
}
CTOR_ARGS["trusted_host"] = {"allowed_hosts": ("allowed_hosts", ["example.com"]), "www_redirect": ("www_redirect", False)}
CTOR_ARGS["forwarded_headers"] = {"trusted_proxies": ("trusted_proxies", ["10.0.0.0/8"]), "trust_forwarded_host": ("trust_forwarded_host", True)}
CTOR_ARGS["https_redirect"] = {"trust_forwarded_proto": ("trust_forwarded_proto", True)}
CTOR_ARGS["basic_auth"] = {"realm": ("realm", "Tools"), "users": ("users", {"u": "p"}), "path_prefix": ("path_prefix", "/admin"), "verifier": ("verifier", lambda user, password: True)}
CTOR_ARGS["rate_limit"] = {"rate": ("requests_per_second", 5.0), "burst": ("burst", 3), "max_clients": ("max_clients", 10)}

# Public C++ names that Python deliberately does not carry (ADR-0017 rule 3). An entry must still be unbound (a test
# checks), so adding the binding later forces the entry to go.
NOT_EXPOSED = {
    ("request", "on_body"): "temporary (ADR-0084): the Python binding of request streaming is the next step; remove this line and bind it then",
    ("request", "on_head"): "temporary (ADR-0084): same as on_body",
    ("request", "on_retry"): "temporary (ADR-0099): the Python binding of retry is the next step; remove this line and bind it then",
    ("request", "save_to"): "temporary (ADR-0102): the Python binding of save_to and its options is the next step; remove this line and bind it then",
    ("request", "save_in"): "temporary (ADR-0102): the Python binding of save_to and its options is the next step; remove this line and bind it then",
    ("request", "create_dirs"): "temporary (ADR-0102): the Python binding of save_to and its options is the next step; remove this line and bind it then",
    ("request", "overwrite"): "temporary (ADR-0102): the Python binding of save_to and its options is the next step; remove this line and bind it then",
    ("request", "include_head"): "temporary (ADR-0102): the Python binding of save_to and its options is the next step; remove this line and bind it then",
    ("client", "retry"): "temporary (ADR-0099): same as request::on_retry",
    ("client", "retry_delay"): "temporary (ADR-0099): same as request::on_retry",
    ("client", "retry_connrefused"): "temporary (ADR-0099): same as request::on_retry",
    ("async_client", "retry"): "temporary (ADR-0099): same as request::on_retry",
    ("async_client", "retry_delay"): "temporary (ADR-0099): same as request::on_retry",
    ("async_client", "retry_connrefused"): "temporary (ADR-0099): same as request::on_retry",
    ("request", "retry"): "temporary (ADR-0099): same as request::on_retry",
    ("request", "retry_delay"): "temporary (ADR-0099): same as request::on_retry",
    ("request", "retry_connrefused"): "temporary (ADR-0099): same as request::on_retry",
    ("download", "auto_compression"): "deleted on a download (ADR-0101): it always asks for identity coding",
    ("download", "retry"): "temporary (ADR-0104): C++ only until the Python binding of retry (ADR-0099) is done",
    ("download", "retry_delay"): "temporary (ADR-0104): same as download::retry",
    ("download", "retry_connrefused"): "temporary (ADR-0104): same as download::retry",
    ("download", "settings_"): "internal hook of configurable (ADR-0094), not meant for users",
    ("client", "settings_"): "internal hook of configurable (ADR-0094), not meant for users",
    ("async_client", "settings_"): "same as client::settings_",
    ("request", "settings_"): "same as client::settings_",
    ("server", "bind"): "not necessary: Server.run binds and serves in one call; bind_to_any_port covers the ephemeral-port case (ADR-0078)",
    ("server", "url"): "not necessary: Python knows the host and the port it passed to Server.run (ADR-0078)",
    ("server", "listen_until_signal"): "not necessary: Server.run already serves until a signal (ADR-0078)",
    ("server", "same_as"): "not necessary: a Python Server is its own object, `is` compares it; the shared handle is a C++ idea (ADR-0081)",
    ("server", "stopper"): "not necessary: Python handlers hold the Server (Python's garbage collector breaks the cycle) and Server.run stops on a signal (ADR-0081)",
    ("server", "route_io"): "not necessary: Python handlers need the GIL anyway, so add_loop_route is the fast path",
    ("server", "route_shared"): "not necessary: add_route already hands Python the request on a pool thread",
    ("server", "route_ws"): "Python registers WebSocket routes with the Server.websocket decorator (ADR-0027)",
    ("router", "route_ws"): "Python registers WebSocket routes with the APIRouter.websocket decorator (ADR-0048)",
    ("router", "route_io"): "same as server::route_io",
    ("router", "route_shared"): "same as server::route_shared",
    ("", "raw_scheme"): "not necessary: a Python scheme is already a dict (or JSON text) that Document.security_scheme takes as it is (ADR-0087)",
    ("", "find_swagger_ui_asset"): "internal lookup behind swagger::mount, not meant for users",
    ("", "level_name"): "the logging module imitates Python's own `logging`, which is the Python side (ADR-0057)",
    ("", "level_from_string"): "same: Python's logging.getLevelName",
    ("", "get_logger"): "same: logging.getLogger; the \"httpp\" logger is bridged to it (httpp/_log.py)",
    ("", "basic_config"): "same: logging.basicConfig",
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
        if owner == "configurable":  # one declaration, three classes
            for user, user_target in CONFIGURABLE_USERS:
                have = _py_names(user_target)
                for name in sorted(names):
                    if (user, name) in NOT_EXPOSED or name in CTOR_ARGS.get(user, {}):
                        continue
                    py = ALIASES.get(user, {}).get(name, name)
                    if py is not None and py not in have:
                        missing.append(f"{header}: {user}::{name} (from configurable; expected Python name: {py})")
            continue
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
        target = {"server": httpp.Server, "router": httpp.APIRouter, "request": httpp.Request, "client": httpp.Client,
                  "async_client": httpp.AsyncClient, "": httpp, "document": httpp.openapi.Document}[owner]
        assert name not in _py_names(target), f"{owner}::{name} now exists in Python ({reason}): drop it from NOT_EXPOSED"


def test_aliases_point_at_real_python_names():
    for owner, aliases in ALIASES.items():
        target = next(t for (h, o), t in TARGETS.items() if o == owner and t is not None)
        for cpp, py in aliases.items():
            if py is not None:
                assert py in _py_names(target), f"alias {owner}::{cpp} -> {py} does not exist"


def test_cpp_setters_are_accepted_as_constructor_arguments():
    for owner, cls in (("client", httpp.Client), ("async_client", httpp.AsyncClient),
                       ("cors", httpp.middleware.CORSMiddleware), ("trusted_host", httpp.middleware.TrustedHostMiddleware),
                       ("https_redirect", httpp.middleware.HTTPSRedirectMiddleware),
                       ("forwarded_headers", httpp.middleware.ProxyHeadersMiddleware),
                       ("basic_auth", httpp.middleware.BasicAuthMiddleware), ("rate_limit", httpp.middleware.RateLimitMiddleware)):
        for setter, (keyword, value) in CTOR_ARGS[owner].items():
            try:
                args = () if owner in ("cors", "trusted_host", "https_redirect", "forwarded_headers", "basic_auth", "rate_limit") else ("http://127.0.0.1:1",)
                cls(*args, **{keyword: value})
            except TypeError as e:
                pytest.fail(f"{owner}::{setter} should be {cls.__name__}({keyword}=...): {e}")
            except Exception:
                pass  # any other error (such as an unreadable ca_file) means the keyword itself was accepted


# Python's constructors take the settings as keywords with the names of the setters (ADR-0103). The only differences:
# the setters that are called once per item (header, param, cookie) take a dict under the plural name, and
# basic_auth takes (user, password).
PLURAL = {"header": "headers", "param": "params", "cookie": "cookies"}

# keyword -> a value it must accept, for the Python classes whose settings are also methods
KWARGS = {
    "Request": {
        "method": "PUT", "params": {"a": "1"}, "headers": {"X": "1"}, "data": "x", "json": {"a": 1}, "form": {"a": "1"},
        "content_type": "text/plain", "cookies": {"a": "b"}, "timeout": 5, "max_redirects": 3, "auto_compression": False,
        "connect_timeout": 2, "max_time": 9, "basic_auth": ("u", "p"), "bearer": "t", "verify": False, "ca_file": "ca.pem",
        "proxy": "http://127.0.0.1:3128", "max_response_size": 100,
    },
    "Download": {
        "enable_progress": False, "resume": True, "force": True, "sha256": "0" * 64, "sha256_url": "http://127.0.0.1:1/s",
        "max_wait": 1, "timeout": 5, "max_redirects": 3, "connect_timeout": 2, "max_time": 9, "basic_auth": ("u", "p"),
        "bearer": "t", "verify": False, "ca_file": "ca.pem", "proxy": "http://127.0.0.1:3128",
    },
}


def test_constructor_keywords_of_client_and_async_client_are_setter_names():
    for owner in ("client", "async_client"):
        for setter, (keyword, _value) in CTOR_ARGS[owner].items():
            assert keyword == PLURAL.get(setter, setter), f"{owner}::{setter} is passed as {keyword}=..., not {setter}=..."


def test_request_and_download_take_every_setting_as_a_keyword_with_the_name_of_its_setter():
    for cls in (httpp.Request, httpp.Download):
        methods = _py_names(cls)
        for keyword, value in KWARGS[cls.__name__].items():
            setter = {v: k for k, v in PLURAL.items()}.get(keyword, keyword)
            assert setter in methods, f"{cls.__name__}({keyword}=...) is not the name of a setter"
            try:
                cls("http://127.0.0.1:1/x", **{keyword: value})
            except TypeError as e:
                pytest.fail(f"{cls.__name__}({keyword}=...) is not accepted: {e}")
            except Exception:
                pass  # any other error (such as an unreadable ca_file) means the keyword itself was accepted
    with pytest.raises(ValueError):
        httpp.Request("http://127.0.0.1:1/x", json={"a": 1}, data="x")  # the three body keywords exclude each other


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
    for pyx in [*src.glob("*.pyx"), *src.glob("cy/*.pxi")]:
        text = pyx.read_text(encoding="utf-8")
        for target in re.findall(r'(?:#include|cdef extern from)\s+"([^"]+)"', text):
            seen += 1
            if target.startswith(("httpp/", "internal/")) or "/" in target:
                assert target in public, f"{pyx.name} includes {target}, which is not a public header"
    assert seen, "found no includes to check: the pattern is stale"
