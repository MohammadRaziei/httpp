// The public client classes. They are thin: what to send and which redirects to follow is
// internal/http_exchange.cpp (shared with downloads later), moving bytes is the engine
// (internal/async_http.cpp). Here: base URL handling, the fluent builders and the sync/async calling styles.
#include "httpp/client.hpp"
#include "httpp/url.hpp"

#include "internal/file_writer.hpp"
#include "internal/http_exchange.hpp"

#include <limits>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace httpp {
namespace {

using detail::endpoint;
using detail::endpoint_of;
using detail::guarded_settings;
using detail::header_list;
using detail::request_spec;
using detail::session_settings;

endpoint parse_base(const url& u, const char* who) {
    if (!url_is_http(u)) throw std::invalid_argument(std::string(who) + ": not a usable http(s) URL: " + u.str());
    return endpoint_of(u, true);
}

} // namespace

// --- settings_ref: each setting and its rule, once, for client, async_client and request (ADR-0093) ---

void detail::settings_ref::timeout(long seconds) const { g_->update([&](session_settings& s) { s.set_timeout(seconds); }); }
long detail::settings_ref::timeout() const { return g_->read([](const session_settings& s) { return s.timeout; }); }
void detail::settings_ref::connect_timeout(long seconds) const { g_->update([&](session_settings& s) { s.set_connect_timeout(seconds); }); }
long detail::settings_ref::connect_timeout() const { return g_->read([](const session_settings& s) { return s.connect_timeout; }); }
void detail::settings_ref::max_time(long seconds) const { g_->update([&](session_settings& s) { s.set_max_time(seconds); }); }
long detail::settings_ref::max_time() const { return g_->read([](const session_settings& s) { return s.max_time; }); }
void detail::settings_ref::retry(int times) const { g_->update([&](session_settings& s) { s.set_retry(times); }); }
int detail::settings_ref::retry() const { return g_->read([](const session_settings& s) { return s.retries; }); }
void detail::settings_ref::retry_delay(long seconds) const { g_->update([&](session_settings& s) { s.set_retry_delay(seconds); }); }
long detail::settings_ref::retry_delay() const { return g_->read([](const session_settings& s) { return s.retry_delay_ms / 1000; }); }
void detail::settings_ref::retry_delay_ms(long ms) const { g_->update([&](session_settings& s) { s.set_retry_delay_ms(ms); }); }
long detail::settings_ref::retry_delay_ms() const { return g_->read([](const session_settings& s) { return s.retry_delay_ms; }); }
void detail::settings_ref::retry_connrefused(bool enable) const { g_->update([&](session_settings& s) { s.retry_connrefused = enable; }); }
bool detail::settings_ref::retry_connrefused() const { return g_->read([](const session_settings& s) { return s.retry_connrefused; }); }
void detail::settings_ref::max_redirects(int count) const { g_->update([&](session_settings& s) { s.set_max_redirects(count); }); }
int detail::settings_ref::max_redirects() const { return g_->read([](const session_settings& s) { return s.redirects(); }); }
void detail::settings_ref::auto_compression(bool enable) const { g_->update([&](session_settings& s) { s.auto_compression = enable; }); }
bool detail::settings_ref::auto_compression() const { return g_->read([](const session_settings& s) { return s.auto_compression; }); }
void detail::settings_ref::basic_auth(const std::string& user, const std::string& password) const {
    g_->update([&](session_settings& s) { s.set_basic(user, password); });
}
void detail::settings_ref::bearer(const std::string& token) const { g_->update([&](session_settings& s) { s.set_bearer(token); }); }
void detail::settings_ref::proxy(const url& proxy_url) const { g_->update([&](session_settings& s) { s.set_proxy(proxy_url); }); }
void detail::settings_ref::verify(bool enable) const { g_->update([&](session_settings& s) { s.verify = enable; }); }
bool detail::settings_ref::verify() const { return g_->read([](const session_settings& s) { return s.verify; }); }
void detail::settings_ref::ca_file(const path& file) const { g_->update([&](session_settings& s) { s.ca_file = file.str(); }); }
path detail::settings_ref::ca_file() const { return path(g_->read([](const session_settings& s) { return s.ca_file; })); }

