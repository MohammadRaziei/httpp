# async route completion, WebSocket connection, loop lanes and the inbox. Included by httpp_cy.pyx (textual include: order matters, see the list there).

cdef class _Done:
    """One-shot handle for an async route: calling it sends the response and
    closes the connection. Safe to call from any thread; extra calls are ignored.
    If it is dropped without being called, the connection is closed unanswered."""
    cdef cpp_responder* _r

    def __cinit__(self):
        self._r = NULL

    def __dealloc__(self):
        if self._r != NULL:
            del self._r
            self._r = NULL

    def __call__(self, int status, object body, object headers):  # a list, or a list subclass carrying after_send
        cdef response res
        _fill_response(res, status, body, headers)
        with nogil:
            self._r.send(res)


cdef void _start_dispatch(void* ctx, const cpp_request& req, const cpp_responder& r) noexcept:
    # GIL held. Hands the request to the route's `dispatch`, which must return quickly; the
    # response is sent later through `done`. Never raises: a failing dispatch answers 500.
    done = _Done.__new__(_Done)
    (<_Done>done)._r = new cpp_responder(r)
    try:
        (<object>ctx)(*_request_args(req), done)
    except BaseException:
        traceback.print_exc()
        try:
            done(500, b"Internal Server Error", [])
        except BaseException:
            pass


cdef void _dispatch_async(void* ctx, const cpp_request& req, const cpp_responder& r) noexcept nogil:
    # Runs on a pool thread that Python knows nothing about, hence the explicit GIL acquire.
    busy_enter()  # see the gate comment above
    try:
        with gil:
            _start_dispatch(ctx, req, r)
    finally:
        busy_exit()


