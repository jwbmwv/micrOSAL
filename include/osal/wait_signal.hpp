// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "detail/atomic_compat.hpp"
#include "diagnostics.hpp"
#include "semaphore.hpp"
#include "stop_token.hpp"
#include <type_traits>

namespace osal
{

/// @brief Explicit producer wake channel for one concurrent waiting consumer.
/// @details Producers mutate observed state first, then notify(). Notifications
///          coalesce in a bounded binary semaphore. No native subscriptions.
///          Requires a native blocking semaphore; bare-metal spin waits are excluded.
class wait_signal
{
public:
    static constexpr bool is_supported =
        timed_semaphore_backend<active_backend> && !std::is_same_v<active_backend, backend_baremetal>;
    static constexpr bool isr_notification_supported =
        is_supported && capabilities<active_backend>::has_isr_semaphore &&
        std::atomic<std::uint32_t>::is_always_lock_free && std::atomic<error_code>::is_always_lock_free;

    wait_signal() noexcept
    {
        if constexpr (is_supported)
        {
            status_        = osal_semaphore_create(&handle_, 0U, 1U);
            auto& counters = diagnostics(diagnostic_resource::semaphore);
            counters.attempt(status_);
            if (status_.ok())
            {
                counters.acquire();
            }
        }
        else
        {
            status_ = error_code::not_supported;
        }
    }
    /// @brief Best-effort destruction; caller must quiesce waiters and producers.
    ~wait_signal() noexcept
    {
        if (status_.ok())
        {
            const result destroyed = osal_semaphore_destroy(&handle_);
            auto&        counters  = diagnostics(diagnostic_resource::semaphore);
            counters.attempt(destroyed);
            if (destroyed.ok())
            {
                counters.release();
            }
        }
    }
    wait_signal(const wait_signal&)               = delete;
    wait_signal& operator=(const wait_signal&)    = delete;
    wait_signal(wait_signal&&)                    = delete;
    wait_signal&         operator=(wait_signal&&) = delete;
    [[nodiscard]] bool   valid() const noexcept { return status().ok(); }
    [[nodiscard]] result status() const noexcept
    {
        return status_.ok() ? result{failure_.load(std::memory_order_acquire)} : status_;
    }
    template<typename Backend = active_backend>
    static constexpr void require_support() noexcept
    {
        static_assert(timed_semaphore_backend<Backend> && !std::is_same_v<Backend, backend_baremetal>,
                      "wait_signal requires native blocking timed semaphore support, not bare-metal spin waits");
    }
    [[nodiscard]] result notify() noexcept { return notify_impl(false); }
    [[nodiscard]] result notify_from_isr() noexcept
    {
        if constexpr (isr_notification_supported)
        {
            return notify_impl(true);
        }
        return error_code::not_supported;
    }
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }

    /// @brief Claims exclusive consumer ownership; release after the wait.
    result begin_wait() noexcept
    {
        if (!status().ok())
        {
            return status();
        }
        return waiting_.test_and_set(std::memory_order_acquire) ? result{error_code::already_exists} : ok();
    }
    void end_wait() noexcept { waiting_.clear(std::memory_order_release); }

    /// @brief Internal wake wait, called only by the owning consumer.
    result wait_for_change(std::uint32_t observed, milliseconds timeout) noexcept
    {
        if (!status().ok())
        {
            return status();
        }
        if (generation() != observed)
        {
            return ok();
        }
        const tick_t ticks = timeout.count() < 0 ? WAIT_FOREVER : clock_utils::ms_to_ticks(timeout);
        const result r     = osal_semaphore_take(&handle_, ticks);
        if (!status().ok())
        {
            return status();
        }
        return r;
    }

private:
    result notify_impl(bool from_isr) noexcept
    {
        if (!status().ok())
        {
            return status();
        }
        generation_.fetch_add(1U, std::memory_order_release);
        const result r = from_isr ? osal_semaphore_give_isr(&handle_) : osal_semaphore_give(&handle_);
        if (r == error_code::overflow)
        {
            return ok();  // An existing permit already guarantees a recheck.
        }
        if (!r.ok())
        {
            failure_.store(r.code(), std::memory_order_release);
        }
        return r;
    }
    active_traits::semaphore_handle_t handle_{};
    result                            status_{error_code::not_initialized};
    std::atomic<error_code>           failure_{error_code::ok};
    std::atomic<std::uint32_t>        generation_{0U};
    std::atomic_flag                  waiting_ = ATOMIC_FLAG_INIT;
};

/// @brief Event-driven cancellable wait on a nonblocking readiness predicate.
/// @details Signal/source/predicate state must outlive this call. Producers must
///          notify after every change that can make the predicate true.
template<typename Predicate>
    requires std::is_nothrow_invocable_r_v<bool, Predicate&>
// The predicate is repeatedly invoked in place, never forwarded or consumed.
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward, hicpp-move-forward)
result wait_until_ready(wait_signal& signal, Predicate&& ready, milliseconds timeout = milliseconds{-1},
                        stop_token token = {}) noexcept
{
    const result claimed = signal.begin_wait();
    if (!claimed.ok())
    {
        return claimed;
    }
    struct release_wait
    {
        explicit release_wait(wait_signal* value) noexcept : signal{value} {}
        ~release_wait() noexcept { signal->end_wait(); }
        release_wait(const release_wait&)            = delete;
        release_wait& operator=(const release_wait&) = delete;
        release_wait(release_wait&&)                 = delete;
        release_wait& operator=(release_wait&&)      = delete;
        wait_signal*  signal;
    } release{&signal};
    const auto    deadline = monotonic_deadline::after(timeout);
    stop_callback callback{token, [](void* p) noexcept { (void)static_cast<wait_signal*>(p)->notify(); }, &signal};
    if (!callback.status().ok())
    {
        return callback.status();
    }
    for (;;)
    {
        const auto observed = signal.generation();
        if (!signal.status().ok())
        {
            return signal.status();
        }
        if (token.stop_requested())
        {
            return error_code::cancelled;
        }
        if (ready())
        {
            return ok();
        }
        if (timeout.count() >= 0 && deadline.expired())
        {
            return error_code::timeout;
        }
        const result r =
            signal.wait_for_change(observed, timeout.count() < 0 ? milliseconds{-1} : deadline.remaining());
        if (!r.ok() && (timeout.count() < 0 || (r != error_code::timeout && r != error_code::would_block) ||
                        (r == error_code::would_block && !deadline.expired())))
        {
            return r;
        }
    }
}

}  // namespace osal
