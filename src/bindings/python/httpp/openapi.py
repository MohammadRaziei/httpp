"""OpenAPI 3.1 documents written by C++ (httpp/openapi.hpp, ADR-0087).

    doc = httpp.openapi.Document("My API", "1.0.0")
    doc.security_scheme("jwt", httpp.security.bearer("JWT"))
    doc.security(["jwt"])
    doc.add("get", "/health", security=[], responses={"200": {"description": "OK"}})
    text = doc.to_json()

`Server.openapi()` builds its document through this class, from the routes and their type hints (ADR-0007); `Document` is for a
document that is not made from routes. The scheme factories are in `httpp.security`.
"""

from .httpp_python import Document

__all__ = ["Document"]