# The "loop inbox": requests for `async def` routes go from the C++ side to the asyncio loop thread
# WITHOUT the C++ thread ever touching the GIL. The C++ thread queues the request and writes one byte
# to a socket the loop is watching; the loop thread wakes up, takes the GIL once and drains the whole
# burst (see httpp/server.py `_get_loop`). A wake byte is only written when the queue goes from
# empty to non-empty, so a burst costs one wake-up, not one per request.
cdef extern from *:
    """
    #include <atomic>
    #include <cerrno>
    #include <deque>
    #include <mutex>
    #include <vector>
    #ifdef _WIN32
    #include <winsock2.h>
    #else
    #include <sys/socket.h>
    #endif
    // One WebSocket connection as seen from Python: a mailbox that the I/O thread fills (no GIL) and the loop thread
    // empties. See ADR-0027.
    struct httpp_py_wsconn {
        std::mutex mu;
        std::deque<std::pair<std::string, bool>> q;  // (message, binary)
        std::size_t bytes = 0, high = 1u << 20;     // queued bytes; reading pauses at `high`, resumes at half of it
        bool paused = false, opened = false, closed = false, drain = false, answered = false;
        bool want = false, armed = false;           // Python waits for news / a wake item is on its way
        std::uint16_t code = 1006;
        std::string reason;
        int lane = 0;
        httpp::websocket ws;                        // set on open, dropped on close (it holds this object through user())
        httpp::ws_acceptor acceptor;
        void* waiter = nullptr;                     // the Python WebSocket, owned (one reference) until release
    };
    // ctx + req + rsp: a request. ws without wake: a WebSocket upgrade (ctx, req). ws with wake: news for a waiting handler.
    struct httpp_py_item {
        void* ctx; std::shared_ptr<const httpp::request> req; httpp::responder rsp;
        std::shared_ptr<httpp_py_wsconn> ws; bool wake = false;
    };
    struct httpp_py_inbox {
        std::mutex mu;
        std::deque<httpp_py_item> q;
        bool armed = false;               // a wake byte is on its way (or the loop is draining)
        std::atomic<long long> wake_fd{-1};
    };
    // Leaked on purpose: a late push during interpreter exit must still find it alive. One inbox per lane.
    static httpp_py_inbox& httpp_py_box(int lane) { static httpp_py_inbox* b = new httpp_py_inbox[64]; return b[lane]; }
    static void httpp_py_inbox_attach(int lane, int fd) { httpp_py_box(lane).wake_fd = fd; }
    static void httpp_py_inbox_push_item(int lane, httpp_py_item&& item) {
        auto& b = httpp_py_box(lane);
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(b.mu);
            b.q.push_back(std::move(item));
            if (!b.armed) b.armed = wake = true;
        }
        const long long fd = b.wake_fd.load();
        if (!wake || fd < 0) return; // not attached yet: the attaching side drains once itself
        const char one = 1;
        const long long n = ::send(static_cast<decltype(::socket(0, 0, 0))>(fd), &one, 1, 0);
        // A full socket buffer (EAGAIN) means wake bytes are already waiting; any other failure
        // must not leave the inbox "armed" forever.
        if (n < 0) {
    #ifdef _WIN32
            const bool full = WSAGetLastError() == WSAEWOULDBLOCK;
    #else
            const bool full = errno == EAGAIN || errno == EWOULDBLOCK;
    #endif
            if (!full) { std::lock_guard<std::mutex> lock(b.mu); b.armed = false; }
        }
    }
    static void httpp_py_inbox_push(int lane, void* ctx, std::shared_ptr<const httpp::request> q, const httpp::responder& r) {
        httpp_py_inbox_push_item(lane, httpp_py_item{ctx, std::move(q), r});
    }
    // Moves up to `max` queued items into `out` under ONE lock. Returns 0 (and disarms, atomically with the
    // emptiness check) when nothing is left. A lock per item cost about 2.5 us per request under load.
    static std::size_t httpp_py_inbox_pop_many(int lane, std::vector<httpp_py_item>& out, std::size_t max) {
        auto& b = httpp_py_box(lane);
        std::lock_guard<std::mutex> lock(b.mu);
        if (b.q.empty()) { b.armed = false; return 0; }
        std::size_t n = 0;
        while (n < max && !b.q.empty()) { out.push_back(std::move(b.q.front())); b.q.pop_front(); ++n; }
        return n;
    }

    // ---- WebSocket glue. The callbacks run on the I/O thread and never touch the GIL. ----
    using httpp_py_wsp = std::shared_ptr<httpp_py_wsconn>;
    // Tell the loop thread that something changed, if it asked to be told (and has not been told yet).
    static void httpp_py_ws_notify(const httpp_py_wsp& c, bool wake) {
        if (!wake) return;
        httpp_py_item it{nullptr, nullptr, httpp::responder{}, c, true};
        httpp_py_inbox_push_item(c->lane, std::move(it));
    }
    static bool httpp_py_ws_take_wake(httpp_py_wsconn& c) {  // under c.mu
        if (!c.want || c.armed) return false;
        return c.armed = true;
    }
    static httpp_py_wsp httpp_py_ws_of(const httpp::websocket& ws) { return std::static_pointer_cast<httpp_py_wsconn>(ws.user()); }
    static httpp::ws_handler httpp_py_make_ws_handler(void* ctx, std::size_t max_message, std::size_t max_buffered,
                                                      int ping_interval, std::size_t high) {
        httpp::ws_handler h;
        h.max_message = max_message; h.max_buffered = max_buffered; h.ping_interval = ping_interval;
        h.on_handshake = [ctx, high](std::shared_ptr<const httpp::request> req, httpp::ws_acceptor acc) {
            auto c = std::make_shared<httpp_py_wsconn>();
            c->lane = httpp_py_my_lane();
            c->high = high;
            c->acceptor = std::move(acc);
            httpp_py_item it{ctx, std::move(req), httpp::responder{}, c, false};
            httpp_py_inbox_push_item(c->lane, std::move(it));
        };
        h.on_open = [](const httpp::websocket& ws) {
            auto c = httpp_py_ws_of(ws);
            bool wake;
            { std::lock_guard<std::mutex> lock(c->mu); c->ws = ws; c->opened = true; wake = httpp_py_ws_take_wake(*c); }
            httpp_py_ws_notify(c, wake);
        };
        h.on_message = [](const httpp::websocket& ws, std::string_view m, bool binary) {
            auto c = httpp_py_ws_of(ws);
            bool wake;
            {
                std::lock_guard<std::mutex> lock(c->mu);
                c->q.emplace_back(std::string(m), binary);
                c->bytes += m.size();
                // Under the lock, so a resume from the loop thread can only come after this pause.
                if (c->bytes >= c->high && !c->paused) { c->paused = true; ws.pause_reading(); }
                wake = httpp_py_ws_take_wake(*c);
            }
            httpp_py_ws_notify(c, wake);
        };
        h.on_drain = [](const httpp::websocket& ws) {
            auto c = httpp_py_ws_of(ws);
            bool wake;
            { std::lock_guard<std::mutex> lock(c->mu); c->drain = true; wake = httpp_py_ws_take_wake(*c); }
            httpp_py_ws_notify(c, wake);
        };
        h.on_close = [](const httpp::websocket& ws, std::uint16_t code, std::string_view reason) {
            auto c = httpp_py_ws_of(ws);
            httpp::websocket old;
            bool wake;
            {
                std::lock_guard<std::mutex> lock(c->mu);
                c->closed = true; c->code = code; c->reason.assign(reason);
                old = std::move(c->ws);  // breaks the cycle handle -> user -> connection -> handle
                c->ws = httpp::websocket{};
                wake = httpp_py_ws_take_wake(*c);
            }
            httpp_py_ws_notify(c, wake);
        };
        return h;
    }
    static void httpp_py_route_ws(httpp::server& s, const std::string& path, void* ctx, std::size_t max_message,
                                  std::size_t max_buffered, int ping_interval, std::size_t high) {
        s.route_ws(path, httpp_py_make_ws_handler(ctx, max_message, max_buffered, ping_interval, high));
    }
    // Python side (loop thread). `arm` = "if there is nothing yet, wake me when there is".
    static void httpp_py_ws_accept(const httpp_py_wsp& c, const std::vector<std::pair<std::string, std::string>>& headers) {
        httpp::ws_acceptor a;
        { std::lock_guard<std::mutex> lock(c->mu); if (c->answered) return; c->answered = true; a = std::move(c->acceptor); c->acceptor = httpp::ws_acceptor{}; }
        a.accept(headers, c);
    }
    static void httpp_py_ws_reject(const httpp_py_wsp& c, int status, const std::string& body) {
        httpp::ws_acceptor a;
        { std::lock_guard<std::mutex> lock(c->mu); if (c->answered) return; c->answered = true; a = std::move(c->acceptor); c->acceptor = httpp::ws_acceptor{}; }
        a.reject(status, body);
    }
    static bool httpp_py_ws_answered(const httpp_py_wsp& c) { std::lock_guard<std::mutex> lock(c->mu); return c->answered; }
    static int httpp_py_ws_wait_open(const httpp_py_wsp& c, bool arm) {
        std::lock_guard<std::mutex> lock(c->mu);
        if (c->opened) return 1;
        if (c->closed) return -1;
        if (arm) c->want = true;
        return 0;
    }
    // 1 = a message in out/binary, 0 = none yet, -1 = none and the connection is over.
    static int httpp_py_ws_pop(const httpp_py_wsp& c, std::string& out, bool& binary) {
        httpp::websocket resume;
        int r;
        {
            std::lock_guard<std::mutex> lock(c->mu);
            if (!c->q.empty()) {
                out = std::move(c->q.front().first); binary = c->q.front().second; c->q.pop_front();
                c->bytes -= out.size();
                if (c->paused && c->bytes <= c->high / 2) { c->paused = false; resume = c->ws; }
                r = 1;
            } else if (c->closed) {
                r = -1;
            } else {
                c->want = true;
                r = 0;
            }
        }
        if (resume) resume.resume_reading();
        return r;
    }
    // 0 sent, 1 closed, 2 full
    static int httpp_py_ws_send(const httpp_py_wsp& c, int kind, const std::string& data) {
        httpp::websocket w;
        { std::lock_guard<std::mutex> lock(c->mu); w = c->ws; }
        if (!w) return 1;
        const auto st = kind == 0 ? w.send_text(data) : kind == 1 ? w.send_binary(data) : w.ping(data);
        return st == httpp::websocket::status::ok ? 0 : st == httpp::websocket::status::closed ? 1 : 2;
    }
    // 1 = there is room (or the connection is over), 0 = wake me when there is.
    static int httpp_py_ws_arm_drain(const httpp_py_wsp& c) {
        std::lock_guard<std::mutex> lock(c->mu);
        if (c->closed || c->drain) { c->drain = false; return 1; }
        c->want = true;
        return 0;
    }
    static void httpp_py_ws_close(const httpp_py_wsp& c, int code, const std::string& reason) {
        httpp::websocket w;
        { std::lock_guard<std::mutex> lock(c->mu); w = c->ws; }
        if (w) w.close(static_cast<std::uint16_t>(code), reason);
    }
    static bool httpp_py_ws_is_open(const httpp_py_wsp& c) {
        std::lock_guard<std::mutex> lock(c->mu);
        return c->opened && !c->closed;
    }
    static bool httpp_py_ws_wait_closed(const httpp_py_wsp& c) {  // true once on_close ran, else asks to be woken
        std::lock_guard<std::mutex> lock(c->mu);
        if (c->closed) return true;
        c->want = true;
        return false;
    }
    static void httpp_py_ws_wake_done(const httpp_py_wsp& c) { std::lock_guard<std::mutex> lock(c->mu); c->want = false; c->armed = false; }
    static void* httpp_py_ws_waiter(const httpp_py_wsp& c) { std::lock_guard<std::mutex> lock(c->mu); return c->waiter; }
    static void httpp_py_ws_set_waiter(const httpp_py_wsp& c, void* w) { std::lock_guard<std::mutex> lock(c->mu); c->waiter = w; }
    """
    cdef cppclass ws_conn "httpp_py_wsconn":
        pass
    ctypedef shared_ptr[ws_conn] ws_conn_ptr "httpp_py_wsp"
    cdef cppclass inbox_item "httpp_py_item":
        void* ctx
        shared_ptr[const cpp_request] req
        cpp_responder rsp
        shared_ptr[ws_conn] ws
        cbool wake
        inbox_item()
    void route_ws_c "httpp_py_route_ws"(cpp_server& s, const string& path, void* ctx, size_t max_message,
                                        size_t max_buffered, int ping_interval, size_t high) except +
    void ws_accept_c "httpp_py_ws_accept"(const shared_ptr[ws_conn]& c, const vector[pair[string, string]]& headers) except + nogil
    void ws_reject_c "httpp_py_ws_reject"(const shared_ptr[ws_conn]& c, int status, const string& body) except + nogil
    cbool ws_answered_c "httpp_py_ws_answered"(const shared_ptr[ws_conn]& c) nogil
    int ws_wait_open_c "httpp_py_ws_wait_open"(const shared_ptr[ws_conn]& c, cbool arm) nogil
    int ws_pop_c "httpp_py_ws_pop"(const shared_ptr[ws_conn]& c, string& out, cbool& binary) nogil
    int ws_send_c "httpp_py_ws_send"(const shared_ptr[ws_conn]& c, int kind, const string& data) except + nogil
    int ws_arm_drain_c "httpp_py_ws_arm_drain"(const shared_ptr[ws_conn]& c) nogil
    void ws_close_c "httpp_py_ws_close"(const shared_ptr[ws_conn]& c, int code, const string& reason) except + nogil
    cbool ws_is_open_c "httpp_py_ws_is_open"(const shared_ptr[ws_conn]& c) nogil
    cbool ws_wait_closed_c "httpp_py_ws_wait_closed"(const shared_ptr[ws_conn]& c) nogil
    void ws_wake_done_c "httpp_py_ws_wake_done"(const shared_ptr[ws_conn]& c) nogil
    void* ws_waiter_c "httpp_py_ws_waiter"(const shared_ptr[ws_conn]& c) nogil
    void ws_set_waiter_c "httpp_py_ws_set_waiter"(const shared_ptr[ws_conn]& c, void* w) nogil
    void inbox_attach "httpp_py_inbox_attach"(int lane, int fd) nogil
    void inbox_push "httpp_py_inbox_push"(int lane, void* ctx, shared_ptr[const cpp_request] req, const cpp_responder& r) nogil
    size_t inbox_pop_many "httpp_py_inbox_pop_many"(int lane, vector[inbox_item]& out, size_t maxn) nogil


