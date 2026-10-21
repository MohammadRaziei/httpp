"""HTTPException and the response classes (Starlette/FastAPI names, native implementation).

`httpp.Response` is the CLIENT's response, so the base class here is only reachable as
`httpp.responses.Response`; the concrete ones (JSONResponse, ...) are also in `httpp`.
A handler may return one of these instead of a body, or `(body, status, headers)`.
"""

import http
import json

from .models import to_jsonable


class HTTPException(Exception):
    """Raise it in a handler or a dependency: the client gets `{"detail": detail}` and `status_code`."""

    def __init__(self, status_code, detail=None, headers=None):
        if not isinstance(status_code, int) or isinstance(status_code, bool) or not 100 <= status_code <= 599:
            raise ValueError("status_code must be an int from 100 to 599")
        if detail is None:
            try:
                detail = http.HTTPStatus(status_code).phrase
            except ValueError:
                detail = ""
        super().__init__(status_code, detail)
        self.status_code, self.detail, self.headers = status_code, detail, headers

    def __str__(self):
        return f"{self.status_code}: {self.detail}"


def _pairs(headers):
    if headers is None:
        return []
    return list(headers.items() if hasattr(headers, "items") else headers)


class Response:
    """Bytes or text with an explicit status, headers and media type."""

    media_type = None

    def __init__(self, content=None, status_code=200, headers=None, media_type=None):
        self.content, self.status_code, self.headers = content, status_code, _pairs(headers)
        if media_type is not None:
            self.media_type = media_type

    def _render(self):
        """The body to send: bytes, str, a generator, or whatever server._to_response accepts."""
        return b"" if self.content is None else self.content

    def _as_return(self):
        """(body, status, headers) for server._to_response; the Content-Type is added unless set."""
        headers = self.headers
        if self.media_type and not any(k.lower() == "content-type" for k, _ in headers):
            ctype = self.media_type
            if ctype.startswith("text/") and "charset" not in ctype:
                ctype += "; charset=utf-8"
            headers = headers + [("Content-Type", ctype)]
        body = self._render()
        if isinstance(body, str):
            body = body.encode("utf-8")
        return body, self.status_code, headers


class JSONResponse(Response):
    media_type = "application/json"

    def _render(self):
        return json.dumps(to_jsonable(self.content), separators=(",", ":"), ensure_ascii=False).encode("utf-8")


class PlainTextResponse(Response):
    media_type = "text/plain"


class HTMLResponse(Response):
    media_type = "text/html"


class RedirectResponse(Response):
    """307 by default (like Starlette), which keeps the method; pass 302/303 for the browser-style redirect."""

    def __init__(self, url, status_code=307, headers=None):
        if not isinstance(status_code, int) or not 300 <= status_code < 400:
            raise ValueError("a redirect status must be 3xx")
        super().__init__(b"", status_code, headers)
        self.headers = [("Location", str(url))] + self.headers


class StreamingResponse(Response):
    """`content` is a generator or async generator of str/bytes pieces."""

    def __init__(self, content, status_code=200, headers=None, media_type=None):
        super().__init__(content, status_code, headers, media_type or "text/plain")


class FileResponse(Response):
    """A file streamed from disk. `path` is trusted: never build it from user input."""

    def __init__(self, path, status_code=200, headers=None, media_type=None):
        super().__init__(path, status_code, headers, media_type)

    def _render(self):
        from .server import file  # late: server imports this module

        return file(self.content, self.media_type)
