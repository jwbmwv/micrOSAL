// SPDX-License-Identifier: Apache-2.0
/// @file osal_c.cpp
/// @brief C interface bridge — translates pure-C calls to the C++ backend API.
/// @details This file is compiled as C++20 and linked into the microsal library.
///          It provides thin extern "C" wrappers that convert between the C-side
///          types (osal_mutex_handle, osal_result_t, etc.) and the C++ types
///          (osal::active_traits::mutex_handle_t, osal::result, etc.).
///
///          Most backend-backed C and C++ handle types are layout-compatible
///          (both are `struct { void* native; }`), so conversion is a
///          reinterpret_cast. Composite helpers such as
///          osal_notification_handle and osal_delayable_work_handle are built
///          directly from other C OSAL primitives in this file and are not
///          cast to backend-native handle types.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

#include <osal/osal.hpp>
#include <osal/osal_c.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(OSAL_BACKEND_LINUX) || defined(OSAL_BACKEND_POSIX)
#include "posix/posix_mutex_policy.hpp"
#endif

extern "C" osal::result osal_mutex_create_config(osal::active_traits::mutex_handle_t* handle,
                                                 const osal::mutex_config*            config) noexcept
{
    if (handle == nullptr || config == nullptr ||
        (config->type != osal::mutex_type::normal && config->type != osal::mutex_type::recursive) ||
        config->protocol > osal::mutex_protocol::priority_ceiling ||
        config->robustness > osal::mutex_robustness::robust)
    {
        return osal::error_code::invalid_argument;
    }
    if (handle->native != nullptr)
    {
        return osal::error_code::already_exists;
    }
#if defined(OSAL_BACKEND_LINUX) || defined(OSAL_BACKEND_POSIX)
    return osal::detail::create_pthread_mutex(handle, *config);
#else
    bool protocol_supported = config->protocol == osal::mutex_protocol::native_default;
#if defined(OSAL_BACKEND_FREERTOS) || defined(OSAL_BACKEND_ZEPHYR)
    protocol_supported = protocol_supported || (config->protocol == osal::mutex_protocol::priority_inheritance &&
                                                osal::active_capabilities::has_priority_inheritance);
#endif
    if (!protocol_supported || config->robustness != osal::mutex_robustness::non_robust)
    {
        return osal::error_code::not_supported;
    }
    return osal_mutex_create(handle, config->type == osal::mutex_type::recursive);
#endif
}

extern "C" osal::result osal_mutex_consistent(osal::active_traits::mutex_handle_t* handle) noexcept
{
    if (handle == nullptr || handle->native == nullptr)
    {
        return osal::error_code::not_initialized;
    }
#if (defined(OSAL_BACKEND_LINUX) || defined(OSAL_BACKEND_POSIX)) && defined(__linux__)
    return osal::detail::pthread_mutex_status(pthread_mutex_consistent(static_cast<pthread_mutex_t*>(handle->native)));
#else
    return osal::error_code::not_supported;
#endif
}

// ---------------------------------------------------------------------------
// Handle conversion helpers (layout-compatible: struct { void* native; })
// ---------------------------------------------------------------------------
template<typename CppHandleType, typename CHandleType>
[[nodiscard]] inline CppHandleType* osal_c_cast(CHandleType* handle) noexcept
{
    return reinterpret_cast<CppHandleType*>(handle);
}

template<typename CppHandleType, typename CHandleType>
[[nodiscard]] inline const CppHandleType* osal_c_cast_const(const CHandleType* handle) noexcept
{
    return reinterpret_cast<const CppHandleType*>(handle);
}

#define OSAL_C_CAST(CppHandleType, c_ptr) (osal_c_cast<CppHandleType>((c_ptr)))

#define OSAL_C_CAST_CONST(CppHandleType, c_ptr) (osal_c_cast_const<CppHandleType>((c_ptr)))

// ---------------------------------------------------------------------------
// Shorthand for the active-backend handle types
// ---------------------------------------------------------------------------
using mtx_h = osal::active_traits::mutex_handle_t;
using sem_h = osal::active_traits::semaphore_handle_t;
using que_h = osal::active_traits::queue_handle_t;
using thr_h = osal::active_traits::thread_handle_t;
using tmr_h = osal::active_traits::timer_handle_t;
using ef_h  = osal::active_traits::event_flags_handle_t;
using cv_h  = osal::active_traits::condvar_handle_t;
using wq_h  = osal::active_traits::work_queue_handle_t;
using mp_h  = osal::active_traits::memory_pool_handle_t;
using rw_h  = osal::active_traits::rwlock_handle_t;
using sb_h  = osal::active_traits::stream_buffer_handle_t;
using mb_h  = osal::active_traits::message_buffer_handle_t;

