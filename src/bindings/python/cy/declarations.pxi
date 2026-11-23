# C++ declarations (cdef extern) for the client, server, download, URL and progress classes. Included by httpp_python.pyx (a textual include: the order of the includes matters).

cdef extern from "httpp/client.hpp" namespace "httpp":
    cdef cppclass response:
        int status
        string body
        vector[pair[string, string]] headers
        string error_message
        string file  # server side: stream the body from this path
        long long elapsed_ms
        size_t bytes_received
        string saved_as
        cbool ok()
        const char* error_name()

    cdef cppclass cpp_client "httpp::client":
        cpp_client(string host, int port, cbool use_ssl) except +
        cpp_client(string base_url) except +
        response get(string path) except + nogil
        response send(string method, string path, vector[pair[string, string]] headers,
                      string body, string content_type) except + nogil
        cpp_client& header(string name, string value) except +
        cpp_client& timeout(double seconds) except +
        cpp_client& max_redirects(int count) except +  # std::invalid_argument -> ValueError
        cpp_client& connect_timeout(double seconds) except +
        cpp_client& max_time(double seconds) except +
        cpp_client& retry(int times) except +
        cpp_client& retry_delay(double seconds) except +
        cpp_client& retry_connrefused(cbool enable) except +
        cpp_client& auto_compression(cbool enable) except +
        cpp_client& basic_auth(string user, string password) except +
        cpp_client& bearer(string token) except +
        cpp_client& proxy(string proxy_url) except +
        cpp_client& verify(cbool enable) except +
        cpp_client& ca_file(string path) except +
        cpp_client& keep_alive(cbool enable) except +

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
        cpp_server& before_request(cpp_before_hook h) except +
        cpp_server& after_request(cpp_handler h) except +
        cpp_server& error_handler(cpp_handler h) except +
        cpp_server& thread_pool(size_t base, size_t max, microseconds grow_after) except +
        cpp_server& read_timeout(int seconds) except +
        cpp_server& write_timeout(int seconds) except +
        cpp_server& keep_alive_timeout(int seconds) except +
        cpp_server& keep_alive_max(size_t requests) except +
        cpp_server& max_body(size_t nbytes) except +
        cpp_server& max_form_body(size_t nbytes) except +
        cpp_server& header_timeout(int seconds) except +
        cpp_server& body_timeout(int seconds) except +
        cpp_server& compression(size_t min_size, int level) except +
        cpp_server& max_connections(size_t n) except +
        cpp_server& async_timeout(int seconds) except +
        cpp_server& reuse_port(cbool on) except +
        cpp_server& io_threads(size_t n) except +
        int bind_to_any_port(string host) except +
        cbool bind(string host, int port) except +
        string url() except +
        cbool listen_after_bind() except + nogil
        cbool listen(string host, int port) except + nogil
        void stop() except +
        void shutdown(cpp_milliseconds grace) except +
        cpp_server& tls(const string& cert_file, const string& key_file, const string& key_password) except +


