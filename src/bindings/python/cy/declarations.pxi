# C++ declarations (cdef extern) for the client, server, download, URL and progress classes. Included by httpp_cy.pyx (a textual include: the order of the includes matters).

cdef extern from "httpp/client.hpp" namespace "httpp":
    cdef cppclass response:
        int status
        string body
        vector[pair[string, string]] headers
        string error_message
        string file  # server side: stream the body from this path
        cbool ok()
        const char* error_name()

    cdef cppclass cpp_client "httpp::client":
        cpp_client(string host, int port, cbool use_ssl) except +
        cpp_client(string base_url) except +
        response get(string path) except + nogil
        response send(string method, string path, vector[pair[string, string]] headers,
                      string body, string content_type) except + nogil
        cpp_client& set_header(string name, string value) except +
        cpp_client& set_timeout(long seconds) except +
        cpp_client& set_follow_redirects(cbool enable) except +
        cpp_client& set_basic_auth(string user, string password) except +
        cpp_client& set_bearer_token(string token) except +
        cpp_client& set_proxy(string proxy_url) except +
        cpp_client& set_verify(cbool enable) except +
        cpp_client& set_ca_file(string path) except +
        cpp_client& set_keep_alive(cbool enable) except +

        @staticmethod
        response fetch(string full_url) except + nogil


cdef extern from "httpp/server.hpp" namespace "httpp":
    cdef cppclass cpp_upload_file "httpp::upload_file":
        string field
        string filename
        string content_type
        string data

    cdef cppclass cpp_form_data "httpp::form_data":
        vector[pair[string, string]] fields
        vector[cpp_upload_file] files

    cdef cppclass cpp_request "httpp::server_request":
        string method
        string path
        string body
        vector[pair[string, string]] headers
        vector[pair[string, string]] query
        vector[pair[string, string]] path_params
        cbool secure
        string client_ip
        cpp_form_data form() except +  # std::invalid_argument -> ValueError

    cdef cppclass cpp_handler "httpp::handler":
        pass

    cdef cppclass cpp_responder "httpp::responder":
        cpp_responder()
        cpp_responder(const cpp_responder&)
        void send(response res) except + nogil

    cdef cppclass cpp_async_handler "httpp::async_handler":
        pass

    cdef cppclass cpp_shared_handler "httpp::shared_handler":
        pass

    cdef cppclass cpp_io_handler "httpp::io_handler":
        pass

    cdef cppclass cpp_before_hook "httpp::server::before_hook":
        pass

    cdef cppclass cpp_server "httpp::server":
        cpp_server() except +
        void route(string method, string path, cpp_handler h) except +
        void route_async(string method, string path, cpp_async_handler h) except +
        void route_shared(string method, string path, cpp_shared_handler h) except +
        void route_io(string method, string path, cpp_io_handler h) except +
        void serve_directory(string mount_path, string local_dir) except +
        # after_hook / error_hook are the same std::function type as handler, so cpp_handler stands for them.
        cpp_server& before(cpp_before_hook h) except +
        cpp_server& after(cpp_handler h) except +
        cpp_server& set_error_handler(cpp_handler h) except +
        cpp_server& set_thread_pool(size_t base, size_t max, microseconds grow_after) except +
        cpp_server& set_read_timeout(int seconds) except +
        cpp_server& set_write_timeout(int seconds) except +
        cpp_server& set_keep_alive_timeout(int seconds) except +
        cpp_server& set_keep_alive_max(size_t requests) except +
        cpp_server& set_max_body(size_t nbytes) except +
        cpp_server& set_header_timeout(int seconds) except +
        cpp_server& set_body_timeout(int seconds) except +
        cpp_server& set_compression(size_t min_size, int level) except +
        cpp_server& set_max_connections(size_t n) except +
        cpp_server& set_async_timeout(int seconds) except +
        cpp_server& set_reuse_port(cbool on) except +
        cpp_server& set_io_threads(size_t n) except +
        int bind_to_any_port(string host) except +
        cbool listen_after_bind() except + nogil
        cbool listen(string host, int port) except + nogil
        void stop() except +
        void shutdown(cpp_milliseconds grace) except +
        cpp_server& set_tls(const string& cert_file, const string& key_file, const string& key_password) except +


