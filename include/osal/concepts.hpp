// SPDX-License-Identifier: Apache-2.0
/// @file concepts.hpp
/// @brief C++20 concepts for backend descriptors and fixed-storage OSAL APIs.
/// @details These concepts validate the existing MicrOSAL compile-time model
///          without changing the backend ABI. They keep diagnostics focused on
///          missing traits/capabilities and make template constraints explicit
///          for fixed-storage primitives and wait predicates.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_core
#pragma once

#include "capabilities.hpp"
#include "types.hpp"

#include <cstddef>
#include <concepts>
#include <type_traits>

namespace osal
{

template<typename Backend>
struct backend_traits;

template<typename T>
concept boolean_testable = std::convertible_to<T, bool>;

template<typename Predicate>
concept predicate = requires(Predicate& pred) {
    { pred() } -> boolean_testable;
};

/// @defgroup osal_concepts OSAL Concepts
/// @brief C++20 concepts that constrain backend descriptors and public templates.
/// @{

/// @brief True for one of MicrOSAL's supported backend tag types.
template<typename Backend>
concept backend_tag = std::same_as<Backend, backend_freertos> || std::same_as<Backend, backend_zephyr> ||
                      std::same_as<Backend, backend_threadx> || std::same_as<Backend, backend_px5> ||
                      std::same_as<Backend, backend_posix> || std::same_as<Backend, backend_linux> ||
                      std::same_as<Backend, backend_baremetal> || std::same_as<Backend, backend_vxworks> ||
                      std::same_as<Backend, backend_nuttx> || std::same_as<Backend, backend_micrium> ||
                      std::same_as<Backend, backend_chibios> || std::same_as<Backend, backend_embos> ||
                      std::same_as<Backend, backend_qnx> || std::same_as<Backend, backend_rtems> ||
                      std::same_as<Backend, backend_integrity> || std::same_as<Backend, backend_cmsis_rtos> ||
                      std::same_as<Backend, backend_cmsis_rtos2>;

/// @brief Minimal layout contract for backend-native handle wrappers.
template<typename Handle>
concept native_handle = std::is_standard_layout_v<Handle> && requires(Handle handle) {
    { handle.native } -> std::convertible_to<void*>;
};

/// @brief Capability contract for one backend specialisation.
template<typename Backend>
concept backend_capabilities_spec = backend_tag<Backend> && requires {
    { capabilities<Backend>::has_recursive_mutex } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_timed_mutex } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_priority_inheritance } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_isr_semaphore } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_timed_semaphore } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_thread_affinity } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_timed_join } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_thread_suspend_resume } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_thread_local_data } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_dynamic_thread_priority } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_task_notification } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_stack_overflow_detection } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_cpu_load_stats } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_isr_queue } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_timed_queue } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_timer } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_periodic_timer } -> std::convertible_to<bool>;
    { capabilities<Backend>::timer_callbacks_may_run_in_isr } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_native_event_flags } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_isr_event_flags } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_timed_event_flags } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_wait_set } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_native_condvar } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_native_work_queue } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_isr_work_queue_submit } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_native_memory_pool } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_native_stream_buffer } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_isr_stream_buffer } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_native_message_buffer } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_isr_message_buffer } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_native_rwlock } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_spinlock } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_barrier } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_monotonic_clock } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_system_clock } -> std::convertible_to<bool>;
    { capabilities<Backend>::has_high_resolution } -> std::convertible_to<bool>;
};

