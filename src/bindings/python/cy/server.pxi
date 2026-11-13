# Server. Included by httpp_cy.pyx (a textual include: the order of the includes matters).

cdef class Server:
    """Minimal HTTP server. Replaces spinning up cpp-httplib/libcurl
    yourself - this is also what `httpp server` (the CLI, similar to
    Python's own `python -m http.server`) is built on."""
    cdef cpp_server* _srv
    cdef list _handlers  # keeps route callables alive; C++ only holds a borrowed pointer

    def __init__(self):
        self._srv = new cpp_server()
        self._handlers = []

    def __dealloc__(self):
        if self._srv is not NULL:
            del self._srv

    def serve_directory(self, str mount_path, str local_dir):
        """Serve `local_dir` under `mount_path`, like `python -m http.server`."""
        self._srv.serve_directory(mount_path.encode("utf-8"), local_dir.encode("utf-8"))

    def _add_cors(self, origins, str origin_regex, methods, headers, bint credentials, expose, int max_age):
        """Install the C++ CORS middleware (include/httpp/middleware.hpp); use Server.add_middleware(CORSMiddleware, ...)."""
        cdef cpp_cors c
        c.set_allow_origins([x.encode("utf-8") for x in origins])
        c.set_allow_origin_regex(origin_regex.encode("utf-8"))
        c.set_allow_methods([x.encode("utf-8") for x in methods])
        c.set_allow_headers([x.encode("utf-8") for x in headers])
        c.set_allow_credentials(credentials)
        c.set_expose_headers([x.encode("utf-8") for x in expose])
        c.set_max_age(max_age)
        c.install(self._srv[0])

    def _add_forwarded_headers(self, trusted_proxies, bint trust_forwarded_host):
        """Install the C++ forwarded-headers rewrite; use Server.add_middleware(ProxyHeadersMiddleware, ...)."""
        cdef cpp_forwarded_headers f
        f.set_trusted_proxies([x.encode("utf-8") for x in trusted_proxies])
        f.set_trust_forwarded_host(trust_forwarded_host)
        f.install(self._srv[0])

    def _add_https_redirect(self, bint trust_forwarded_proto):
        """Install the C++ HTTPS redirect middleware; use Server.add_middleware(HTTPSRedirectMiddleware, ...)."""
        cdef cpp_https_redirect r
        r.set_trust_forwarded_proto(trust_forwarded_proto)
        r.install(self._srv[0])

    def set_tls(self, cert_file, key_file, password=None):
        """Serve https and wss: a certificate chain (PEM, leaf first) and its private key (PEM; `password` when encrypted).
        The files are read now; ValueError names the file for a missing or unreadable file, bad PEM, a wrong password or a
        key that does not belong to the certificate. Before listening. One port serves TLS or plain HTTP, not both."""
        cdef string pw = b"" if password is None else (<str>password).encode("utf-8")
        try:
            self._srv.set_tls(_fs(cert_file), _fs(key_file), pw)
        except RuntimeError as e:
            raise ValueError(str(e)) from None
        return self

    def _add_trusted_host(self, hosts, bint www_redirect):
        """Install the C++ trusted-host middleware; use Server.add_middleware(TrustedHostMiddleware, ...)."""
        cdef cpp_trusted_host* t = new cpp_trusted_host([x.encode("utf-8") for x in hosts])
        try:
            t.set_www_redirect(www_redirect)
            t.install(self._srv[0])
        finally:
            del t

    def add_route(self, str method, str path, handler):
        """Low-level: register `handler` for `method` + `path` ("/users/:id").
        `handler(method, path, body, headers, query, path_params)` returns (status, body, headers):
        body is bytes, or a str / pathlib.Path = path of a file to stream (trusted, never from user input), or a
        callable returning the next bytes piece / None when done (chunked). headers: list of pairs.
        Use the decorators on httpp.Server (see httpp/server.py) instead.
        Must be called before listen()."""
        if not callable(handler):
            raise TypeError("handler must be callable")
        self._handlers.append(handler)
        self._srv.route(method.encode("utf-8"), path.encode("utf-8"),
                        make_handler(_dispatch, <void*>handler))

    def add_async_route(self, str method, str path, dispatch):
        """Low-level, like add_route, but non-blocking: `dispatch(method, path,
        body, headers, query, path_params, done)` must return quickly and
        arrange for `done(status, body, headers)` to be called later, from any
        thread. The worker thread is free in the meantime."""
        if not callable(dispatch):
            raise TypeError("dispatch must be callable")
        self._handlers.append(dispatch)
        self._srv.route_async(method.encode("utf-8"), path.encode("utf-8"),
                              make_async_handler(_dispatch_async, <void*>dispatch))

    def add_loop_route(self, str method, str path, dispatch):
        """Like add_async_route, but `dispatch` runs on the asyncio loop thread that drains the inbox
        (see httpp/server.py), not on a pool thread, and the C++ side never takes the GIL for it. Needs
        _attach_inbox() and a loop calling _drain_inbox(); httpp.Server arranges both when it listens."""
        if not callable(dispatch):
            raise TypeError("dispatch must be callable")
        self._add_raw_loop_route(method, path, lambda raw, done: dispatch(*raw._args(), done))

    def _add_raw_route(self, str method, str path, fn):
        """What httpp.Server uses for `def` routes: `fn(raw)` -> (status, body, headers), where `raw` is a
        RawRequest (built on demand). Runs on a worker thread, with the GIL."""
        if not callable(fn):
            raise TypeError("handler must be callable")
        self._handlers.append(fn)
        self._srv.route_shared(method.encode("utf-8"), path.encode("utf-8"),
                               make_shared_handler(_dispatch_raw, <void*>fn))

    def _add_raw_loop_route(self, str method, str path, dispatch):
        """What httpp.Server uses for `async def` routes: `dispatch(raw, done)` runs on the asyncio loop
        thread, fed through the inbox straight from the I/O thread (one thread hop, no GIL on the C++
        side), and must return quickly; `done(status, body, headers)` sends the answer, from any thread."""
        if not callable(dispatch):
            raise TypeError("dispatch must be callable")
        self._handlers.append(dispatch)
        self._srv.route_io(method.encode("utf-8"), path.encode("utf-8"),
                           make_io_handler(_dispatch_inbox, <void*>dispatch))

    def _add_ws_route(self, str path, dispatch, size_t max_message=1048576, size_t max_buffered=1048576,
                      int ping_interval=0, size_t max_queued=1048576):
        """What httpp.Server.websocket uses: `dispatch(raw, core)` runs on the asyncio loop thread (fed through the lane's
        inbox, the C++ side never takes the GIL), must return at once, and answers the upgrade through `core` (a _WsConn)
        from its own task. `max_queued` is how much received data may wait for the handler before the socket is no longer read."""
        if not callable(dispatch):
            raise TypeError("dispatch must be callable")
        self._handlers.append(dispatch)
        route_ws_c(self._srv[0], path.encode("utf-8"), <void*>dispatch, max_message, max_buffered, ping_interval, max_queued)

    def _add_raw_hybrid_route(self, str method, str path, dispatch):
        """Like _add_raw_loop_route, for a handler that cannot suspend: `dispatch(raw, done, inline=False)`
        runs on the I/O thread itself (inline=True) when the loop thread sleeps and no `def` handler runs,
        and on the loop thread otherwise. Needs the loop to use httpp.server's _GatedSelector."""
        if not callable(dispatch):
            raise TypeError("dispatch must be callable")
        self._handlers.append(dispatch)
        self._srv.route_io(method.encode("utf-8"), path.encode("utf-8"),
                           make_io_handler(_dispatch_hybrid, <void*>dispatch))

    # --- Hooks. Each one replaces the previous (like the C++ setters); httpp/server.py chains several. ---
    def set_before_hook(self, hook):
        """hook(method, path, body, headers, query, path_params) -> None | (status, body, headers):
        a tuple answers the request without calling the route."""
        if not callable(hook):
            raise TypeError("hook must be callable")
        self._handlers.append(hook)
        self._srv.before(make_before(_dispatch_before, <void*>hook))
        return self

    def set_after_hook(self, hook):
        """hook(method, path, body, headers, query, path_params, status, resp_headers) -> None |
        (status, resp_headers). Status and headers only; the body is never passed."""
        if not callable(hook):
            raise TypeError("hook must be callable")
        self._handlers.append(hook)
        self._srv.after(make_handler(_dispatch_after, <void*>hook))
        return self

    def set_error_hook(self, hook):
        """hook(method, path, body, headers, query, path_params, status) -> None | (status, body,
        headers), for 4xx/5xx the server made itself (404, 405, 413, ...)."""
        if not callable(hook):
            raise TypeError("hook must be callable")
        self._handlers.append(hook)
        self._srv.set_error_handler(make_handler(_dispatch_error, <void*>hook))
        return self

    # --- Settings; call them before listening. Each returns self. ---
    def set_thread_pool(self, size_t base, size_t max_threads=0, grow_after_ms=0.5):
        """Worker threads for sync handlers: `base` are kept, up to `max_threads` used under load
        (0 = 4 x base). A job that waited `grow_after_ms` for a free thread adds one more thread to the pool."""
        if base == 0:
            raise ValueError("the thread pool needs at least 1 thread")
        if 0 < max_threads < base:
            raise ValueError("max_threads can't be smaller than threads")
        self._srv.set_thread_pool(base, max_threads, microseconds(<long long>(grow_after_ms * 1000)))
        return self

    def set_read_timeout(self, int seconds):
        _check_seconds(seconds)
        self._srv.set_read_timeout(seconds)
        return self

    def set_write_timeout(self, int seconds):
        _check_seconds(seconds)
        self._srv.set_write_timeout(seconds)
        return self

    def set_keep_alive_timeout(self, int seconds):
        _check_seconds(seconds)
        self._srv.set_keep_alive_timeout(seconds)
        return self

    def set_keep_alive_max(self, size_t requests):
        self._srv.set_keep_alive_max(requests)
        return self

    def set_max_body(self, size_t nbytes):
        """Largest request body accepted (413 above it); 0 = no limit."""
        self._srv.set_max_body(nbytes)
        return self

    def set_header_timeout(self, int seconds):
        """Seconds the whole request head may take to arrive (slowloris guard); 0 = no limit."""
        self._srv.set_header_timeout(seconds)
        return self

    def set_body_timeout(self, int seconds):
        """Seconds the whole request body may take to arrive after its head (slow-drip guard); 0 = no limit."""
        self._srv.set_body_timeout(seconds)
        return self

    def set_compression(self, size_t min_size, int level=1):
        """Gzip text answers of at least min_size bytes for clients that accept it (level 1 fastest .. 9 smallest);
        min_size 0 turns it off. Files, streams and already encoded bodies are never touched."""
        self._srv.set_compression(min_size, level)
        return self

    def set_max_connections(self, size_t n):
        """Open connections allowed at once (one more gets a 503 and is closed); 0 = no limit."""
        self._srv.set_max_connections(n)
        return self

    def set_async_timeout(self, int seconds):
        """add_async_route: answer 504 if `done` isn't called within this; 0 = never."""
        _check_seconds(seconds)
        self._srv.set_async_timeout(seconds)
        return self

    def set_reuse_port(self, cbool on):
        """Several processes may listen on one port, the kernel balancing between them (Linux only;
        listen() then fails elsewhere). Used by `run(workers=N)`."""
        self._srv.set_reuse_port(on)
        return self

    def set_io_threads(self, size_t n):
        """How many I/O threads read, parse and answer (default 1); each has its own acceptor on the same port
        (needs SO_REUSEPORT; without it the server keeps one). Before listening."""
        self._srv.set_io_threads(n)
        return self

    def bind_to_any_port(self, str host):
        return self._srv.bind_to_any_port(host.encode("utf-8"))

    def listen_after_bind(self):
        cdef cbool ok
        with nogil:
            ok = self._srv.listen_after_bind()
        if not ok:
            raise OSError("httpp: server failed to listen")

    def listen(self, str host, int port):
        cdef string h = host.encode("utf-8")
        cdef cbool ok
        with nogil:
            ok = self._srv.listen(h, port)
        if not ok:
            raise OSError(f"httpp: could not listen on {host}:{port} (port in use, or no permission?)")

    def stop(self):
        self._srv.stop()

    def shutdown(self, grace=10.0):
        """Graceful stop: refuse new connections, close idle ones, let requests in progress finish (with
        "Connection: close"), and cut whatever is still open after `grace` seconds (default 10). listen() returns
        when done. Does nothing when not listening; stop() is the immediate one."""
        if isinstance(grace, bool) or not isinstance(grace, (int, float)):
            raise TypeError("grace must be a number of seconds")
        if not (0 <= grace < 1e9):  # also false for nan
            raise ValueError("grace must be a number of seconds from 0 up")
        self._srv.shutdown(cpp_milliseconds(<long long>(grace * 1000)))
