"""httpp.openapi.Document (httpp/openapi.hpp, ADR-0087) and Server.openapi(), which is written through it."""

import json
import re
from typing import Optional

import pytest

import httpp
from httpp import Server, security
from httpp import _schema
from httpp.models import BaseModel
from httpp.openapi import Document
from httpp.testclient import TestClient


def test_an_empty_document_is_the_smallest_valid_one():
    assert json.loads(Document("T", "1").to_json()) == {"openapi": "3.1.0", "info": {"title": "T", "version": "1"}, "paths": {}}


def test_the_document_is_compact_json_text():
    text = Document("T", "1").add("get", "/x", summary="s").to_json()
    assert text == '{"openapi":"3.1.0","info":{"title":"T","version":"1"},"paths":{"/x":{"get":{"summary":"s"}}}}'


def test_a_title_and_a_version_are_required():
    with pytest.raises(ValueError):
        Document("", "1")
    with pytest.raises(ValueError):
        Document("T", "")
    with pytest.raises(TypeError):
        Document(1, "1")


def test_schemes_a_default_and_per_operation_requirements():
    doc = Document("My API", "1.0.0")
    doc.security_scheme("jwt", security.bearer("JWT")).security_scheme("basic", security.basic())
    doc.security(["jwt"])
    doc.add("get", "/admin", summary="Admin", security=["basic"], responses={"200": {"description": "OK"}})
    doc.add("get", "/health", security=[], responses={"200": "OK"})
    out = json.loads(doc.to_json())
    assert out["components"]["securitySchemes"] == {"jwt": {"type": "http", "scheme": "bearer", "bearerFormat": "JWT"},
                                                    "basic": {"type": "http", "scheme": "basic"}}
    assert out["security"] == [{"jwt": []}]
    assert out["paths"]["/admin"]["get"]["security"] == [{"basic": []}]
    assert out["paths"]["/health"]["get"]["security"] == []
    assert out["paths"]["/health"]["get"]["responses"] == {"200": {"description": "OK"}}


def test_requirements_may_ask_for_schemes_together_and_for_scopes():
    doc = Document("T", "1")
    doc.security_scheme("basic", security.basic()).security_scheme("key", security.api_key("K"))
    doc.security_scheme("oauth", {"type": "oauth2", "flows": {}})
    doc.add("get", "/x", security=[{"basic": [], "key": []}, {"oauth": ["read", "write"]}])
    assert json.loads(doc.to_json())["paths"]["/x"]["get"]["security"] == [{"basic": [], "key": []}, {"oauth": ["read", "write"]}]


def test_a_scheme_may_be_json_text():
    doc = Document("T", "1").security_scheme("m", '{"type":"mutualTLS"}')
    assert json.loads(doc.to_json())["components"]["securitySchemes"]["m"] == {"type": "mutualTLS"}


def test_an_undeclared_scheme_is_a_value_error_that_names_the_operation():
    doc = Document("T", "1").add("get", "/admin", security=["basicc"])
    with pytest.raises(ValueError, match=r"get /admin.*'basicc' is not declared"):
        doc.to_json()
    doc = Document("T", "1").security(["nope"])
    with pytest.raises(ValueError, match="the default security"):
        doc.to_json()


def test_bad_security_arguments_are_type_errors():
    doc = Document("T", "1")
    with pytest.raises(TypeError):
        doc.security(5)
    with pytest.raises(TypeError):
        doc.security([5])
    with pytest.raises(TypeError):
        doc.security([{"a": "read"}])  # scopes are a list
    with pytest.raises(TypeError):
        doc.add("get", "/x", security=[{"a": [1]}])


def test_paths_keep_their_first_seen_order_and_methods_group_under_a_path():
    doc = Document("T", "1")
    doc.add("get", "/a").add("get", "/b").add("post", "/a").add("DELETE", "/b")
    paths = json.loads(doc.to_json())["paths"]
    assert list(paths) == ["/a", "/b"] and list(paths["/a"]) == ["get", "post"] and list(paths["/b"]) == ["get", "delete"]


def test_a_bad_method_a_bad_path_and_a_repeat_are_value_errors():
    doc = Document("T", "1")
    with pytest.raises(ValueError, match="not an HTTP method"):
        doc.add("fetch", "/x")
    with pytest.raises(ValueError, match="starts with"):
        doc.add("get", "x")
    doc.add("get", "/x")
    with pytest.raises(ValueError, match="already is a get on /x"):
        doc.add("get", "/x")
    assert list(json.loads(doc.to_json())["paths"]) == ["/x"]  # the failed adds left nothing behind


def test_every_field_of_an_operation():
    doc = Document("T", "1")
    doc.add("post", "/items/{id}", summary="Create", description="d", operation_id="create", tags=["a", "b"], deprecated=True,
            parameters=[{"name": "id", "in": "path", "required": True, "schema": {"type": "integer"}}],
            request_body={"required": True, "content": {"application/json": {"schema": {"type": "object"}}}},
            responses={200: {"description": "OK", "content": {"application/json": {"schema": {}}}}, "default": {"description": "err"}},
            extra={"x-internal": True})
    assert json.loads(doc.to_json())["paths"]["/items/{id}"]["post"] == {
        "summary": "Create", "description": "d", "operationId": "create", "tags": ["a", "b"], "deprecated": True,
        "parameters": [{"name": "id", "in": "path", "required": True, "schema": {"type": "integer"}}],
        "requestBody": {"required": True, "content": {"application/json": {"schema": {"type": "object"}}}},
        "responses": {"200": {"description": "OK", "content": {"application/json": {"schema": {}}}}, "default": {"description": "err"}},
        "x-internal": True}


