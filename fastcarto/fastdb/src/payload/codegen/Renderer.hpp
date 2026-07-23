#pragma once

#include "payload/spec/CompiledSpec.hpp"
#include "payload/spec/RuntimeTopology.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace fastdb::payload::codegen {

struct OutputLimitExceeded final {
    std::uint64_t actual;
    std::uint64_t limit;
};

class CheckedOutput final {
public:
    explicit CheckedOutput(std::uint64_t max_bytes) noexcept
        : max_bytes_(max_bytes) {}

    void reserve(std::size_t requested) {
        const std::size_t native_limit =
            max_bytes_ > static_cast<std::uint64_t>(
                             std::numeric_limits<std::size_t>::max())
                ? std::numeric_limits<std::size_t>::max()
                : static_cast<std::size_t>(max_bytes_);
        bytes_.reserve(requested < native_limit ? requested : native_limit);
    }

    CheckedOutput& operator+=(std::string_view value) {
        append(value);
        return *this;
    }
    CheckedOutput& operator+=(const std::string& value) {
        append(value);
        return *this;
    }
    CheckedOutput& operator+=(const char* value) {
        append(std::string_view{value});
        return *this;
    }
    CheckedOutput& operator+=(char value) {
        push_back(value);
        return *this;
    }

    void push_back(char value) {
        flush_pending_newline();
        if (value == '\n') {
            pending_newline_ = true;
            return;
        }
        append_immediate(std::string_view{&value, 1U});
    }
    std::string finish() && {
        flush_pending_newline();
        return std::move(bytes_);
    }
    std::string finish_trimming_blank_line() && {
        if (!pending_newline_ || bytes_.empty() || bytes_.back() != '\n') {
            flush_pending_newline();
        } else {
            pending_newline_ = false;
        }
        return std::move(bytes_);
    }

private:
    void append(std::string_view value) {
        if (value.empty()) {
            return;
        }
        flush_pending_newline();
        if (value.back() == '\n') {
            append_immediate(value.substr(0U, value.size() - 1U));
            pending_newline_ = true;
            return;
        }
        append_immediate(value);
    }

    void append_immediate(std::string_view value) {
        require_growth(static_cast<std::uint64_t>(value.size()));
        bytes_.append(value.data(), value.size());
    }

    void flush_pending_newline() {
        if (!pending_newline_) {
            return;
        }
        require_growth(UINT64_C(1));
        bytes_.push_back('\n');
        pending_newline_ = false;
    }

    void require_growth(std::uint64_t additional) const {
        const std::uint64_t current = static_cast<std::uint64_t>(bytes_.size());
        const std::uint64_t actual =
            additional > std::numeric_limits<std::uint64_t>::max() - current
                ? std::numeric_limits<std::uint64_t>::max()
                : current + additional;
        if (actual > max_bytes_) {
            throw OutputLimitExceeded{actual, max_bytes_};
        }
    }

    std::uint64_t max_bytes_;
    std::string bytes_;
    bool pending_newline_{false};
};

std::string render_cpp(const spec::CompiledSpec& compiled,
                       const spec::RuntimeTopology& topology,
                       std::uint64_t max_total_bytes);
std::string render_rust(const spec::CompiledSpec& compiled,
                        const spec::RuntimeTopology& topology,
                        std::uint64_t max_total_bytes);
std::string render_python(const spec::CompiledSpec& compiled,
                          const spec::RuntimeTopology& topology,
                          std::uint64_t max_total_bytes);
std::string render_typescript(const spec::CompiledSpec& compiled,
                              const spec::RuntimeTopology& topology,
                              std::uint64_t max_total_bytes);

}  // namespace fastdb::payload::codegen