# Bridge from a plain C function pointer (+ context) to httpp::handler, which
# is a std::function Cython can't build itself.
cdef extern from *:
    """
    #include "httpp/server.hpp"
    #include <memory>
    #include <stdexcept>
    typedef bool (*httpp_py_before_trampoline)(void*, const httpp::server_request&, httpp::response&);
    static httpp::server::before_hook httpp_py_make_before(httpp_py_before_trampoline fn, void* ctx) {
        return [fn, ctx](const httpp::server_request& q, httpp::response& r) { return fn(ctx, q, r); };
    }
    // Body pulled piece by piece from Python. `ctx` is an owned reference: the shared_ptr (copied along
    // with the std::function) releases it once the last copy is gone, wherever that happens.
    // next(): 1 = piece in `chunk`, 0 = done, <0 = failed (-> throw -> httpp drops the connection).
    typedef int (*httpp_py_stream_fn)(void*, std::size_t, std::string&);
    typedef void (*httpp_py_release_fn)(void*);
    static void httpp_py_set_stream(httpp::response& res, httpp_py_stream_fn next, httpp_py_release_fn release, void* ctx) {
        std::shared_ptr<void> keep(ctx, release);
        res.stream = [next, keep](std::size_t offset, std::string& chunk) {
            const int r = next(keep.get(), offset, chunk);
            if (r < 0) throw std::runtime_error("python stream failed");
            return r > 0;
        };
    }
    // The job to run after the answer was written (response::after_send). `ctx` is an owned reference, released
    // when the last copy of the std::function is gone, wherever that happens.
    typedef void (*httpp_py_run_fn)(void*);
    static void httpp_py_set_after_send(httpp::response& res, httpp_py_run_fn run, httpp_py_release_fn release, void* ctx) {
        std::shared_ptr<void> keep(ctx, release);
        res.after_send = [run, keep]() { run(keep.get()); };
    }
    static void httpp_py_reset(httpp::response& res) { res = httpp::response(); }
    // Pool threads are not Python threads. PyGILState_Ensure/Release would create a thread state on
    // every request and destroy it again (the counter drops to 0), which is very slow on free-threaded
    // CPython. One Ensure per thread that is never released keeps the state alive, so the later
    // `with gil` blocks only attach and detach it.
    // ponytail: the state of an exited pool thread is freed at interpreter shutdown, not earlier.
    static void httpp_py_pin_thread() {
        static thread_local bool pinned = false;
        if (pinned) return;
        pinned = true;
        PyGILState_Ensure();
        PyEval_SaveThread();
    }
    typedef void (*httpp_py_trampoline)(void*, const httpp::server_request&, httpp::response&);
    static httpp::handler httpp_py_make_handler(httpp_py_trampoline fn, void* ctx) {
        return [fn, ctx](const httpp::server_request& q, httpp::response& r) { fn(ctx, q, r); };
    }
    typedef void (*httpp_py_async_trampoline)(void*, const httpp::server_request&, const httpp::responder&);
    static httpp::async_handler httpp_py_make_async_handler(httpp_py_async_trampoline fn, void* ctx) {
        return [fn, ctx](const httpp::server_request& q, httpp::responder r) { fn(ctx, q, r); };
    }
    typedef void (*httpp_py_shared_trampoline)(void*, std::shared_ptr<const httpp::server_request>, httpp::response&);
    static httpp::shared_handler httpp_py_make_shared_handler(httpp_py_shared_trampoline fn, void* ctx) {
        return [fn, ctx](std::shared_ptr<const httpp::server_request> q, httpp::response& r) { fn(ctx, std::move(q), r); };
    }
    typedef void (*httpp_py_io_trampoline)(void*, std::shared_ptr<const httpp::server_request>, const httpp::responder&);
    static httpp::io_handler httpp_py_make_io_handler(httpp_py_io_trampoline fn, void* ctx) {
        return [fn, ctx](std::shared_ptr<const httpp::server_request> q, httpp::responder r) { fn(ctx, std::move(q), r); };
    }
    """
    void httpp_py_pin_thread() noexcept nogil
    ctypedef void (*trampoline_fn "httpp_py_trampoline")(void*, const cpp_request&, response&) noexcept nogil
    cpp_handler make_handler "httpp_py_make_handler"(trampoline_fn fn, void* ctx)
    ctypedef void (*async_trampoline_fn "httpp_py_async_trampoline")(void*, const cpp_request&, const cpp_responder&) noexcept nogil
    cpp_async_handler make_async_handler "httpp_py_make_async_handler"(async_trampoline_fn fn, void* ctx)
    ctypedef void (*shared_trampoline_fn "httpp_py_shared_trampoline")(void*, shared_ptr[const cpp_request], response&) noexcept nogil
    cpp_shared_handler make_shared_handler "httpp_py_make_shared_handler"(shared_trampoline_fn fn, void* ctx)
    ctypedef void (*io_trampoline_fn "httpp_py_io_trampoline")(void*, shared_ptr[const cpp_request], const cpp_responder&) noexcept nogil
    cpp_io_handler make_io_handler "httpp_py_make_io_handler"(io_trampoline_fn fn, void* ctx)
    ctypedef cbool (*before_trampoline_fn "httpp_py_before_trampoline")(void*, const cpp_request&, response&) noexcept nogil
    cpp_before_hook make_before "httpp_py_make_before"(before_trampoline_fn fn, void* ctx)
    ctypedef int (*stream_fn "httpp_py_stream_fn")(void*, size_t, string&) noexcept nogil
    ctypedef void (*release_fn "httpp_py_release_fn")(void*) noexcept nogil
    ctypedef void (*run_fn "httpp_py_run_fn")(void*) noexcept nogil
    void set_after_send "httpp_py_set_after_send"(response& res, run_fn run, release_fn release, void* ctx)