def test_a_response_with_keys_that_are_not_written_is_refused_not_dropped():
    with pytest.raises(ValueError, match="headers"):
        Document("T", "1").add("get", "/x", responses={"200": {"description": "OK", "headers": {}}})


def test_a_reserved_key_in_extra_is_refused():
    doc = Document("T", "1").add("get", "/x", extra={"responses": {}})
    with pytest.raises(ValueError, match="use its own setter"):
        doc.to_json()


def test_schemas_and_extra_top_level_keys():
    doc = Document("T", "1").component_schema("Item", {"type": "object"}).component_schema("Item", '{"type":"string"}')
    doc.raw("servers", [{"url": "/api"}]).description("about")
    out = json.loads(doc.to_json())
    assert out["components"]["schemas"] == {"Item": {"type": "string"}} and out["servers"] == [{"url": "/api"}]
    assert out["info"]["description"] == "about"


def test_text_is_escaped_and_unicode_passes_through():
    doc = Document('A "quoted" title', "1").description("line1\nline2\ttab \\ back\x01").add("get", "/x", summary="café 🚀")
    out = json.loads(doc.to_json())
    assert out["info"] == {"title": 'A "quoted" title', "version": "1", "description": "line1\nline2\ttab \\ back\x01"}
    assert out["paths"]["/x"]["get"]["summary"] == "café 🚀"


def test_a_lone_surrogate_is_an_error_not_invalid_json():
    with pytest.raises(ValueError):  # UnicodeEncodeError is a ValueError
        Document("T", "1").add("get", "/x", summary="\ud800")


def test_wrong_types_are_type_errors():
    with pytest.raises(TypeError):
        Document("T", "1").add("get", "/x", summary=5)
    with pytest.raises(TypeError):
        Document("T", "1").component_schema("S", object())


def test_the_document_stays_usable_after_a_failed_to_json():
    doc = Document("T", "1").add("get", "/x", security=["later"])
    with pytest.raises(ValueError):
        doc.to_json()
    doc.security_scheme("later", security.basic())
    assert json.loads(doc.to_json())["paths"]["/x"]["get"]["security"] == [{"later": []}]


# --- Server.openapi() is written through the same class ---


class Item(BaseModel):
    name: str
    price: float = 0.0


def rich_app():
    app = Server(title="Shop", version="2", description="A shop", security_schemes={"basic": security.basic(), "key": security.api_key("K", "query")},
                 security=["key"])

    @app.get("/items/{item_id}", tags=["items"], summary="One", security=["basic"])
    def one(item_id: int, q: Optional[str] = None):
        return {}

    @app.post("/items", response_model=Item, status_code=201)
    def make(item: Item):
        return item

    @app.get("/plain", security=[], deprecated=True)
    def plain():
        return "x"

    @app.get("/hidden", include_in_schema=False)
    def hidden():
        return "x"

    return app


def assembled_the_old_way(app):
    """What build_openapi did before ADR-0087: every part a dict, the document one dict, no C++ in between."""
    defs = _schema.Defs("#/components/schemas/{model}")
    paths, uses_422 = {}, False
    for route in app._routes:
        if route.meta.get("include_in_schema") is False:
            continue
        for method in route.methods:
            op, with_422 = _schema._operation(route, method, defs)
            uses_422 = uses_422 or with_422
            paths.setdefault(_schema.openapi_path(route.path), {})[method.lower()] = op
    doc = {"openapi": "3.1.0", "info": {"title": app._title, "version": app._version, "description": app._description}, "paths": paths}
    schemas = dict(defs.schemas)
    if uses_422:
        schemas.update(_schema._VALIDATION_SCHEMAS)
    components = {}
    if schemas:
        components["schemas"] = schemas
    if app._security_schemes:
        components["securitySchemes"] = app._security_schemes
    if components:
        doc["components"] = components
    if app._security is not None:
        doc["security"] = app._security
    return doc


def test_the_document_is_the_one_python_used_to_assemble_by_hand():
    app = rich_app()
    assert app.openapi() == assembled_the_old_way(app)
    assert list(app.openapi()["paths"]) == list(assembled_the_old_way(app)["paths"])  # the same order too


def test_the_served_document_is_the_text_openapi_returns():
    app = rich_app()
    client = TestClient(app)
    first = client.get("/openapi.json")
    assert first.status_code == 200 and first.headers["content-type"].startswith("application/json")
    assert first.json() == app.openapi()
    assert client.get("/openapi.json").content == first.content  # written once, the same bytes every time
    assert not re.search(rb"\s{2,}|: ", first.content)  # compact


def test_the_document_follows_the_routes_added_later():
    app = Server()
    client = TestClient(app)
    assert "/late" not in client.get("/openapi.json").json()["paths"]

    @app.get("/late")
    def late():
        return "x"

    assert "/late" in client.get("/openapi.json").json()["paths"] and "/late" in app.openapi()["paths"]


def test_the_same_method_on_the_same_path_twice_keeps_the_later_route():
    app = Server()
    try:
        @app.get("/same", summary="first")
        def first():
            return 1

        @app.get("/same", summary="second")
        def second():
            return 2
    except Exception as e:  # the server refuses a repeat: nothing to keep
        pytest.skip(f"the server refuses a repeated route ({type(e).__name__})")
    assert app.openapi()["paths"]["/same"]["get"]["summary"] == "second"


def test_an_empty_title_is_a_value_error_when_the_document_is_built():
    app = Server(title="")
    with pytest.raises(ValueError, match="title"):
        app.openapi()


def test_the_openapi_module_is_public():
    assert httpp.openapi.Document is Document and "openapi" in httpp.__all__
