// SPDX-License-Identifier: GPL-3.0-or-later
// Turns a title's image into something the Windows loader maps but does not
// run (`victory-loader/src/prepare.rs`; DanziLoader / victory-perf, see
// NOTICE.md). Pure byte manipulation.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "pe/image.hpp"

namespace danzi::loader {


class PrepareError : public std::exception {
public:
    explicit PrepareError(const pe::PeError& e) : shown_(e.display()) {}
    explicit PrepareError(const char* patch) : shown_(std::string("cannot patch image: ") + patch) {}
    const std::string& display() const { return shown_; }
    const char* what() const noexcept override { return shown_.c_str(); }

private:
    std::string shown_;
};


struct PreparedImage {


    pe::ImageInfo info;

    std::vector<std::uint8_t> bytes;

    std::array<std::uint8_t, 32> original_hash{};


    std::string file_name() const;
};


PreparedImage prepare(std::span<const std::uint8_t> original);

}   
