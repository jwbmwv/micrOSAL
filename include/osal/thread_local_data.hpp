// SPDX-License-Identifier: Apache-2.0
/// @file thread_local_data.hpp
/// @brief Header-only emulated thread-local key/value storage.
/// @details Provides per-thread pointer storage through dynamically-acquired
///          keys, similar to pthread TLS keys.
///
///          Default mode is a backend-agnostic emulation using C++
///          `thread_local`. On selected backends, defining
///          `OSAL_THREAD_LOCAL_USE_NATIVE=1` switches to native TLS:
///          - POSIX / Linux / NuttX / QNX / RTEMS / INTEGRITY: pthread TLS
///          - FreeRTOS: task-local storage slots
///
///          Native mode may differ slightly in key lifecycle semantics and is
///          therefore opt-in.
#pragma once

#include "error.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#ifndef OSAL_THREAD_LOCAL_MAX_KEYS
#define OSAL_THREAD_LOCAL_MAX_KEYS 16U
#endif

#ifndef OSAL_THREAD_LOCAL_USE_NATIVE
#define OSAL_THREAD_LOCAL_USE_NATIVE 0
#endif

// Derive the pthread-family group macro locally in case this header is
// included without first going through backends.hpp (which sets
// OSAL_BACKEND_HAS_PTHREAD from the same backend-macro group).
#if !defined(OSAL_BACKEND_HAS_PTHREAD) &&                                                         \
    (defined(OSAL_BACKEND_POSIX) || defined(OSAL_BACKEND_LINUX) || defined(OSAL_BACKEND_NUTTX) || \
     defined(OSAL_BACKEND_QNX) || defined(OSAL_BACKEND_RTEMS) || defined(OSAL_BACKEND_INTEGRITY))
#define OSAL_BACKEND_HAS_PTHREAD 1
#endif

#if (OSAL_THREAD_LOCAL_USE_NATIVE != 0) && defined(OSAL_BACKEND_HAS_PTHREAD)
#define OSAL_THREAD_LOCAL_USE_NATIVE_PTHREAD 1
#else
#define OSAL_THREAD_LOCAL_USE_NATIVE_PTHREAD 0
#endif

#if (OSAL_THREAD_LOCAL_USE_NATIVE != 0) && defined(OSAL_BACKEND_FREERTOS) && \
    defined(configNUM_THREAD_LOCAL_STORAGE_POINTERS) && (configNUM_THREAD_LOCAL_STORAGE_POINTERS > 0)
#define OSAL_THREAD_LOCAL_USE_NATIVE_FREERTOS 1
#else
#define OSAL_THREAD_LOCAL_USE_NATIVE_FREERTOS 0
#endif

#if defined(OSAL_BACKEND_BAREMETAL)
#define OSAL_THREAD_LOCAL_USE_NATIVE_BAREMETAL 1
#else
#define OSAL_THREAD_LOCAL_USE_NATIVE_BAREMETAL 0
#endif

#if OSAL_THREAD_LOCAL_USE_NATIVE_BAREMETAL
#ifndef OSAL_BAREMETAL_MAX_TASKS
#define OSAL_BAREMETAL_MAX_TASKS 8U
#endif
extern "C" int osal_baremetal_current_task_index() noexcept;
#endif

#if OSAL_THREAD_LOCAL_USE_NATIVE_PTHREAD
#include <pthread.h>
#endif

#if OSAL_THREAD_LOCAL_USE_NATIVE_FREERTOS
#include "task.h"
#endif

namespace osal
{

namespace detail
{

class tls_registry
{
public:
    static constexpr std::size_t k_max_keys = static_cast<std::size_t>(OSAL_THREAD_LOCAL_MAX_KEYS);

