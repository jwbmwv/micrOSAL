// SPDX-License-Identifier: Apache-2.0
/// @file emulated_task_notify.inl
/// @brief Task-notification stub for backends without a native notify primitive.
/// @details All three task-notification functions return
///          @c error_code::not_supported.  The @c if @c constexpr guard in
///          @c osal::thread ensures none of these are called when
///          @c has_task_notification == false; the stubs exist solely to
///          satisfy the linker in case the guard is bypassed.
///
///          #include inside the `extern "C"` block of any backend .cpp where
///          capabilities<backend>::has_task_notification == false.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

// ---------------------------------------------------------------------------
// Task notification (stub — not natively supported on this backend)
// ---------------------------------------------------------------------------

/// @brief Send a direct-to-task notification — not supported on this backend.
/// @return @c error_code::not_supported always.
osal::result osal_task_notify(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept
{
    (void)handle;
    (void)value;
    return osal::error_code::not_supported;
}

/// @brief Send a direct-to-task notification from ISR — not supported on this backend.
/// @return @c error_code::not_supported always.
osal::result osal_task_notify_isr(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept
{
    (void)handle;
    (void)value;
    return osal::error_code::not_supported;
}

/// @brief Wait for a task notification — not supported on this backend.
/// @return @c error_code::not_supported always.
osal::result osal_task_notify_wait(std::uint32_t clear_on_entry, std::uint32_t clear_on_exit, std::uint32_t* value_out,
                                   osal::tick_t timeout_ticks) noexcept
{
    (void)clear_on_entry;
    (void)clear_on_exit;
    (void)value_out;
    (void)timeout_ticks;
    return osal::error_code::not_supported;
}
