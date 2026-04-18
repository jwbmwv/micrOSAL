// SPDX-License-Identifier: Apache-2.0
/// @file work_queue.hpp
/// @brief OSAL work queue — deferred callback execution
/// @details Provides osal::work_queue for posting work items (function + arg)
///          to a dedicated worker thread.  On backends with native work queues
///          (Zephyr k_work_queue, NuttX work_queue(), VxWorks jobQueueLib) the
///          native primitive is used directly.  On all other backends, a
///          thread + ring-buffer emulation is provided automatically.
///
///          The class is non-copyable, non-movable, and stores no dynamically
///          allocated memory visible to the caller — the worker thread stack
///          is caller-supplied.
///
///          Usage:
///          @code
///          alignas(16) static uint8_t wq_stack[4096];
///          osal::work_queue wq{wq_stack, sizeof(wq_stack), 16, "my_wq"};
///          wq.submit([](void*) { do_something(); });
///          wq.flush();
///          @endcode
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_work_queue
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>

#ifndef OSAL_WORK_QUEUE_MAX_DEPTH
/// @brief Maximum depth for the internal work item ring buffer (emulated mode).
#define OSAL_WORK_QUEUE_MAX_DEPTH 32U
#endif

/// @brief Signature for a work item callback.
using osal_work_func_t = void (*)(void* arg);

