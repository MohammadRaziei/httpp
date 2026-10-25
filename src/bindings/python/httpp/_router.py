"""APIRouter: a group of routes with a shared path prefix, the Python side of httpp::router (include/httpp/router.hpp).

Same rules as C++, so the two behave alike: the join order is outer prefix, the router's own prefix, then the route's path;
a prefix gets a leading "/" and loses trailing ones; a route path "/" under a prefix is the prefix itself. Nothing is
looked up at request time: Server.include() registers every route with its full path, exactly as if it had been
written by hand.

One difference from C++ that the GIL's world forces: a route here is adapted to the C++ handler (parameters, models,
dependencies, `def` or `async def`) when it is registered on a Server, not when it is added to the router, so the
router only remembers the function and its options.
"""

from collections import namedtuple

RouterEntry = namedtuple("RouterEntry", "path methods fn meta")


def _clean_prefix(prefix):
    prefix = prefix.rstrip("/")
    return "/" + prefix if prefix and not prefix.startswith("/") else prefix


def _clean_path(path):
    return path if path.startswith("/") else "/" + path


def join(prefix, path):
    """prefix + path for an already clean prefix; "/" under a prefix is the prefix itself."""
    path = _clean_path(path)
    if not prefix:
        return path
    return prefix if path == "/" else prefix + path


class APIRouter:
    """Routes with a shared prefix, built apart from any Server and added with Server.include(router, prefix="").

    users = APIRouter("/users", tags=["users"])
    @users.get("/{user_id}")        # GET /users/{user_id}
    api = APIRouter("/api"); api.include(users)    # GET /api/users/{user_id}
    app.include(api, "/v1")         # GET /v1/api/users/{user_id}

    `tags` are added to the OpenAPI tags of the routes registered on this router. The route options are those of
    Server.route(); a wrong one raises TypeError here, an unsupported HTTP method when the router is included.
    """

    def __init__(self, prefix="", *, tags=None):
        self._prefix = _clean_prefix(prefix)
        self._tags = list(tags or ())
        self._routes = []

    @property
    def prefix(self):
        return self._prefix

    @property
    def routes(self):
        """Every route as a RouterEntry(path, methods, fn, meta); paths are relative to this router (its prefix is not in them)."""
        return list(self._routes)

    def route(self, path, methods=("GET",), **meta):
        from .server import _ROUTE_META  # server.py imports this module, so only here
        unknown = set(meta) - _ROUTE_META
        if unknown:
            raise TypeError(f"route() got unexpected options {sorted(unknown)}; known: {sorted(_ROUTE_META)}")
        if isinstance(methods, str):
            raise TypeError("methods must be a list or tuple of HTTP methods, not a string")
        methods = tuple(methods)
        if self._tags:
            meta = dict(meta, tags=self._tags + list(meta.get("tags") or ()))

        def register(fn):
            self._routes.append(RouterEntry(_clean_path(path), methods, fn, meta))
            return fn  # unchanged, like Server.route

        return register

    def get(self, path, **meta):
        return self.route(path, ("GET",), **meta)

    def post(self, path, **meta):
        return self.route(path, ("POST",), **meta)

    def put(self, path, **meta):
        return self.route(path, ("PUT",), **meta)

    def patch(self, path, **meta):
        return self.route(path, ("PATCH",), **meta)

    def delete(self, path, **meta):
        return self.route(path, ("DELETE",), **meta)

    def include(self, other, prefix=""):
        """Add every route of `other` under prefix + other.prefix. `other` may change afterwards; it is not kept."""
        outer = _clean_prefix(prefix) + other._prefix
        self._routes.extend([e._replace(path=join(outer, e.path)) for e in other._routes])
        return self

    include_router = include  # FastAPI's name


def include_into(server, router, prefix=""):
    """Server.include: register every route of `router` on `server` under prefix + router.prefix."""
    outer = _clean_prefix(prefix) + router.prefix
    for entry in router.routes:
        server.route(join(outer, entry.path), entry.methods, **entry.meta)(entry.fn)
