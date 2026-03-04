// SPDX-License-Identifier: Apache-2.0
/// @file emulated_spinlock.inl
/// @brief Spinlock stub for backends that do not provide a native spinlock.
/// @details All five spinlock functions return @c error_code::not_supported
///          (create, lock, try_lock) or a no-op (destroy, unlock).
///          The @c if @c constexpr guard in @c osal::spinlock ensures none of
///          these are called when @c has_spinlock == false; the stubs exist
///          solely to satisfy the linker in case the guard is bypassed.
///
///          #include inside the `extern "C"` block of any backend .cpp where
///          capabilities<backend>::has_spinlock == false.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

// ---------------------------------------------------------------------------
// Spinlock (stub — not natively supported on this backend)
// ---------------------------------------------------------------------------

/// @brief Create a spinlock — not supported on this backend.
/// @return @c error_code::not_supported always.
osal::result osal_spinlock_create(osal::active_traits::spinlock_handle_t* handle) noexcept
{
    if (handle)
    {
        handle->native = nullptr;
    }
    return osal::error_code::not_supported;
}

/// @brief Destroy a spinlock stub — always succeeds (no-op).
/// @return @c osal::ok() always.
osal::result osal_spinlock_destroy(osal::active_traits::spinlock_handle_t* handle) noexcept
{
    (void)handle;
    return osal::ok();
}

/// @brief Acquire the spinlock — not supported on this backend.
/// @return @c error_code::not_supported always.
osal::result osal_spinlock_lock(osal::active_traits::spinlock_handle_t* handle) noexcept
{
    (void)handle;
    return osal::error_code::not_supported;
}

/// @brief Try to acquire the spinlock — not supported on this backend.
/// @return @c false always.
bool osal_spinlock_try_lock(osal::active_traits::spinlock_handle_t* handle) noexcept
{
    (void)handle;
    return false;
}

/// @brief Release the spinlock — no-op on this backend.
void osal_spinlock_unlock(osal::active_traits::spinlock_handle_t* handle) noexcept
{
    (void)handle;
}
