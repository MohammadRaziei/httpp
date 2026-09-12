#include "httpp/progress.hpp"

// indicators.hpp is included ONLY in this translation unit. It is compiled
// into httpp's .so/.a object code and is never included by any public
// httpp/*.hpp header, so consumers of httpp::progressbar::bar never see it
// and don't need it on their include path.
#include <indicators/indicators.hpp>

namespace httpp::progress {

struct bar::impl {
    std::size_t total;
    std::size_t current = 0;
    bool finished = false;
    indicators::ProgressBar indicator;

    impl(std::size_t total_, std::string description)
        : total(total_),
          indicator(indicators::option::BarWidth{40},
                    indicators::option::Start{"["},
                    indicators::option::End{"]"},
                    indicators::option::PrefixText{std::move(description)},
                    indicators::option::ShowPercentage{true},
                    indicators::option::MaxProgress{total_}) {}
};

bar::bar(std::size_t total, std::string description)
    : impl_(std::make_unique<impl>(total, std::move(description))) {}

bar::~bar() = default;
bar::bar(bar&&) noexcept = default;
bar& bar::operator=(bar&&) noexcept = default;

void bar::update(std::size_t n) {
    set_progress(impl_->current + n);
}

void bar::set_progress(std::size_t current) {
    if (current > impl_->total) current = impl_->total;
    impl_->current = current;
    impl_->indicator.set_progress(current);
    if (current >= impl_->total) {
        impl_->finished = true;
    }
}

void bar::finish() {
    set_progress(impl_->total);
    impl_->indicator.mark_as_completed();
    impl_->finished = true;
}

std::size_t bar::total() const { return impl_->total; }
std::size_t bar::current() const { return impl_->current; }
bool bar::is_finished() const { return impl_->finished; }

} // namespace httpp::progress