// --- client (a keep-alive session; calls block, over the shared engine) ---

struct client::impl {
    endpoint where;
    guarded_settings settings;
    explicit impl(endpoint e) : where(std::move(e)) {}
    impl(endpoint e, session_settings s) : where(std::move(e)), settings(std::move(s)) {}
};

client::client(const std::string& host, int port, bool use_ssl) {
    endpoint e;
    e.scheme = use_ssl ? "https" : "http";
    e.host = host;
    e.port = port;
    impl_ = std::make_shared<impl>(std::move(e));
}

client::client(const url& base_url) : impl_(std::make_shared<impl>(parse_base(base_url, "httpp::client"))) {}

client::~client() = default;
client::client(const client&) noexcept = default;
client& client::operator=(const client&) noexcept = default;
client::client(client&&) noexcept = default;
client& client::operator=(client&&) noexcept = default;

client client::clone() const {
    client out(*this);
    out.impl_ = std::make_shared<impl>(impl_->where, impl_->settings.snapshot());
    return out;
}

detail::settings_ref client::settings_() const { return detail::settings_ref(impl_->settings); }

httpp::request client::make_request(const std::string& path) const {
    return httpp::request(url(detail::endpoint_url(impl_->where, path)), impl_->settings.snapshot());
}

response client::send(const std::string& method, const std::string& path, const headers_t& headers,
                      const std::string& body, const std::string& content_type) {
    request_spec spec;
    response error;
    if (!impl_->settings.make_spec(impl_->where, method, path, headers, body, content_type, spec, error)) return error;
    return detail::run_blocking(std::make_shared<request_spec>(std::move(spec)));
}

response client::get(const std::string& path) { return send("GET", path); }
response client::get(const std::string& path, const headers_t& headers) { return send("GET", path, headers); }
response client::head(const std::string& path, const headers_t& headers) { return send("HEAD", path, headers); }
response client::options(const std::string& path, const headers_t& headers) { return send("OPTIONS", path, headers); }
response client::post(const std::string& path, const std::string& body, const std::string& ct, const headers_t& h) {
    return send("POST", path, h, body, ct);
}
response client::put(const std::string& path, const std::string& body, const std::string& ct, const headers_t& h) {
    return send("PUT", path, h, body, ct);
}
response client::patch(const std::string& path, const std::string& body, const std::string& ct, const headers_t& h) {
    return send("PATCH", path, h, body, ct);
}
response client::del(const std::string& path, const std::string& body, const std::string& ct, const headers_t& h) {
    return send("DELETE", path, h, body, ct);
}

client& client::header(std::string name, std::string value) {
    impl_->settings.update([&](session_settings& s) { s.set_default_header(std::move(name), std::move(value)); });
    return *this;
}
client& client::keep_alive(bool enable) {
    impl_->settings.update([&](session_settings& s) { s.keep_alive = enable; });
    return *this;
}
bool client::keep_alive() const { return impl_->settings.read([](const session_settings& s) { return s.keep_alive; }); }

response client::fetch(const url& target) {
    const std::string full_url = target.str();
    if (!url_is_http(target))
        throw std::invalid_argument("httpp::client::fetch: unsupported or invalid URL: " + full_url);
    session_settings once;
    once.keep_alive = false; // one request: do not park the connection
    request_spec spec;
    response error;
    if (!once.make_spec_for_url(full_url, "", "GET", {}, {}, {}, spec, error)) return error;
    return detail::run_blocking(std::make_shared<request_spec>(std::move(spec)));
}

// --- request (fluent builder; also what the `httpp curl` command runs on) ---

struct request::impl {
    std::string url;
    std::string method;       // empty until set explicitly or via a body
    std::string body;
    std::string content_type = "application/x-www-form-urlencoded";
    header_list headers;      // repeatable, in the order given
    header_list params, cookies;
    guarded_settings settings; // timeout, redirects, auth, proxy, TLS, size limit
    std::function<bool(std::string_view)> on_piece; // on_body(): streams the final body (ADR-0077)
    std::function<bool(const response&)> on_head_fn;
    std::function<void(const response&, int, long)> on_retry_fn; // on_retry() (ADR-0097)
    // save_to() / save_in() (ADR-0097)
    std::optional<path> save_file;   // save_to(): this file
    std::optional<path> save_folder; // save_in(): the folder, the name comes from the answer
    bool make_dirs = false;
    bool replace = true;
    bool head_in_file = false;

