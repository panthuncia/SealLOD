#pragma once
#include <optional>
#include <utility>

template <class T>
class DefaultedOptional {
    std::optional<T> value_;
    T default_;

public:
    // Construct with a required default; start undefined.
    explicit constexpr DefaultedOptional(T default_value)
        : value_(std::nullopt), default_(std::move(default_value)) {
    }

    // Start defined with an initial value and keep a default.
    constexpr DefaultedOptional(T default_value, T initial_value)
        : value_(std::move(initial_value)), default_(std::move(default_value)) {
    }

    constexpr bool HasValue() const noexcept { return value_.has_value(); }
    constexpr explicit operator bool() const noexcept { return HasValue(); }
    constexpr void Reset() noexcept { value_.reset(); }
    constexpr void ResetToDefault() { value_ = default_; }

    // Read access
    constexpr const T& Get() const noexcept { return value_ ? *value_ : default_; }

    constexpr T ValueOrDefault() const { return value_.value_or(default_); }

    constexpr T& Ensure() {
        if (!value_) value_.emplace(default_);
        return *value_;
    }

    constexpr const T* operator->() const noexcept { return &Get(); }
    constexpr const T& operator*()  const noexcept { return Get(); }

    constexpr T& operator*() { return Ensure(); }

    constexpr DefaultedOptional& operator=(T v) {
        value_ = std::move(v);
        return *this;
    }

    constexpr const T& DefaultValue() const noexcept { return default_; }
    constexpr void DetDefault(T v) {
        default_ = std::move(v);
    }
};