# The "gate" and the busy count. They let an `async def` handler that cannot suspend run on the I/O thread
# (no hop to the asyncio loop thread) without breaking two rules:
#  1. The I/O thread never waits for the GIL. So it only takes the inline path while no `def` handler, hook
#     or stream callback is running or waiting for the GIL on another C++ thread (`busy` is zero). Python
#     threads that httpp does not know about (user threads, executors) can still hold the GIL; that wait is
#     bounded by the interpreter's switch interval, and is the one case this does not cover.
#  2. Code that runs on an asyncio loop never runs at the same time as other code of that loop (asyncio code
#     between two awaits is atomic, and user code relies on it). The loop thread owns the gate while it runs
#     callbacks and gives it up only while it sleeps in its selector (see httpp/server.py `_GatedSelector`).
#     The I/O thread may use the loop only while it holds the gate. `loop_waits` keeps the I/O thread from
#     re-taking the gate for ever while the loop thread wants it back.
cdef extern from *:
    """
    #include <atomic>
    #include <mutex>
    // A "lane" is one I/O thread together with one asyncio loop thread: its own inbox, its own gate. With a GIL
    // there is a single lane (more loops would only fight for it); on a free-threaded Python there is one per
    // I/O thread. An I/O thread always feeds the lane it was given the first time it dispatched a request.
    static constexpr int HTTPP_PY_MAX_LANES = 64;
    struct httpp_py_gate_lane {
        std::mutex m;
        std::atomic<bool> loop_waits{false};
    };
    struct httpp_py_state {
        httpp_py_gate_lane gates[HTTPP_PY_MAX_LANES];
        std::atomic<int> nlanes{1};
        std::atomic<int> next_lane{0};
        std::atomic<int> busy{0};
        std::atomic<bool> gil_free{false};
    };
    // Leaked on purpose: a late call during interpreter exit must still find it alive.
    static httpp_py_state& httpp_py_st() { static httpp_py_state* g = new httpp_py_state; return *g; }
    static void httpp_py_busy_enter() { ++httpp_py_st().busy; }
    static void httpp_py_busy_exit() { --httpp_py_st().busy; }
    static void httpp_py_set_lanes(int n, bool gil_free) {
        auto& st = httpp_py_st();
        if (n < 1) n = 1;
        if (n > HTTPP_PY_MAX_LANES) n = HTTPP_PY_MAX_LANES;
        st.nlanes = n;
        st.next_lane = 0; // the I/O threads of the next listen() start again from lane 0
        st.gil_free = gil_free;
    }
    static int httpp_py_my_lane() {
        static thread_local int lane = -1;
        auto& st = httpp_py_st();
        if (lane < 0) lane = st.next_lane++;
        return lane % st.nlanes.load();
    }
    static bool httpp_py_gate_try(int lane) {
        auto& st = httpp_py_st();
        auto& g = st.gates[lane];
        if (g.loop_waits.load()) return false;
        if (!st.gil_free.load() && st.busy.load() != 0) return false;
        return g.m.try_lock();
    }
    static void httpp_py_gate_unlock(int lane) { httpp_py_st().gates[lane].m.unlock(); }
    static void httpp_py_gate_loop_enter(int lane) {
        auto& g = httpp_py_st().gates[lane];
        g.loop_waits = true;
        g.m.lock();
        g.loop_waits = false;
    }
    static void httpp_py_gate_loop_leave(int lane) { httpp_py_st().gates[lane].m.unlock(); }
    """
    void busy_enter "httpp_py_busy_enter"() noexcept nogil
    void busy_exit "httpp_py_busy_exit"() noexcept nogil
    void set_lanes_c "httpp_py_set_lanes"(int n, bint gil_free) noexcept nogil
    int my_lane "httpp_py_my_lane"() noexcept nogil
    bint gate_try "httpp_py_gate_try"(int lane) noexcept nogil
    void gate_unlock "httpp_py_gate_unlock"(int lane) noexcept nogil
    void gate_loop_enter "httpp_py_gate_loop_enter"(int lane) noexcept nogil
    void gate_loop_leave "httpp_py_gate_loop_leave"(int lane) noexcept nogil
    void reset_response "httpp_py_reset"(response& res)
    void set_stream "httpp_py_set_stream"(response& res, stream_fn next, release_fn release, void* ctx)


