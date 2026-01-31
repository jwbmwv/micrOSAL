// SPDX-License-Identifier: Apache-2.0
/// @file emulated_barrier.inl
/// @brief Barrier stub for backends that do not provide a native barrier.
/// @details All barrier functions return @c error_code::not_supported (create,
///          wait) or a no-op (destroy).  The @c if @c constexpr guard in
///          @c osal::barrier ensures none of these are called when
///          @c has_barrier == false; the stubs exist solely to satisfy the
///          linker in case the guard is bypassed.
///
///          #include inside the `extern "C"` block of any backend .cpp where
///          capabilities<backend>::has_barrier == false.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

// ---------------------------------------------------------------------------
// Barrier (stub — not natively supported on this backend)
// ---------------------------------------------------------------------------

/// @brief Create a barrier — not supported on this backend.
/// @return @c error_code::not_supported always.
osal::result osal_barrier_create(osal::active_traits::barrier_handle_t* handle, unsigned /*count*/) noexcept
{
    if (handle)
    {
        handle->native = nullptr;
    }
    return osal::error_code::not_supported;
}

/// @brief Destroy a barrier stub — always succeeds (no-op).
/// @return @c osal::ok() always.
osal::result osal_barrier_destroy(osal::active_traits::barrier_handle_t* handle) noexcept
{
    (void)handle;
    return osal::ok();
}

/// @brief Wait at a barrier — not supported on this backend.
/// @return @c error_code::not_supported always.
osal::result osal_barrier_wait(osal::active_traits::barrier_handle_t* handle) noexcept
{
    (void)handle;
    return osal::error_code::not_supported;
}
