#pragma once

#include <optional>
#include <string>
#include <utility>

#include "us3_turbo_access/common/error_code.h"

namespace us3_turbo_access::gateway {

using ErrorCode = ::us3_turbo_access::common::ErrorCode;

/**
 * @brief Failure detail returned by gateway operations.
 */
struct Error {
  ErrorCode   code{ErrorCode::kSuccess};
  std::string message;
  bool        retryable{false};
  std::string request_id;
};

/**
 * @brief Typed result wrapper used across gateway internals.
 *
 * Callers must inspect `success()` before reading `value()` or `error()`.
 */
template <typename T>
class Result {
 public:
  static Result Success(T value) { return Result(std::move(value)); }
  static Result Failure(Error err) { return Result(std::move(err)); }

  [[nodiscard]] bool         success() const { return success_; }
  [[nodiscard]] const T&     value() const { return *value_; }
  [[nodiscard]] T&           value() { return *value_; }
  [[nodiscard]] const Error& error() const { return error_; }

 private:
  explicit Result(T value) : success_(true), value_(std::move(value)) {}
  explicit Result(Error err) : success_(false), error_(std::move(err)) {}

  bool             success_{false};
  std::optional<T> value_{};
  Error            error_{};
};

/**
 * @brief Constructs a normalized error object.
 */
[[nodiscard]] inline Error MakeError(ErrorCode code, std::string message,
                                     bool retryable = false,
                                     std::string request_id = {}) {
  return Error{.code = code,
               .message = std::move(message),
               .retryable = retryable,
               .request_id = std::move(request_id)};
}

}  // namespace us3_turbo_access::gateway
