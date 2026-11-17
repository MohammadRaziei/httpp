# Cookies and Basic credentials: the rules live in C++ (httpp/cookie.hpp, server_request), Python only calls them, so both sides agree.
# Included by httpp_python.pyx. The helpers build a throwaway server_request around the header, which is the public way to reach cookies() and basic_auth().

cdef extern from *:
    """
    #include <optional>
    #include <string>
    #include <utility>
    #include <vector>
    #include "httpp/cookie.hpp"
    #include "httpp/server.hpp"
    static std::string httpp_py_set_cookie(const std::string& name, const std::string& value, const std::string& path, const std::string& domain,
                                            bool has_max_age, long long max_age, bool secure, bool http_only, int site) {
        httpp::cookie c(name, value);
        c.set_path(path).set_domain(domain).set_secure(secure).set_http_only(http_only);
        if (has_max_age) c.set_max_age(max_age);
        if (site == 0) c.set_same_site(httpp::same_site::lax);
        else if (site == 1) c.set_same_site(httpp::same_site::strict);
        else if (site == 2) c.set_same_site(httpp::same_site::none);
        return c.to_set_cookie();
    }
    static std::vector<std::pair<std::string, std::string>> httpp_py_cookies(const std::string& header) {
        httpp::server_request r;
        r.headers.emplace_back("Cookie", header);
        return r.cookies();
    }
    static bool httpp_py_basic(const std::string& header, std::string& user, std::string& password) {
        httpp::server_request r;
        r.headers.emplace_back("Authorization", header);
        auto got = r.basic_auth();
        if (!got) return false;
        user = std::move(got->first);
        password = std::move(got->second);
        return true;
    }
    """
    string httpp_py_set_cookie(const string&, const string&, const string&, const string&, cbool, long long, cbool, cbool, int) except +
    vector[pair[string, string]] httpp_py_cookies(const string&) except +
    cbool httpp_py_basic(const string&, string&, string&) except +


def _enc(str s):
    return s.encode("utf-8", "surrogateescape")


def _dec(const string& s):
    return s.decode("utf-8", "surrogateescape")


def _cookie_header(str name, str value, str path, str domain, max_age, bint secure, bint http_only, int site):
    """The value of a Set-Cookie header (httpp::cookie::to_set_cookie); ValueError for a name or value that cannot be sent."""
    cdef cbool has_max_age = max_age is not None
    cdef long long age = <long long>max_age if has_max_age else 0
    return _dec(httpp_py_set_cookie(_enc(name), _enc(value), _enc(path), _enc(domain), has_max_age, age, secure, http_only, site))


def _parse_cookies(str header):
    """[(name, value)] of a Cookie header, in order (httpp::cookie::parse_cookie_header)."""
    cdef vector[pair[string, string]] pairs = httpp_py_cookies(_enc(header))
    return [(_dec(p.first), _dec(p.second)) for p in pairs]


def _parse_basic(str header):
    """(user, password) of an Authorization: Basic header, or None."""
    cdef string user, password
    if not httpp_py_basic(_enc(header), user, password):
        return None
    return (_dec(user), _dec(password))