cdef void _dispatch_inbox(void* ctx, shared_ptr[const cpp_request] req, const cpp_responder& r) noexcept nogil:
    # Runs on an I/O thread (route_io): queue the request for that thread's asyncio loop. No GIL, no copy.
    inbox_push(my_lane(), ctx, req, r)


cdef void _start_raw(void* ctx, shared_ptr[const cpp_request] req, const cpp_responder& r, bint inline=False, int lane=0) noexcept:
    # GIL held, on a loop thread (or, for inline=True, on an I/O thread holding that lane's gate). Hands the
    # request to the route's `dispatch(raw, done)` (a hybrid route's takes two more arguments, `inline` and
    # `lane`), which must return quickly; the response is sent later through `done`. Never raises: a failing
    # dispatch answers 500.
    done = _Done.__new__(_Done)
    (<_Done>done)._r = new cpp_responder(r)
    try:
        if inline:
            (<object>ctx)(_new_raw(req), done, True, lane)
        else:
            (<object>ctx)(_new_raw(req), done)
    except BaseException:
        traceback.print_exc()
        try:
            done(500, b"Internal Server Error", [])
        except BaseException:
            pass


cdef void _dispatch_hybrid(void* ctx, shared_ptr[const cpp_request] req, const cpp_responder& r) noexcept nogil:
    # Runs on an I/O thread (route_io). The inline path only when that thread's loop is asleep and (with a GIL)
    # nothing else is running Python (see the gate comment); anything else is queued for the loop thread.
    cdef int lane = my_lane()
    if not gate_try(lane):
        inbox_push(lane, ctx, req, r)
        return
    httpp_py_pin_thread()
    with gil:
        _start_raw(ctx, req, r, True, lane)
    gate_unlock(lane)