cdef extern from "httpp/download.hpp" namespace "httpp":
    cdef cppclass download_result:
        cbool ok
        cbool skipped
        int status
        string error
        string sha256

    cdef cppclass microseconds "std::chrono::microseconds":
        microseconds(long long)
    cdef cppclass cpp_milliseconds "std::chrono::milliseconds":
        cpp_milliseconds(long long)

    cdef cppclass cpp_download "httpp::download":
        cpp_download(string url, string dest_path) except +
        cpp_download& output(string dest_path) except +
        cpp_download& enable_progress(cbool enable) except +
        cpp_download& disable_progress() except +
        cpp_download& follow_redirects(cbool enable) except +
        cpp_download& resume(cbool enable) except +
        cpp_download& force(cbool enable) except +
        cpp_download& sha256(string hex) except +
        cpp_download& sha256_url(string checksum_url) except +
        cpp_download& set_timeout(long seconds) except +
        cpp_download& set_max_wait(cpp_milliseconds max_wait) except +
        download_result run() except + nogil


cdef extern from "httpp/sha256.hpp" namespace "httpp":
    string cpp_sha256_hex "httpp::sha256_hex"(const void* data, size_t size) except + nogil
    string cpp_sha256_file "httpp::sha256_file"(string path) except + nogil
    string cpp_parse_sha256_checksum "httpp::parse_sha256_checksum"(string text, string file_name) except +


cdef extern from "httpp/progress.hpp" namespace "httpp::progress":
    cdef cppclass cpp_bar "httpp::progress::bar":
        cpp_bar(size_t total, string description) except +
        void update(size_t n) except +
        void set_progress(size_t current) except +
        void finish() except +
        size_t total()
        size_t current()
        cbool is_finished()


cdef extern from "httpp/url.hpp" namespace "httpp":
    cdef cppclass cpp_url "httpp::url":
        @staticmethod
        cpp_url parse(const string& raw) except +
        @staticmethod
        string encode_component(const string& text) except +
        @staticmethod
        string decode_component(const string& text) except +
        @staticmethod
        string build_query(const vector[pair[string, string]]& params) except +
        cbool valid()
        const string& scheme()
        const string& host()
        bint is_http()
        const string& username()
        const string& password()
        int port()
        const string& path()
        const string& query()


cdef extern from "httpp/client.hpp" namespace "httpp::client":
    cdef cppclass cpp_curl_request "httpp::request":
        cpp_curl_request(string url) except +
        cpp_curl_request& method(string m) except +
        cpp_curl_request& header(string name, string value) except +
        cpp_curl_request& data(string body) except +
        cpp_curl_request& content_type(string type) except +
        cpp_curl_request& param(string name, string value) except +
        cpp_curl_request& json(string document) except +
        cpp_curl_request& form(vector[pair[string, string]] fields) except +
        cpp_curl_request& basic_auth(string user, string password) except +
        cpp_curl_request& bearer(string token) except +
        cpp_curl_request& cookie(string name, string value) except +
        cpp_curl_request& timeout(long seconds) except +
        cpp_curl_request& follow_redirects(cbool enable) except +
        cpp_curl_request& verify(cbool enable) except +
        cpp_curl_request& ca_file(string path) except +
        cpp_curl_request& proxy(string proxy_url) except +
        cpp_curl_request& max_response_size(size_t bytes) except +
        response run() except + nogil