// ---------------------------------------------------------------------------
// Layout compatibility contract — ensures reinterpret_cast is safe
// ---------------------------------------------------------------------------
#define OSAL_ASSERT_HANDLE_LAYOUT(CType, CppType)                                            \
    static_assert(sizeof(CType) == sizeof(CppType), #CType " / " #CppType " size mismatch"); \
    static_assert(alignof(CType) == alignof(CppType), #CType " / " #CppType " alignment mismatch")

OSAL_ASSERT_HANDLE_LAYOUT(osal_mutex_handle, mtx_h);
OSAL_ASSERT_HANDLE_LAYOUT(osal_semaphore_handle, sem_h);
OSAL_ASSERT_HANDLE_LAYOUT(osal_queue_handle, que_h);
OSAL_ASSERT_HANDLE_LAYOUT(osal_thread_handle, thr_h);
OSAL_ASSERT_HANDLE_LAYOUT(osal_timer_handle, tmr_h);
OSAL_ASSERT_HANDLE_LAYOUT(osal_event_flags_handle, ef_h);
OSAL_ASSERT_HANDLE_LAYOUT(osal_condvar_handle, cv_h);
OSAL_ASSERT_HANDLE_LAYOUT(osal_work_queue_handle, wq_h);
OSAL_ASSERT_HANDLE_LAYOUT(osal_memory_pool_handle, mp_h);
OSAL_ASSERT_HANDLE_LAYOUT(osal_rwlock_handle, rw_h);
OSAL_ASSERT_HANDLE_LAYOUT(osal_stream_buffer_handle, sb_h);
OSAL_ASSERT_HANDLE_LAYOUT(osal_message_buffer_handle, mb_h);

#undef OSAL_ASSERT_HANDLE_LAYOUT

namespace
{

inline osal_result_t to_c(osal::result r) noexcept
{
    return static_cast<osal_result_t>(osal::detail::to_underlying(r.code()));
}

inline osal_result_t record_c_creation(osal::diagnostic_resource resource, osal::result status) noexcept
{
    auto& counters = osal::diagnostics(resource);
    counters.attempt(status);
    if (status.ok())
    {
        counters.acquire();
    }
    return to_c(status);
}

template<typename Handle, typename Destroy>
osal_result_t record_c_destruction(osal::diagnostic_resource resource, Handle* handle, Destroy destroy) noexcept
{
    const bool owned  = handle != nullptr && handle->native != nullptr;
    const auto status = destroy(handle);
    if (owned && status.ok())
    {
        osal::diagnostics(resource).release();
    }
    return to_c(status);
}

enum class c_delayable_state : std::uint8_t
{
    idle     = 0U,
    armed    = 1U,
    queued   = 2U,
    running  = 3U,
    stopping = 4U,
};

constexpr std::uint32_t c_delayable_state_mask      = 7U;
constexpr std::uint32_t c_delayable_generation_step = 8U;

[[nodiscard]] bool notification_valid(const osal_notification_handle* handle) noexcept
{
    return (handle != nullptr) && (handle->valid != 0U);
}

[[nodiscard]] bool notification_index_valid(const osal_notification_handle* handle, std::size_t index) noexcept
{
    return notification_valid(handle) && (index < handle->slot_count);
}

[[nodiscard]] bool delayable_valid(const osal_delayable_work_handle* handle) noexcept
{
    return (handle != nullptr) && (__atomic_load_n(&handle->valid, __ATOMIC_ACQUIRE) != 0U);
}

// Forward declaration for the work-queue callback trampoline.
void c_delayable_work_work_cb(void* arg) noexcept;

[[nodiscard]] bool delayable_state_is(const osal_delayable_work_handle* handle, c_delayable_state state) noexcept
{
    return delayable_valid(handle) && ((__atomic_load_n(&handle->state, __ATOMIC_ACQUIRE) & c_delayable_state_mask) ==
                                       osal::detail::to_underlying(state));
}

[[nodiscard]] bool delayable_backend_supported() noexcept
{
    return osal::delayable_work_backend<osal::active_backend>;
}

[[nodiscard]] c_delayable_state delayable_state_load(const osal_delayable_work_handle* handle) noexcept
{
    return static_cast<c_delayable_state>(__atomic_load_n(&handle->state, __ATOMIC_ACQUIRE) & c_delayable_state_mask);
}

void delayable_state_store(osal_delayable_work_handle* handle, c_delayable_state state) noexcept
{
    const auto current = __atomic_load_n(&handle->state, __ATOMIC_RELAXED);
    __atomic_store_n(&handle->state, (current & ~c_delayable_state_mask) | osal::detail::to_underlying(state),
                     __ATOMIC_RELEASE);
}

bool delayable_state_compare_exchange(osal_delayable_work_handle* handle, c_delayable_state* expected,
                                      c_delayable_state desired) noexcept
{
    auto       expected_raw = __atomic_load_n(&handle->state, __ATOMIC_ACQUIRE);
    const auto observed     = static_cast<c_delayable_state>(expected_raw & c_delayable_state_mask);
    if (observed != *expected)
    {
        *expected = observed;
        return false;
    }
    const auto desired_raw = (expected_raw & ~c_delayable_state_mask) | osal::detail::to_underlying(desired);
    const bool exchanged   = __atomic_compare_exchange_n(&handle->state, &expected_raw, desired_raw, false,
                                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    *expected              = static_cast<c_delayable_state>(expected_raw & c_delayable_state_mask);
    return exchanged;
}

void delayable_clear_dispatch_error(osal_delayable_work_handle* handle) noexcept
{
    __atomic_store_n(&handle->dispatch_error, OSAL_OK, __ATOMIC_RELEASE);
    __atomic_store_n(&handle->dispatch_failed, 0U, __ATOMIC_RELEASE);
}

void delayable_set_dispatch_error(osal_delayable_work_handle* handle, osal_result_t rc) noexcept
{
    __atomic_store_n(&handle->dispatch_error, rc, __ATOMIC_RELEASE);
    __atomic_store_n(&handle->dispatch_failed, 1U, __ATOMIC_RELEASE);
}

[[nodiscard]] osal_result_t delayable_consume_dispatch_error(osal_delayable_work_handle* handle) noexcept
{
    if (__atomic_exchange_n(&handle->dispatch_failed, 0U, __ATOMIC_ACQ_REL) == 0U)
    {
        return OSAL_OK;
    }

    return __atomic_exchange_n(&handle->dispatch_error, OSAL_OK, __ATOMIC_ACQ_REL);
}

[[nodiscard]] osal::milliseconds delayable_poll_interval() noexcept
{
    // Keep polling nonzero when a tick converts to less than one millisecond.
    const auto tick = osal::clock_utils::ticks_to_ms(static_cast<osal::tick_t>(1));
    return (tick.count() > 0) ? tick : osal::milliseconds{1};
}

// Caller holds handle->mutex.
osal_result_t delayable_enqueue_locked(osal_delayable_work_handle* handle) noexcept
{
    delayable_state_store(handle, c_delayable_state::queued);
    const osal_result_t rc = osal_c_work_queue_submit(handle->queue, c_delayable_work_work_cb, handle);
    if (rc != OSAL_OK)
    {
        delayable_set_dispatch_error(handle, rc);
        delayable_state_store(handle, c_delayable_state::idle);
    }
    return rc;
}

// Caller holds handle->mutex.
osal_result_t delayable_arm_locked(osal_delayable_work_handle* handle, osal_tick_t delay_ticks) noexcept
{
    const auto          started = static_cast<std::uint64_t>(osal_clock_ticks());
    const osal_result_t set_rc  = osal_c_timer_set_period(&handle->timer, delay_ticks);
    if (set_rc != OSAL_OK)
    {
        return set_rc;
    }

    const auto delay = static_cast<std::uint64_t>(delay_ticks);
    __atomic_store_n(&handle->start_low, static_cast<std::uint32_t>(started), __ATOMIC_RELAXED);
    __atomic_store_n(&handle->start_high, static_cast<std::uint32_t>(started >> 32U), __ATOMIC_RELAXED);
    __atomic_store_n(&handle->delay_low, static_cast<std::uint32_t>(delay), __ATOMIC_RELAXED);
    __atomic_store_n(&handle->delay_high, static_cast<std::uint32_t>(delay >> 32U), __ATOMIC_RELAXED);
    const auto generation =
        (__atomic_load_n(&handle->state, __ATOMIC_RELAXED) & ~c_delayable_state_mask) + c_delayable_generation_step;
    __atomic_store_n(&handle->state, generation | osal::detail::to_underlying(c_delayable_state::armed),
                     __ATOMIC_RELEASE);
    const osal_result_t start_rc = osal_c_timer_start(&handle->timer);
    if (start_rc != OSAL_OK)
    {
        delayable_state_store(handle, c_delayable_state::idle);
    }
    return start_rc;
}

[[nodiscard]] osal::milliseconds ticks_to_duration(osal_tick_t ticks) noexcept
{
    if (ticks == OSAL_WAIT_FOREVER)
    {
        return osal::milliseconds{-1};
    }
    return osal::clock_utils::ticks_to_ms(static_cast<osal::tick_t>(ticks));
}

[[nodiscard]] osal_tick_t remaining_ticks_until(osal::monotonic_clock::time_point deadline) noexcept
{
    const osal::monotonic_deadline wait_deadline = osal::monotonic_deadline::at(deadline);
    if (wait_deadline.expired())
    {
        return OSAL_NO_WAIT;
    }

    return static_cast<osal_tick_t>(osal::clock_utils::ms_to_ticks(wait_deadline.remaining()));
}

void notification_unlock(const osal_notification_handle* handle) noexcept
{
    (void)osal_mutex_unlock(OSAL_C_CAST(mtx_h, &handle->mutex));
}

void delayable_unlock(osal_delayable_work_handle* handle) noexcept
{
    (void)osal_mutex_unlock(OSAL_C_CAST(mtx_h, &handle->mutex));
}

// This callback may execute in ISR context on some backends, so it
// only performs atomic state changes and ISR-safe work submission.
void c_delayable_work_timer_cb(void* arg) noexcept
{
    auto* handle = static_cast<osal_delayable_work_handle*>(arg);
    if (!delayable_valid(handle))
    {
        return;
    }

    auto expected = __atomic_load_n(&handle->state, __ATOMIC_ACQUIRE);
    if ((expected & c_delayable_state_mask) != osal::detail::to_underlying(c_delayable_state::armed))
    {
        return;
    }
    const auto started = (static_cast<std::uint64_t>(__atomic_load_n(&handle->start_high, __ATOMIC_RELAXED)) << 32U) |
                         __atomic_load_n(&handle->start_low, __ATOMIC_RELAXED);
    const auto delay = (static_cast<std::uint64_t>(__atomic_load_n(&handle->delay_high, __ATOMIC_RELAXED)) << 32U) |
                       __atomic_load_n(&handle->delay_low, __ATOMIC_RELAXED);
    const auto elapsed = static_cast<osal::tick_t>(osal_clock_ticks() - static_cast<osal::tick_t>(started));
    if (static_cast<std::uint64_t>(elapsed) < delay)
    {
        return;
    }
    const auto queued = (expected & ~c_delayable_state_mask) | osal::detail::to_underlying(c_delayable_state::queued);
    if (!__atomic_compare_exchange_n(&handle->state, &expected, queued, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        return;
    }

    const osal_result_t submit_rc =
        osal::active_capabilities::timer_callbacks_may_run_in_isr
            ? osal_c_work_queue_submit_from_isr(handle->queue, c_delayable_work_work_cb, handle)
            : osal_c_work_queue_submit(handle->queue, c_delayable_work_work_cb, handle);
    if (submit_rc != OSAL_OK)
    {
        delayable_set_dispatch_error(handle, submit_rc);
        delayable_state_store(handle, c_delayable_state::idle);
    }
}

void c_delayable_work_work_cb(void* arg) noexcept
{
    auto* handle = static_cast<osal_delayable_work_handle*>(arg);
    if (!delayable_valid(handle))
    {
        return;
    }

    c_delayable_state expected = c_delayable_state::queued;
    if (!delayable_state_compare_exchange(handle, &expected, c_delayable_state::running))
    {
        return;
    }

    handle->func(handle->arg);

    expected = c_delayable_state::running;
    (void)delayable_state_compare_exchange(handle, &expected, c_delayable_state::idle);
}

}  // namespace

// ============================================================================
// Clock
// ============================================================================

extern "C" int64_t osal_c_clock_monotonic_ms(void)
{
    return osal_clock_monotonic_ms();
}

extern "C" int64_t osal_c_clock_system_ms(void)
{
    return osal_clock_system_ms();
}

extern "C" osal_tick_t osal_c_clock_ticks(void)
{
    return osal_clock_ticks();
}

extern "C" uint32_t osal_c_clock_tick_period_us(void)
{
    return osal_clock_tick_period_us();
}

extern "C" int64_t osal_c_clock_high_resolution_ns(void)
{
    return osal::high_resolution_clock::now().time_since_epoch().count();
}

extern "C" int64_t osal_c_clock_high_resolution_resolution_ns(void)
{
    return osal::high_resolution_clock::resolution().count();
}

extern "C" int osal_c_clock_high_resolution_supported(void)
{
    return osal::high_resolution_clock::is_supported ? 1 : 0;
}

// ============================================================================
// Mutex
// ============================================================================

extern "C" osal_result_t osal_c_mutex_create(osal_mutex_handle* handle, int recursive)
{
    if (handle == nullptr)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->native != nullptr)
    {
        return OSAL_ALREADY_EXISTS;
    }
    return record_c_creation(osal::diagnostic_resource::mutex,
                             osal_mutex_create(OSAL_C_CAST(mtx_h, handle), recursive != 0));
}

extern "C" osal_result_t osal_c_diagnostics_read(osal_diagnostic_resource resource, osal_diagnostic_snapshot* snapshot)
{
    if (snapshot == nullptr || resource < OSAL_DIAGNOSTIC_MUTEX || resource > OSAL_DIAGNOSTIC_EMULATED_WORK_QUEUE)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    const auto values = osal::diagnostics(static_cast<osal::diagnostic_resource>(resource)).snapshot();
    *snapshot         = {values.capacity, values.in_use,     values.high_water, values.attempts, values.successes,
                         values.failures, values.contention, values.drops,      values.backlog};
    return OSAL_OK;
}

extern "C" osal_result_t osal_c_mutex_create_with_cfg(osal_mutex_handle* handle, const osal_mutex_config* cfg)
{
    if (cfg == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    return osal_c_mutex_create(handle, cfg->recursive != 0 ? 1 : 0);
}

extern "C" osal_result_t osal_c_mutex_create_with_policy(osal_mutex_handle* handle, const osal_mutex_policy_config* cfg)
{
    if (cfg == nullptr || cfg->protocol < OSAL_MUTEX_NATIVE_DEFAULT || cfg->protocol > OSAL_MUTEX_PRIORITY_CEILING ||
        cfg->robustness < OSAL_MUTEX_NON_ROBUST || cfg->robustness > OSAL_MUTEX_ROBUST)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    const osal::mutex_config config{cfg->recursive != 0 ? osal::mutex_type::recursive : osal::mutex_type::normal,
                                    static_cast<osal::mutex_protocol>(cfg->protocol),
                                    static_cast<osal::mutex_robustness>(cfg->robustness), cfg->priority_ceiling};
    return record_c_creation(osal::diagnostic_resource::mutex,
                             osal_mutex_create_config(OSAL_C_CAST(mtx_h, handle), &config));
}

extern "C" osal_result_t osal_c_mutex_consistent(osal_mutex_handle* handle)
{
    return to_c(osal_mutex_consistent(OSAL_C_CAST(mtx_h, handle)));
}

extern "C" osal_result_t osal_c_mutex_destroy(osal_mutex_handle* handle)
{
    return record_c_destruction(osal::diagnostic_resource::mutex, OSAL_C_CAST(mtx_h, handle), osal_mutex_destroy);
}

extern "C" osal_result_t osal_c_mutex_lock(osal_mutex_handle* handle, osal_tick_t timeout)
{
    return to_c(osal_mutex_lock(OSAL_C_CAST(mtx_h, handle), timeout));
}

extern "C" osal_result_t osal_c_mutex_try_lock(osal_mutex_handle* handle)
{
    return to_c(osal_mutex_try_lock(OSAL_C_CAST(mtx_h, handle)));
}

extern "C" osal_result_t osal_c_mutex_unlock(osal_mutex_handle* handle)
{
    return to_c(osal_mutex_unlock(OSAL_C_CAST(mtx_h, handle)));
}

// ============================================================================
// Semaphore
// ============================================================================

extern "C" osal_result_t osal_c_semaphore_create(osal_semaphore_handle* handle, unsigned int initial_count,
                                                 unsigned int max_count)
{
    if (handle == nullptr || max_count == 0U || initial_count > max_count)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->native != nullptr)
    {
        return OSAL_ALREADY_EXISTS;
    }
    return record_c_creation(osal::diagnostic_resource::semaphore,
                             osal_semaphore_create(OSAL_C_CAST(sem_h, handle), initial_count, max_count));
}

extern "C" osal_result_t osal_c_semaphore_create_with_cfg(osal_semaphore_handle*       handle,
                                                          const osal_semaphore_config* cfg)
{
    if (cfg == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    return osal_c_semaphore_create(handle, cfg->initial_count, cfg->max_count);
}

extern "C" osal_result_t osal_c_semaphore_destroy(osal_semaphore_handle* handle)
{
    return record_c_destruction(osal::diagnostic_resource::semaphore, OSAL_C_CAST(sem_h, handle),
                                osal_semaphore_destroy);
}

extern "C" osal_result_t osal_c_semaphore_give(osal_semaphore_handle* handle)
{
    return to_c(osal_semaphore_give(OSAL_C_CAST(sem_h, handle)));
}

extern "C" osal_result_t osal_c_semaphore_give_isr(osal_semaphore_handle* handle)
{
    return to_c(osal_semaphore_give_isr(OSAL_C_CAST(sem_h, handle)));
}

extern "C" osal_result_t osal_c_semaphore_take(osal_semaphore_handle* handle, osal_tick_t timeout)
{
    return to_c(osal_semaphore_take(OSAL_C_CAST(sem_h, handle), timeout));
}

extern "C" osal_result_t osal_c_semaphore_try_take(osal_semaphore_handle* handle)
{
    return to_c(osal_semaphore_try_take(OSAL_C_CAST(sem_h, handle)));
}

// ============================================================================
// Queue
// ============================================================================

extern "C" osal_result_t osal_c_queue_create(osal_queue_handle* handle, void* buffer, size_t item_size, size_t capacity)
{
    if (handle == nullptr || buffer == nullptr || item_size == 0U || capacity == 0U || capacity > SIZE_MAX / item_size)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->native != nullptr)
    {
        return OSAL_ALREADY_EXISTS;
    }
    return record_c_creation(osal::diagnostic_resource::queue,
                             osal_queue_create(OSAL_C_CAST(que_h, handle), buffer, item_size, capacity));
}

extern "C" osal_result_t osal_c_queue_create_with_cfg(osal_queue_handle* handle, const osal_queue_config* cfg)
{
    if (cfg == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    return osal_c_queue_create(handle, cfg->buffer, cfg->item_size, cfg->capacity);
}

extern "C" osal_result_t osal_c_queue_destroy(osal_queue_handle* handle)
{
    return record_c_destruction(osal::diagnostic_resource::queue, OSAL_C_CAST(que_h, handle), osal_queue_destroy);
}

extern "C" osal_result_t osal_c_queue_send(osal_queue_handle* handle, const void* item, osal_tick_t timeout)
{
    return to_c(osal_queue_send(OSAL_C_CAST(que_h, handle), item, timeout));
}

extern "C" osal_result_t osal_c_queue_send_isr(osal_queue_handle* handle, const void* item)
{
    return to_c(osal_queue_send_isr(OSAL_C_CAST(que_h, handle), item));
}

extern "C" osal_result_t osal_c_queue_receive(osal_queue_handle* handle, void* item, osal_tick_t timeout)
{
    return to_c(osal_queue_receive(OSAL_C_CAST(que_h, handle), item, timeout));
}

extern "C" osal_result_t osal_c_queue_receive_isr(osal_queue_handle* handle, void* item)
{
    return to_c(osal_queue_receive_isr(OSAL_C_CAST(que_h, handle), item));
}

extern "C" osal_result_t osal_c_queue_peek(osal_queue_handle* handle, void* item, osal_tick_t timeout)
{
    return to_c(osal_queue_peek(OSAL_C_CAST(que_h, handle), item, timeout));
}

extern "C" size_t osal_c_queue_count(const osal_queue_handle* handle)
{
    return osal_queue_count(OSAL_C_CAST_CONST(que_h, handle));
}

extern "C" size_t osal_c_queue_free(const osal_queue_handle* handle)
{
    return osal_queue_free(OSAL_C_CAST_CONST(que_h, handle));
}

// ============================================================================
// Thread
// ============================================================================

extern "C" osal_result_t osal_c_thread_create(osal_thread_handle* handle, osal_c_thread_entry_t entry, void* arg,
                                              osal_priority_t priority, osal_affinity_t affinity, void* stack,
                                              size_t stack_bytes, const char* name)
{
    if (handle == nullptr || entry == nullptr)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->native != nullptr)
    {
        return OSAL_ALREADY_EXISTS;
    }
    return record_c_creation(
        osal::diagnostic_resource::thread,
        osal_thread_create(OSAL_C_CAST(thr_h, handle), entry, arg, priority, affinity, stack, stack_bytes, name));
}

extern "C" osal_result_t osal_c_thread_create_with_cfg(osal_thread_handle* handle, const osal_thread_config* cfg)
{
    if (cfg == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    return osal_c_thread_create(handle, cfg->entry, cfg->arg, cfg->priority, cfg->affinity, cfg->stack,
                                cfg->stack_bytes, cfg->name);
}

extern "C" osal_result_t osal_c_thread_join(osal_thread_handle* handle, osal_tick_t timeout)
{
    const bool owned  = handle != nullptr && handle->native != nullptr;
    const auto status = osal_thread_join(OSAL_C_CAST(thr_h, handle), timeout);
    if (owned && status.ok())
    {
        osal::diagnostics(osal::diagnostic_resource::thread).release();
    }
    return to_c(status);
}

extern "C" osal_result_t osal_c_thread_detach(osal_thread_handle* handle)
{
    return record_c_destruction(osal::diagnostic_resource::thread, OSAL_C_CAST(thr_h, handle), osal_thread_detach);
}

extern "C" osal_result_t osal_c_thread_set_priority(osal_thread_handle* handle, osal_priority_t priority)
{
    return to_c(osal_thread_set_priority(OSAL_C_CAST(thr_h, handle), priority));
}

extern "C" osal_result_t osal_c_thread_set_affinity(osal_thread_handle* handle, osal_affinity_t affinity)
{
    return to_c(osal_thread_set_affinity(OSAL_C_CAST(thr_h, handle), affinity));
}

extern "C" osal_result_t osal_c_thread_suspend(osal_thread_handle* handle)
{
    return to_c(osal_thread_suspend(OSAL_C_CAST(thr_h, handle)));
}

extern "C" osal_result_t osal_c_thread_resume(osal_thread_handle* handle)
{
    return to_c(osal_thread_resume(OSAL_C_CAST(thr_h, handle)));
}

extern "C" void osal_c_thread_yield(void)
{
    osal_thread_yield();
}

extern "C" void osal_c_thread_sleep_ms(uint32_t ms)
{
    osal_thread_sleep_ms(ms);
}

// ============================================================================
// Thread-Local Data
// ============================================================================

extern "C" osal_result_t osal_c_tls_key_create(osal_tls_key_handle* handle)
{
    if (handle == nullptr)
    {
        return OSAL_INVALID_ARGUMENT;
    }

    std::uint8_t key = 0U;
    if (!osal::detail::tls_registry::acquire(&key))
    {
        return OSAL_OUT_OF_RESOURCES;
    }

    handle->key   = key;
    handle->valid = 1U;
    return OSAL_OK;
}

extern "C" osal_result_t osal_c_tls_key_destroy(osal_tls_key_handle* handle)
{
    if (handle == nullptr)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->valid == 0U)
    {
        return OSAL_NOT_INITIALIZED;
    }

    osal::detail::tls_registry::release(handle->key);
    handle->key   = 0U;
    handle->valid = 0U;
    return OSAL_OK;
}

extern "C" osal_result_t osal_c_tls_set(osal_tls_key_handle* handle, void* value)
{
    if (handle == nullptr)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->valid == 0U)
    {
        return OSAL_NOT_INITIALIZED;
    }

    osal::detail::tls_storage::set(handle->key, value);
    return OSAL_OK;
}

extern "C" void* osal_c_tls_get(const osal_tls_key_handle* handle)
{
    if (handle == nullptr || handle->valid == 0U)
    {
        return nullptr;
    }
    return osal::detail::tls_storage::get(handle->key);
}

// ============================================================================
// Timer
// ============================================================================

extern "C" osal_result_t osal_c_timer_create(osal_timer_handle* handle, const char* name,
                                             osal_c_timer_callback_t callback, void* arg, osal_tick_t period_ticks,
                                             int auto_reload)
{
    if (handle == nullptr || callback == nullptr || period_ticks == 0U)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->native != nullptr)
    {
        return OSAL_ALREADY_EXISTS;
    }
    return record_c_creation(
        osal::diagnostic_resource::timer,
        osal_timer_create(OSAL_C_CAST(tmr_h, handle), name, callback, arg, period_ticks, auto_reload != 0));
}

extern "C" osal_result_t osal_c_timer_create_with_cfg(osal_timer_handle* handle, const osal_timer_config* cfg)
{
    if (cfg == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    return osal_c_timer_create(handle, cfg->name, cfg->callback, cfg->arg, cfg->period_ticks, cfg->auto_reload);
}

extern "C" osal_result_t osal_c_timer_destroy(osal_timer_handle* handle)
{
    return record_c_destruction(osal::diagnostic_resource::timer, OSAL_C_CAST(tmr_h, handle), osal_timer_destroy);
}

extern "C" osal_result_t osal_c_timer_start(osal_timer_handle* handle)
{
    return to_c(osal_timer_start(OSAL_C_CAST(tmr_h, handle)));
}

extern "C" osal_result_t osal_c_timer_stop(osal_timer_handle* handle)
{
    return to_c(osal_timer_stop(OSAL_C_CAST(tmr_h, handle)));
}

extern "C" osal_result_t osal_c_timer_reset(osal_timer_handle* handle)
{
    return to_c(osal_timer_reset(OSAL_C_CAST(tmr_h, handle)));
}

extern "C" osal_result_t osal_c_timer_set_period(osal_timer_handle* handle, osal_tick_t new_period_ticks)
{
    return to_c(osal_timer_set_period(OSAL_C_CAST(tmr_h, handle), new_period_ticks));
}

extern "C" int osal_c_timer_is_active(const osal_timer_handle* handle)
{
    return osal_timer_is_active(OSAL_C_CAST_CONST(tmr_h, handle)) ? 1 : 0;
}

// ============================================================================
// Event Flags
// ============================================================================

extern "C" osal_result_t osal_c_event_flags_create(osal_event_flags_handle* handle)
{
    return to_c(osal_event_flags_create(OSAL_C_CAST(ef_h, handle)));
}

extern "C" osal_result_t osal_c_event_flags_destroy(osal_event_flags_handle* handle)
{
    return to_c(osal_event_flags_destroy(OSAL_C_CAST(ef_h, handle)));
}

extern "C" osal_result_t osal_c_event_flags_set(osal_event_flags_handle* handle, osal_event_bits_t bits)
{
    return to_c(osal_event_flags_set(OSAL_C_CAST(ef_h, handle), bits));
}

extern "C" osal_result_t osal_c_event_flags_clear(osal_event_flags_handle* handle, osal_event_bits_t bits)
{
    return to_c(osal_event_flags_clear(OSAL_C_CAST(ef_h, handle), bits));
}

extern "C" osal_event_bits_t osal_c_event_flags_get(const osal_event_flags_handle* handle)
{
    return osal_event_flags_get(OSAL_C_CAST_CONST(ef_h, handle));
}

extern "C" osal_result_t osal_c_event_flags_wait_any(osal_event_flags_handle* handle, osal_event_bits_t wait_bits,
                                                     osal_event_bits_t* actual_bits, int clear_on_exit,
                                                     osal_tick_t timeout)
{
    return to_c(
        osal_event_flags_wait_any(OSAL_C_CAST(ef_h, handle), wait_bits, actual_bits, clear_on_exit != 0, timeout));
}

extern "C" osal_result_t osal_c_event_flags_wait_all(osal_event_flags_handle* handle, osal_event_bits_t wait_bits,
                                                     osal_event_bits_t* actual_bits, int clear_on_exit,
                                                     osal_tick_t timeout)
{
    return to_c(
        osal_event_flags_wait_all(OSAL_C_CAST(ef_h, handle), wait_bits, actual_bits, clear_on_exit != 0, timeout));
}

extern "C" osal_result_t osal_c_event_flags_set_isr(osal_event_flags_handle* handle, osal_event_bits_t bits)
{
    return to_c(osal_event_flags_set_isr(OSAL_C_CAST(ef_h, handle), bits));
}

// ============================================================================
// Condition Variable
// ============================================================================

extern "C" osal_result_t osal_c_condvar_create(osal_condvar_handle* handle)
{
    return to_c(osal_condvar_create(OSAL_C_CAST(cv_h, handle)));
}

extern "C" osal_result_t osal_c_condvar_destroy(osal_condvar_handle* handle)
{
    return to_c(osal_condvar_destroy(OSAL_C_CAST(cv_h, handle)));
}

extern "C" osal_result_t osal_c_condvar_wait(osal_condvar_handle* handle, osal_mutex_handle* mutex, osal_tick_t timeout)
{
    return to_c(osal_condvar_wait(OSAL_C_CAST(cv_h, handle), OSAL_C_CAST(mtx_h, mutex), timeout));
}

extern "C" osal_result_t osal_c_condvar_notify_one(osal_condvar_handle* handle)
{
    return to_c(osal_condvar_notify_one(OSAL_C_CAST(cv_h, handle)));
}

extern "C" osal_result_t osal_c_condvar_notify_all(osal_condvar_handle* handle)
{
    return to_c(osal_condvar_notify_all(OSAL_C_CAST(cv_h, handle)));
}

// ============================================================================
// Notification
// ============================================================================

extern "C" osal_result_t osal_c_notification_create(osal_notification_handle* handle, std::uint32_t* values,
                                                    std::uint8_t* pending, std::size_t slot_count)
{
    if ((handle == nullptr) || (values == nullptr) || (pending == nullptr) || (slot_count == 0U)) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->valid != 0U)
    {
        return OSAL_ALREADY_EXISTS;
    }

    std::memset(values, 0, slot_count * sizeof(std::uint32_t));
    std::memset(pending, 0, slot_count * sizeof(std::uint8_t));

    handle->values     = values;
    handle->pending    = pending;
    handle->slot_count = slot_count;
    handle->valid      = 0U;

    const osal_result_t mtx_rc = osal_c_mutex_create(&handle->mutex, 0);
    if (mtx_rc != OSAL_OK)
    {
        handle->values     = nullptr;
        handle->pending    = nullptr;
        handle->slot_count = 0U;
        return mtx_rc;
    }

    const osal_result_t cv_rc = osal_c_condvar_create(&handle->condvar);
    if (cv_rc != OSAL_OK)
    {
        (void)osal_c_mutex_destroy(&handle->mutex);
        handle->values     = nullptr;
        handle->pending    = nullptr;
        handle->slot_count = 0U;
        return cv_rc;
    }

    handle->valid = 1U;
    return OSAL_OK;
}

extern "C" osal_result_t osal_c_notification_create_with_cfg(osal_notification_handle*       handle,
                                                             const osal_notification_config* cfg)
{
    if (cfg == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    return osal_c_notification_create(handle, cfg->values, cfg->pending, cfg->slot_count);
}

extern "C" osal_result_t osal_c_notification_destroy(osal_notification_handle* handle)
{
    if (handle == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (!notification_valid(handle)) [[unlikely]]
    {
        return OSAL_NOT_INITIALIZED;
    }

    handle->valid              = 0U;
    const osal_result_t cv_rc  = osal_c_condvar_destroy(&handle->condvar);
    const osal_result_t mtx_rc = osal_c_mutex_destroy(&handle->mutex);
    handle->values             = nullptr;
    handle->pending            = nullptr;
    handle->slot_count         = 0U;
    return (cv_rc != OSAL_OK) ? cv_rc : mtx_rc;
}

extern "C" osal_result_t osal_c_notification_notify(osal_notification_handle* handle, std::uint32_t value,
                                                    osal_notification_action action, std::size_t index)
{
    if (!notification_index_valid(handle, index)) [[unlikely]]
    {
        return notification_valid(handle) ? OSAL_INVALID_ARGUMENT : OSAL_NOT_INITIALIZED;
    }

    const osal::result lock_rc = osal_mutex_lock(OSAL_C_CAST(mtx_h, &handle->mutex), osal::WAIT_FOREVER);
    if (!lock_rc.ok())
    {
        return to_c(lock_rc);
    }

    switch (action)
    {
    case OSAL_NOTIFICATION_SET_BITS:
        handle->values[index] |= value;
        handle->pending[index] = 1U;
        break;
    case OSAL_NOTIFICATION_INCREMENT:
        ++handle->values[index];
        handle->pending[index] = 1U;
        break;
    case OSAL_NOTIFICATION_OVERWRITE:
        handle->values[index]  = value;
        handle->pending[index] = 1U;
        break;
    case OSAL_NOTIFICATION_NO_OVERWRITE:
        if (handle->pending[index] != 0U)
        {
            notification_unlock(handle);
            return OSAL_WOULD_BLOCK;
        }
        handle->values[index]  = value;
        handle->pending[index] = 1U;
        break;
    default:
        notification_unlock(handle);
        return OSAL_INVALID_ARGUMENT;
    }

    (void)osal_c_condvar_notify_all(&handle->condvar);
    notification_unlock(handle);
    return OSAL_OK;
}

extern "C" osal_result_t osal_c_notification_wait(osal_notification_handle* handle, std::size_t index,
                                                  std::uint32_t* value_out, std::uint32_t clear_on_entry,
                                                  std::uint32_t clear_on_exit, osal_tick_t timeout)
{
    if (!notification_index_valid(handle, index)) [[unlikely]]
    {
        return notification_valid(handle) ? OSAL_INVALID_ARGUMENT : OSAL_NOT_INITIALIZED;
    }

    const osal::result lock_rc = osal_mutex_lock(OSAL_C_CAST(mtx_h, &handle->mutex), osal::WAIT_FOREVER);
    if (!lock_rc.ok())
    {
        return to_c(lock_rc);
    }

    handle->values[index] &= ~clear_on_entry;

    if (handle->pending[index] == 0U)
    {
        if (timeout == OSAL_WAIT_FOREVER)
        {
            while (handle->pending[index] == 0U)
            {
                const osal::result wait_rc = osal_condvar_wait(OSAL_C_CAST(cv_h, &handle->condvar),
                                                               OSAL_C_CAST(mtx_h, &handle->mutex), osal::WAIT_FOREVER);
                if (!wait_rc.ok())
                {
                    notification_unlock(handle);
                    return to_c(wait_rc);
                }
            }
        }
        else
        {
            const auto deadline = osal::monotonic_clock::now() + ticks_to_duration(timeout);
            while (handle->pending[index] == 0U)
            {
                const osal_tick_t remaining = remaining_ticks_until(deadline);
                if (remaining == OSAL_NO_WAIT)
                {
                    notification_unlock(handle);
                    return OSAL_TIMEOUT;
                }

                const osal::result wait_rc =
                    osal_condvar_wait(OSAL_C_CAST(cv_h, &handle->condvar), OSAL_C_CAST(mtx_h, &handle->mutex),
                                      static_cast<osal::tick_t>(remaining));
                if ((wait_rc == osal::error_code::timeout) && (handle->pending[index] == 0U))
                {
                    notification_unlock(handle);
                    return OSAL_TIMEOUT;
                }
                if (!wait_rc.ok() && (wait_rc != osal::error_code::timeout))
                {
                    notification_unlock(handle);
                    return to_c(wait_rc);
                }
            }
        }
    }

    if (value_out != nullptr)
    {
        *value_out = handle->values[index];
    }

    handle->values[index] &= ~clear_on_exit;
    handle->pending[index] = 0U;
    notification_unlock(handle);
    return OSAL_OK;
}

extern "C" osal_result_t osal_c_notification_clear(osal_notification_handle* handle, std::uint32_t bits,
                                                   std::size_t index)
{
    if (!notification_index_valid(handle, index)) [[unlikely]]
    {
        return notification_valid(handle) ? OSAL_INVALID_ARGUMENT : OSAL_NOT_INITIALIZED;
    }

    const osal::result lock_rc = osal_mutex_lock(OSAL_C_CAST(mtx_h, &handle->mutex), osal::WAIT_FOREVER);
    if (!lock_rc.ok())
    {
        return to_c(lock_rc);
    }

    handle->values[index] &= ~bits;
    notification_unlock(handle);
    return OSAL_OK;
}

extern "C" osal_result_t osal_c_notification_reset(osal_notification_handle* handle, std::size_t index)
{
    if (!notification_index_valid(handle, index)) [[unlikely]]
    {
        return notification_valid(handle) ? OSAL_INVALID_ARGUMENT : OSAL_NOT_INITIALIZED;
    }

    const osal::result lock_rc = osal_mutex_lock(OSAL_C_CAST(mtx_h, &handle->mutex), osal::WAIT_FOREVER);
    if (!lock_rc.ok())
    {
        return to_c(lock_rc);
    }

    handle->values[index]  = 0U;
    handle->pending[index] = 0U;
    notification_unlock(handle);
    return OSAL_OK;
}

extern "C" int osal_c_notification_pending(const osal_notification_handle* handle, std::size_t index)
{
    if (!notification_index_valid(handle, index))
    {
        return 0;
    }

    if (!osal_mutex_lock(OSAL_C_CAST(mtx_h, &handle->mutex), osal::WAIT_FOREVER).ok())
    {
        return 0;
    }

    const int pending = (handle->pending[index] != 0U) ? 1 : 0;
    notification_unlock(handle);
    return pending;
}

extern "C" std::uint32_t osal_c_notification_peek(const osal_notification_handle* handle, std::size_t index)
{
    if (!notification_index_valid(handle, index))
    {
        return 0U;
    }

    if (!osal_mutex_lock(OSAL_C_CAST(mtx_h, &handle->mutex), osal::WAIT_FOREVER).ok())
    {
        return 0U;
    }

    const std::uint32_t value = handle->values[index];
    notification_unlock(handle);
    return value;
}

// ============================================================================
// Work Queue
// ============================================================================

extern "C" osal_result_t osal_c_work_queue_create(osal_work_queue_handle* handle, void* stack, size_t stack_bytes,
                                                  size_t depth, const char* name)
{
    if (handle == nullptr || depth == 0U)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->native != nullptr)
    {
        return OSAL_ALREADY_EXISTS;
    }
    return record_c_creation(osal::diagnostic_resource::work_queue,
                             osal_work_queue_create(OSAL_C_CAST(wq_h, handle), stack, stack_bytes, depth, name));
}

extern "C" osal_result_t osal_c_work_queue_create_with_cfg(osal_work_queue_handle*       handle,
                                                           const osal_work_queue_config* cfg)
{
    if (cfg == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    return osal_c_work_queue_create(handle, cfg->stack, cfg->stack_bytes, cfg->depth, cfg->name);
}

extern "C" osal_result_t osal_c_work_queue_destroy(osal_work_queue_handle* handle)
{
    return record_c_destruction(osal::diagnostic_resource::work_queue, OSAL_C_CAST(wq_h, handle),
                                osal_work_queue_destroy);
}

extern "C" osal_result_t osal_c_work_queue_submit(osal_work_queue_handle* handle, osal_c_work_func_t func, void* arg)
{
    return to_c(osal_work_queue_submit(OSAL_C_CAST(wq_h, handle), func, arg));
}

extern "C" osal_result_t osal_c_work_queue_submit_from_isr(osal_work_queue_handle* handle, osal_c_work_func_t func,
                                                           void* arg)
{
    return to_c(osal_work_queue_submit_from_isr(OSAL_C_CAST(wq_h, handle), func, arg));
}

extern "C" osal_result_t osal_c_work_queue_flush(osal_work_queue_handle* handle, osal_tick_t timeout)
{
    return to_c(osal_work_queue_flush(OSAL_C_CAST(wq_h, handle), timeout));
}

extern "C" osal_result_t osal_c_work_queue_cancel_all(osal_work_queue_handle* handle)
{
    return to_c(osal_work_queue_cancel_all(OSAL_C_CAST(wq_h, handle)));
}

extern "C" size_t osal_c_work_queue_pending(const osal_work_queue_handle* handle)
{
    return osal_work_queue_pending(OSAL_C_CAST_CONST(wq_h, handle));
}

// ============================================================================
// Delayable Work
// ============================================================================

extern "C" osal_result_t osal_c_delayable_work_create(osal_delayable_work_handle* handle, osal_work_queue_handle* queue,
                                                      osal_c_work_func_t func, void* arg, const char* name)
{
    if ((handle == nullptr) || (queue == nullptr) || (func == nullptr) || (queue->native == nullptr)) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->valid != 0U)
    {
        return OSAL_ALREADY_EXISTS;
    }
    if (!delayable_backend_supported()) [[unlikely]]
    {
        return OSAL_NOT_SUPPORTED;
    }

    handle->queue           = queue;
    handle->func            = func;
    handle->arg             = arg;
    handle->dispatch_error  = OSAL_OK;
    handle->state           = static_cast<std::uint32_t>(c_delayable_state::idle);
    handle->start_low       = 0U;
    handle->start_high      = 0U;
    handle->delay_low       = 0U;
    handle->delay_high      = 0U;
    handle->dispatch_failed = 0U;
    handle->valid           = 0U;

    const osal_result_t mtx_rc = osal_c_mutex_create(&handle->mutex, 0);
    if (mtx_rc != OSAL_OK)
    {
        return mtx_rc;
    }

    const osal_result_t cv_rc = osal_c_condvar_create(&handle->condvar);
    if (cv_rc != OSAL_OK)
    {
        (void)osal_c_mutex_destroy(&handle->mutex);
        return cv_rc;
    }

    const osal_result_t timer_rc = osal_c_timer_create(&handle->timer, name, c_delayable_work_timer_cb, handle, 1U, 0);
    if (timer_rc != OSAL_OK)
    {
        (void)osal_c_condvar_destroy(&handle->condvar);
        (void)osal_c_mutex_destroy(&handle->mutex);
        return timer_rc;
    }

    handle->valid = 1U;
    return OSAL_OK;
}

extern "C" osal_result_t osal_c_delayable_work_create_with_cfg(osal_delayable_work_handle*       handle,
                                                               const osal_delayable_work_config* cfg)
{
    if (cfg == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    return osal_c_delayable_work_create(handle, cfg->queue, cfg->func, cfg->arg, cfg->name);
}

extern "C" osal_result_t osal_c_delayable_work_destroy(osal_delayable_work_handle* handle)
{
    if (handle == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (!delayable_valid(handle)) [[unlikely]]
    {
        return OSAL_NOT_INITIALIZED;
    }

    const osal::result lock_rc = osal_mutex_lock(OSAL_C_CAST(mtx_h, &handle->mutex), osal::WAIT_FOREVER);
    if (!lock_rc.ok())
    {
        return to_c(lock_rc);
    }
    if (!delayable_state_is(handle, c_delayable_state::idle))
    {
        delayable_unlock(handle);
        return OSAL_WOULD_BLOCK;
    }
    __atomic_store_n(&handle->valid, 0U, __ATOMIC_RELEASE);
    delayable_unlock(handle);

    const osal_result_t timer_rc = osal_c_timer_destroy(&handle->timer);
    const osal_result_t cv_rc    = osal_c_condvar_destroy(&handle->condvar);
    const osal_result_t mtx_rc   = osal_c_mutex_destroy(&handle->mutex);
    handle->queue                = nullptr;
    handle->func                 = nullptr;
    handle->arg                  = nullptr;
    handle->dispatch_error       = OSAL_OK;
    handle->state                = static_cast<std::uint8_t>(c_delayable_state::idle);
    handle->dispatch_failed      = 0U;
    if (timer_rc != OSAL_OK)
    {
        return timer_rc;
    }
    if (cv_rc != OSAL_OK)
    {
        return cv_rc;
    }
    return mtx_rc;
}

extern "C" osal_result_t osal_c_delayable_work_schedule(osal_delayable_work_handle* handle, osal_tick_t delay_ticks)
{
    if (!delayable_valid(handle)) [[unlikely]]
    {
        return OSAL_NOT_INITIALIZED;
    }

    const osal::result lock_rc = osal_mutex_lock(OSAL_C_CAST(mtx_h, &handle->mutex), osal::WAIT_FOREVER);
    if (!lock_rc.ok())
    {
        return to_c(lock_rc);
    }

    if (!delayable_state_is(handle, c_delayable_state::idle))
    {
        delayable_unlock(handle);
        return OSAL_ALREADY_EXISTS;
    }

    delayable_clear_dispatch_error(handle);

    const osal_result_t rc =
        (delay_ticks == OSAL_NO_WAIT) ? delayable_enqueue_locked(handle) : delayable_arm_locked(handle, delay_ticks);

    delayable_unlock(handle);
    return rc;
}

extern "C" osal_result_t osal_c_delayable_work_reschedule(osal_delayable_work_handle* handle, osal_tick_t delay_ticks)
{
    if (!delayable_valid(handle)) [[unlikely]]
    {
        return OSAL_NOT_INITIALIZED;
    }

    const osal::result lock_rc = osal_mutex_lock(OSAL_C_CAST(mtx_h, &handle->mutex), osal::WAIT_FOREVER);
    if (!lock_rc.ok())
    {
        return to_c(lock_rc);
    }

    c_delayable_state current = delayable_state_load(handle);
    if ((current == c_delayable_state::queued) || (current == c_delayable_state::running) ||
        (current == c_delayable_state::stopping))
    {
        delayable_unlock(handle);
        return OSAL_WOULD_BLOCK;
    }

    if (current == c_delayable_state::armed)
    {
        c_delayable_state expected = c_delayable_state::armed;
        if (!delayable_state_compare_exchange(handle, &expected, c_delayable_state::stopping))
        {
            delayable_unlock(handle);
            return OSAL_WOULD_BLOCK;
        }
        const osal_result_t stopped = osal_c_timer_stop(&handle->timer);
        if (stopped != OSAL_OK)
        {
            delayable_state_store(handle, c_delayable_state::armed);
            delayable_unlock(handle);
            return stopped;
        }
        delayable_state_store(handle, c_delayable_state::idle);
    }

    delayable_clear_dispatch_error(handle);

    const osal_result_t rc =
        (delay_ticks == OSAL_NO_WAIT) ? delayable_enqueue_locked(handle) : delayable_arm_locked(handle, delay_ticks);

    delayable_unlock(handle);
    return rc;
}

extern "C" osal_result_t osal_c_delayable_work_cancel(osal_delayable_work_handle* handle)
{
    if (!delayable_valid(handle)) [[unlikely]]
    {
        return OSAL_NOT_INITIALIZED;
    }

    const osal::result lock_rc = osal_mutex_lock(OSAL_C_CAST(mtx_h, &handle->mutex), osal::WAIT_FOREVER);
    if (!lock_rc.ok())
    {
        return to_c(lock_rc);
    }

    if (delayable_state_is(handle, c_delayable_state::idle))
    {
        delayable_clear_dispatch_error(handle);
        delayable_unlock(handle);
        return OSAL_OK;
    }

    c_delayable_state expected = c_delayable_state::armed;
    if (!delayable_state_compare_exchange(handle, &expected, c_delayable_state::stopping))
    {
        delayable_unlock(handle);
        return (expected == c_delayable_state::idle) ? OSAL_OK : OSAL_WOULD_BLOCK;
    }

    const osal_result_t stop_rc = osal_c_timer_stop(&handle->timer);
    delayable_state_store(handle, (stop_rc == OSAL_OK) ? c_delayable_state::idle : c_delayable_state::armed);
    if (stop_rc == OSAL_OK)
    {
        delayable_clear_dispatch_error(handle);
    }
    delayable_unlock(handle);
    return stop_rc;
}

extern "C" osal_result_t osal_c_delayable_work_flush(osal_delayable_work_handle* handle, osal_tick_t timeout)
{
    if (!delayable_valid(handle)) [[unlikely]]
    {
        return OSAL_NOT_INITIALIZED;
    }

    const auto poll_interval = delayable_poll_interval();
    if (timeout == OSAL_WAIT_FOREVER)
    {
        while (!delayable_state_is(handle, c_delayable_state::idle))
        {
            osal_thread_sleep_ms(static_cast<std::uint32_t>(poll_interval.count()));
        }
    }
    else
    {
        const auto deadline = osal::monotonic_clock::now() + ticks_to_duration(timeout);
        while (!delayable_state_is(handle, c_delayable_state::idle))
        {
            if (remaining_ticks_until(deadline) == OSAL_NO_WAIT)
            {
                return OSAL_TIMEOUT;
            }
            osal_thread_sleep_ms(static_cast<std::uint32_t>(poll_interval.count()));
        }
    }

    return delayable_consume_dispatch_error(handle);
}

extern "C" int osal_c_delayable_work_scheduled(const osal_delayable_work_handle* handle)
{
    if (!delayable_valid(handle)) [[unlikely]]
    {
        return 0;
    }

    return delayable_state_is(handle, c_delayable_state::armed) ? 1 : 0;
}

extern "C" int osal_c_delayable_work_pending(const osal_delayable_work_handle* handle)
{
    if (!delayable_valid(handle)) [[unlikely]]
    {
        return 0;
    }

    const c_delayable_state current = delayable_state_load(handle);
    return (current != c_delayable_state::idle) ? 1 : 0;
}

extern "C" int osal_c_delayable_work_running(const osal_delayable_work_handle* handle)
{
    if (!delayable_valid(handle)) [[unlikely]]
    {
        return 0;
    }

    return delayable_state_is(handle, c_delayable_state::running) ? 1 : 0;
}

// ============================================================================
// Memory Pool
// ============================================================================

extern "C" osal_result_t osal_c_memory_pool_create(osal_memory_pool_handle* handle, void* buffer, size_t buf_bytes,
                                                   size_t block_size, size_t block_count, const char* name)
{
    return to_c(osal_memory_pool_create(OSAL_C_CAST(mp_h, handle), buffer, buf_bytes, block_size, block_count, name));
}

extern "C" osal_result_t osal_c_memory_pool_create_with_cfg(osal_memory_pool_handle*       handle,
                                                            const osal_memory_pool_config* cfg)
{
    if (cfg == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    return to_c(osal_memory_pool_create(OSAL_C_CAST(mp_h, handle), cfg->buffer, cfg->buf_bytes, cfg->block_size,
                                        cfg->block_count, cfg->name));
}

extern "C" osal_result_t osal_c_memory_pool_destroy(osal_memory_pool_handle* handle)
{
    return to_c(osal_memory_pool_destroy(OSAL_C_CAST(mp_h, handle)));
}

extern "C" void* osal_c_memory_pool_allocate(osal_memory_pool_handle* handle)
{
    return osal_memory_pool_allocate(OSAL_C_CAST(mp_h, handle));
}

extern "C" void* osal_c_memory_pool_allocate_timed(osal_memory_pool_handle* handle, osal_tick_t timeout)
{
    return osal_memory_pool_allocate_timed(OSAL_C_CAST(mp_h, handle), timeout);
}

extern "C" osal_result_t osal_c_memory_pool_deallocate(osal_memory_pool_handle* handle, void* block)
{
    return to_c(osal_memory_pool_deallocate(OSAL_C_CAST(mp_h, handle), block));
}

extern "C" size_t osal_c_memory_pool_available(const osal_memory_pool_handle* handle)
{
    return osal_memory_pool_available(OSAL_C_CAST_CONST(mp_h, handle));
}

// ============================================================================
// Read-Write Lock
// ============================================================================

extern "C" osal_result_t osal_c_rwlock_create(osal_rwlock_handle* handle)
{
    return to_c(osal_rwlock_create(OSAL_C_CAST(rw_h, handle)));
}

extern "C" osal_result_t osal_c_rwlock_destroy(osal_rwlock_handle* handle)
{
    return to_c(osal_rwlock_destroy(OSAL_C_CAST(rw_h, handle)));
}

extern "C" osal_result_t osal_c_rwlock_read_lock(osal_rwlock_handle* handle, osal_tick_t timeout)
{
    return to_c(osal_rwlock_read_lock(OSAL_C_CAST(rw_h, handle), timeout));
}

extern "C" osal_result_t osal_c_rwlock_read_unlock(osal_rwlock_handle* handle)
{
    return to_c(osal_rwlock_read_unlock(OSAL_C_CAST(rw_h, handle)));
}

extern "C" osal_result_t osal_c_rwlock_write_lock(osal_rwlock_handle* handle, osal_tick_t timeout)
{
    return to_c(osal_rwlock_write_lock(OSAL_C_CAST(rw_h, handle), timeout));
}

extern "C" osal_result_t osal_c_rwlock_write_unlock(osal_rwlock_handle* handle)
{
    return to_c(osal_rwlock_write_unlock(OSAL_C_CAST(rw_h, handle)));
}

// ============================================================================
// Stream Buffer
// ============================================================================

extern "C" osal_result_t osal_c_stream_buffer_create(osal_stream_buffer_handle* handle, void* buffer, size_t capacity,
                                                     size_t trigger_level)
{
    return to_c(osal_stream_buffer_create(OSAL_C_CAST(sb_h, handle), buffer, capacity, trigger_level));
}

extern "C" osal_result_t osal_c_stream_buffer_create_with_cfg(osal_stream_buffer_handle*       handle,
                                                              const osal_stream_buffer_config* cfg)
{
    if (cfg == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    return to_c(osal_stream_buffer_create(OSAL_C_CAST(sb_h, handle), cfg->buffer, cfg->capacity, cfg->trigger_level));
}

extern "C" osal_result_t osal_c_stream_buffer_destroy(osal_stream_buffer_handle* handle)
{
    return to_c(osal_stream_buffer_destroy(OSAL_C_CAST(sb_h, handle)));
}

extern "C" osal_result_t osal_c_stream_buffer_send(osal_stream_buffer_handle* handle, const void* data, size_t len,
                                                   osal_tick_t timeout)
{
    return to_c(osal_stream_buffer_send(OSAL_C_CAST(sb_h, handle), data, len, timeout));
}

extern "C" osal_result_t osal_c_stream_buffer_send_isr(osal_stream_buffer_handle* handle, const void* data, size_t len)
{
    return to_c(osal_stream_buffer_send_isr(OSAL_C_CAST(sb_h, handle), data, len));
}
extern "C" size_t osal_c_stream_buffer_receive(osal_stream_buffer_handle* handle, void* buf, size_t max_len,
                                               osal_tick_t timeout)
{
    return osal_stream_buffer_receive(OSAL_C_CAST(sb_h, handle), buf, max_len, timeout);
}

extern "C" size_t osal_c_stream_buffer_receive_isr(osal_stream_buffer_handle* handle, void* buf, size_t max_len)
{
    return osal_stream_buffer_receive_isr(OSAL_C_CAST(sb_h, handle), buf, max_len);
}

extern "C" size_t osal_c_stream_buffer_available(const osal_stream_buffer_handle* handle)
{
    return osal_stream_buffer_available(OSAL_C_CAST_CONST(sb_h, handle));
}

extern "C" size_t osal_c_stream_buffer_free_space(const osal_stream_buffer_handle* handle)
{
    return osal_stream_buffer_free_space(OSAL_C_CAST_CONST(sb_h, handle));
}

extern "C" osal_result_t osal_c_stream_buffer_reset(osal_stream_buffer_handle* handle)
{
    return to_c(osal_stream_buffer_reset(OSAL_C_CAST(sb_h, handle)));
}

// ============================================================================
// Message Buffer
// ============================================================================

extern "C" osal_result_t osal_c_message_buffer_create(osal_message_buffer_handle* handle, void* buffer, size_t capacity)
{
    return to_c(osal_message_buffer_create(OSAL_C_CAST(mb_h, handle), buffer, capacity));
}

extern "C" osal_result_t osal_c_message_buffer_create_with_cfg(osal_message_buffer_handle*       handle,
                                                               const osal_message_buffer_config* cfg)
{
    if (cfg == nullptr) [[unlikely]]
    {
        return OSAL_INVALID_ARGUMENT;
    }
    return to_c(osal_message_buffer_create(OSAL_C_CAST(mb_h, handle), cfg->buffer, cfg->capacity));
}

extern "C" osal_result_t osal_c_message_buffer_destroy(osal_message_buffer_handle* handle)
{
    return to_c(osal_message_buffer_destroy(OSAL_C_CAST(mb_h, handle)));
}

extern "C" osal_result_t osal_c_message_buffer_send(osal_message_buffer_handle* handle, const void* msg, size_t len,
                                                    osal_tick_t timeout)
{
    return to_c(osal_message_buffer_send(OSAL_C_CAST(mb_h, handle), msg, len, timeout));
}

extern "C" osal_result_t osal_c_message_buffer_send_isr(osal_message_buffer_handle* handle, const void* msg, size_t len)
{
    return to_c(osal_message_buffer_send_isr(OSAL_C_CAST(mb_h, handle), msg, len));
}
extern "C" size_t osal_c_message_buffer_receive(osal_message_buffer_handle* handle, void* buf, size_t max_len,
                                                osal_tick_t timeout)
{
    return osal_message_buffer_receive(OSAL_C_CAST(mb_h, handle), buf, max_len, timeout);
}

extern "C" size_t osal_c_message_buffer_receive_isr(osal_message_buffer_handle* handle, void* buf, size_t max_len)
{
    return osal_message_buffer_receive_isr(OSAL_C_CAST(mb_h, handle), buf, max_len);
}

extern "C" size_t osal_c_message_buffer_available(const osal_message_buffer_handle* handle)
{
    return osal_message_buffer_available(OSAL_C_CAST_CONST(mb_h, handle));
}

extern "C" size_t osal_c_message_buffer_free_space(const osal_message_buffer_handle* handle)
{
    return osal_message_buffer_free_space(OSAL_C_CAST_CONST(mb_h, handle));
}

extern "C" osal_result_t osal_c_message_buffer_reset(osal_message_buffer_handle* handle)
{
    return to_c(osal_message_buffer_reset(OSAL_C_CAST(mb_h, handle)));
}
