#pragma once

#include "httpp/export.hpp"

#include <cstddef>
#include <iterator>
#include <memory>
#include <string>

namespace httpp::progress {

// tqdm-like: a terminal progress bar with a known total.
// Backed by the vendored `indicators` library (src/third_party/indicators),
// but that dependency is hidden behind this pointer (see
// src/core/progressbar.cpp) and never appears in this public header.
class HTTPP_API bar {
public:
    explicit bar(std::size_t total, std::string description = "");
    ~bar();

    bar(bar&&) noexcept;
    bar& operator=(bar&&) noexcept;
    bar(const bar&) = delete;
    bar& operator=(const bar&) = delete;

    void update(std::size_t n = 1);          // advance by n
    void set_progress(std::size_t current);  // set an absolute position
    void finish();                           // jump to total and stop

    std::size_t total() const;
    std::size_t current() const;
    bool is_finished() const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// trange-like: an iterable [0, total) range that owns a `bar` and advances
// it by one on every step of the loop, e.g.:
//
//   for (auto i : httpp::progressbar::range(100, "working")) { ... }
class HTTPP_API range {
public:
    explicit range(std::size_t total, std::string description = "")
        : total_(total), bar_(total, std::move(description)) {}

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using pointer = const std::size_t*;
        using reference = std::size_t;

        iterator(std::size_t pos, range* owner) : pos_(pos), owner_(owner) {}

        std::size_t operator*() const { return pos_; }

        iterator& operator++() {
            if (owner_) owner_->bar_.update();
            ++pos_;
            return *this;
        }

        bool operator!=(const iterator& other) const { return pos_ != other.pos_; }

    private:
        std::size_t pos_;
        range* owner_;
    };

    iterator begin() { return iterator(0, this); }
    iterator end() { return iterator(total_, nullptr); }

private:
    std::size_t total_;
    bar bar_;
};

} // namespace httpp::progress
