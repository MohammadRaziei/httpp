"""OpenAPI security schemes (httpp/security.py): documentation for the Swagger "Authorize" button."""

import pytest

from httpp import APIRouter, Server, security


def make(**kw):
    return Server(**kw)


def test_the_factories_write_openapi_scheme_objects():
    assert security.basic() == {"type": "http", "scheme": "basic"}
    assert security.bearer("JWT", "a token") == {"type": "http", "scheme": "bearer", "bearerFormat": "JWT", "description": "a token"}
    assert security.bearer() == {"type": "http", "scheme": "bearer"}
    assert security.api_key("X-Key") == {"type": "apiKey", "name": "X-Key", "in": "header"}
    assert security.api_key("k", "query")["in"] == "query"
    assert security.api_key("sid", "cookie")["in"] == "cookie"


def test_the_factories_refuse_bad_arguments():
    with pytest.raises(ValueError):
        security.api_key("k", "body")
    with pytest.raises(TypeError):
        security.api_key("")
    with pytest.raises(TypeError):
        security.bearer(5)
    with pytest.raises(TypeError):
        security.basic(description=1)


def test_a_document_without_security_has_no_security_keys():
    app = make()

    @app.get("/x")
    def x():
        return "x"

    doc = app.openapi()
    assert "security" not in doc and "securitySchemes" not in doc.get("components", {}) and "security" not in doc["paths"]["/x"]["get"]


def test_schemes_the_default_and_a_per_route_requirement():
    app = make(security_schemes={"basic": security.basic(), "jwt": security.bearer("JWT")}, security=["jwt"])

    @app.get("/admin", security=["basic"])
    def admin():
        return "a"

    @app.get("/health", security=[])
    def health():
        return "ok"

    @app.get("/data")
    def data():
        return "d"

    doc = app.openapi()
    assert doc["components"]["securitySchemes"] == {"basic": {"type": "http", "scheme": "basic"}, "jwt": {"type": "http", "scheme": "bearer", "bearerFormat": "JWT"}}
    assert doc["security"] == [{"jwt": []}]
    assert doc["paths"]["/admin"]["get"]["security"] == [{"basic": []}]
    assert doc["paths"]["/health"]["get"]["security"] == []  # public: overrides the default
    assert "security" not in doc["paths"]["/data"]["get"]    # inherits the document's


def test_requirement_forms_alternatives_together_and_scopes():
    app = make(security_schemes={"basic": security.basic(), "key": security.api_key("K"), "oauth": {"type": "oauth2", "flows": {}}})

    @app.get("/a", security=["basic", "key"])  # either
    def a():
        return "a"

    @app.get("/b", security=[{"basic": [], "key": []}])  # both
    def b():
        return "b"

    @app.get("/c", security="basic")  # a bare name
    def c():
        return "c"

    @app.get("/d", security=[{"oauth": ["read", "write"]}])
    def d():
        return "d"

    paths = app.openapi()["paths"]
    assert paths["/a"]["get"]["security"] == [{"basic": []}, {"key": []}]
    assert paths["/b"]["get"]["security"] == [{"basic": [], "key": []}]
    assert paths["/c"]["get"]["security"] == [{"basic": []}]
    assert paths["/d"]["get"]["security"] == [{"oauth": ["read", "write"]}]


def test_an_unknown_scheme_fails_where_it_is_written():
    app = make(security_schemes={"basic": security.basic()})
    with pytest.raises(ValueError, match="unknown security scheme 'bsic'"):
        app.get("/x", security=["bsic"])(lambda: "x")
    with pytest.raises(ValueError, match="unknown security scheme"):
        make(security=["nothing"])
    with pytest.raises(TypeError):
        app.get("/y", security=[5])(lambda: "y")
    with pytest.raises(TypeError):
        app.get("/z", security=[{"basic": "read"}])(lambda: "z")
    with pytest.raises(ValueError):
        make(security_schemes={"x": {"type": "nope"}})
    with pytest.raises(TypeError):
        make(security_schemes=["basic"])


def test_a_router_default_applies_to_its_routes_and_a_route_can_override_it():
    app = make(security_schemes={"basic": security.basic(), "jwt": security.bearer()})
    router = APIRouter("/admin", security=["basic"])

    @router.get("/users")
    def users():
        return "u"

    @router.get("/ping", security=[])
    def ping():
        return "p"

    @router.get("/token", security=["jwt"])
    def token():
        return "t"

    app.include(router)
    paths = app.openapi()["paths"]
    assert paths["/admin/users"]["get"]["security"] == [{"basic": []}]
    assert paths["/admin/ping"]["get"]["security"] == []
    assert paths["/admin/token"]["get"]["security"] == [{"jwt": []}]


def test_a_router_with_a_name_the_server_did_not_declare_fails_at_include():
    app = make(security_schemes={"basic": security.basic()})
    router = APIRouter(security=["jwt"])

    @router.get("/x")
    def x():
        return "x"

    with pytest.raises(ValueError, match="unknown security scheme 'jwt'"):
        app.include(router)


def test_the_served_document_carries_the_schemes():
    from httpp.testclient import TestClient

    app = make(security_schemes={"basic": security.basic()}, security=["basic"])
    body = TestClient(app).get("/openapi.json").json()
    assert body["components"]["securitySchemes"]["basic"]["scheme"] == "basic" and body["security"] == [{"basic": []}]
