// SPDX-License-Identifier: Apache-2.0
/// @file cpp_compat.hpp
/// @brief C++23 / C++26 compatibility and feature-gated enhancements
/// @details Library features follow standard-library feature macros. Compiler
///          optimization hints can also be available as C++20 extensions.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_core
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <type_traits>
#include <utility>

#if __has_include(<version>)
#include <version>
#endif

// ---------------------------------------------------------------------------
// 1. [[assume(expr)]] attribute (C++23)
// ---------------------------------------------------------------------------
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(assume) >= 202207L
#define OSAL_ASSUME(expr) [[assume(expr)]]
#endif
#endif

#ifndef OSAL_ASSUME
#if defined(__clang__)
#define OSAL_ASSUME(expr) __builtin_assume(expr)
#elif defined(__GNUC__) && __GNUC__ >= 13
#define OSAL_ASSUME(expr) __attribute__((assume(expr)))
#else
#define OSAL_ASSUME(expr) (void)0
#endif
#endif

// ---------------------------------------------------------------------------
// 2. std::unreachable() (C++23)
// ---------------------------------------------------------------------------
#if defined(__cpp_lib_unreachable) && __cpp_lib_unreachable >= 202202L
#include <utility>
#define OSAL_UNREACHABLE() std::unreachable()
#elif defined(__GNUC__) || defined(__clang__)
#define OSAL_UNREACHABLE() __builtin_unreachable()
#else
#define OSAL_UNREACHABLE() (void)0
#endif

// ---------------------------------------------------------------------------
// 3. std::to_underlying() (C++23)
// ---------------------------------------------------------------------------
#if defined(__cpp_lib_to_underlying) && __cpp_lib_to_underlying >= 202102L
#include <utility>
namespace osal::detail
{
using std::to_underlying;
}
#else
namespace osal::detail
{
template<typename Enum>
constexpr std::underlying_type_t<Enum> to_underlying(Enum e) noexcept
{
    return static_cast<std::underlying_type_t<Enum>>(e);
}
}  // namespace osal::detail
#endif

// ---------------------------------------------------------------------------
// 4. std::inplace_vector or bounded fallback (C++26)
// ---------------------------------------------------------------------------
#if defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202406L
#include <inplace_vector>
#endif

namespace osal::detail
{
/// @brief Fixed-capacity storage for trivial snapshot values with checked, non-throwing mutation.
/// @details Both implementations reserve N elements of storage. Only [begin(), end()) is active.
///          Capacity overflow returns false without modifying the container.
template<typename T, std::size_t N>
    requires(std::is_trivially_copyable_v<T> && std::is_trivially_default_constructible_v<T> &&
             std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_copy_assignable_v<T> && std::is_nothrow_move_assignable_v<T>)
class bounded_vector
{
public:
    using value_type = T;
    using size_type  = std::size_t;

    constexpr bounded_vector() noexcept = default;

    [[nodiscard]] constexpr bool      empty() const noexcept { return size() == 0U; }
    [[nodiscard]] constexpr bool      full() const noexcept { return size() == N; }
    [[nodiscard]] constexpr size_type size() const noexcept
    {
#if defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202406L
        return data_.size();
#else
        return size_;
#endif
    }
    [[nodiscard]] static constexpr size_type capacity() noexcept { return N; }

    constexpr bool push_back(const T& value) noexcept
    {
#if defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202406L
        return static_cast<bool>(data_.try_push_back(value));
#else
        if (size_ >= N)
        {
            return false;
        }
        data_[size_++] = value;
        return true;
#endif
    }

    constexpr bool push_back(T&& value) noexcept
    {
#if defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202406L
        return static_cast<bool>(data_.try_push_back(std::move(value)));
#else
        if (size_ >= N)
        {
            return false;
        }
        data_[size_++] = std::move(value);
        return true;
#endif
    }

    constexpr bool resize(size_type count) noexcept
    {
        if (count > N)
        {
            return false;
        }
#if defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202406L
        data_.resize(count);
#else
        for (size_type index = size_; index < count; ++index)
        {
            data_[index] = T{};
        }
        size_ = count;
#endif
        return true;
    }

    constexpr void clear() noexcept
    {
#if defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202406L
        data_.clear();
#else
        size_ = 0U;
#endif
    }

    [[nodiscard]] constexpr T*       data() noexcept { return data_.data(); }
    [[nodiscard]] constexpr const T* data() const noexcept { return data_.data(); }

    [[nodiscard]] constexpr T*       begin() noexcept { return data(); }
    [[nodiscard]] constexpr const T* begin() const noexcept { return data(); }
    [[nodiscard]] constexpr T*       end() noexcept { return empty() ? data() : data() + size(); }
    [[nodiscard]] constexpr const T* end() const noexcept { return empty() ? data() : data() + size(); }

    [[nodiscard]] constexpr T&       operator[](size_type idx) noexcept { return data_[idx]; }
    [[nodiscard]] constexpr const T& operator[](size_type idx) const noexcept { return data_[idx]; }

private:
#if defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202406L
    std::inplace_vector<T, N> data_{};
#else
    std::array<T, N> data_{};
    size_type        size_{0U};
#endif
};
}  // namespace osal::detail