cdef class _WsConn:
    """One WebSocket connection's mailbox, for the loop thread of its lane (see httpp/_websocket.py). Everything here is a
    thin call into the C++ glue above; nothing blocks."""
    cdef shared_ptr[ws_conn] _c

    def accept(self, list headers):
        """Answer the upgrade with 101; the connection opens a moment later (see wait_open). ValueError for a bad
        Sec-WebSocket-Protocol."""
        cdef vector[pair[string, string]] hs = _header_pairs(headers)
        ws_accept_c(self._c, hs)

    def reject(self, int status=403, bytes body=b""):
        cdef string b = body
        ws_reject_c(self._c, status, b)

    def answered(self):
        return ws_answered_c(self._c)

    def wait_open(self, bint arm=True):
        """1 open, -1 ended before it opened, 0 not yet (and, with arm, wake me)."""
        return ws_wait_open_c(self._c, arm)

    def pop(self):
        """(1, message) with a str for text and bytes for binary, (0, None) when nothing waits (armed for a wake), (-1, None) when
        the connection is over and everything was read."""
        cdef string out
        cdef cbool binary = False
        cdef int r
        with nogil:
            r = ws_pop_c(self._c, out, binary)
        if r != 1:
            return r, None
        if binary:
            return 1, <bytes>out
        return 1, (<bytes>out).decode("utf-8")  # the engine checked it

    def send(self, int kind, data):
        """0 sent, 1 closed, 2 full. kind: 0 text, 1 binary, 2 ping."""
        cdef string d = data if isinstance(data, bytes) else (<str>data).encode("utf-8")
        cdef int r
        with nogil:
            r = ws_send_c(self._c, kind, d)
        return r

    def arm_drain(self):
        """1 there is room (or it ended), 0 wake me when there is."""
        return ws_arm_drain_c(self._c)

    def close(self, int code=1000, str reason=""):
        cdef string r = reason.encode("utf-8")
        ws_close_c(self._c, code, r)

    def is_open(self):
        return ws_is_open_c(self._c)

    def wait_closed(self):
        """True once the connection has ended for good (so close_info is final); else armed for a wake."""
        return ws_wait_closed_c(self._c)

    def close_info(self):
        """(code, reason) of the end of the connection: 1006 without a close frame."""
        cdef shared_ptr[ws_conn] c = self._c
        return _ws_close_info(c)

    def set_waiter(self, waiter):
        """The object whose `_wake()` the loop calls when news arrives for a call that armed. Holds one reference until release()."""
        Py_INCREF(waiter)
        ws_set_waiter_c(self._c, <void*>waiter)

    def release(self):
        cdef void* w = ws_waiter_c(self._c)
        if w != NULL:
            ws_set_waiter_c(self._c, NULL)
            Py_DECREF(<object>w)


