"""/openapi.json, /docs, model_json_schema(), and the query / body parameters the OpenAPI document describes."""

import datetime
import decimal
import enum
import json
import os
import threading
import time
import uuid
from contextlib import contextmanager
from typing import Annotated, Dict, List, Literal, Optional, Set, Tuple, Union

import pytest

import httpp
from httpp import Server
from httpp.models import BaseModel, Field


@contextmanager
def running(app):
    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        yield f"http://127.0.0.1:{port}"
    finally:
        app.stop()
        th.join()


def get(url):
    return httpp.get(url)


def post(url, data):
    return httpp.post(url, json=data)


# --- model_json_schema against Pydantic -----------------------------------------------------------

class Color(enum.Enum):
    red = "red"
    blue = "blue"


def define(lib):
    BaseModel, Field = lib.BaseModel, lib.Field

    class Address(BaseModel):
        street: str
        zip: str = Field(pattern=r"^\d{5}$")

    class User(BaseModel):
        id: int
        name: str = Field(min_length=2, max_length=10, description="the name", examples=["Ada"])
        email: Optional[str] = None
        age: int = Field(default=18, ge=0, le=150)
        tags: List[str] = []
        address: Optional[Address] = None
        color: Color = Color.red
        score: float = Field(default=0.0, gt=-1, lt=100.5, multiple_of=0.5)
        joined: Optional[datetime.date] = None
        when: datetime.datetime = None
        uid: uuid.UUID = None
        kind: Literal["a", "b"] = "a"
        one: Literal[1] = 1
        pair: Tuple[int, str] = (1, "x")
        seen: Set[int] = set()
        mapping: Dict[str, float] = {}
        either: Union[int, str] = 1
        amount: decimal.Decimal = decimal.Decimal(1)
        blob: bytes = b""
        ann: Annotated[int, Field(ge=3)] = 3
        many: List[Address] = Field(default_factory=list, min_length=1, max_length=4)

    return User


def test_model_json_schema_matches_pydantic():
    pydantic = pytest.importorskip("pydantic")
    ours = json.loads(json.dumps(define(__import__("httpp.models", fromlist=["x"])).model_json_schema()))
    theirs = json.loads(json.dumps(define(pydantic).model_json_schema()))
    assert ours == theirs


def test_model_json_schema_of_a_model_that_refers_to_itself():
    class Node(BaseModel):
        value: int
        children: List["Node"] = []

    schema = Node.model_json_schema()
    assert schema["$ref"] == "#/$defs/Node"
    assert schema["$defs"]["Node"]["properties"]["children"]["items"] == {"$ref": "#/$defs/Node"}


def test_forbid_and_alias_and_exclude():
    class M(BaseModel):
        model_config = {"extra": "forbid"}
        a_b: int = Field(alias="aB")
        hidden: int = Field(default=0, exclude=True)

    s = M.model_json_schema()
    assert s["additionalProperties"] is False and list(s["properties"]) == ["aB"] and s["required"] == ["aB"]


# --- the document ----------------------------------------------------------------------------------

class Item(BaseModel):
    name: str = Field(min_length=1)
    price: float = Field(gt=0)


def make_app():
    app = Server(title="Shop", version="2.0", description="A shop")

    @app.get("/items/{item_id}", tags=["items"], summary="Read one")
    def read_item(item_id: int, q: Optional[str] = None, tags: List[str] = [], limit: int = 10):
        """Return one item."""
        return {"item_id": item_id, "q": q, "tags": tags, "limit": limit}

    @app.post("/items", response_model=Item, status_code=201)
    def create_item(item: Item):
        return item

    @app.get("/plain", include_in_schema=False)
    def plain():
        return "x"

    @app.route("/multi/<name>", methods=("GET", "DELETE"), deprecated=True)
    def multi(name):
        return name

    return app


def test_openapi_document_describes_the_routes():
    doc = make_app().openapi()
    assert doc["openapi"] == "3.1.0"
    assert doc["info"] == {"title": "Shop", "version": "2.0", "description": "A shop"}
    assert set(doc["paths"]) == {"/items/{item_id}", "/items", "/multi/{name}"}  # /plain, /docs, /openapi.json are hidden

    read = doc["paths"]["/items/{item_id}"]["get"]
    assert read["summary"] == "Read one" and read["tags"] == ["items"] and read["description"] == "Return one item."
    assert read["operationId"] == "read_item_items__item_id__get"
    params = {p["name"]: p for p in read["parameters"]}
    assert params["item_id"] == {"name": "item_id", "in": "path", "required": True, "schema": {"type": "integer"}}
    assert params["q"]["required"] is False and params["q"]["schema"] == {"anyOf": [{"type": "string"}, {"type": "null"}], "default": None}
    assert params["tags"]["schema"] == {"type": "array", "items": {"type": "string"}, "default": []}
    assert params["limit"]["schema"] == {"type": "integer", "default": 10}
    assert read["responses"]["422"]["content"]["application/json"]["schema"] == {"$ref": "#/components/schemas/HTTPValidationError"}

    create = doc["paths"]["/items"]["post"]
    assert create["requestBody"] == {"required": True, "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Item"}}}}
    assert create["responses"]["201"]["content"]["application/json"]["schema"] == {"$ref": "#/components/schemas/Item"}

    multi = doc["paths"]["/multi/{name}"]
    assert set(multi) == {"get", "delete"} and multi["get"]["deprecated"] is True
    assert multi["get"]["parameters"][0]["schema"] == {"type": "string"}  # unannotated path parameter

    assert doc["components"]["schemas"]["Item"]["required"] == ["name", "price"]
    assert {"ValidationError", "HTTPValidationError"} <= set(doc["components"]["schemas"])