    static bool acquire(std::uint8_t* out_key) noexcept
    {
        if (out_key == nullptr)
        {
            return false;
        }

        const std::uint32_t limit = (k_max_keys >= 32U) ? 32U : static_cast<std::uint32_t>(k_max_keys);
        auto&               mask  = used_mask();

        for (std::uint32_t idx = 0U; idx < limit; ++idx)
        {
            const std::uint32_t bit      = (1U << idx);
            std::uint32_t       expected = mask.load(std::memory_order_relaxed);
            while ((expected & bit) == 0U)
            {
                if (mask.compare_exchange_weak(expected, expected | bit, std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
                {
                    (void)generations()[idx].fetch_add(1U, std::memory_order_acq_rel);
                    *out_key = static_cast<std::uint8_t>(idx);
                    return true;
                }
            }
        }
        return false;
    }

    static void release(std::uint8_t key) noexcept
    {
        if (key >= k_max_keys || key >= 32U)
        {
            return;
        }

        const std::uint32_t bit = (1U << key);
        (void)used_mask().fetch_and(~bit, std::memory_order_acq_rel);
        (void)generations()[key].fetch_add(1U, std::memory_order_acq_rel);
    }

    [[nodiscard]] static std::uint32_t generation(std::uint8_t key) noexcept
    {
        if (key >= k_max_keys || key >= 32U)
        {
            return 0U;
        }
        return generations()[key].load(std::memory_order_acquire);
    }

private:
    static std::atomic<std::uint32_t>& used_mask() noexcept
    {
        static std::atomic<std::uint32_t> mask{0U};
        return mask;
    }

    static std::array<std::atomic<std::uint32_t>, k_max_keys>& generations() noexcept
    {
        static std::array<std::atomic<std::uint32_t>, k_max_keys> gens{};
        return gens;
    }
};

class tls_storage
{
public:
    static void set(std::uint8_t key, void* value) noexcept
    {
        if (key >= tls_registry::k_max_keys)
        {
            return;
        }
        const std::uint32_t gen = tls_registry::generation(key);
        values_[key]            = value;
        generations_seen_[key]  = gen;
    }

    [[nodiscard]] static void* get(std::uint8_t key) noexcept
    {
        if (key >= tls_registry::k_max_keys)
        {
            return nullptr;
        }
        const std::uint32_t gen = tls_registry::generation(key);
        return (generations_seen_[key] == gen) ? values_[key] : nullptr;
    }

private:
    inline static thread_local std::array<void*, tls_registry::k_max_keys>         values_{};
    inline static thread_local std::array<std::uint32_t, tls_registry::k_max_keys> generations_seen_{};
};

#if OSAL_THREAD_LOCAL_USE_NATIVE_BAREMETAL
class baremetal_tls_storage
{
public:
    static constexpr std::size_t k_slot_count = static_cast<std::size_t>(OSAL_BAREMETAL_MAX_TASKS) + 1U;

    static void set(std::uint8_t key, void* value) noexcept
    {
        if (key >= tls_registry::k_max_keys)
        {
            return;
        }
        const std::size_t   slot     = current_slot();
        const std::uint32_t gen      = tls_registry::generation(key);
        values_[slot][key]           = value;
        generations_seen_[slot][key] = gen;
    }

    [[nodiscard]] static void* get(std::uint8_t key) noexcept
    {
        if (key >= tls_registry::k_max_keys)
        {
            return nullptr;
        }
        const std::size_t   slot = current_slot();
        const std::uint32_t gen  = tls_registry::generation(key);
        return (generations_seen_[slot][key] == gen) ? values_[slot][key] : nullptr;
    }

private:
    [[nodiscard]] static std::size_t current_slot() noexcept
    {
        const int current = osal_baremetal_current_task_index();
        if (current < 0)
        {
            return 0U;
        }
        const std::size_t slot = static_cast<std::size_t>(current) + 1U;
        return (slot < k_slot_count) ? slot : 0U;
    }

    inline static std::array<std::array<void*, tls_registry::k_max_keys>, k_slot_count>         values_{};
    inline static std::array<std::array<std::uint32_t, tls_registry::k_max_keys>, k_slot_count> generations_seen_{};
};
#endif

#if OSAL_THREAD_LOCAL_USE_NATIVE_FREERTOS
class freertos_tls_slot_registry
{
public:
    static constexpr std::size_t k_max_slots = (static_cast<std::size_t>(configNUM_THREAD_LOCAL_STORAGE_POINTERS) <
                                                static_cast<std::size_t>(OSAL_THREAD_LOCAL_MAX_KEYS))
                                                   ? static_cast<std::size_t>(configNUM_THREAD_LOCAL_STORAGE_POINTERS)
                                                   : static_cast<std::size_t>(OSAL_THREAD_LOCAL_MAX_KEYS);

    static bool acquire(std::uint8_t* out_slot) noexcept
    {
        if (out_slot == nullptr)
        {
            return false;
        }

        const std::uint32_t limit = (k_max_slots >= 32U) ? 32U : static_cast<std::uint32_t>(k_max_slots);
        auto&               mask  = used_mask();

        for (std::uint32_t idx = 0U; idx < limit; ++idx)
        {
            const std::uint32_t bit      = (1U << idx);
            std::uint32_t       expected = mask.load(std::memory_order_relaxed);
            while ((expected & bit) == 0U)
            {
                if (mask.compare_exchange_weak(expected, expected | bit, std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
                {
                    *out_slot = static_cast<std::uint8_t>(idx);
                    return true;
                }
            }
        }
        return false;
    }

    static void release(std::uint8_t slot) noexcept
    {
        if (slot >= k_max_slots || slot >= 32U)
        {
            return;
        }
        const std::uint32_t bit = (1U << slot);
        (void)used_mask().fetch_and(~bit, std::memory_order_acq_rel);
    }

private:
    static std::atomic<std::uint32_t>& used_mask() noexcept
    {
        static std::atomic<std::uint32_t> mask{0U};
        return mask;
    }
};
#endif

}  // namespace detail

class thread_local_data
{
public:
    thread_local_data() noexcept
    {
#if OSAL_THREAD_LOCAL_USE_NATIVE_PTHREAD
        valid_ = (pthread_key_create(&native_key_, nullptr) == 0);
#elif OSAL_THREAD_LOCAL_USE_NATIVE_FREERTOS
        valid_ = detail::freertos_tls_slot_registry::acquire(&key_);
#else
        valid_ = detail::tls_registry::acquire(&key_);
#endif
    }

    ~thread_local_data() noexcept
    {
        if (valid_)
        {
#if OSAL_THREAD_LOCAL_USE_NATIVE_PTHREAD
            (void)pthread_key_delete(native_key_);
#elif OSAL_THREAD_LOCAL_USE_NATIVE_FREERTOS
            detail::freertos_tls_slot_registry::release(key_);
#else
            detail::tls_registry::release(key_);
#endif
        }
    }

    thread_local_data(const thread_local_data&)            = delete;
    thread_local_data& operator=(const thread_local_data&) = delete;
    thread_local_data(thread_local_data&&)                 = delete;
    thread_local_data& operator=(thread_local_data&&)      = delete;

    [[nodiscard]] static constexpr bool uses_native_backend() noexcept
    {
#if OSAL_THREAD_LOCAL_USE_NATIVE_PTHREAD || OSAL_THREAD_LOCAL_USE_NATIVE_FREERTOS || \
    OSAL_THREAD_LOCAL_USE_NATIVE_BAREMETAL
        return true;
#else
        return false;
#endif
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    result set(void* value) noexcept
    {
        if (!valid_)
        {
            return error_code::not_initialized;
        }
#if OSAL_THREAD_LOCAL_USE_NATIVE_PTHREAD
        if (pthread_setspecific(native_key_, value) != 0)
        {
            return error_code::out_of_resources;
        }
#elif OSAL_THREAD_LOCAL_USE_NATIVE_FREERTOS
        const TaskHandle_t task = xTaskGetCurrentTaskHandle();
        if (task == nullptr)
        {
            return error_code::not_initialized;
        }
        vTaskSetThreadLocalStoragePointer(task, static_cast<BaseType_t>(key_), value);
#elif OSAL_THREAD_LOCAL_USE_NATIVE_BAREMETAL
        detail::baremetal_tls_storage::set(key_, value);
#else
        detail::tls_storage::set(key_, value);
#endif
        return osal::ok();
    }

    [[nodiscard]] void* get() const noexcept
    {
        if (!valid_)
        {
            return nullptr;
        }
#if OSAL_THREAD_LOCAL_USE_NATIVE_PTHREAD
        return pthread_getspecific(native_key_);
#elif OSAL_THREAD_LOCAL_USE_NATIVE_FREERTOS
        const TaskHandle_t task = xTaskGetCurrentTaskHandle();
        if (task == nullptr)
        {
            return nullptr;
        }
        return pvTaskGetThreadLocalStoragePointer(task, static_cast<BaseType_t>(key_));
#elif OSAL_THREAD_LOCAL_USE_NATIVE_BAREMETAL
        return detail::baremetal_tls_storage::get(key_);
#else
        return detail::tls_storage::get(key_);
#endif
    }

private:
#if OSAL_THREAD_LOCAL_USE_NATIVE_PTHREAD
    pthread_key_t native_key_{};
#else
    std::uint8_t key_{0U};
#endif
    bool valid_{false};
};

}  // namespace osal
