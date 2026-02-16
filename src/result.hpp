// =============================================================================
// MirageSystem - Result Type for Unified Error Handling
// =============================================================================
// A Result<T, E> type that encapsulates either a success value or an error.
// Inspired by Rust's Result type, providing explicit error handling without
// exceptions.
//
// Usage:
//   Result<int, std::string> divide(int a, int b) {
//       if (b == 0) return Err<int>("Division by zero");
//       return Ok(a / b);
//   }
//
//   auto result = divide(10, 2);
//   if (result.is_ok()) {
//       std::cout << result.value() << std::endl;
//   } else {
//       std::cerr << result.error() << std::endl;
//   }
// =============================================================================

#pragma once

#include <variant>
#include <string>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace mirage {

// =============================================================================
// Error Types
// =============================================================================

// Generic error with message
struct Error {
    std::string message;
    int code = 0;

    Error() = default;
    explicit Error(std::string msg, int c = 0) : message(std::move(msg)), code(c) {}
    explicit Error(const char* msg, int c = 0) : message(msg), code(c) {}

    bool operator==(const Error& other) const {
        return code == other.code && message == other.message;
    }
};

// Vulkan-specific error
struct VulkanError : Error {
    int vk_result = 0;  // VkResult value

    VulkanError() = default;
    explicit VulkanError(std::string msg, int vk_res = 0)
        : Error(std::move(msg), vk_res), vk_result(vk_res) {}
};

// IO error (file, network)
struct IoError : Error {
    enum class Kind {
        NotFound,
        PermissionDenied,
        ConnectionRefused,
        Timeout,
        Other
    };
    Kind kind = Kind::Other;

    IoError() = default;
    explicit IoError(std::string msg, Kind k = Kind::Other)
        : Error(std::move(msg)), kind(k) {}
};

// =============================================================================
// Result<T, E> Type
// =============================================================================

template<typename T, typename E = Error>
class Result {
public:
    // Success constructor
    Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}

    // Error constructor (from E or derived)
    template<typename Err, typename = std::enable_if_t<std::is_convertible_v<Err, E>>>
    Result(Err error) : data_(std::in_place_index<1>, E(std::move(error))) {}

    // Check status
    bool is_ok() const { return data_.index() == 0; }
    bool is_err() const { return data_.index() == 1; }

    explicit operator bool() const { return is_ok(); }

    // Access value (throws if error)
    T& value() & {
        if (is_err()) throw std::runtime_error("Result is error: " + error().message);
        return std::get<0>(data_);
    }

    const T& value() const& {
        if (is_err()) throw std::runtime_error("Result is error: " + error().message);
        return std::get<0>(data_);
    }

    T&& value() && {
        if (is_err()) throw std::runtime_error("Result is error: " + error().message);
        return std::get<0>(std::move(data_));
    }

    // Access error (throws if success)
    E& error() & {
        if (is_ok()) throw std::runtime_error("Result is ok, no error");
        return std::get<1>(data_);
    }

    const E& error() const& {
        if (is_ok()) throw std::runtime_error("Result is ok, no error");
        return std::get<1>(data_);
    }

    // Safe access with default
    T value_or(T default_value) const& {
        return is_ok() ? std::get<0>(data_) : std::move(default_value);
    }

    T value_or(T default_value) && {
        return is_ok() ? std::get<0>(std::move(data_)) : std::move(default_value);
    }

    // Optional-style access
    std::optional<T> ok() const& {
        if (is_ok()) return std::get<0>(data_);
        return std::nullopt;
    }

    std::optional<E> err() const& {
        if (is_err()) return std::get<1>(data_);
        return std::nullopt;
    }

    // Map success value
    template<typename F>
    auto map(F&& f) const& -> Result<decltype(f(std::declval<T>())), E> {
        using U = decltype(f(std::declval<T>()));
        if (is_ok()) return Result<U, E>(f(std::get<0>(data_)));
        return Result<U, E>(std::get<1>(data_));
    }

    // Map error
    template<typename F>
    auto map_err(F&& f) const& -> Result<T, decltype(f(std::declval<E>()))> {
        using U = decltype(f(std::declval<E>()));
        if (is_err()) return Result<T, U>(f(std::get<1>(data_)));
        return Result<T, U>(std::get<0>(data_));
    }

    // Unwrap with custom error message
    T expect(const char* msg) const& {
        if (is_err()) throw std::runtime_error(std::string(msg) + ": " + error().message);
        return std::get<0>(data_);
    }

private:
    std::variant<T, E> data_;
};

// =============================================================================
// Result<void, E> Specialization
// =============================================================================

template<typename E>
class Result<void, E> {
public:
    // Success constructor
    Result() : data_(std::monostate{}) {}

    // Error constructor
    template<typename Err, typename = std::enable_if_t<std::is_convertible_v<Err, E>>>
    Result(Err error) : data_(E(std::move(error))) {}

    bool is_ok() const { return std::holds_alternative<std::monostate>(data_); }
    bool is_err() const { return std::holds_alternative<E>(data_); }
    explicit operator bool() const { return is_ok(); }

    void value() const {
        if (is_err()) throw std::runtime_error("Result is error: " + error().message);
    }

    E& error() & {
        if (is_ok()) throw std::runtime_error("Result is ok, no error");
        return std::get<E>(data_);
    }

    const E& error() const& {
        if (is_ok()) throw std::runtime_error("Result is ok, no error");
        return std::get<E>(data_);
    }

    std::optional<E> err() const& {
        if (is_err()) return std::get<E>(data_);
        return std::nullopt;
    }

private:
    std::variant<std::monostate, E> data_;
};

// =============================================================================
// Helper Functions
// =============================================================================

// Create success result
template<typename T>
Result<std::decay_t<T>, Error> Ok(T&& value) {
    return Result<std::decay_t<T>, Error>(std::forward<T>(value));
}

// Create void success
inline Result<void, Error> Ok() {
    return Result<void, Error>();
}

// Create error result
template<typename T, typename E = Error>
Result<T, E> Err(E error) {
    return Result<T, E>(std::move(error));
}

template<typename T>
Result<T, Error> Err(std::string message, int code = 0) {
    return Result<T, Error>(Error(std::move(message), code));
}

template<typename T>
Result<T, Error> Err(const char* message, int code = 0) {
    return Result<T, Error>(Error(message, code));
}

// =============================================================================
// Macros for Early Return
// =============================================================================

// TRY macro: unwrap result or return error
// Usage: auto value = TRY(some_function());
#define MIRAGE_TRY(expr) \
    ({ \
        auto _result = (expr); \
        if (_result.is_err()) return _result.error(); \
        std::move(_result).value(); \
    })

// TRY_OR macro: unwrap result or use default
#define MIRAGE_TRY_OR(expr, default_val) \
    ((expr).value_or(default_val))

} // namespace mirage