    // What one run of the request keeps while it writes: the file, the path chosen, why it stopped.
    struct saving {
        detail::file_writer out;
        std::string saved_as;
        std::string error;
    };

    explicit impl(std::string u) : url(std::move(u)) {}

    // The spec for this request. False (and `error` set) when it cannot even start.
    bool make_spec(request_spec& out, response& error) const {
        header_list call = headers;
        if (!cookies.empty() && !detail::has_header(call, "Cookie")) {
            std::string joined;
            for (const auto& [name, value] : cookies) {
                if (!joined.empty()) joined += "; ";
                joined += name + "=" + value;
            }
            call.emplace_back("Cookie", std::move(joined));
        }
        // Streamed bytes are passed on as the server sent them: ask for no coding, as downloads do (ADR-0051).
        const bool saving_body = save_file || save_folder;
        if ((on_piece || saving_body) && !detail::has_header(call, "Accept-Encoding")) call.emplace_back("Accept-Encoding", "identity");
        if (!settings.make_spec_for_url(url, params.empty() ? std::string() : httpp::build_query(params), method, call, body,
                                        content_type, out, error))
            return false;
        if (saving_body) {
            auto keep = std::make_shared<saving>();
            auto sink = std::make_shared<detail::body_sink>();
            // Copies, not `this`: the sink runs on the network thread, maybe after this request is gone (run_async on a temporary).
            sink->on_head = [keep, head = on_head_fn, file = save_file, folder = save_folder, requested = url, dirs = make_dirs,
                             replace_it = replace, with_head = head_in_file](const response& r) {
                if (head && !head(r)) return detail::body_sink::verdict::skip; // nothing is written for an answer that is refused
                path target;
                if (file) {
                    target = *file;
                } else {
                    std::string name = r.filename();
                    if (name.empty()) name = httpp::url_filename(httpp::parse_url(requested));
                    if (name.empty()) {
                        keep->error = "no file name to use in '" + requested + "'";
                        return detail::body_sink::verdict::abort;
                    }
                    target = folder->str().empty() ? path(name) : *folder / path(name);
                }
                // reopened for every accepted head: a retry after a cut-off body starts the file again
                if (!keep->out.open(target, detail::file_writer::mode::truncate, dirs, replace_it)) {
                    keep->error = keep->out.error();
                    return detail::body_sink::verdict::abort;
                }
                keep->saved_as = target.str();
                if (with_head) {
                    const std::string text = detail::head_text(r);
                    if (!keep->out.write(text.data(), text.size())) {
                        keep->error = keep->out.error();
                        return detail::body_sink::verdict::abort;
                    }
                }
                return detail::body_sink::verdict::stream;
            };
            sink->on_data = [keep, piece = on_piece](const char* data, std::size_t size) {
                if (!keep->out.write(data, size)) {
                    keep->error = keep->out.error();
                    return false;
                }
                return !piece || piece(std::string_view(data, size));
            };
            out.sink = std::move(sink);
            out.on_finish = [keep](response& r) {
                keep->out.close(); // flushed before the caller reads the file
                r.saved_as = keep->saved_as;
                if (!keep->error.empty()) {
                    r.error = error_kind::file;
                    r.error_message = keep->error;
                }
            };
        } else if (on_piece) {
            auto sink = std::make_shared<detail::body_sink>();
            sink->on_head = [head = on_head_fn](const response& r) {
                return !head || head(r) ? detail::body_sink::verdict::stream : detail::body_sink::verdict::skip;
            };
            sink->on_data = [piece = on_piece](const char* data, std::size_t size) { return piece(std::string_view(data, size)); };
            out.sink = std::move(sink);
        }
        out.on_retry = on_retry_fn;
        return true;
    }
};

request::request(const url& target) : impl_(std::make_unique<impl>(target.str())) {}
request::request(const url& target, const session_settings& from) : impl_(std::make_unique<impl>(target.str())) {
    impl_->settings.update([&](session_settings& s) { s = from; });
}
request::~request() = default;
request::request(request&&) noexcept = default;
request& request::operator=(request&&) noexcept = default;

