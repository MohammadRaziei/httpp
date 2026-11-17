# The OpenAPI document writer (include/httpp/openapi.hpp, ADR-0087): the Python face of httpp::openapi::document.
# Python's own document (httpp/_schema.py) is assembled and written through it, so the rules for the paths, the security names and
# the JSON text live once, in C++.

import json as _oa_json_module

ctypedef vector[vector[pair[string, vector[string]]]] cpp_oa_requirements

cdef extern from "httpp/openapi.hpp" namespace "httpp::openapi":
    cdef cppclass cpp_oa_scheme "httpp::openapi::scheme":
        string json

    cdef cppclass cpp_oa_operation "httpp::openapi::operation":
        cpp_oa_operation(string method, string path) except +
        void summary(string text)
        void description(string text)
        void operation_id(string id)
        void tag(string name)
        void deprecated(bint on)
        void security(cpp_oa_requirements r)
        void parameter_json(string object_json)
        void request_body(string object_json)
        void response(string status, string description, string content_json)
        void raw(string key, string json)

    cdef cppclass cpp_oa_document "httpp::openapi::document":
        cpp_oa_document(string title, string version) except +
        void description(string text)
        void security_scheme(string name, cpp_oa_scheme s) except +
        void security(cpp_oa_requirements r)
        void component_schema(string name, string json) except +
        void raw(string key, string json) except +
        void add(cpp_oa_operation op) except +
        string to_json() except +


cdef bytes _oa_text(object value, str what):
    if not isinstance(value, str):
        raise TypeError(f"{what} must be a string, not {type(value).__name__}")
    return (<str>value).encode("utf-8")


cdef bytes _oa_json(object value, str what):
    """JSON text for `value`: a str is taken to be JSON text already, anything else is dumped (compact)."""
    if isinstance(value, str):
        return (<str>value).encode("utf-8")
    try:
        return _oa_json_module.dumps(value, separators=(",", ":")).encode("utf-8")
    except (TypeError, ValueError) as e:
        raise TypeError(f"{what} must be JSON text or something json.dumps can write: {e}") from None


cdef cpp_oa_requirements _oa_requirements(object spec, str what) except *:
    """A list of requirements: a scheme name, or a {name: [scopes]} dict; the list means "any one of them", [] means none."""
    cdef cpp_oa_requirements out
    cdef vector[pair[string, vector[string]]] one
    cdef pair[string, vector[string]] item
    if isinstance(spec, (str, dict)):
        spec = [spec]
    if not isinstance(spec, (list, tuple)):
        raise TypeError(f"{what} is a scheme name or a list of them, not {type(spec).__name__}")
    for req in spec:
        one.clear()
        if isinstance(req, str):
            req = {req: []}
        if not isinstance(req, dict):
            raise TypeError(f"{what}: a requirement is a scheme name or a {{name: [scopes]}} dict, not {req!r}")
        for name, scopes in req.items():
            item.first = _oa_text(name, what + ": a scheme name")
            item.second.clear()
            if not isinstance(scopes, (list, tuple)):
                raise TypeError(f"{what}: the scopes of {name!r} must be a list of strings")
            for scope in scopes:
                item.second.push_back(_oa_text(scope, what + ": a scope"))
            one.push_back(item)
        out.push_back(one)
    return out


