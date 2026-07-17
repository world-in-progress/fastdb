#pragma once

#include "payload/error/Error.hpp"

#include <cstddef>
#include <utility>
#include <variant>

namespace fastdb::payload::error {

template <typename T>
class Result final {
public:
    static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    static Result failure(Error error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    bool has_value() const noexcept { return value_.index() == 0U; }

    T& value() & { return std::get<0>(value_); }
    const T& value() const& { return std::get<0>(value_); }
    T&& value() && { return std::get<0>(std::move(value_)); }

    Error& error() & { return std::get<1>(value_); }
    const Error& error() const& { return std::get<1>(value_); }
    Error&& error() && { return std::get<1>(std::move(value_)); }

private:
    template <std::size_t Index, typename Value>
    explicit Result(std::in_place_index_t<Index>, Value&& value)
        : value_(std::in_place_index<Index>, std::forward<Value>(value)) {}

    std::variant<T, Error> value_;
};

template <>
class Result<void> final {
public:
    static Result success() { return Result(std::in_place_index<0>); }

    static Result failure(Error error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    bool has_value() const noexcept { return value_.index() == 0U; }

    Error& error() & { return std::get<1>(value_); }
    const Error& error() const& { return std::get<1>(value_); }
    Error&& error() && { return std::get<1>(std::move(value_)); }

private:
    explicit Result(std::in_place_index_t<0>)
        : value_(std::in_place_index<0>) {}
    explicit Result(std::in_place_index_t<1>, Error error)
        : value_(std::in_place_index<1>, std::move(error)) {}

    std::variant<std::monostate, Error> value_;
};

}  // namespace fastdb::payload::error
