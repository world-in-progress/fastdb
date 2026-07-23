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
        require_growth(UINT64_C(1));
        bytes_.push_back(value);
    }
    void pop_back() { bytes_.pop_back(); }
    std::size_t size() const noexcept { return bytes_.size(); }
    int compare(std::size_t position, std::size_t count,
                const char* value) const {
        return bytes_.compare(position, count, value);
    }
    std::string finish() && { return std::move(bytes_); }

private:
    void append(std::string_view value) {
        require_growth(static_cast<std::uint64_t>(value.size()));
        bytes_.append(value.data(), value.size());
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
