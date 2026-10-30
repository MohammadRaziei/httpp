#include "httpp/websocket.hpp"
#include "internal/websocket.hpp"

#include <stdexcept>

namespace httpp {
namespace {

websocket::status to_status(detail::ws_status s) {
    switch (s) {
        case detail::ws_status::ok: return websocket::status::ok;
        case detail::ws_status::full: return websocket::status::full;
        default: return websocket::status::closed;
    }
}

} // namespace

websocket::status websocket::send_text(std::string_view text) const {
    return shared_ ? to_status(shared_->send(detail::ws_text, text)) : status::closed;
}

websocket::status websocket::send_binary(std::string_view data) const {
    return shared_ ? to_status(shared_->send(detail::ws_binary, data)) : status::closed;
}

websocket::status websocket::ping(std::string_view payload) const {
    return shared_ ? to_status(shared_->send(detail::ws_ping, payload)) : status::closed;
}

void websocket::close(std::uint16_t code, std::string_view reason) const {
    if (!detail::ws_close_code_valid(code)) throw std::invalid_argument("websocket::close: a close code must be 1000-1003, 1007-1014 or 3000-4999");
    if (shared_) shared_->close(code, reason);
}

void websocket::pause_reading() const { if (shared_) shared_->pause_reading(true); }
void websocket::resume_reading() const { if (shared_) shared_->pause_reading(false); }
const std::shared_ptr<void>& websocket::user() const {
    static const std::shared_ptr<void> none;
    return shared_ ? shared_->user() : none;
}
bool websocket::is_open() const { return shared_ && shared_->is_open(); }
std::size_t websocket::buffered_amount() const { return shared_ ? shared_->buffered() : 0; }

} // namespace httpp