def test_openapi_document_is_valid_openapi():
    validator = pytest.importorskip("openapi_spec_validator")
    validator.validate(make_app().openapi())


def test_openapi_is_rebuilt_when_a_route_is_added():
    app = Server()
    assert "/late" not in app.openapi()["paths"]
    app.get("/late")(lambda: "x")
    assert "/late" in app.openapi()["paths"]


def test_colon_path_params_are_described_too():
    app = Server()

    @app.get("/n/:i")
    def n(i: int):
        return str(i)

    assert "/n/{i}" in app.openapi()["paths"]


# --- the server really does what the document says -------------------------------------------------

def test_query_and_path_parameters_are_converted_and_defaulted():
    with running(make_app()) as url:
        r = get(f"{url}/items/7?q=hi&tags=a&tags=b&limit=3")
        assert r.status == 200 and json.loads(r.text) == {"item_id": 7, "q": "hi", "tags": ["a", "b"], "limit": 3}
        r = get(f"{url}/items/7")
        assert json.loads(r.text) == {"item_id": 7, "q": None, "tags": [], "limit": 10}


def test_bad_query_value_answers_422_with_detail():
    with running(make_app()) as url:
        r = get(f"{url}/items/7?limit=abc")
        assert r.status == 422
        (err,) = json.loads(r.text)["detail"]
        assert err["type"] == "int_parsing" and err["loc"] == ["query", "limit"] and err["input"] == "abc"


def test_missing_required_query_parameter():
    app = Server()

    @app.get("/s")
    def s(term: str):
        return term

    with running(app) as url:
        r = get(f"{url}/s")
        assert r.status == 422
        assert json.loads(r.text)["detail"] == [{"type": "missing", "loc": ["query", "term"], "msg": "Field required", "input": None}]
        assert get(f"{url}/s?term=x").text == "x"


def test_json_body_is_validated_into_a_model_and_response_model_applies():
    with running(make_app()) as url:
        r = post(f"{url}/items", {"name": "tea", "price": "2.5", "extra": 1})
        assert r.status == 201 and json.loads(r.text) == {"name": "tea", "price": 2.5}  # converted, extras dropped

        r = post(f"{url}/items", {"name": "", "price": 0})
        assert r.status == 422
        errors = json.loads(r.text)["detail"]
        assert [(e["loc"], e["type"]) for e in errors] == [(["body", "name"], "string_too_short"), (["body", "price"], "greater_than")]

        r = httpp.post(f"{url}/items", data=b"{nope")
        assert r.status == 422 and json.loads(r.text)["detail"][0]["type"] == "json_invalid"
        r = httpp.post(f"{url}/items", data=b"")
        assert r.status == 422 and json.loads(r.text)["detail"][0]["type"] == "missing"


def test_a_model_can_be_returned_and_a_bad_return_is_a_500():
    app = Server()

    @app.get("/m")
    def m():
        return Item(name="x", price=1)

    @app.get("/bad", response_model=Item)
    def bad():
        return {"name": "x"}

    with running(app) as url:
        assert json.loads(get(f"{url}/m").text) == {"name": "x", "price": 1.0}
        assert get(f"{url}/bad").status == 500


def test_field_as_a_parameter_default_is_refused():
    app = Server()
    with pytest.raises(TypeError, match="Field\\(\\) as a parameter default"):
        @app.get("/x")
        def x(q: int = Field(ge=1)):
            return ""


def test_unknown_route_option():
    with pytest.raises(TypeError, match="unexpected options"):
        Server().get("/x", sumary="typo")


# --- /docs ------------------------------------------------------------------------------------------

def test_docs_page_and_assets_are_served_offline():
    with running(make_app()) as url:
        page = get(f"{url}/docs")
        assert page.status == 200 and "swagger-ui-bundle.js" in page.text and '"/openapi.json"' in page.text
        assert "https://" not in page.text  # nothing comes from a CDN
        js = get(f"{url}/docs/swagger-ui-bundle.js")
        assert js.status == 200 and len(js.body) > 1_000_000
        assert get(f"{url}/docs/swagger-ui.css").status == 200
        assert 'rel="icon" type="image/svg+xml" href="/docs/swagger-api-icon.svg"' in page.text
        assert get(f"{url}/docs/swagger-api-icon.svg").status == 200
        vendored = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "third_party", "swagger-ui")
        if os.path.isdir(vendored):  # compiled in, and byte-identical to the vendored files
            for name, ctype in (("swagger-ui-bundle.js", "text/javascript"), ("swagger-ui.css", "text/css"),
                               ("swagger-api-icon.svg", "image/svg+xml")):
                r = get(f"{url}/docs/{name}")
                with open(os.path.join(vendored, name), "rb") as f:
                    assert r.content == f.read()
                assert r.header("content-type") == ctype
        assert json.loads(get(f"{url}/openapi.json").text)["info"]["title"] == "Shop"


def test_docs_and_openapi_can_be_switched_off():
    with running(Server(docs_url=None)) as url:
        assert get(f"{url}/docs").status == 404
        assert get(f"{url}/openapi.json").status == 200
    with running(Server(openapi_url=None)) as url:
        assert get(f"{url}/docs").status == 404 and get(f"{url}/openapi.json").status == 404


def test_docs_title_is_escaped():
    app = Server(title="<script>alert(1)</script>")
    with running(app) as url:
        assert "<script>alert(1)" not in get(f"{url}/docs").text.split("<script")[0]
