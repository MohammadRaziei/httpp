# RawRequest (a server request read on demand) and form parsing. Included by httpp_cy.pyx (a textual include: the order of the includes matters).

cdef class RawRequest:
    """A request the way C++ holds it, read on demand. Nothing is turned into Python objects until it is
    asked for, so a handler that wants one path parameter pays for one string, not for the whole request.
    It shares the C++ request (no copy) and keeps it alive for as long as it exists."""
    cdef shared_ptr[const cpp_request] _req

    @property
    def method(self):
        return self._req.get().method.decode("utf-8", errors="replace")

    @property
    def path(self):
        return self._req.get().path.decode("utf-8", errors="replace")

    @property
    def body(self):
        cdef bytes b = self._req.get().body
        return b

    @property
    def secure(self):
        return bool(self._req.get().secure)

    def headers(self):
        """[(name, value), ...] in wire order."""
        return _pairs(self._req.get().headers)

    def query(self):
        return _pairs(self._req.get().query)

    def path_params(self):
        """{name: value}; for a repeated name the first one wins."""
        cdef const cpp_request* r = self._req.get()
        cdef dict out = {}
        cdef size_t i
        for i in range(r.path_params.size()):
            name = r.path_params[i].first.decode("utf-8", errors="replace")
            if name not in out:
                out[name] = r.path_params[i].second.decode("utf-8", errors="replace")
        return out

    def form(self):
        """(fields, files) of a urlencoded or multipart body, parsed by C++ on the spot: fields is [(name, value), ...],
        files is [(field, filename, content_type, data_bytes), ...]. ValueError for a malformed multipart body."""
        return _form_tuple(deref(self._req.get()).form())

    def _args(self):
        """The old flat shape (method, path, body, headers, query, path_params-as-pairs)."""
        return _request_args(deref(self._req.get()))


cdef tuple _form_tuple(cpp_form_data form):
    cdef list fields = []
    cdef list files = []
    cdef size_t i
    cdef bytes data
    for i in range(form.fields.size()):
        fields.append((form.fields[i].first.decode("utf-8", errors="replace"),
                       form.fields[i].second.decode("utf-8", errors="replace")))
    for i in range(form.files.size()):
        data = form.files[i].data
        files.append((form.files[i].field.decode("utf-8", errors="replace"),
                      form.files[i].filename.decode("utf-8", errors="replace"),
                      form.files[i].content_type.decode("utf-8", errors="replace"), data))
    return fields, files


def parse_form(bytes body, str content_type):
    """The same parser for a body and Content-Type that are not a C++ request (a hook's request): (fields, files)."""
    cdef cpp_request req
    req.body = body
    req.headers.push_back(pair[string, string](b"Content-Type", content_type.encode("utf-8")))
    return _form_tuple(req.form())


cdef RawRequest _new_raw(shared_ptr[const cpp_request] req):
    cdef RawRequest raw = RawRequest.__new__(RawRequest)
    raw._req = req
    return raw


cdef void _push_headers(response& res, object headers) except *:
    cdef string k, v
    for name, value in headers:
        k = (<str>name).encode("utf-8")
        v = (<str>value).encode("utf-8")
        res.headers.push_back(pair[string, string](k, v))


cdef int _stream_next(void* ctx, size_t offset, string& chunk) noexcept nogil:
    # The provider returns the next piece (bytes) or None at the end. Runs on whatever thread is
    # writing the response, which Python knows nothing about.
    busy_enter()  # see the gate comment above
    try:
        with gil:
            try:
                piece = (<object>ctx)()
                if piece is None:
                    return 0
                chunk = piece  # bytes only; anything else raises TypeError -> -1
            except BaseException:
                traceback.print_exc()
                return -1
            return 1
    finally:
        busy_exit()


cdef void _stream_release(void* ctx) noexcept nogil:
    # The provider's `finally` (an open file, a connection) must run now. On free-threaded CPython the
    # object itself is freed only when the thread that created it next runs Python (its refcounts
    # are merged then), and that thread may sit idle in the pool until the next request.
    busy_enter()  # see the gate comment above
    try:
        with gil:
            provider = <object>ctx
            close = getattr(provider, "close", None)
            if close is not None:
                try:
                    close()
                except BaseException:
                    traceback.print_exc()
            provider = None
            Py_DECREF(<object>ctx)
    finally:
        busy_exit()