extern "C"
{
    osal::result osal_work_queue_create(osal::active_traits::work_queue_handle_t* handle, void* stack,
                                        std::size_t stack_bytes, std::size_t depth, const char* name) noexcept;

    osal::result osal_work_queue_destroy(osal::active_traits::work_queue_handle_t* handle) noexcept;

    osal::result osal_work_queue_submit(osal::active_traits::work_queue_handle_t* handle, osal_work_func_t func,
                                        void* arg) noexcept;

    osal::result osal_work_queue_submit_from_isr(osal::active_traits::work_queue_handle_t* handle,
                                                 osal_work_func_t func, void* arg) noexcept;

    osal::result osal_work_queue_flush(osal::active_traits::work_queue_handle_t* handle,
                                       osal::tick_t                              timeout_ticks) noexcept;

    osal::result osal_work_queue_cancel_all(osal::active_traits::work_queue_handle_t* handle) noexcept;

    std::size_t osal_work_queue_pending(const osal::active_traits::work_queue_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_work_queue OSAL Work Queue
/// @brief Deferred work execution on a dedicated worker thread.
/// @{

/// @brief Signature for a work item callback.
using work_func_t = osal_work_func_t;

/// @brief A single deferrable unit of work.
struct work_item
{
    work_func_t func = nullptr;  ///< Function to execute.
    void*       arg  = nullptr;  ///< Argument passed to func.
};

// ---------------------------------------------------------------------------
// work_queue_config — constexpr-constructible, place in .rodata / FLASH
// ---------------------------------------------------------------------------

/// @brief Immutable configuration for work queue creation.
/// @details Declare as @c const to place in .rodata (FLASH).  The @c stack
///          pointer targets RAM, but the config descriptor itself lives in FLASH.
///
///          @code
///          alignas(16) static uint8_t stack[4096];
///          const osal::work_queue_config wq_cfg{stack, sizeof(stack), 16, "my_wq"};
///          osal::work_queue wq{wq_cfg};
///          @endcode
struct work_queue_config
{
    void*       stack       = nullptr;  ///< Caller-supplied stack for the worker thread.
    std::size_t stack_bytes = 0;        ///< Size of the stack in bytes.
    std::size_t depth       = 16;       ///< Maximum number of queued work items.
    const char* name        = "wq";     ///< Human-readable name (for debugging).
};

/// @brief OSAL work queue — deferred execution of callbacks on a dedicated thread.
///
/// @details On backends with native work queue support (Zephyr, NuttX, VxWorks),
///          the native primitive is used.  On all other backends, an emulated
///          implementation is provided using a thread + ring buffer + condvar.
///
///          The caller supplies the worker thread stack.  The internal ring
///          buffer is allocated by the backend (typically via `new` on
///          POSIX/Linux, or from a static pool on RTOS backends).
class work_queue
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and starts the work queue.
    /// @param stack        Caller-supplied stack for the worker thread.
    /// @param stack_bytes  Size of the stack in bytes.
    /// @param depth        Maximum number of queued work items (capped at
    ///                     OSAL_WORK_QUEUE_MAX_DEPTH).
    /// @param name         Human-readable name (for debugging).
    work_queue(void* stack, std::size_t stack_bytes, std::size_t depth = 16, const char* name = "wq") noexcept
        : valid_(false), handle_{}
    {
        const std::size_t capped = (depth > OSAL_WORK_QUEUE_MAX_DEPTH) ? OSAL_WORK_QUEUE_MAX_DEPTH : depth;
        valid_                   = osal_work_queue_create(&handle_, stack, stack_bytes, capped, name).ok();
    }

    /// @brief Constructs from an immutable config (config may reside in FLASH).
    /// @param cfg  Configuration — typically declared @c const.
    /// @complexity O(1)
    explicit work_queue(const work_queue_config& cfg) noexcept : valid_(false), handle_{}
    {
        const std::size_t capped = (cfg.depth > OSAL_WORK_QUEUE_MAX_DEPTH) ? OSAL_WORK_QUEUE_MAX_DEPTH : cfg.depth;
        valid_                   = osal_work_queue_create(&handle_, cfg.stack, cfg.stack_bytes, capped, cfg.name).ok();
    }

    /// @brief Destroys the work queue.  Pending items are cancelled and the
    ///        worker thread is joined.
    ~work_queue() noexcept
    {
        if (valid_)
        {
            (void)osal_work_queue_destroy(&handle_);
            valid_ = false;
        }
    }

    work_queue(const work_queue&)            = delete;
    work_queue& operator=(const work_queue&) = delete;
    work_queue(work_queue&&)                 = delete;
    work_queue& operator=(work_queue&&)      = delete;

    // ---- submit ------------------------------------------------------------

    /// @brief Submits a work item for deferred execution.
    /// @param func  Function to call on the worker thread.
    /// @param arg   Argument passed to @p func.
    /// @return result::ok() on success; error_code::overflow if the queue is full.
    [[nodiscard]] result submit(work_func_t func, void* arg = nullptr) noexcept
    {
        return osal_work_queue_submit(&handle_, func, arg);
    }

    /// @brief Submits a work item from ISR context.
    /// @return error_code::not_supported on backends without ISR-safe queues.
    [[nodiscard]] result submit_from_isr(work_func_t func, void* arg = nullptr) noexcept
    {
        return osal_work_queue_submit_from_isr(&handle_, func, arg);
    }

    // ---- control -----------------------------------------------------------

    /// @brief Blocks until all currently-queued items have been executed.
    /// @param timeout  Maximum time to wait.
    /// @return result::ok() if flushed; error_code::timeout on expiry.
    [[nodiscard]] result flush(milliseconds timeout = milliseconds{5000}) noexcept
    {
        const tick_t ticks = (timeout.count() < 0) ? WAIT_FOREVER : clock_utils::ms_to_ticks(timeout);
        return osal_work_queue_flush(&handle_, ticks);
    }

    /// @brief Cancels all pending (not yet started) items.
    [[nodiscard]] result cancel_all() noexcept { return osal_work_queue_cancel_all(&handle_); }

    // ---- query -------------------------------------------------------------

    /// @brief Returns the number of items currently queued (not yet executed).
    [[nodiscard]] std::size_t pending() const noexcept { return osal_work_queue_pending(&handle_); }

    /// @brief Returns true if the work queue was successfully initialised.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    bool                               valid_;
    active_traits::work_queue_handle_t handle_{};
};

/// @} // osal_work_queue

}  // namespace osal