detail::settings_ref request::settings_() const { return detail::settings_ref(impl_->settings); }

request request::clone() const {
    request out(url(impl_->url), impl_->settings.snapshot());
    impl& to = *out.impl_;
    const impl& from = *impl_;
    // Every field of impl except `settings` (copied above). A field added to impl must be added here too; the test
    // `a_request_clone_carries_every_field` in test_clone.cpp fails when one is missed.
    to.method = from.method;
    to.body = from.body;
    to.content_type = from.content_type;
    to.headers = from.headers;
    to.params = from.params;
    to.cookies = from.cookies;
    to.on_piece = from.on_piece;
    to.on_head_fn = from.on_head_fn;
    to.on_retry_fn = from.on_retry_fn;
    to.save_file = from.save_file;
    to.save_folder = from.save_folder;
    to.make_dirs = from.make_dirs;
    to.replace = from.replace;
    to.head_in_file = from.head_in_file;
    return out;
}

request& request::method(std::string m) & { impl_->method = std::move(m); return *this; }
request& request::header(std::string name, std::string value) & {
    impl_->headers.emplace_back(std::move(name), std::move(value));
    return *this;
}
request& request::param(std::string name, std::string value) & {
    impl_->params.emplace_back(std::move(name), std::move(value));
    return *this;
}
request& request::data(std::string body) & {
    impl_->body = std::move(body);
    if (impl_->method.empty()) impl_->method = "POST";
    return *this;
}
request& request::json(std::string document) & {
    impl_->body = std::move(document);
    impl_->content_type = "application/json";
    if (impl_->method.empty()) impl_->method = "POST";
    return *this;
}
request& request::form(const std::vector<std::pair<std::string, std::string>>& fields) & {
    impl_->body = httpp::build_query(fields);
    impl_->content_type = "application/x-www-form-urlencoded";
    if (impl_->method.empty()) impl_->method = "POST";
    return *this;
}
request& request::content_type(std::string type) & { impl_->content_type = std::move(type); return *this; }
request& request::cookie(std::string name, std::string value) & {
    httpp::cookie check(name, value); // refuses what could not be sent as a Cookie pair
    impl_->cookies.emplace_back(std::move(name), std::move(value));
    return *this;
}
request& request::max_response_size(std::size_t bytes) & {
    impl_->settings.update([&](session_settings& s) { s.max_response_size = bytes; });
    return *this;
}
request& request::on_body(std::function<bool(std::string_view piece)> piece) & {
    impl_->on_piece = std::move(piece);
    return *this;
}
request& request::on_head(std::function<bool(const response& head)> head) & {
    impl_->on_head_fn = std::move(head);
    return *this;
}
request& request::on_retry(std::function<void(const response& last, int retry_number, long delay_seconds)> notice) & {
    impl_->on_retry_fn = std::move(notice);
    return *this;
}
request& request::save_to(const path& file) & {
    impl_->save_file = file;
    impl_->save_folder.reset(); // the last of save_to() and save_in() wins
    return *this;
}
request& request::save_in(const path& folder) & {
    impl_->save_folder = folder;
    impl_->save_file.reset();
    return *this;
}
request& request::create_dirs(bool enable) & {
    impl_->make_dirs = enable;
    return *this;
}
request& request::overwrite(bool enable) & {
    impl_->replace = enable;
    return *this;
}
request& request::include_head(bool enable) & {
    impl_->head_in_file = enable;
    return *this;
}

response request::run() const {
    request_spec spec;
    response error;
    if (!impl_->make_spec(spec, error)) return error;
    return detail::run_blocking(std::make_shared<request_spec>(std::move(spec)));
}

std::future<response> request::run_async() const {
    auto promise = std::make_shared<std::promise<response>>();
    auto future = promise->get_future();
    run_async([promise](response out) { promise->set_value(std::move(out)); });
    return future;
}

void request::run_async(std::function<void(response)> done) const {
    auto spec = std::make_shared<request_spec>();
    response error;
    if (!impl_->make_spec(*spec, error)) {
        done(std::move(error));
        return;
    }
    detail::run_chain(std::move(spec), std::move(done));
}

// --- async_client: a base URL, shared defaults and at most `max_connections` requests at a time ---