cdef class Document:
    """An OpenAPI 3.1 document described with calls and written as JSON text by C++ (httpp::openapi::document).

        doc = httpp.openapi.Document("My API", "1.0.0")
        doc.security_scheme("jwt", httpp.security.bearer("JWT"))
        doc.security(["jwt"])                                              # the default of every operation
        doc.add("get", "/admin", summary="Admin", security=["jwt"], responses={"200": {"description": "OK"}})
        doc.add("get", "/health", security=[], responses={"200": {"description": "OK"}})
        text = doc.to_json()

    `Server.openapi()` builds its document from the routes through this class. Use it directly for a document that is not your
    routes (a gateway, a hand-written API). Pieces that are more than a few fields (a schema, a parameter, a request body) are JSON
    text or anything `json.dumps` writes; nothing is parsed or checked, so valid JSON is your promise. `to_json()` raises
    ValueError for a security name that was not declared, a method or path OpenAPI does not allow, the same method on a path twice,
    a duplicate operation id and text that is not UTF-8.
    """
    cdef cpp_oa_document* _d

    def __cinit__(self, str title, str version):
        self._d = new cpp_oa_document(_oa_text(title, "title"), _oa_text(version, "version"))

    def __dealloc__(self):
        if self._d != NULL:
            del self._d

    def description(self, str text):
        """The description of the API (`info.description`)."""
        self._d.description(_oa_text(text, "description"))
        return self

    def security_scheme(self, str name, scheme):
        """Declare a scheme under `name` (the same name again replaces it). `scheme` is a dict such as `httpp.security.basic()` or JSON text."""
        cdef cpp_oa_scheme s
        s.json = _oa_json(scheme, "a security scheme")
        self._d.security_scheme(_oa_text(name, "a security scheme name"), s)
        return self

    def security(self, spec):
        """The default requirement of every operation that sets none: a scheme name, or a list of names and {name: [scopes]} dicts
        ("any one of them"). [] marks the whole API public."""
        self._d.security(_oa_requirements(spec, "security"))
        return self

    def component_schema(self, str name, schema):
        """A named schema under components.schemas; `schema` is a dict or JSON text."""
        self._d.component_schema(_oa_text(name, "a schema name"), _oa_json(schema, "a schema"))
        return self

    def raw(self, str key, value):
        """Any other top-level key ("servers", "tags", "x-..."); `value` is a JSON-able value or JSON text."""
        self._d.raw(_oa_text(key, "a key"), _oa_json(value, "a value"))
        return self

    def add(self, str method, str path, *, summary=None, description=None, operation_id=None, tags=(), deprecated=False,
            security=None, parameters=(), request_body=None, responses=None, extra=None):
        """Add an operation. `method` in any case, `path` starting with "/". `security` None leaves the document's default, a list
        sets this operation's requirements and [] marks it public. `parameters` is a list of parameter objects (dicts or JSON
        text), `request_body` the requestBody object, `responses` {status: {"description": ..., "content": ...}} and `extra`
        {key: value} for any other key of the operation object."""
        cdef cpp_oa_operation* op = new cpp_oa_operation(_oa_text(method, "method"), _oa_text(path, "path"))
        try:
            if summary is not None:
                op.summary(_oa_text(summary, "summary"))
            if description is not None:
                op.description(_oa_text(description, "description"))
            if operation_id is not None:
                op.operation_id(_oa_text(operation_id, "operation_id"))
            for tag in tags:
                op.tag(_oa_text(tag, "a tag"))
            if deprecated:
                op.deprecated(True)
            if security is not None:
                op.security(_oa_requirements(security, "security"))
            for parameter in parameters:
                op.parameter_json(_oa_json(parameter, "a parameter"))
            if request_body is not None:
                op.request_body(_oa_json(request_body, "request_body"))
            if responses is not None:
                for status, response in responses.items():
                    if isinstance(response, str):
                        response = {"description": response}
                    unknown = set(response) - {"description", "content"}
                    if unknown:
                        raise ValueError(f"response {status}: only description and content are written, not {', '.join(sorted(unknown))}")
                    content = response.get("content")
                    op.response(_oa_text(str(status), "a response status"), _oa_text(response.get("description", ""), "a response description"),
                                b"" if content is None else _oa_json(content, "response content"))
            if extra:
                for key, value in extra.items():
                    op.raw(_oa_text(key, "a key"), _oa_json(value, "a value"))
            self._d.add(deref(op))
        finally:
            del op
        return self

    def to_json(self):
        """The document as compact JSON text."""
        return self._d.to_json().decode("utf-8")