cdef void _after_send_run(void* ctx) noexcept nogil:
    # Runs on a pool thread, after the answer was written.
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
        with gil:
            try:
                (<object>ctx)()
            except BaseException:
                traceback.print_exc()
    finally:
        busy_exit()


cdef void _after_send_release(void* ctx) noexcept nogil:
    busy_enter()  # see the gate comment above
    try:
        with gil:
            Py_DECREF(<object>ctx)
    finally:
        busy_exit()


cdef void _fill_response(response& res, int status, object body, object headers) except *:
    # body: bytes (as is) | str (path of a file to stream, trusted) | callable (stream provider).
    if isinstance(body, bytes):
        res.body = <bytes>body
    elif isinstance(body, str):
        res.file = os.fsencode(body)
    elif callable(body):
        Py_INCREF(body)  # owned by the C++ side now, dropped by _stream_release
        set_stream(res, _stream_next, _stream_release, <void*>body)
    else:
        raise TypeError("response body must be bytes, a file path (str) or a stream provider")
    res.status = status
    _push_headers(res, headers)
    if type(headers) is not list:  # a list subclass (httpp.server._AfterHeaders) carries the after_send job
        after = getattr(headers, "after_send", None)
        if after is not None:
            Py_INCREF(after)  # owned by the C++ side now, dropped by _after_send_release
            set_after_send(res, _after_send_run, _after_send_release, <void*>after)


cdef void _fail(response& res) noexcept:
    # Drops whatever was half-filled (body, file, stream, headers) and answers a plain 500.
    reset_response(res)
    res.status = 500
    res.body = b"Internal Server Error"


cdef void _run_handler(object fn, const cpp_request& req, response& res) except *:
    # fn(*request_args) -> (status, body, headers)
    status, body, headers = fn(*_request_args(req))
    _fill_response(res, status, body, headers)


cdef void _dispatch(void* ctx, const cpp_request& req, response& res) noexcept nogil:
    # Called from a server worker thread that Python knows nothing about,
    # hence the explicit GIL acquire.
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
        with gil:
            try:
                _run_handler(<object>ctx, req, res)
            except BaseException:
                # The traceback goes to stderr for the developer; the client only
                # ever sees a generic 500 (no internals leaked).
                traceback.print_exc()
                _fail(res)
    finally:
        busy_exit()


cdef void _dispatch_raw(void* ctx, shared_ptr[const cpp_request] req, response& res) noexcept nogil:
    # Like _dispatch, but the handler gets a RawRequest (shared with C++, built on demand).
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
        with gil:
            try:
                status, body, headers = (<object>ctx)(_new_raw(req))
                _fill_response(res, status, body, headers)
            except BaseException:
                traceback.print_exc()
                _fail(res)
    finally:
        busy_exit()


cdef cbool _dispatch_before(void* ctx, const cpp_request& req, response& res) noexcept nogil:
    # hook(*request_args) -> None (go on to the route) | (status, body, headers) (answer it here)
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
        with gil:
            try:
                out = (<object>ctx)(*_request_args(req))
                if out is None:
                    return False
                status, body, headers = out
                _fill_response(res, status, body, headers)
                return True
            except BaseException:
                traceback.print_exc()
                _fail(res)
                return True
    finally:
        busy_exit()


cdef void _dispatch_after(void* ctx, const cpp_request& req, response& res) noexcept nogil:
    # hook(*request_args, status, headers) -> None | (status, headers). A failing hook makes it a 500.
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
        with gil:
            try:
                out = (<object>ctx)(*_request_args(req), res.status, _pairs(res.headers))
                if out is not None:
                    status, headers = out
                    res.status = status
                    res.headers.clear()
                    _push_headers(res, headers)
            except BaseException:
                traceback.print_exc()
                res.status = 500
    finally:
        busy_exit()


cdef void _dispatch_error(void* ctx, const cpp_request& req, response& res) noexcept nogil:
    # hook(*request_args, status) -> None (keep the plain error) | (status, body, headers)
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
        with gil:
            try:
                out = (<object>ctx)(*_request_args(req), res.status)
                if out is not None:
                    status, body, headers = out
                    _fill_response(res, status, body, headers)
            except BaseException:
                traceback.print_exc()
    finally:
        busy_exit()