struct async_client::impl {
    endpoint where;
    guarded_settings settings;
    std::size_t max_connections = 8;

    // at most max_connections requests in flight; the rest wait here
    struct waiting {
        std::shared_ptr<request_spec> spec;
        std::function<void(response)> done;
    };
    std::mutex mu;
    std::deque<waiting> queue;
    std::size_t in_flight = 0;

    void start(waiting w, const std::shared_ptr<impl>& self) {
        detail::run_chain(w.spec, [self, done = std::move(w.done)](response out) {
            done(std::move(out));
            self->finished(self);
        });
    }

    void finished(const std::shared_ptr<impl>& self) {
        waiting next;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (queue.empty()) { --in_flight; return; }
            next = std::move(queue.front());
            queue.pop_front();
        }
        start(std::move(next), self); // the freed slot goes straight to the next waiting request
    }
};

async_client::async_client(const url& base_url, std::size_t max_connections)
    : impl_(std::make_shared<impl>()) {
    impl_->where = parse_base(base_url, "httpp::async_client");
    impl_->max_connections = max_connections == 0 ? 1 : max_connections;
}
async_client::~async_client() = default;
async_client::async_client(const async_client&) noexcept = default;
async_client& async_client::operator=(const async_client&) noexcept = default;
async_client::async_client(async_client&&) noexcept = default;
async_client& async_client::operator=(async_client&&) noexcept = default;

async_client async_client::clone() const {
    async_client out(*this);
    auto fresh = std::make_shared<impl>(); // its queue and its count of requests in flight start empty
    fresh->where = impl_->where;
    fresh->max_connections = impl_->max_connections;
    const session_settings now = impl_->settings.snapshot();
    fresh->settings.update([&](session_settings& s) { s = now; });
    out.impl_ = std::move(fresh);
    return out;
}

detail::settings_ref async_client::settings_() const { return detail::settings_ref(impl_->settings); }

httpp::request async_client::make_request(const std::string& path) const {
    return httpp::request(url(detail::endpoint_url(impl_->where, path)), impl_->settings.snapshot());
}

std::future<response> async_client::send(const std::string& method, const std::string& path,
                                         const headers_t& headers, const std::string& body,
                                         const std::string& content_type) {
    auto promise = std::make_shared<std::promise<response>>();
    auto future = promise->get_future();
    send(method, path, headers, body, content_type, [promise](response out) { promise->set_value(std::move(out)); });
    return future;
}

void async_client::send(const std::string& method, const std::string& path, const headers_t& headers,
                        const std::string& body, const std::string& content_type,
                        std::function<void(response)> done) {
    auto& st = *impl_;
    auto spec = std::make_shared<request_spec>();
    response error;
    if (!st.settings.make_spec(st.where, method, path, headers, body, content_type, *spec, error)) {
        done(std::move(error));
        return;
    }

    impl::waiting w{spec, std::move(done)};
    {
        std::lock_guard<std::mutex> lock(st.mu);
        if (st.in_flight >= st.max_connections) {
            st.queue.push_back(std::move(w));
            return;
        }
        ++st.in_flight;
    }
    st.start(std::move(w), impl_);
}

std::future<response> async_client::get(const std::string& path, const headers_t& h) { return send("GET", path, h); }
std::future<response> async_client::head(const std::string& path, const headers_t& h) { return send("HEAD", path, h); }
std::future<response> async_client::post(const std::string& p, const std::string& b, const std::string& ct, const headers_t& h) {
    return send("POST", p, h, b, ct);
}
std::future<response> async_client::put(const std::string& p, const std::string& b, const std::string& ct, const headers_t& h) {
    return send("PUT", p, h, b, ct);
}
std::future<response> async_client::patch(const std::string& p, const std::string& b, const std::string& ct, const headers_t& h) {
    return send("PATCH", p, h, b, ct);
}
std::future<response> async_client::del(const std::string& p, const std::string& b, const std::string& ct, const headers_t& h) {
    return send("DELETE", p, h, b, ct);
}

async_client& async_client::header(std::string name, std::string value) {
    impl_->settings.update([&](session_settings& s) { s.set_default_header(std::move(name), std::move(value)); });
    return *this;
}

} // namespace httpp
