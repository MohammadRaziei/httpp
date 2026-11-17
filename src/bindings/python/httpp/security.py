"""OpenAPI security schemes: documentation for the Swagger UI "Authorize" button, nothing more.

    from httpp import Server, security

    app = Server(security_schemes={"basic": security.basic(), "jwt": security.bearer("JWT")}, security=["jwt"])

    @app.get("/admin", security=["basic"])     # this route is documented as needing Basic credentials
    def admin(): ...

    @app.get("/health", security=[])           # public although the server default asks for a JWT
    def health(): ...

This declares what the API expects; it checks nothing. The check is yours: `BasicAuthMiddleware` (httpp.middleware), a dependency that
reads `request.basic_auth()`, a JWT library of your choice. The document is built once and cached, so none of this costs anything per request.

A requirement is a scheme name, or a dict {name: [scopes]} when several schemes are needed together (and, for OAuth2 and OpenID Connect,
their scopes); a list of requirements means "any one of them". Every name must be declared in `Server(security_schemes=...)`: a typo raises
ValueError where the route is registered, not when /openapi.json is first read.
"""

from collections.abc import Mapping

_LOCATIONS = ("header", "query", "cookie")
_TYPES = ("http", "apiKey", "oauth2", "openIdConnect", "mutualTLS")


def _described(scheme, description):
    if description is not None:
        if not isinstance(description, str):
            raise TypeError("description must be a string")
        scheme["description"] = description
    return scheme


def basic(description=None):
    """HTTP Basic (RFC 7617): the browser's user and password sent as `Authorization: Basic ...`."""
    return _described({"type": "http", "scheme": "basic"}, description)


def bearer(bearer_format=None, description=None):
    """A bearer token, `Authorization: Bearer <token>`; `bearer_format` is only a hint for readers of the document ("JWT")."""
    scheme = {"type": "http", "scheme": "bearer"}
    if bearer_format is not None:
        if not isinstance(bearer_format, str) or not bearer_format:
            raise TypeError("bearer_format must be a non-empty string")
        scheme["bearerFormat"] = bearer_format
    return _described(scheme, description)


def api_key(name, location="header", description=None):
    """A key in a header (the default), a query parameter or a cookie called `name`."""
    if not isinstance(name, str) or not name:
        raise TypeError("name must be a non-empty string")
    if location not in _LOCATIONS:
        raise ValueError(f"location must be one of {', '.join(_LOCATIONS)}, not {location!r}")
    return _described({"type": "apiKey", "name": name, "in": location}, description)


def check_schemes(schemes):
    """The declared schemes as a dict {name: scheme dict}; ValueError / TypeError for anything that is not one."""
    if schemes is None:
        return {}
    if not isinstance(schemes, Mapping):
        raise TypeError("security_schemes maps a name to a scheme (security.basic(), security.bearer(), security.api_key(...))")
    out = {}
    for name, scheme in schemes.items():
        if not isinstance(name, str) or not name:
            raise TypeError("a security scheme name must be a non-empty string")
        if not isinstance(scheme, Mapping) or scheme.get("type") not in _TYPES:
            raise ValueError(f"security scheme {name!r} must be an OpenAPI security scheme object whose type is one of {', '.join(_TYPES)}")
        out[name] = dict(scheme)
    return out


def requirements(spec, schemes, what):
    """`spec` (None, a name, or a list of names / {name: [scopes]} dicts) as OpenAPI requirement objects [{name: [scopes]}, ...].
    None stays None (not given); [] stays [] (explicitly none). `what` names the place for error messages."""
    if spec is None:
        return None
    if isinstance(spec, (str, Mapping)):
        spec = [spec]
    if not isinstance(spec, (list, tuple)):
        raise TypeError(f"{what}: security is a scheme name or a list of them")
    out = []
    for item in spec:
        entry = {item: []} if isinstance(item, str) else dict(item) if isinstance(item, Mapping) else None
        if not entry:
            raise TypeError(f"{what}: a security requirement is a scheme name or a {{name: [scopes]}} dict, not {item!r}")
        for name, scopes in entry.items():
            if name not in schemes:
                known = ", ".join(sorted(schemes)) or "none declared"
                raise ValueError(f"{what}: unknown security scheme {name!r}; declare it in Server(security_schemes=...) (known: {known})")
            if not isinstance(scopes, (list, tuple)) or not all(isinstance(s, str) for s in scopes):
                raise TypeError(f"{what}: the scopes of {name!r} must be a list of strings")
            entry[name] = list(scopes)
        out.append(entry)
    return out
