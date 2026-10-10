#include "internal/tls_session.hpp"

#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincrypt.h>
#elif defined(__APPLE__)
#  include <CoreFoundation/CoreFoundation.h>
#  include <Security/Security.h>
#endif

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

namespace httpp::detail {
namespace {

std::string mbed_error(int code) {
    char buf[200];
    mbedtls_strerror(code, buf, sizeof buf);
    return buf;
}

// The operating system's trusted roots, parsed into `crt`. Same sources as the sync client's
// cpp-httplib: Windows ROOT+CA stores, the macOS keychain, or the usual bundle files on Unix.
bool load_system_roots(mbedtls_x509_crt& crt) {
    bool loaded = false;
#ifdef _WIN32
    for (const wchar_t* name : {L"ROOT", L"CA"}) {
        HCERTSTORE store = CertOpenSystemStoreW(0, name);
        if (!store) continue;
        for (PCCERT_CONTEXT c = nullptr; (c = CertEnumCertificatesInStore(store, c)) != nullptr;)
            if (mbedtls_x509_crt_parse_der(&crt, c->pbCertEncoded, c->cbCertEncoded) == 0) loaded = true;
        CertCloseStore(store, 0);
    }
#elif defined(__APPLE__)
    const SecTrustSettingsDomain domains[] = {kSecTrustSettingsDomainSystem, kSecTrustSettingsDomainAdmin,
                                              kSecTrustSettingsDomainUser};
    for (auto domain : domains) {
        CFArrayRef certs = nullptr;
        if (SecTrustSettingsCopyCertificates(domain, &certs) != errSecSuccess || !certs) {
            if (certs) CFRelease(certs);
            continue;
        }
        for (CFIndex i = 0, n = CFArrayGetCount(certs); i < n; ++i) {
            CFDataRef der = SecCertificateCopyData((SecCertificateRef)CFArrayGetValueAtIndex(certs, i));
            if (!der) continue;
            if (mbedtls_x509_crt_parse_der(&crt, CFDataGetBytePtr(der), (size_t)CFDataGetLength(der)) == 0) loaded = true;
            CFRelease(der);
        }
        CFRelease(certs);
    }
#else
    for (const char* f : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                          "/etc/ssl/ca-bundle.pem", "/etc/pki/tls/cacert.pem", "/etc/ssl/cert.pem"})
        if (mbedtls_x509_crt_parse_file(&crt, f) >= 0) return true;
    for (const char* d : {"/etc/ssl/certs", "/etc/pki/tls/certs", "/usr/share/ca-certificates"})
        if (mbedtls_x509_crt_parse_path(&crt, d) >= 0) return true;
#endif
    return loaded;
}

} // namespace

struct tls_context {
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    tls_context() { mbedtls_ssl_config_init(&conf); mbedtls_x509_crt_init(&ca); }
    ~tls_context() { mbedtls_ssl_config_free(&conf); mbedtls_x509_crt_free(&ca); }
    tls_context(const tls_context&) = delete;
    tls_context& operator=(const tls_context&) = delete;
};

std::shared_ptr<const tls_context> make_tls_context(bool verify, const std::string& ca_file, std::string& error) {
    static std::once_flag psa_once;
    static int psa_ok = 0;
    std::call_once(psa_once, [] { psa_ok = psa_crypto_init() == PSA_SUCCESS; });
    if (!psa_ok) { error = "cannot initialise the crypto backend"; return nullptr; }

    auto c = std::make_shared<tls_context>();
    int r = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) { error = mbed_error(r); return nullptr; }
    if (!verify) {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
        return c;
    }
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    if (!ca_file.empty()) {
        if ((r = mbedtls_x509_crt_parse_file(&c->ca, ca_file.c_str())) < 0) {
            error = "cannot read CA file '" + ca_file + "': " + mbed_error(r);
            return nullptr;
        }
    } else if (!load_system_roots(c->ca)) {
        error = "cannot load the system CA certificates";
        return nullptr;
    }
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, nullptr);
    return c;
}

