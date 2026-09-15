// SPDX-License-Identifier: Apache-2.0
/// @file error.hpp
/// @brief OSAL error and result types
/// @details Defines osal::result and osal::error_code — the canonical return type
///          for all OSAL operations.  No exceptions are used anywhere in the OSAL;
///          all error information is carried via this type.
///
///          Rules:
///          - All OSAL functions that can fail return osal::result or bool (noexcept).
///          - result is a lightweight enum; its storage cost is one int.
///          - Use osal_assert() (back-end supplied) for unrecoverable invariant
///            violations.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_core
#pragma once

#if !defined(__cplusplus) || (__cplusplus < 202002L)
#error "micrOSAL requires at least C++20. Compile with -std=c++20 (or newer)."
#endif

#include "detail/cpp_compat.hpp"
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#include <expected>
#endif

namespace osal
{

/// @defgroup osal_core OSAL Core
/// @brief Foundational types shared by all OSAL backends.
/// @{

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

/// @brief Enumeration of all OSAL error codes.
/// @details Returned by OSAL operations that can fail.  The set is intentionally
///          small; backend-specific errors are mapped to the nearest canonical value.
enum class error_code : std::int32_t
{
    ok                = 0,   ///< Success — no error.
    timeout           = 1,   ///< Operation timed out.
    would_block       = 2,   ///< Non-blocking call would have blocked.
    invalid_argument  = 3,   ///< One or more arguments are invalid.
    not_supported     = 4,   ///< Operation not supported on this backend/config.
    out_of_resources  = 5,   ///< Insufficient OS/backend resources.
    permission_denied = 6,   ///< Caller lacks the required privilege.
    already_exists    = 7,   ///< Resource already created / acquired.
    not_initialized   = 8,   ///< Object has not been successfully initialized.
    overflow          = 9,   ///< Queue or counter overflow.
    underflow         = 10,  ///< Queue or counter underflow.
    deadlock_detected = 11,  ///< Deadlock condition detected (debug mode).
    not_owner         = 12,  ///< Calling thread does not own the resource.
    isr_invalid       = 13,  ///< Operation is not valid from an ISR context.
    barrier_serial =
        14,         ///< This thread is the barrier's serial thread (POSIX PTHREAD_BARRIER_SERIAL_THREAD; not an error).
    unknown = 255,  ///< Unmapped / unspecified backend error.
};

// ---------------------------------------------------------------------------
// result — lightweight status wrapper
// ---------------------------------------------------------------------------

/// @brief Lightweight result type returned by OSAL operations.
/// @details Wraps osal::error_code. Intentionally not std::expected — MicrOSAL
///          keeps this type ABI-stable, allocation-free, and exception-free
///          under the library's C++20 baseline.
///
/// @note  The implicit conversion from error_code makes it easy to write:
///        @code
///          return osal::error_code::timeout;
///        @endcode
///        while letting callers do @code if (r.ok()) @endcode.
struct result
{
    /// @brief Constructs a successful result.
    constexpr result() noexcept : code_(error_code::ok) {}

    /// @brief Constructs from an error code.
    /// @param[in] c  The error code.
    constexpr result(error_code c) noexcept : code_(c) {}  // NOLINT(google-explicit-constructor)

    /// @brief Returns true if the operation succeeded.
    /// @return true if error_code == ok.
    [[nodiscard]] constexpr bool ok() const noexcept { return code_ == error_code::ok; }

    /// @brief Returns the underlying error code.
    /// @return The error_code enumeration value.
    [[nodiscard]] constexpr error_code code() const noexcept { return code_; }

    /// @brief Contextual bool conversion — true on success.
    constexpr explicit operator bool() const noexcept { return ok(); }

    /// @brief Equality comparison.
    constexpr bool operator==(const result& o) const noexcept { return code_ == o.code_; }
    /// @brief Inequality comparison.
    constexpr bool operator!=(const result& o) const noexcept { return code_ != o.code_; }
    /// @brief Comparison with raw error_code.
    constexpr bool operator==(error_code c) const noexcept { return code_ == c; }
    /// @brief Comparison with raw error_code.
    constexpr bool operator!=(error_code c) const noexcept { return code_ != c; }

    /// @brief Runs a result-returning callback on success; otherwise preserves the error.
    /// @details Available in C++20. Use to_expected() for value-returning monadic chains.
    template<typename Function>
        requires std::is_same_v<std::invoke_result_t<Function>, result>
    constexpr result and_then(Function&& function) const noexcept(std::is_nothrow_invocable_v<Function>)
    {
        if (ok())
        {
            return std::invoke(std::forward<Function>(function));
        }
        return *this;
    }

    /// @brief Status-only synonym for and_then(); the callback must return result.
    /// @details This does not transform a value. Use to_expected().transform() for that operation.
    template<typename Function>
        requires std::is_same_v<std::invoke_result_t<Function>, result>
    constexpr result transform(Function&& function) const noexcept(std::is_nothrow_invocable_v<Function>)
    {
        return and_then(std::forward<Function>(function));
    }

    /// @brief Runs a result-returning error handler on failure; otherwise preserves success.
    template<typename Function>
        requires std::is_same_v<std::invoke_result_t<Function, error_code>, result>
    constexpr result or_else(Function&& function) const noexcept(std::is_nothrow_invocable_v<Function, error_code>)
    {
        if (!ok())
        {
            return std::invoke(std::forward<Function>(function), code());
        }
        return *this;
    }

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
    /// @brief Converts result to std::expected<void, error_code> (C++23).
    [[nodiscard]] constexpr std::expected<void, error_code> to_expected() const noexcept
    {
        if (ok())
        {
            return {};
        }
        return std::unexpected(code_);
    }
#endif

private:
    error_code code_;
};

/// @brief Convenience constant for a successful result.
constexpr result ok() noexcept
{
    return {};
}

/// @} // osal_core

}  // namespace osal