cdef extern from *:
    """
    static std::pair<std::uint16_t, std::string> httpp_py_ws_info(const httpp_py_wsp& c) {
        std::lock_guard<std::mutex> lock(c->mu);
        return {c->code, c->reason};
    }
    """
    pair[unsigned short, string] ws_info_c "httpp_py_ws_info"(const shared_ptr[ws_conn]& c) nogil


cdef tuple _ws_close_info(shared_ptr[ws_conn]& c):
    cdef pair[unsigned short, string] r = ws_info_c(c)
    return r.first, r.second.decode("utf-8", errors="replace")


cdef void _start_ws(void* ctx, shared_ptr[const cpp_request] req, shared_ptr[ws_conn] c) noexcept:
    # GIL held, on a loop thread: a WebSocket upgrade arrived. `dispatch(raw, core)` starts the handler's task and returns at once;
    # one that fails answers 500 (nothing was answered yet) and a dropped answer closes the connection.
    core = _WsConn.__new__(_WsConn)
    (<_WsConn>core)._c = c
    try:
        (<object>ctx)(_new_raw(req), core)
    except BaseException:
        traceback.print_exc()
        try:
            (<_WsConn>core).reject(500, b"")
        except BaseException:
            pass


cdef void _ws_wake(shared_ptr[ws_conn] c) noexcept:
    # GIL held, on the loop thread: the handler asked to be told about news.
    ws_wake_done_c(c)
    cdef void* w = ws_waiter_c(c)
    if w != NULL:
        try:
            (<object>w)._wake()
        except BaseException:
            traceback.print_exc()