struct tls_session::impl {
    std::shared_ptr<const tls_context> ctx; // keeps conf and trust roots alive
    mbedtls_ssl_context ssl;
    std::string in;        // ciphertext waiting to be consumed by mbedtls
    std::size_t in_pos = 0;
    std::string out;       // ciphertext waiting to be sent
    bool ready = false;

    static int send_cb(void* p, const unsigned char* b, std::size_t n) {
        static_cast<impl*>(p)->out.append(reinterpret_cast<const char*>(b), n);
        return static_cast<int>(n);
    }
    static int recv_cb(void* p, unsigned char* b, std::size_t n) {
        auto* s = static_cast<impl*>(p);
        std::size_t avail = s->in.size() - s->in_pos;
        if (avail == 0) return MBEDTLS_ERR_SSL_WANT_READ;
        std::size_t k = std::min(n, avail);
        std::memcpy(b, s->in.data() + s->in_pos, k);
        s->in_pos += k;
        if (s->in_pos == s->in.size()) { s->in.clear(); s->in_pos = 0; }
        return static_cast<int>(k);
    }
    std::size_t pending() const { return in.size() - in_pos; }
};

tls_session::tls_session(std::shared_ptr<const tls_context> ctx, const std::string& host)
    : impl_(std::make_unique<impl>()) {
    impl_->ctx = std::move(ctx);
    mbedtls_ssl_init(&impl_->ssl);
    int r = mbedtls_ssl_setup(&impl_->ssl, &impl_->ctx->conf);
    if (r == 0) r = mbedtls_ssl_set_hostname(&impl_->ssl, host.c_str()); // SNI + name check
    if (r != 0) { error_ = mbed_error(r); impl_.reset(); return; }
    mbedtls_ssl_set_bio(&impl_->ssl, impl_.get(), impl::send_cb, impl::recv_cb, nullptr);
}

tls_session::~tls_session() { if (impl_) mbedtls_ssl_free(&impl_->ssl); }

bool tls_session::ok() const { return impl_ != nullptr; }

void tls_session::feed(const char* data, std::size_t n) { impl_->in.append(data, n); }

std::string tls_session::take_output() { std::string s; s.swap(impl_->out); return s; }

tls_session::result tls_session::handshake() {
    if (impl_->ready) return result::ok;
    for (;;) {
        std::size_t before = impl_->pending();
        int r = mbedtls_ssl_handshake(&impl_->ssl);
        if (r == 0) { impl_->ready = true; return result::ok; }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            // mbedtls may stop with input left over after consuming a record: go again while it makes progress.
            if (impl_->pending() > 0 && impl_->pending() < before) continue;
            return result::more;
        }
        error_ = "TLS handshake failed: " + mbed_error(r);
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            char buf[512];
            mbedtls_x509_crt_verify_info(buf, sizeof buf, "", mbedtls_ssl_get_verify_result(&impl_->ssl));
            std::string why = buf;
            while (!why.empty() && (why.back() == '\n' || why.back() == ' ')) why.pop_back();
            error_ += " (" + why + ")";
        }
        return result::failed;
    }
}

tls_session::result tls_session::write(const std::string& plain) {
    std::size_t off = 0;
    while (off < plain.size()) {
        int r = mbedtls_ssl_write(&impl_->ssl, reinterpret_cast<const unsigned char*>(plain.data()) + off,
                                  plain.size() - off);
        if (r > 0) { off += static_cast<std::size_t>(r); continue; }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue; // memory BIO: never blocks
        error_ = "TLS write failed: " + mbed_error(r);
        return result::failed;
    }
    return result::ok;
}

tls_session::result tls_session::read(std::string& plain) {
    unsigned char buf[16384];
    for (;;) {
        std::size_t before = impl_->pending();
        int r = mbedtls_ssl_read(&impl_->ssl, buf, sizeof buf);
        if (r > 0) { plain.append(reinterpret_cast<char*>(buf), static_cast<std::size_t>(r)); return result::ok; }
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue; // TLS 1.3, harmless
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (impl_->pending() > 0 && impl_->pending() < before) continue; // consumed a record, more input waiting
            return result::more;
        }
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return result::closed;
        error_ = "TLS read failed: " + mbed_error(r);
        return result::failed;
    }
}

} // namespace httpp::detail