/// @brief Trait contract for one backend specialisation.
template<typename Backend>
concept backend_traits_spec = backend_tag<Backend> &&
                              requires {
                                  { backend_traits<Backend>::name } -> std::convertible_to<const char*>;
                                  { backend_traits<Backend>::default_stack_bytes } -> std::convertible_to<stack_size_t>;
                                  typename backend_traits<Backend>::thread_handle_t;
                                  typename backend_traits<Backend>::mutex_handle_t;
                                  typename backend_traits<Backend>::semaphore_handle_t;
                                  typename backend_traits<Backend>::queue_handle_t;
                                  typename backend_traits<Backend>::timer_handle_t;
                                  typename backend_traits<Backend>::event_flags_handle_t;
                                  typename backend_traits<Backend>::wait_set_handle_t;
                                  typename backend_traits<Backend>::work_queue_handle_t;
                                  typename backend_traits<Backend>::condvar_handle_t;
                                  typename backend_traits<Backend>::memory_pool_handle_t;
                                  typename backend_traits<Backend>::stream_buffer_handle_t;
                                  typename backend_traits<Backend>::message_buffer_handle_t;
                                  typename backend_traits<Backend>::rwlock_handle_t;
                                  typename backend_traits<Backend>::spinlock_handle_t;
                                  typename backend_traits<Backend>::barrier_handle_t;
                              } && native_handle<typename backend_traits<Backend>::thread_handle_t> &&
                              native_handle<typename backend_traits<Backend>::mutex_handle_t> &&
                              native_handle<typename backend_traits<Backend>::semaphore_handle_t> &&
                              native_handle<typename backend_traits<Backend>::queue_handle_t> &&
                              native_handle<typename backend_traits<Backend>::timer_handle_t> &&
                              native_handle<typename backend_traits<Backend>::event_flags_handle_t> &&
                              native_handle<typename backend_traits<Backend>::wait_set_handle_t> &&
                              native_handle<typename backend_traits<Backend>::work_queue_handle_t> &&
                              native_handle<typename backend_traits<Backend>::condvar_handle_t> &&
                              native_handle<typename backend_traits<Backend>::memory_pool_handle_t> &&
                              native_handle<typename backend_traits<Backend>::stream_buffer_handle_t> &&
                              native_handle<typename backend_traits<Backend>::message_buffer_handle_t> &&
                              native_handle<typename backend_traits<Backend>::rwlock_handle_t> &&
                              native_handle<typename backend_traits<Backend>::spinlock_handle_t> &&
                              native_handle<typename backend_traits<Backend>::barrier_handle_t>;

/// @brief Full backend descriptor contract used by active_backend validation.
template<typename Backend>
concept backend_descriptor = backend_capabilities_spec<Backend> && backend_traits_spec<Backend>;

/// @brief Backend has timed queue send/receive operations.
template<typename Backend>
concept timed_queue_backend = backend_descriptor<Backend> && capabilities<Backend>::has_timed_queue;

/// @brief Backend has timed semaphore take operations.
template<typename Backend>
concept timed_semaphore_backend = backend_descriptor<Backend> && capabilities<Backend>::has_timed_semaphore;

/// @brief Backend exposes native or emulated timers through the OSAL API.
template<typename Backend>
concept timer_backend = backend_descriptor<Backend> && capabilities<Backend>::has_timer;

/// @brief Backend supports timed thread join.
template<typename Backend>
concept timed_join_backend = backend_descriptor<Backend> && capabilities<Backend>::has_timed_join;

/// @brief Backend supports runtime thread affinity changes.
template<typename Backend>
concept thread_affinity_backend = backend_descriptor<Backend> && capabilities<Backend>::has_thread_affinity;

/// @brief Backend supports runtime thread priority updates.
template<typename Backend>
concept dynamic_thread_priority_backend =
    backend_descriptor<Backend> && capabilities<Backend>::has_dynamic_thread_priority;

/// @brief Backend supports direct task notifications.
template<typename Backend>
concept task_notification_backend = backend_descriptor<Backend> && capabilities<Backend>::has_task_notification;

/// @brief Backend exposes a native wait-set / poll primitive.
template<typename Backend>
concept wait_set_backend = backend_descriptor<Backend> && capabilities<Backend>::has_wait_set;

/// @brief Backend can safely hand timer expiry to work_queue submission.
template<typename Backend>
concept delayable_work_backend =
    backend_descriptor<Backend> && capabilities<Backend>::has_timer &&
    (!capabilities<Backend>::timer_callbacks_may_run_in_isr || capabilities<Backend>::has_isr_work_queue_submit);

/// @brief Queue/mailbox element type accepted by the backend C ABI.
template<typename T>
concept queue_element = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

/// @brief Ring-buffer element type with explicit fixed-storage construction rules.
template<typename T>
concept ring_buffer_element = std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>;

/// @brief Predicate callable accepted by condition-variable wait helpers.
template<typename Predicate>
concept wait_predicate = predicate<Predicate>;

/// @brief Queue depth used by fixed-capacity queue types.
template<queue_depth_t N>
concept valid_queue_depth = (N > 0U);

/// @brief Positive compile-time extent for fixed-storage containers.
template<std::size_t N>
concept positive_extent = (N > 0U);

/// @brief Stream-buffer trigger level within the declared capacity.
template<std::size_t Capacity, std::size_t TriggerLevel>
concept valid_trigger_level = (Capacity > 0U) && (TriggerLevel > 0U) && (TriggerLevel <= Capacity);

/// @brief Notification slot count for indexed notification objects.
template<std::size_t Slots>
concept valid_notification_slot_count = (Slots > 0U);

/// @} // osal_concepts

}  // namespace osal