def _gate_enter(int lane=0):
    """Loop thread: take the lane's gate (waits, without the GIL, while an inline handler runs on the I/O thread)."""
    with nogil:
        gate_loop_enter(lane)


def _gate_leave(int lane=0):
    """Loop thread: give the gate up, just before sleeping in the selector."""
    gate_loop_leave(lane)


def _set_lanes(int n, bint gil_free):
    """Before listening: how many lanes (I/O thread + loop thread pairs) the I/O threads are spread over."""
    set_lanes_c(n, gil_free)


def _attach_inbox(int lane, int wake_fd):
    """Tell the lane's inbox which socket to poke when requests arrive (the loop watches its other end)."""
    inbox_attach(lane, wake_fd)


def _drain_inbox(int lane=0, int budget=256):
    """Loop thread, GIL held: dispatch up to `budget` queued requests. True if some may remain, so
    the caller should call again after letting other callbacks run (a huge burst must not starve them).
    The batch is taken WITH the GIL held on purpose: the mutex is only ever held for a moment and never
    by anyone waiting for the GIL, and letting go of the GIL per request would invite a thread switch."""
    cdef vector[inbox_item] batch
    cdef size_t left = <size_t>budget
    cdef size_t n, i
    while left > 0:
        n = inbox_pop_many(lane, batch, left)  # 0 means empty, and the inbox is disarmed
        if n == 0:
            return False
        for i in range(n):
            if batch[i].ws.get() != NULL:
                if batch[i].wake:
                    _ws_wake(batch[i].ws)
                else:
                    _start_ws(batch[i].ctx, batch[i].req, batch[i].ws)
            else:
                _start_raw(batch[i].ctx, batch[i].req, batch[i].rsp)
        batch.clear()
        left -= n
    return True


cdef int _check_seconds(int seconds) except -1:
    if seconds < 0:
        raise ValueError("a timeout can't be negative")
    return 0
