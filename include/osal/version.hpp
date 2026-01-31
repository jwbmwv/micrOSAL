// SPDX-License-Identifier: Apache-2.0
/// @file version.hpp
/// @brief Compile-time version information for MicrOSAL.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_core
#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Version macros
// ---------------------------------------------------------------------------

/// @defgroup osal_version Version
/// @brief Compile-time version macros and query API.
/// @{

#define MICRO_OSAL_VERSION_MAJOR 0
#define MICRO_OSAL_VERSION_MINOR 0
#define MICRO_OSAL_VERSION_PATCH 1

/// @brief Encodes major.minor.patch into a single integer: (M * 10000 + m * 100 + p).
#define MICRO_OSAL_VERSION_NUMBER \
    (MICRO_OSAL_VERSION_MAJOR * 10000 + MICRO_OSAL_VERSION_MINOR * 100 + MICRO_OSAL_VERSION_PATCH)

/// @brief Version as a string literal, e.g. "0.0.1".
#define MICRO_OSAL_VERSION_STRING "0.0.1"

/// @} // osal_version

namespace osal
{

/// @brief Compile-time version descriptor.
struct version_info
{
    std::uint16_t major;  ///< Major version component.
    std::uint16_t minor;  ///< Minor version component.
    std::uint16_t patch;  ///< Patch version component.

    /// @brief Returns the version encoded as (major * 10000 + minor * 100 + patch).
    [[nodiscard]] constexpr std::uint32_t number() const noexcept
    {
        return static_cast<std::uint32_t>(major) * 10000U + static_cast<std::uint32_t>(minor) * 100U +
               static_cast<std::uint32_t>(patch);
    }

    /// @brief Returns the version as a string literal, e.g. "0.0.1".
    [[nodiscard]] static constexpr const char* string() noexcept { return MICRO_OSAL_VERSION_STRING; }
};

/// @brief Returns the library version at compile-time.
/// @code
///   constexpr auto v = osal::version();
///   static_assert(v.major == 0 && v.minor == 0 && v.patch == 1);
/// @endcode
[[nodiscard]] inline constexpr version_info version() noexcept
{
    return {MICRO_OSAL_VERSION_MAJOR, MICRO_OSAL_VERSION_MINOR, MICRO_OSAL_VERSION_PATCH};
}

}  // namespace osal
