import json

import httpp
from httpp import openapi, security

app = httpp.Server(docs_url=None, openapi_url=None)  # no page and no document of its own


@app.get("/hello/{name}")
def hello(name: str):
    return {"hello": name}


@app.get("/health")
def health():
    return {"ok": True}


doc = openapi.Document("Hello API", "1.0.0")
doc.security_scheme("jwt", security.bearer("JWT"))
doc.security(["jwt"])  # the default of every operation
doc.add("get", "/hello/{name}", summary="Say hello",
        parameters=[{"name": "name", "in": "path", "required": True, "schema": {"type": "string"}}],
        responses={200: {"description": "A greeting"}})
doc.add("get", "/health", security=[], responses={200: {"description": "OK"}})  # public although the default asks for a JWT


@app.get("/openapi.json")
def spec():
    return json.loads(doc.to_json())


httpp.Swagger("/openapi.json", title="Hello API").mount(app, "/docs")

if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)