# The route-path grammar lives in C++ (include/httpp/route_pattern.hpp); Python asks the same parser instead of keeping its own.
cdef extern from *:
    """
    #include "httpp/route_pattern.hpp"
    #include <string>
    #include <vector>
    namespace httpp_py {
    // One entry per segment: {"lit", text, ""} or {"param", name, "str" | "int" | "float"}. Throws std::invalid_argument (-> ValueError).
    inline std::vector<std::vector<std::string>> route_segments(const std::string& path) {
        std::vector<std::vector<std::string>> out;
        for (const httpp::route_segment& s : httpp::parse_route_pattern(path)) {
            if (!s.is_param) { out.push_back({"lit", s.text, ""}); continue; }
            out.push_back({"param", s.text, s.type == httpp::param_type::integer ? "int" : s.type == httpp::param_type::number ? "float" : "str"});
        }
        return out;
    }
    }
    """
    vector[vector[string]] httpp_py_route_segments "httpp_py::route_segments"(string path) except +


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
        cpp_milliseconds(long long) noexcept nogil
        long long count() noexcept nogil

    cdef cppclass cpp_download "httpp::download":
        cpp_download(string url, string dest_path) except +
        cpp_download& header(string name, string value) except +  # std::invalid_argument -> ValueError
        cpp_download& output(string dest_path) except +
        cpp_download& enable_progress(cbool enable) except +
        cpp_download& disable_progress() except +
        cpp_download& max_redirects(int count) except +  # std::invalid_argument -> ValueError
        int max_redirects() except +
        cpp_download& resume(cbool enable) except +
        cpp_download& force(cbool enable) except +
        cpp_download& sha256(string hex) except +
        cpp_download& sha256_url(string checksum_url) except +
        cpp_download& timeout(double seconds) except +
        double timeout() except +
        cpp_download& connect_timeout(double seconds) except +  # std::invalid_argument -> ValueError
        cpp_download& max_time(double seconds) except +  # std::invalid_argument -> ValueError
        cpp_download& retry(int times) except +
        cpp_download& retry_delay(double seconds) except +
        cpp_download& retry_connrefused(cbool enable) except +
        cpp_download& basic_auth(string user, string password) except +
        cpp_download& bearer(string token) except +
        cpp_download& proxy(string proxy_url) except +  # std::invalid_argument -> ValueError
        cpp_download& verify(cbool enable) except +
        cpp_download& ca_file(string path) except +
        cpp_download& max_wait(cpp_milliseconds wait) except +
        cpp_milliseconds max_wait() except +
        download_result run() except + nogil


cdef extern from "httpp/file_lock.hpp" namespace "httpp":
    cdef cppclass cpp_file_lock "httpp::file_lock":
        cpp_file_lock(string target, cpp_milliseconds stale_after) except + nogil
        cbool owned() noexcept
        cbool waited() noexcept

cdef extern from "httpp/utils.hpp" namespace "httpp":
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
        cbool finished()


cdef extern from "httpp/url.hpp" namespace "httpp":
    cdef cppclass cpp_url "httpp::url":  # urlparser::url (ADR-0067)
        cpp_url()
    cpp_url cpp_parse_url "httpp::parse_url"(const string& raw) except +
    cbool cpp_url_valid "httpp::url_valid"(const cpp_url& u)
    cbool cpp_url_is_http "httpp::url_is_http"(const cpp_url& u)
    string cpp_url_scheme "httpp::url_scheme"(const cpp_url& u) except +
    string cpp_url_host "httpp::url_host"(const cpp_url& u) except +
    string cpp_url_username "httpp::url_username"(const cpp_url& u) except +
    string cpp_url_password "httpp::url_password"(const cpp_url& u) except +
    int cpp_url_port "httpp::url_port"(const cpp_url& u)
    string cpp_url_path "httpp::url_path"(const cpp_url& u) except +
    string cpp_url_query "httpp::url_query"(const cpp_url& u) except +
    string cpp_url_filename "httpp::url_filename"(const cpp_url& u) except +
    string cpp_encode_component "httpp::encode_component"(const string& text) except +
    string cpp_decode_component "httpp::decode_component"(const string& text) except +
    string cpp_build_query "httpp::build_query"(const vector[pair[string, string]]& params) except +
    void cpp_fetch_public_suffix_list "httpp::fetch_public_suffix_list"(const string& source, long timeout_seconds) except + nogil


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
        cpp_curl_request& timeout(double seconds) except +
        cpp_curl_request& max_redirects(int count) except +  # std::invalid_argument -> ValueError
        cpp_curl_request& auto_compression(cbool enable) except +
        cpp_curl_request& connect_timeout(double seconds) except +  # std::invalid_argument -> ValueError
        cpp_curl_request& max_time(double seconds) except +  # std::invalid_argument -> ValueError
        cpp_curl_request& retry(int times) except +
        cpp_curl_request& retry_delay(double seconds) except +
        cpp_curl_request& retry_connrefused(cbool enable) except +
        cpp_curl_request& save_to(string file) except +
        cpp_curl_request& save_in(string folder) except +
        cpp_curl_request& create_dirs(cbool enable) except +
        cpp_curl_request& overwrite(cbool enable) except +
        cpp_curl_request& include_head(cbool enable) except +
        cpp_curl_request& verify(cbool enable) except +
        cpp_curl_request& ca_file(string path) except +
        cpp_curl_request& proxy(string proxy_url) except +
        cpp_curl_request& max_response_size(size_t bytes) except +
        response run() except + nogil
