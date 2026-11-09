# truly asynchronous requests (Request.run_async, AsyncClient core). Included by httpp_cy.pyx (textual include: order matters, see the list there).

# ---------------------------------------------------------------------------------------------
# Truly asynchronous requests. Request.run_async() and AsyncClient hand the request to the C++ network
# thread (Asio + llhttp, no thread per request); when the response is ready that thread takes the GIL
# just long enough to schedule the result on the asyncio loop that asked for it.
# ---------------------------------------------------------------------------------------------

cdef extern from "httpp/client.hpp" namespace "httpp":
    cdef cppclass cpp_async_client "httpp::async_client":
        cpp_async_client(string base_url, size_t max_connections) except +
        cpp_async_client& set_header(string name, string value) except +
        cpp_async_client& set_timeout(long seconds) except +
        cpp_async_client& set_follow_redirects(cbool enable) except +
        cpp_async_client& set_basic_auth(string user, string password) except +
        cpp_async_client& set_bearer_token(string token) except +
        cpp_async_client& set_verify(cbool enable) except +
        cpp_async_client& set_ca_file(string path) except +
        cpp_async_client& set_proxy(string host, int port) except +

cdef extern from *:
    """
    #include <functional>
    #include "httpp/client.hpp"
    typedef void (*httpp_py_done_fn)(void*, httpp::response*);
    static void httpp_py_run_request(const httpp::client::request& r, void* ctx, httpp_py_done_fn fn) {
        r.run_async([ctx, fn](httpp::response res) { fn(ctx, &res); });
    }
    static void httpp_py_client_send(httpp::async_client& c, const std::string& method, const std::string& path,
                                     const std::vector<std::pair<std::string, std::string>>& headers,
                                     const std::string& body, const std::string& ctype, void* ctx, httpp_py_done_fn fn) {
        c.send(method, path, headers, body, ctype, [ctx, fn](httpp::response res) { fn(ctx, &res); });
    }
    """
    ctypedef void (*done_fn "httpp_py_done_fn")(void*, response*) noexcept nogil
    void httpp_py_run_request(const cpp_curl_request&, void*, done_fn) except + nogil
    void httpp_py_client_send(cpp_async_client&, const string&, const string&, const vector[pair[string, string]]&,
                              const string&, const string&, void*, done_fn) except + nogil


def _resolve(fut, value):
    # runs on the event loop thread; the awaiting task may have been cancelled meanwhile
    if not fut.done():
        fut.set_result(value)


cdef void _done_trampoline(void* ctx, response* res) noexcept nogil:
    with gil:
        holder = <object>ctx
        Py_DECREF(holder)  # balances the Py_INCREF made when the request was started
        try:
            loop, fut = holder
            result = _make_response(res[0])
            loop.call_soon_threadsafe(_resolve, fut, result)
        except BaseException:
            pass  # e.g. the loop was closed: nobody is waiting any more


cdef class _AsyncClientCore:
    """The C++ httpp::async_client: a base URL, shared defaults and at most `max_connections`
    requests in flight, on the non-blocking network thread. Used by httpp.AsyncClient."""
    cdef cpp_async_client* _cli

    def __cinit__(self):
        self._cli = NULL

    def __init__(self, str base_url, size_t max_connections=8, *, headers=None, timeout=None,
                 cbool follow_redirects=False, auth=None, token=None, cbool verify=True, str ca_file=None,
                 proxy=None):
        self._cli = new cpp_async_client(base_url.encode("utf-8"), max_connections)
        _apply_client_options(self._cli, headers, timeout, follow_redirects, auth, token, verify, ca_file, proxy)

    def __dealloc__(self):
        if self._cli is not NULL:
            del self._cli

    def request(self, str method, str path, *, params=None, headers=None, data=None, json=None, form=None,
                str content_type=None):
        """Start one request and return an asyncio Future for its Response (await it)."""
        import asyncio
        cdef string m = method.encode("utf-8")
        cdef string p = _with_params(path, params).encode("utf-8")
        cdef string body
        cdef string ctype
        cdef vector[pair[string, string]] hdrs = _header_pairs(headers)
        if json is not None:
            body = _json.dumps(json).encode("utf-8")
            ctype = b"application/json"
        elif form is not None:
            body = _encode_query(form).encode("utf-8")
            ctype = b"application/x-www-form-urlencoded"
        elif data is not None:
            body = _to_bytes(data)
        if content_type is not None:
            ctype = content_type.encode("utf-8")
        loop = asyncio.get_running_loop()
        fut = loop.create_future()
        holder = (loop, fut)
        Py_INCREF(holder)  # owned by the C++ callback until _done_trampoline releases it
        try:
            with nogil:
                httpp_py_client_send(self._cli[0], m, p, hdrs, body, ctype, <void*>holder, <done_fn>_done_trampoline)
        except BaseException:
            Py_DECREF(holder)
            raise
        return fut
