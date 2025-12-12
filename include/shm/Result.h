#pragma once
#include <string>
#include <variant>
#include <optional>

namespace shm {

/**
 * @brief Error codes for Shared Memory operations.
 */
enum class Error {
    None = 0,
    Timeout,
    BufferTooSmall,
    InvalidArgs,
    NotConnected,
    ResourceExhausted,
    InternalError,
    ProtocolViolation
};

/**
 * @brief A simple Result type holding either a value or an error.
 * @tparam T The value type.
 */
template <typename T>
class Result {
    std::variant<T, Error> data;

public:
    Result(T value) : data(std::move(value)) {}
    Result(Error error) : data(error) {}

    bool HasError() const {
        return std::holds_alternative<Error>(data);
    }

    Error GetError() const {
        if (HasError()) return std::get<Error>(data);
        return Error::None;
    }

    T Value() const {
        if (HasError()) throw std::runtime_error("Attempted to access value of error result");
        return std::get<T>(data);
    }

    T ValueOr(T defaultValue) const {
        if (HasError()) return defaultValue;
        return std::get<T>(data);
    }

    /**
     * @brief Evaluates to true if the result is successful (no error).
     */
    explicit operator bool() const {
        return !HasError();
    }
};

/**
 * @brief Specialization for void.
 */
template <>
class Result<void> {
    Error error;

public:
    Result() : error(Error::None) {}
    Result(Error e) : error(e) {}

    static Result<void> Success() { return Result(); }
    static Result<void> Failure(Error e) { return Result(e); }

    bool HasError() const { return error != Error::None; }
    Error GetError() const { return error; }

    /**
     * @brief Evaluates to true if the result is successful (no error).
     * This allows usage like: if (!host.Init(...)) { ... }
     */
    explicit operator bool() const {
        return !HasError();
    }
};

}
