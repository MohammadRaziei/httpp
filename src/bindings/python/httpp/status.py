"""HTTP status constants: `httpp.status.HTTP_404_NOT_FOUND == 404` (Starlette's names)."""

import http as _http

for _s in _http.HTTPStatus:
    globals()[f"HTTP_{_s.value}_{_s.name}"] = _s.value
# the name of 422 changed between Python versions; both are always there
HTTP_422_UNPROCESSABLE_ENTITY = HTTP_422_UNPROCESSABLE_CONTENT = 422
del _s
