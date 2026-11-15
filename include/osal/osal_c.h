/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file osal_c.h
 * @brief Pure-C interface for MicrOSAL
 * @details Allows C code to use all major OSAL primitives — mutex, semaphore,
 *          queue, mailbox (via a single-slot queue), thread, timer, event
 *          flags, clock, condvar, work queue, notification, delayable work,
 *          memory pool, and read-write lock — without requiring a C++
 *          compiler.
 *
 *          Usage from C:
 *          @code
 *          #include <osal/osal_c.h>
 *
 *          osal_mutex_handle mtx;
 *          osal_c_mutex_create(&mtx, 0);
 *          osal_c_mutex_lock(&mtx, OSAL_WAIT_FOREVER);
 *          osal_c_mutex_unlock(&mtx);
 *          osal_c_mutex_destroy(&mtx);
 *          @endcode
 *
 *          Link against the microsal library (which contains the C++ bridge).
 *
 * @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
 */
#pragma once
#ifndef OSAL_C_H
#define OSAL_C_H

#include <stddef.h>  // NOLINT(hicpp-deprecated-headers,modernize-deprecated-headers)
#include <stdint.h>  // NOLINT(hicpp-deprecated-headers,modernize-deprecated-headers)

/* ======================================================================== */
/* Branch prediction hints                                                  */
/* ======================================================================== */

/**
 * @brief Hint that a boolean condition is expected to evaluate true.
 * @details This is a code-generation hint only. It never changes semantics.
 *          Use these macros only with boolean conditions or expressions used
 *          directly as conditions.
 *
 *          Apply them sparingly and only on measured or strongly justified
 *          hot paths where the cold branch is expected to stay cold in steady
 *          state. In C++20-only implementation files, prefer standard
 *          `[[likely]]` / `[[unlikely]]` branch attributes when they fit.
 */
#if defined(__has_builtin)
#if __has_builtin(__builtin_expect)
#define OSAL_DETAIL_HAS_BUILTIN_EXPECT 1
#endif
#endif

#if !defined(OSAL_DETAIL_HAS_BUILTIN_EXPECT)
#if defined(__GNUC__) || defined(__clang__)
#define OSAL_DETAIL_HAS_BUILTIN_EXPECT 1
#else
#define OSAL_DETAIL_HAS_BUILTIN_EXPECT 0
#endif
#endif

#ifndef OSAL_LIKELY
#if OSAL_DETAIL_HAS_BUILTIN_EXPECT
#define OSAL_LIKELY(expr) (__builtin_expect(!!(expr), 1))
#else
#define OSAL_LIKELY(expr) (!!(expr))
#endif
#endif

#ifndef OSAL_UNLIKELY
#if OSAL_DETAIL_HAS_BUILTIN_EXPECT
#define OSAL_UNLIKELY(expr) (__builtin_expect(!!(expr), 0))
#else
#define OSAL_UNLIKELY(expr) (!!(expr))
#endif
#endif

#undef OSAL_DETAIL_HAS_BUILTIN_EXPECT

#if ((defined(OSAL_CFG_TICK_TYPE_U16) ? 1 : 0) + (defined(OSAL_CFG_TICK_TYPE_U32) ? 1 : 0) + \
     (defined(OSAL_CFG_TICK_TYPE_U64) ? 1 : 0)) > 1
#error "OSAL: only one of OSAL_CFG_TICK_TYPE_U16/U32/U64 may be defined"
#endif

#ifdef __cplusplus
#define OSAL_C_MUTABLE mutable
extern "C"
{
#else
#define OSAL_C_MUTABLE
#endif

    /* ======================================================================== */
    /* Error codes                                                              */
    /* ======================================================================== */

    /** @brief Error codes returned by all osal_c_* functions. */
    enum osal_error
    {
        OSAL_OK                = 0,
        OSAL_TIMEOUT           = 1,
        OSAL_WOULD_BLOCK       = 2,
        OSAL_INVALID_ARGUMENT  = 3,
        OSAL_NOT_SUPPORTED     = 4,
        OSAL_OUT_OF_RESOURCES  = 5,
        OSAL_PERMISSION_DENIED = 6,
        OSAL_ALREADY_EXISTS    = 7,
        OSAL_NOT_INITIALIZED   = 8,
        OSAL_OVERFLOW          = 9,
        OSAL_UNDERFLOW         = 10,
        OSAL_DEADLOCK_DETECTED = 11,
        OSAL_NOT_OWNER         = 12,
        OSAL_ISR_INVALID       = 13,
        OSAL_BARRIER_SERIAL    = 14,
        OSAL_OWNER_DIED        = 15,
        OSAL_NOT_RECOVERABLE   = 16,
        OSAL_CANCELLED         = 17,
        OSAL_UNKNOWN           = 255
    };

    /** @brief Tick count type (maps to OSAL timeout token). */
#if defined(OSAL_CFG_TICK_TYPE_U64)
    typedef uint64_t osal_tick_t;
#elif defined(OSAL_CFG_TICK_TYPE_U16)
typedef uint16_t osal_tick_t;
#else
typedef uint32_t osal_tick_t;
#endif

#if defined(OSAL_CFG_NATIVE_TICK_BITS)
#if !((OSAL_CFG_NATIVE_TICK_BITS == 16) || (OSAL_CFG_NATIVE_TICK_BITS == 32) || (OSAL_CFG_NATIVE_TICK_BITS == 64))
#error "OSAL_CFG_NATIVE_TICK_BITS must be 16, 32, or 64"
#endif
#ifdef __cplusplus
    static_assert((sizeof(osal_tick_t) * 8U) <= (OSAL_CFG_NATIVE_TICK_BITS),
                  "osal_tick_t is wider than backend native tick type");
#else
    _Static_assert((sizeof(osal_tick_t) * 8U) <= (OSAL_CFG_NATIVE_TICK_BITS),
                   "osal_tick_t is wider than backend native tick type");
#endif
#endif

    /** @brief Event flag bitmask — 32 bits, one bit per flag. */
    typedef uint32_t osal_event_bits_t;

    /** @brief Thread priority — higher numeric value = higher priority. */
    typedef int32_t osal_priority_t;

    /** @brief CPU affinity mask. Bit N = core N. 0 = any core. */
    typedef uint32_t osal_affinity_t;

    /** @brief Result type returned by osal_c_* functions. */
    typedef int32_t osal_result_t;

/* ======================================================================== */
/* Constants                                                                */
/* ======================================================================== */

/** @brief Wait forever / infinite timeout. */
#if defined(OSAL_CFG_TICK_TYPE_U64)
#define OSAL_WAIT_FOREVER ((osal_tick_t)UINT64_MAX)
#elif defined(OSAL_CFG_TICK_TYPE_U16)
#define OSAL_WAIT_FOREVER ((osal_tick_t)UINT16_MAX)
#else
#define OSAL_WAIT_FOREVER ((osal_tick_t)UINT32_MAX)
#endif

/** @brief Do not wait / non-blocking. */
#define OSAL_NO_WAIT ((osal_tick_t)0U)

/** @brief Lowest valid thread priority. */
#define OSAL_PRIORITY_LOWEST 0
/** @brief Default / normal thread priority. */
#define OSAL_PRIORITY_NORMAL 128
/** @brief Highest valid thread priority. */
#define OSAL_PRIORITY_HIGHEST 255

/** @brief No affinity constraint (any core). */
#define OSAL_AFFINITY_ANY 0U

    /* ======================================================================== */
    /* Callback types                                                           */
    /* ======================================================================== */

    /**
     * @brief Thread entry callback invoked by a newly started OSAL thread.
     * @param[in] arg  Opaque argument supplied at thread creation.
     * @note Runs in the created thread's context.
     */
    typedef void (*osal_c_thread_entry_t)(void* arg);

    /**
     * @brief Timer-expiry callback.
     * @param[in] arg  Opaque argument supplied at timer creation.
     * @note Runs in the context permitted by the active backend; it may be ISR
     *       context when the timer capability allows callbacks from an ISR.
     */
    typedef void (*osal_c_timer_callback_t)(void* arg);

    /**
     * @brief Work-queue item callback.
     * @param[in] arg  Opaque argument supplied at submission.
     * @note Runs asynchronously in the work-queue worker thread.
     */
    typedef void (*osal_c_work_func_t)(void* arg);

    /** @brief Notification update action. */
    typedef enum
    {
        OSAL_NOTIFICATION_SET_BITS     = 0,
        OSAL_NOTIFICATION_INCREMENT    = 1,
        OSAL_NOTIFICATION_OVERWRITE    = 2,
        OSAL_NOTIFICATION_NO_OVERWRITE = 3
    } osal_notification_action;

    /* ======================================================================== */
    /* Configuration structs (const / FLASH-resident)                           */
    /* ======================================================================== */

    /**
     * @brief Declare config structs as @c const to place them in .rodata (FLASH).
     *
     * Config structs carry all creation-time parameters.  The corresponding
     * @c osal_c_*_create_with_cfg() functions accept a @c const pointer to the
     * config, keeping only the mutable handle in RAM.
     */

    /** @brief Mutex creation config. */
    typedef struct
    {
        int recursive; /**< Non-zero for recursive mutex. */
    } osal_mutex_config;

    typedef enum
    {
        OSAL_MUTEX_NATIVE_DEFAULT       = 0,
        OSAL_MUTEX_PROTOCOL_NONE        = 1,
        OSAL_MUTEX_PRIORITY_INHERITANCE = 2,
        OSAL_MUTEX_PRIORITY_CEILING     = 3
    } osal_mutex_protocol;

    typedef enum
    {
        OSAL_MUTEX_NON_ROBUST = 0,
        OSAL_MUTEX_ROBUST     = 1
    } osal_mutex_robustness;

    /** Separate descriptor preserves the legacy osal_mutex_config layout. */
    typedef struct
    {
        int                   recursive;
        osal_mutex_protocol   protocol;
        osal_mutex_robustness robustness;
        int                   priority_ceiling;
    } osal_mutex_policy_config;

    typedef enum
    {
        OSAL_DIAGNOSTIC_MUTEX = 0,
        OSAL_DIAGNOSTIC_SEMAPHORE,
        OSAL_DIAGNOSTIC_QUEUE,
        OSAL_DIAGNOSTIC_THREAD,
        OSAL_DIAGNOSTIC_TIMER,
        OSAL_DIAGNOSTIC_WORK_QUEUE,
        OSAL_DIAGNOSTIC_EMULATED_MEMORY_POOL,
        OSAL_DIAGNOSTIC_EMULATED_WORK_QUEUE
    } osal_diagnostic_resource;

    typedef struct
    {
        uint32_t capacity, in_use, high_water;
        uint32_t attempts, successes, failures;
        uint32_t contention, drops, backlog;
    } osal_diagnostic_snapshot;

    /** @brief Semaphore creation config. */
    typedef struct
    {
        unsigned int initial_count; /**< Initial permit count. */
        unsigned int max_count;     /**< Maximum permit count. */
    } osal_semaphore_config;

    /** @brief Queue creation config. */
    typedef struct
    {
        void*  buffer;    /**< Caller-supplied buffer (>= item_size * capacity). */
        size_t item_size; /**< Size of each item in bytes. */
        size_t capacity;  /**< Maximum number of items. */
    } osal_queue_config;

    /** @brief Thread creation config. */
    typedef struct
    {
        osal_c_thread_entry_t entry;       /**< Thread entry function. */
        void*                 arg;         /**< Argument passed to entry. */
        osal_priority_t       priority;    /**< Thread priority (0-255). */
        osal_affinity_t       affinity;    /**< CPU affinity mask (0 = any). */
        void*                 stack;       /**< Caller-supplied stack buffer. */
        size_t                stack_bytes; /**< Size of stack buffer in bytes. */
        const char*           name;        /**< Debug name (may be NULL). */
    } osal_thread_config;

    /** @brief Timer creation config. */
    typedef struct
    {
        const char*             name;         /**< Debug name (may be NULL). */
        osal_c_timer_callback_t callback;     /**< Function called on expiry. */
        void*                   arg;          /**< Argument forwarded to callback. */
        osal_tick_t             period_ticks; /**< Timer period in RTOS ticks. */
        int                     auto_reload;  /**< Non-zero for periodic. */
    } osal_timer_config;

    /** @brief Work queue creation config. */
    typedef struct
    {
        void*       stack;       /**< Caller-supplied stack for the worker thread. */
        size_t      stack_bytes; /**< Size of stack in bytes. */
        size_t      depth;       /**< Maximum number of pending work items. */
        const char* name;        /**< Debug name (may be NULL). */
    } osal_work_queue_config;

    /** @brief Memory pool creation config. */
    typedef struct
    {
        void*       buffer;      /**< Caller-supplied backing storage. */
        size_t      buf_bytes;   /**< Size of buffer in bytes. */
        size_t      block_size;  /**< Size of each block in bytes. */
        size_t      block_count; /**< Number of blocks. */
        const char* name;        /**< Debug name (may be NULL). */
    } osal_memory_pool_config;

/**
 * @brief Per-message length-header overhead (bytes).
 *
 * The message buffer prefixes each message with a 2-byte length field.
 * The backing buffer passed to osal_c_message_buffer_create must therefore
 * be at least (max_payload + OSAL_MSG_HEADER_BYTES + 1) bytes.
 */
#define OSAL_MSG_HEADER_BYTES 2U

    /** @brief Stream buffer creation config. */
    typedef struct
    {
        void*  buffer;        /**< Caller-supplied ring storage (capacity+1 bytes). */
        size_t capacity;      /**< Usable byte capacity (N). */
        size_t trigger_level; /**< Min bytes before receive unblocks (1..N). */
    } osal_stream_buffer_config;

    /** @brief Message buffer creation config. */
    typedef struct
    {
        void*  buffer;   /**< Caller-supplied ring storage (capacity+1 bytes). */
        size_t capacity; /**< Total ring byte capacity including header overhead. */
    } osal_message_buffer_config;

    /* ======================================================================== */
    /* Opaque handle types                                                      */
    /* ======================================================================== */

    /**
     * Backend-backed handles are layout-compatible with
     * osal::active_traits::*_handle_t (which is `struct { void* native; }`).
     */
    typedef struct
    {
        void* native;
    } osal_mutex_handle;
    typedef struct
    {
        void* native;
    } osal_semaphore_handle;
    typedef struct
    {
        void* native;
    } osal_queue_handle;
    typedef struct
    {
        void* native;
    } osal_thread_handle;
    typedef struct
    {
        void* native;
    } osal_timer_handle;
    typedef struct
    {
        void* native;
    } osal_event_flags_handle;
    typedef struct
    {
        void* native;
    } osal_condvar_handle;
    typedef struct
    {
        void* native;
    } osal_work_queue_handle;
    typedef struct
    {
        void* native;
    } osal_memory_pool_handle;
    typedef struct
    {
        void* native;
    } osal_rwlock_handle;
    typedef struct
    {
        void* native;
    } osal_stream_buffer_handle;
    typedef struct
    {
        void* native;
    } osal_message_buffer_handle;

    /** @brief Notification creation config. */
    typedef struct
    {
        uint32_t* values;     /**< Caller-supplied slot values array. */
        uint8_t*  pending;    /**< Caller-supplied pending flags array. */
        size_t    slot_count; /**< Number of slots in both arrays. */
    } osal_notification_config;

    /**
     * @brief Composite C notification handle.
     * @details Pure-C helper built from a mutex, condvar, and caller-supplied
     *          slot arrays. Unlike backend-backed handles above, it is not
     *          layout-compatible with an @c active_traits handle. The arrays
     *          are borrowed and must outlive the helper.
     * @note Operations use a mutex and are not ISR-safe.
     */
    typedef struct
    {
        OSAL_C_MUTABLE osal_mutex_handle   mutex;
        OSAL_C_MUTABLE osal_condvar_handle condvar;
        uint32_t*                          values;
        uint8_t*                           pending;
        size_t                             slot_count;
        uint8_t                            valid;
    } osal_notification_handle;

    /** @brief Delayable work creation config. */
    typedef struct
    {
        osal_work_queue_handle* queue; /**< Target work queue. */
        osal_c_work_func_t      func;  /**< Callback executed on the queue. */
        void*                   arg;   /**< Argument passed to @c func. */
        const char*             name;  /**< Debug name for the backing timer. */
    } osal_delayable_work_config;

    /**
     * @brief Composite C delayable-work handle.
     * @details Pure-C helper built from a timer, mutex, condvar, and a borrowed
     *          work queue handle. The queue must outlive this helper.
     * @note Schedule, cancel, flush, and destroy operations require thread context.
     */
    typedef struct
    {
        osal_timer_handle       timer;
        osal_mutex_handle       mutex;
        osal_condvar_handle     condvar;
        osal_work_queue_handle* queue;
        osal_c_work_func_t      func;
        void*                   arg;
        osal_result_t           dispatch_error;
        uint32_t                state;
        uint32_t                start_low;
        uint32_t                start_high;
        uint32_t                delay_low;
        uint32_t                delay_high;
        uint8_t                 dispatch_failed;
        uint8_t                 valid;
    } osal_delayable_work_handle;

    /** @brief C TLS key handle. */
    typedef struct
    {
        uint8_t key;   /**< Internal key index. */
        uint8_t valid; /**< Non-zero when the key is active. */
    } osal_tls_key_handle;

    /* ======================================================================== */
    /* Clock                                                                    */
    /* ======================================================================== */

    /**
     * @brief Returns monotonic time in milliseconds since an arbitrary epoch.
     * @return Current monotonic time in milliseconds; the epoch is backend-defined.
     */
    int64_t osal_c_clock_monotonic_ms(void);

    /**
     * @brief Returns system (wall-clock) time in milliseconds since Unix epoch.
     * @return Current system time in milliseconds; the value may move when wall time is adjusted.
     */
    int64_t osal_c_clock_system_ms(void);

    /**
     * @brief Returns the current OSAL tick count.
     * @return Current tick count in @c osal_tick_t units.
     */
    osal_tick_t osal_c_clock_ticks(void);

    /**
     * @brief Returns the duration of one OSAL tick in microseconds.
     * @return Nominal tick period in microseconds.
     */
    uint32_t osal_c_clock_tick_period_us(void);

    /**
     * @brief Returns the best available high-resolution time source in nanoseconds.
     * @return Current high-resolution timestamp; query support and resolution before relying on native precision.
     */
    int64_t osal_c_clock_high_resolution_ns(void);

    /**
     * @brief Returns the nominal resolution of the high-resolution source in nanoseconds.
     * @return Nominal resolution reported by the selected high-resolution source.
     */
    int64_t osal_c_clock_high_resolution_resolution_ns(void);

    /**
     * @brief Tests whether the active backend provides a native high-resolution source.
     * @return Nonzero when supported, otherwise zero.
     */
    int osal_c_clock_high_resolution_supported(void);

    /** @brief Absolute monotonic deadline helper for live timeout checks in C. */
    typedef struct
    {
        int64_t expiry_ms;
    } osal_monotonic_deadline;

    /** @brief Absolute high-resolution deadline helper for live timeout checks in C. */
    typedef struct
    {
        int64_t expiry_ns;
    } osal_high_resolution_deadline;

    /**
     * @brief Adds a positive delta to a signed timestamp with saturation.
     * @param[in] base   Starting timestamp.
     * @param[in] delta  Delta to add; non-positive values leave @p base unchanged.
     * @return Saturated sum, never greater than @c INT64_MAX.
     * @internal
     */
    static inline int64_t osal_c_detail_saturating_add_i64(int64_t base, int64_t delta)
    {
        if (delta <= 0)
        {
            return base;
        }
        return (base >= (INT64_MAX - delta)) ? INT64_MAX : (base + delta);
    }

    /**
     * @brief Computes nonnegative time remaining before an absolute timestamp.
     * @param[in] expiry  Absolute deadline.
     * @param[in] now     Current timestamp in the same units.
     * @return @c expiry - @c now when positive, otherwise zero.
     * @internal
     */
    static inline int64_t osal_c_detail_remaining_i64(int64_t expiry, int64_t now)
    {
        return (now >= expiry) ? 0 : (expiry - now);
    }

    /**
     * @brief Converts positive microseconds to nanoseconds with saturation.
     * @param[in] timeout_us  Duration to convert.
     * @return Zero for non-positive input, otherwise the converted or saturated value.
     * @internal
     */
    static inline int64_t osal_c_detail_us_to_ns_saturating(int64_t timeout_us)
    {
        if (timeout_us <= 0)
        {
            return 0;
        }
        return (timeout_us >= (INT64_MAX / 1000)) ? INT64_MAX : (timeout_us * 1000);
    }

    /**
     * @brief Converts nanoseconds to microseconds, rounding positive values upward.
     * @param[in] duration_ns  Duration to convert.
     * @return Zero for non-positive input, otherwise a ceiling-rounded value.
     * @internal
     */
    static inline int64_t osal_c_detail_ns_to_us_ceil(int64_t duration_ns)
    {
        if (duration_ns <= 0)
        {
            return 0;
        }
        if (duration_ns > (INT64_MAX - 999))
        {
            return (INT64_MAX / 1000) + (((INT64_MAX % 1000) != 0) ? 1 : 0);
        }
        return (duration_ns + 999) / 1000;
    }

    /**
     * @brief Creates a monotonic deadline relative to the current time.
     * @param[in] timeout_ms  Relative timeout in milliseconds; non-positive values expire immediately.
     * @return Deadline containing a saturated absolute monotonic timestamp.
     */
    static inline osal_monotonic_deadline osal_c_monotonic_deadline_after_ms(int64_t timeout_ms)
    {
        const int64_t           now_ms = osal_c_clock_monotonic_ms();
        osal_monotonic_deadline deadline;
        deadline.expiry_ms = (timeout_ms <= 0) ? now_ms : osal_c_detail_saturating_add_i64(now_ms, timeout_ms);
        return deadline;
    }

    /**
     * @brief Creates a monotonic deadline from an absolute timestamp.
     * @param[in] deadline_ms  Absolute monotonic timestamp in milliseconds.
     * @return Deadline containing @p deadline_ms.
     */
    static inline osal_monotonic_deadline osal_c_monotonic_deadline_at_ms(int64_t deadline_ms)
    {
        osal_monotonic_deadline deadline;
        deadline.expiry_ms = deadline_ms;
        return deadline;
    }

    /**
     * @brief Tests whether a monotonic deadline has expired.
     * @param[in] deadline  Deadline to test.
     * @return Nonzero when current monotonic time is at or past the deadline.
     */
    static inline int osal_c_monotonic_deadline_expired(osal_monotonic_deadline deadline)
    {
        return (osal_c_clock_monotonic_ms() >= deadline.expiry_ms) ? 1 : 0;
    }

    /**
     * @brief Returns the remaining monotonic time in milliseconds.
     * @param[in] deadline  Deadline to query.
     * @return Remaining time, saturated at zero.
     */
    static inline int64_t osal_c_monotonic_deadline_remaining_ms(osal_monotonic_deadline deadline)
    {
        return osal_c_detail_remaining_i64(deadline.expiry_ms, osal_c_clock_monotonic_ms());
    }

    /**
     * @brief Restarts a deadline relative to the current time.
     * @param[in,out] deadline    Deadline to overwrite; must not be null.
     * @param[in]     timeout_ms  Relative timeout in milliseconds; non-positive values expire immediately.
     */
    static inline void osal_c_monotonic_deadline_restart_ms(osal_monotonic_deadline* deadline, int64_t timeout_ms)
    {
        *deadline = osal_c_monotonic_deadline_after_ms(timeout_ms);
    }

    /**
     * @brief Creates a high-resolution deadline relative to the current time.
     * @param[in] timeout_us  Relative timeout in microseconds; non-positive values expire immediately.
     * @return Deadline containing a saturated absolute nanosecond timestamp.
     */
    static inline osal_high_resolution_deadline osal_c_high_resolution_deadline_after_us(int64_t timeout_us)
    {
        const int64_t                 now_ns = osal_c_clock_high_resolution_ns();
        osal_high_resolution_deadline deadline;
        deadline.expiry_ns =
            (timeout_us <= 0) ? now_ns
                              : osal_c_detail_saturating_add_i64(now_ns, osal_c_detail_us_to_ns_saturating(timeout_us));
        return deadline;
    }

    /**
     * @brief Creates a high-resolution deadline from an absolute timestamp.
     * @param[in] deadline_ns  Absolute high-resolution timestamp in nanoseconds.
     * @return Deadline containing @p deadline_ns.
     */
    static inline osal_high_resolution_deadline osal_c_high_resolution_deadline_at_ns(int64_t deadline_ns)
    {
        osal_high_resolution_deadline deadline;
        deadline.expiry_ns = deadline_ns;
        return deadline;
    }

    /**
     * @brief Tests whether a high-resolution deadline has expired.
     * @param[in] deadline  Deadline to test.
     * @return Nonzero when the high-resolution clock is at or past the deadline.
     */
    static inline int osal_c_high_resolution_deadline_expired(osal_high_resolution_deadline deadline)
    {
        return (osal_c_clock_high_resolution_ns() >= deadline.expiry_ns) ? 1 : 0;
    }

    /**
     * @brief Returns the remaining high-resolution time in nanoseconds.
     * @param[in] deadline  Deadline to query.
     * @return Remaining time, saturated at zero.
     */
    static inline int64_t osal_c_high_resolution_deadline_remaining_ns(osal_high_resolution_deadline deadline)
    {
        return osal_c_detail_remaining_i64(deadline.expiry_ns, osal_c_clock_high_resolution_ns());
    }

    /**
     * @brief Returns the remaining high-resolution time in microseconds.
     * @param[in] deadline  Deadline to query.
     * @return Remaining time rounded up to whole microseconds and saturated at zero.
     */
    static inline int64_t osal_c_high_resolution_deadline_remaining_us(osal_high_resolution_deadline deadline)
    {
        return osal_c_detail_ns_to_us_ceil(osal_c_high_resolution_deadline_remaining_ns(deadline));
    }

    /**
     * @brief Restarts a high-resolution deadline relative to the current time.
     * @param[in,out] deadline    Deadline to overwrite; must not be null.
     * @param[in]     timeout_us  Relative timeout in microseconds; non-positive values expire immediately.
     */
    static inline void osal_c_high_resolution_deadline_restart_us(osal_high_resolution_deadline* deadline,
                                                                  int64_t                        timeout_us)
    {
        *deadline = osal_c_high_resolution_deadline_after_us(timeout_us);
    }

    /* ======================================================================== */
    /* Mutex                                                                    */
    /* ======================================================================== */

    /**
     * @brief Creates a mutex with the requested recursion behavior.
     * @param[out] handle     Handle to initialize; must not be null or already own a mutex.
     * @param[in]  recursive  Nonzero to request recursive locking.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, support, or backend error.
     */
    osal_result_t osal_c_mutex_create(osal_mutex_handle* handle, int recursive);

    /**
     * @brief Reads the process-wide diagnostic counters for one resource category.
     * @param[in]  resource  Resource category; must name a defined diagnostic resource.
     * @param[out] snapshot  Receives the sampled counters and gauges; must not be null.
     * @return @c OSAL_OK on success, or a canonical invalid-argument error for an invalid resource or null output.
     */
    osal_result_t osal_c_diagnostics_read(osal_diagnostic_resource resource, osal_diagnostic_snapshot* snapshot);

    /**
     * @brief Creates a mutex from a read-only configuration.
     * @param[out] handle  Handle to initialize; must not be null or already own a mutex.
     * @param[in]  cfg     Configuration; must not be null and must remain valid for this call.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, support, or backend error.
     */
    osal_result_t osal_c_mutex_create_with_cfg(osal_mutex_handle* handle, const osal_mutex_config* cfg);

    /**
     * @brief Creates a mutex with protocol and robustness policy.
     * @param[out] handle  Handle to initialize; must not be null or already own a mutex.
     * @param[in]  cfg     Policy configuration; must not be null and must remain valid for this call.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, support, or backend error.
     */
    osal_result_t osal_c_mutex_create_with_policy(osal_mutex_handle* handle, const osal_mutex_policy_config* cfg);

    /**
     * @brief Marks a recovered robust mutex consistent after repairing protected state.
     * @param[in,out] handle  Initialized robust mutex handle.
     * @return @c OSAL_OK on success; otherwise a canonical initialization, support, ownership, or backend error.
     * @pre Call only after acquiring a robust mutex that reported owner death and repairing its protected state.
     */
    osal_result_t osal_c_mutex_consistent(osal_mutex_handle* handle);

    /**
     * @brief Destroys an initialized mutex.
     * @param[in,out] handle  Mutex handle to destroy.
     * @return @c OSAL_OK on success; otherwise a canonical initialization, ownership, or backend error.
     */
    osal_result_t osal_c_mutex_destroy(osal_mutex_handle* handle);

    /**
     * @brief Locks a mutex, waiting up to the specified timeout.
     * @param[in,out] handle   Initialized mutex handle.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_NO_WAIT polls and @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK on acquisition; otherwise a canonical timeout, deadlock, ownership, initialization,
     *         or backend error.
     */
    osal_result_t osal_c_mutex_lock(osal_mutex_handle* handle, osal_tick_t timeout);

    /**
     * @brief Attempts to lock a mutex without waiting.
     * @param[in,out] handle  Initialized mutex handle.
     * @return @c OSAL_OK on acquisition; otherwise a canonical would-block, ownership, initialization, or backend
     * error.
     */
    osal_result_t osal_c_mutex_try_lock(osal_mutex_handle* handle);

    /**
     * @brief Unlocks a mutex held by the calling thread.
     * @param[in,out] handle  Initialized mutex handle.
     * @return @c OSAL_OK on success; otherwise a canonical initialization, ownership, or backend error.
     */
    osal_result_t osal_c_mutex_unlock(osal_mutex_handle* handle);

    /* ======================================================================== */
    /* Semaphore                                                                */
    /* ======================================================================== */

    /**
     * @brief Creates a counting semaphore.
     * @param[out] handle        Handle to initialize; must not be null or already own a semaphore.
     * @param[in]  initial_count Initial permit count.
     * @param[in]  max_count     Maximum permit count; must be nonzero and at least @p initial_count.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, or backend error.
     */
    osal_result_t osal_c_semaphore_create(osal_semaphore_handle* handle, unsigned int initial_count,
                                          unsigned int max_count);

    /**
     * @brief Creates a semaphore from a read-only configuration.
     * @param[out] handle  Handle to initialize; must not be null or already own a semaphore.
     * @param[in]  cfg     Configuration; must not be null.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, or backend error.
     */
    osal_result_t osal_c_semaphore_create_with_cfg(osal_semaphore_handle* handle, const osal_semaphore_config* cfg);

    /**
     * @brief Destroys an initialized semaphore.
     * @param[in,out] handle  Semaphore handle to destroy.
     * @return @c OSAL_OK on success; otherwise a canonical initialization or backend error.
     */
    osal_result_t osal_c_semaphore_destroy(osal_semaphore_handle* handle);

    /**
     * @brief Gives one permit to a semaphore.
     * @param[in,out] handle  Initialized semaphore handle.
     * @return @c OSAL_OK on success; otherwise a canonical overflow, initialization, or backend error.
     */
    osal_result_t osal_c_semaphore_give(osal_semaphore_handle* handle);

    /**
     * @brief Gives one permit from interrupt context.
     * @param[in,out] handle  Initialized semaphore handle.
     * @return @c OSAL_OK on success; otherwise the backend's canonical maximum-count, ISR, initialization,
     *         or support error.
     * @pre Call only from ISR context and only when the active backend supports ISR semaphore give.
     */
    osal_result_t osal_c_semaphore_give_isr(osal_semaphore_handle* handle);

    /**
     * @brief Takes one semaphore permit, waiting up to the specified timeout.
     * @param[in,out] handle   Initialized semaphore handle.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_NO_WAIT polls and @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK on success; otherwise a canonical timeout, initialization, or backend error.
     */
    osal_result_t osal_c_semaphore_take(osal_semaphore_handle* handle, osal_tick_t timeout);

    /**
     * @brief Attempts to take one semaphore permit without waiting.
     * @param[in,out] handle  Initialized semaphore handle.
     * @return @c OSAL_OK on success; otherwise a canonical would-block, initialization, or backend error.
     */
    osal_result_t osal_c_semaphore_try_take(osal_semaphore_handle* handle);

    /* ======================================================================== */
    /* Queue                                                                    */
    /* ======================================================================== */

    /**
     * @brief Creates a fixed-capacity message queue using caller-owned storage.
     * @param[out] handle     Handle to initialize; must not be null or already own a queue.
     * @param[in]  buffer     Writable backing buffer, at least @p item_size * @p capacity bytes.
     * @param[in]  item_size  Size of each copied item in bytes; must be nonzero.
     * @param[in]  capacity   Maximum number of items; must be nonzero.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, or backend error.
     * @pre The buffer remains alive and writable until the queue is destroyed.
     */
    osal_result_t osal_c_queue_create(osal_queue_handle* handle, void* buffer, size_t item_size, size_t capacity);

    /**
     * @brief Creates a queue from a read-only configuration descriptor.
     * @param[out] handle  Handle to initialize; must not be null or already own a queue.
     * @param[in]  cfg     Configuration; its buffer must remain alive until queue destruction.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, or backend error.
     */
    osal_result_t osal_c_queue_create_with_cfg(osal_queue_handle* handle, const osal_queue_config* cfg);

    /**
     * @brief Destroys an initialized queue.
     * @param[in,out] handle  Queue handle to destroy.
     * @return @c OSAL_OK on success; otherwise a canonical initialization or backend error.
     */
    osal_result_t osal_c_queue_destroy(osal_queue_handle* handle);

    /**
     * @brief Copies an item into a queue, waiting up to the specified timeout.
     * @param[in,out] handle   Initialized queue handle.
     * @param[in]     item     Pointer to one item of the configured item size.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_NO_WAIT polls and @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK on success; @c OSAL_TIMEOUT when waiting expires; otherwise a canonical backend error.
     */
    osal_result_t osal_c_queue_send(osal_queue_handle* handle, const void* item, osal_tick_t timeout);

    /**
     * @brief Copies an item into a queue from interrupt context without waiting.
     * @param[in,out] handle  Initialized queue handle.
     * @param[in]     item    Pointer to one item of the configured item size.
     * @return @c OSAL_OK on success; otherwise the backend's canonical full-queue, ISR, initialization,
     *         or support error.
     * @pre Call only from ISR context and only when the active backend supports ISR queue operations.
     */
    osal_result_t osal_c_queue_send_isr(osal_queue_handle* handle, const void* item);

    /**
     * @brief Copies the next item from a queue, waiting up to the specified timeout.
     * @param[in,out] handle   Initialized queue handle.
     * @param[out]    item     Writable storage for one item of the configured item size.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_NO_WAIT polls and @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK on success; @c OSAL_TIMEOUT when waiting expires; otherwise a canonical backend error.
     */
    osal_result_t osal_c_queue_receive(osal_queue_handle* handle, void* item, osal_tick_t timeout);

    /**
     * @brief Copies the next item from a queue in interrupt context without waiting.
     * @param[in,out] handle  Initialized queue handle.
     * @param[out]    item    Writable storage for one item of the configured item size.
     * @return @c OSAL_OK on success; otherwise the backend's canonical empty-queue, ISR, initialization,
     *         or support error.
     * @pre Call only from ISR context and only when the active backend supports ISR queue operations.
     */
    osal_result_t osal_c_queue_receive_isr(osal_queue_handle* handle, void* item);

    /**
     * @brief Copies the next queue item without removing it, waiting up to the timeout.
     * @param[in,out] handle   Initialized queue handle.
     * @param[out]    item     Writable storage for one item of the configured item size.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_NO_WAIT polls and @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK on success; otherwise a canonical timeout, empty-queue, initialization, or backend error.
     */
    osal_result_t osal_c_queue_peek(osal_queue_handle* handle, void* item, osal_tick_t timeout);

    /**
     * @brief Returns the number of items currently queued.
     * @param[in] handle  Queue handle to query.
     * @return Current item count, or zero when @p handle is null or invalid.
     */
    size_t osal_c_queue_count(const osal_queue_handle* handle);

    /**
     * @brief Returns the number of free item slots.
     * @param[in] handle  Queue handle to query.
     * @return Free slot count, or zero when @p handle is null or invalid.
     */
    size_t osal_c_queue_free(const osal_queue_handle* handle);

    /* ======================================================================== */
    /* Thread                                                                   */
    /* ======================================================================== */

    /**
     * @brief Creates and starts a thread.
     * @param[out] handle      Handle to initialize; must not be null or already own a thread.
     * @param[in]  entry       Entry function invoked by the new thread; must not be null.
     * @param[in]  arg         Opaque argument passed to @p entry.
     * @param[in]  priority    OSAL priority in the range 0 (lowest) through 255 (highest).
     * @param[in]  affinity    CPU affinity mask; @c OSAL_AFFINITY_ANY imposes no constraint.
     * @param[in]  stack       Optional caller-owned stack buffer; backend requirements vary.
     * @param[in]  stack_bytes Size of @p stack in bytes.
     * @param[in]  name        Optional debug name; may be null.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, support, or backend error.
     * @pre Caller-supplied stack storage, when used, remains valid for the thread's lifetime.
     */
    osal_result_t osal_c_thread_create(osal_thread_handle* handle, osal_c_thread_entry_t entry, void* arg,
                                       osal_priority_t priority, osal_affinity_t affinity, void* stack,
                                       size_t stack_bytes, const char* name);
    /**
     * @brief Creates and starts a thread from a read-only configuration.
     * @param[out] handle  Handle to initialize; must not be null or already own a thread.
     * @param[in]  cfg     Configuration; must not be null.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, support, or backend error.
     * @pre Caller-supplied stack storage and entry argument remain valid as required by the thread.
     */
    osal_result_t osal_c_thread_create_with_cfg(osal_thread_handle* handle, const osal_thread_config* cfg);

    /**
     * @brief Waits for a joinable thread to finish.
     * @param[in,out] handle   Thread handle; on successful join its resource is released.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK on join; @c OSAL_TIMEOUT on expiry; otherwise a canonical ownership,
     *         initialization, support, or backend error.
     */
    osal_result_t osal_c_thread_join(osal_thread_handle* handle, osal_tick_t timeout);

    /**
     * @brief Detaches a thread so its resources are reclaimed without a later join.
     * @param[in,out] handle  Thread handle to detach; resource ownership is released on success.
     * @return @c OSAL_OK on success; otherwise a canonical initialization, ownership, or backend error.
     */
    osal_result_t osal_c_thread_detach(osal_thread_handle* handle);

    /**
     * @brief Changes a running thread's priority.
     * @param[in] handle    Initialized thread handle.
     * @param[in] priority  New OSAL priority from 0 (lowest) through 255 (highest).
     * @return @c OSAL_OK on success; otherwise a canonical validation, support, initialization, or backend error.
     */
    osal_result_t osal_c_thread_set_priority(osal_thread_handle* handle, osal_priority_t priority);

    /**
     * @brief Changes a running thread's CPU affinity.
     * @param[in] handle    Initialized thread handle.
     * @param[in] affinity  New affinity mask; @c OSAL_AFFINITY_ANY removes the constraint.
     * @return @c OSAL_OK on success, @c OSAL_NOT_SUPPORTED when affinity is unavailable,
     *         or another canonical validation/backend error.
     */
    osal_result_t osal_c_thread_set_affinity(osal_thread_handle* handle, osal_affinity_t affinity);

    /**
     * @brief Suspends a thread when the backend supports suspension.
     * @param[in] handle  Initialized thread handle.
     * @return @c OSAL_OK on success, @c OSAL_NOT_SUPPORTED when suspension is unavailable,
     *         or another canonical initialization/backend error.
     */
    osal_result_t osal_c_thread_suspend(osal_thread_handle* handle);

    /**
     * @brief Resumes a suspended thread when the backend supports resumption.
     * @param[in] handle  Initialized thread handle.
     * @return @c OSAL_OK on success, @c OSAL_NOT_SUPPORTED when resumption is unavailable,
     *         or another canonical initialization/backend error.
     */
    osal_result_t osal_c_thread_resume(osal_thread_handle* handle);

    /**
     * @brief Yields the calling thread's execution opportunity.
     * @note Call from thread context; the backend determines scheduling behavior.
     */
    void osal_c_thread_yield(void);

    /**
     * @brief Sleeps the calling thread for a duration expressed in milliseconds.
     * @param[in] ms  Requested sleep duration in milliseconds.
     * @note This operation blocks the calling thread and is not for ISR context.
     */
    void osal_c_thread_sleep_ms(uint32_t ms);

    /* ======================================================================== */
    /* Thread-Local Data                                                        */
    /* ======================================================================== */

    /**
     * @brief Allocates a thread-local key.
     * @param[out] handle  Receives the key and becomes valid on success.
     * @return @c OSAL_OK on success, @c OSAL_INVALID_ARGUMENT for a null output,
     *         or @c OSAL_OUT_OF_RESOURCES when the key table is full.
     */
    osal_result_t osal_c_tls_key_create(osal_tls_key_handle* handle);

    /**
     * @brief Releases a thread-local key.
     * @param[in,out] handle  Key handle to invalidate.
     * @return @c OSAL_OK on success, @c OSAL_INVALID_ARGUMENT for a null handle,
     *         or @c OSAL_NOT_INITIALIZED when the key is already invalid.
     */
    osal_result_t osal_c_tls_key_destroy(osal_tls_key_handle* handle);
    /**
     * @brief Sets the calling thread's value for a thread-local key.
     * @param[in] handle  Valid key handle.
     * @param[in] value   Opaque pointer value; ownership remains with the caller.
     * @return @c OSAL_OK on success, @c OSAL_INVALID_ARGUMENT for a null handle,
     *         or @c OSAL_NOT_INITIALIZED if the key is invalid.
     */
    osal_result_t osal_c_tls_set(osal_tls_key_handle* handle, void* value);
    /**
     * @brief Gets the calling thread's value for a thread-local key.
     * @param[in] handle  Key handle to query.
     * @return Stored pointer for this thread, or null when unset or the handle is invalid.
     * @note The returned pointer is borrowed; this function does not transfer ownership.
     *
     * @code
     * osal_tls_key_handle tls;
     * if (osal_c_tls_key_create(&tls) == OSAL_OK)
     * {
     *     int worker_id = 7;
     *     osal_c_tls_set(&tls, &worker_id);
     *
     *     int* my_id = (int*)osal_c_tls_get(&tls);
     *     // my_id is per-thread
     *
     *     osal_c_tls_key_destroy(&tls);
     * }
     * @endcode
     */
    void* osal_c_tls_get(const osal_tls_key_handle* handle);

    /* ======================================================================== */
    /* Timer                                                                    */
    /* ======================================================================== */

    /**
     * @brief Creates a timer without starting it.
     * @param[out] handle       Handle to initialize; must not be null or already own a timer.
     * @param[in]  name         Optional debug name; may be null.
     * @param[in]  callback     Non-null function invoked on timer expiry.
     * @param[in]  arg          Opaque pointer forwarded to @p callback.
     * @param[in]  period_ticks Timer period in OSAL ticks; must be nonzero.
     * @param[in]  auto_reload  Nonzero for periodic operation; zero for one-shot.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, support, or backend error.
     * @pre Timer support is required. Callback context follows the active backend's timer capability.
     * @note The callback must obey the active backend's ISR/thread-context restrictions.
     */
    osal_result_t osal_c_timer_create(osal_timer_handle* handle, const char* name, osal_c_timer_callback_t callback,
                                      void* arg, osal_tick_t period_ticks, int auto_reload);

    /**
     * @brief Creates a timer from a read-only configuration.
     * @param[out] handle  Handle to initialize; must not be null or already own a timer.
     * @param[in]  cfg     Configuration; must not be null.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, support, or backend error.
     */
    osal_result_t osal_c_timer_create_with_cfg(osal_timer_handle* handle, const osal_timer_config* cfg);

    /**
     * @brief Destroys an initialized timer.
     * @param[in,out] handle  Timer handle to destroy.
     * @return @c OSAL_OK on success; otherwise a canonical initialization or backend error.
     */
    osal_result_t osal_c_timer_destroy(osal_timer_handle* handle);

    /**
     * @brief Starts a created timer.
     * @param[in,out] handle  Initialized timer handle.
     * @return @c OSAL_OK on success; otherwise a canonical initialization, support, or backend error.
     */
    osal_result_t osal_c_timer_start(osal_timer_handle* handle);

    /**
     * @brief Stops a running timer.
     * @param[in,out] handle  Initialized timer handle.
     * @return @c OSAL_OK on success; otherwise a canonical initialization, support, or backend error.
     */
    osal_result_t osal_c_timer_stop(osal_timer_handle* handle);

    /**
     * @brief Restarts a timer from the beginning of its current period.
     * @param[in,out] handle  Initialized timer handle.
     * @return @c OSAL_OK on success; otherwise a canonical initialization, support, or backend error.
     */
    osal_result_t osal_c_timer_reset(osal_timer_handle* handle);

    /**
     * @brief Changes an initialized timer's period.
     * @param[in,out] handle           Initialized timer handle.
     * @param[in]     new_period_ticks New period in OSAL ticks.
     * @return @c OSAL_OK on success; otherwise a canonical validation, initialization, support, or backend error.
     */
    osal_result_t osal_c_timer_set_period(osal_timer_handle* handle, osal_tick_t new_period_ticks);

    /**
     * @brief Tests whether a timer is currently active.
     * @param[in] handle  Timer handle to query.
     * @return One when active; zero when inactive or the handle is invalid.
     */
    int osal_c_timer_is_active(const osal_timer_handle* handle);

    /* ======================================================================== */
    /* Event Flags                                                              */
    /* ======================================================================== */

    /**
     * @brief Creates an event-flags group.
     * @param[out] handle  Handle to initialize.
     * @return @c OSAL_OK on success; otherwise a canonical initialization/resource/backend error.
     */
    osal_result_t osal_c_event_flags_create(osal_event_flags_handle* handle);

    /**
     * @brief Destroys an event-flags group.
     * @param[in,out] handle  Group handle to destroy.
     * @return @c OSAL_OK on success; otherwise a canonical initialization/backend error.
     */
    osal_result_t osal_c_event_flags_destroy(osal_event_flags_handle* handle);

    /**
     * @brief Sets (ORs) event bits in a group.
     * @param[in,out] handle  Initialized group handle.
     * @param[in]     bits    Bit mask to set.
     * @return @c OSAL_OK on success; otherwise a canonical initialization/backend error.
     */
    osal_result_t osal_c_event_flags_set(osal_event_flags_handle* handle, osal_event_bits_t bits);

    /**
     * @brief Clears selected event bits in a group.
     * @param[in,out] handle  Initialized group handle.
     * @param[in]     bits    Bit mask to clear.
     * @return @c OSAL_OK on success; otherwise a canonical initialization/backend error.
     */
    osal_result_t osal_c_event_flags_clear(osal_event_flags_handle* handle, osal_event_bits_t bits);

    /**
     * @brief Reads the current event bits without blocking.
     * @param[in] handle  Group handle to query.
     * @return Current bit pattern, or zero when the handle is invalid.
     */
    osal_event_bits_t osal_c_event_flags_get(const osal_event_flags_handle* handle);

    /**
     * @brief Waits until any requested event bit is set.
     * @param[in]     handle         Initialized group handle.
     * @param[in]     wait_bits      Bit mask to wait for; any set bit satisfies the wait.
     * @param[out]    actual_bits    Optional output for the observed group bits; may be null.
     * @param[in]     clear_on_exit  Nonzero to clear the matched bits before returning.
     * @param[in]     timeout        Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK when matched, @c OSAL_TIMEOUT on expiry, or a canonical validation/backend error.
     */
    osal_result_t osal_c_event_flags_wait_any(osal_event_flags_handle* handle, osal_event_bits_t wait_bits,
                                              osal_event_bits_t* actual_bits, int clear_on_exit, osal_tick_t timeout);

    /**
     * @brief Waits until all requested event bits are set.
     * @param[in]     handle         Initialized group handle.
     * @param[in]     wait_bits      Bit mask to wait for; all set bits must be present.
     * @param[out]    actual_bits    Optional output for the observed group bits; may be null.
     * @param[in]     clear_on_exit  Nonzero to clear the matched bits before returning.
     * @param[in]     timeout        Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK when matched, @c OSAL_TIMEOUT on expiry, or a canonical validation/backend error.
     */
    osal_result_t osal_c_event_flags_wait_all(osal_event_flags_handle* handle, osal_event_bits_t wait_bits,
                                              osal_event_bits_t* actual_bits, int clear_on_exit, osal_tick_t timeout);

    /**
     * @brief Sets event bits from interrupt context.
     * @param[in,out] handle  Initialized group handle.
     * @param[in]     bits    Bit mask to set.
     * @return @c OSAL_OK on success; otherwise the backend's canonical ISR, support, initialization, or backend error.
     * @pre Call only from ISR context and only when the backend supports ISR event-flag updates.
     */
    osal_result_t osal_c_event_flags_set_isr(osal_event_flags_handle* handle, osal_event_bits_t bits);

    /* ======================================================================== */
    /* Condition Variable                                                       */
    /* ======================================================================== */

    /**
     * @brief Creates a condition variable.
     * @param[out] handle  Handle to initialize.
     * @return @c OSAL_OK on success; otherwise a canonical initialization/resource/backend error.
     */
    osal_result_t osal_c_condvar_create(osal_condvar_handle* handle);

    /**
     * @brief Destroys a condition variable.
     * @param[in,out] handle  Condition-variable handle to destroy.
     * @return @c OSAL_OK on success; otherwise a canonical initialization/backend error.
     */
    osal_result_t osal_c_condvar_destroy(osal_condvar_handle* handle);
    /**
     * @brief Atomically unlocks a mutex while waiting, then reacquires it before returning.
     * @param[in,out] handle   Initialized condition-variable handle.
     * @param[in,out] mutex    Initialized mutex owned by the calling thread.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK when the wait completes; @c OSAL_TIMEOUT on expiry; otherwise a canonical ownership,
     *         initialization, or backend error.
     * @pre The caller owns @p mutex; it is held again when this function returns.
     * @note Recheck the associated predicate after waking; condition-variable waits may wake spuriously.
     */
    osal_result_t osal_c_condvar_wait(osal_condvar_handle* handle, osal_mutex_handle* mutex, osal_tick_t timeout);
    /**
     * @brief Wakes one thread waiting on the condition variable.
     * @param[in,out] handle  Initialized condition-variable handle.
     * @return @c OSAL_OK on success; otherwise a canonical initialization/backend error.
     */
    osal_result_t osal_c_condvar_notify_one(osal_condvar_handle* handle);

    /**
     * @brief Wakes all threads waiting on the condition variable.
     * @param[in,out] handle  Initialized condition-variable handle.
     * @return @c OSAL_OK on success; otherwise a canonical initialization/backend error.
     */
    osal_result_t osal_c_condvar_notify_all(osal_condvar_handle* handle);

    /* ======================================================================== */
    /* Notification                                                             */
    /* ======================================================================== */

    /**
     * @brief Creates a notification helper with caller-owned slot storage.
     * @param[out] handle     Composite handle to initialize; must not be null or already valid.
     * @param[in]  values     Writable array of @p slot_count values; must not be null.
     * @param[in]  pending    Writable array of @p slot_count pending flags; must not be null.
     * @param[in]  slot_count Number of slots in both arrays; must be nonzero.
     * @return @c OSAL_OK on success; otherwise a canonical argument, initialization, resource, or backend error.
     * @pre Both arrays remain alive and writable until the helper is destroyed.
     * @note Operations use a mutex and are not ISR-safe.
     */
    osal_result_t osal_c_notification_create(osal_notification_handle* handle, uint32_t* values, uint8_t* pending,
                                             size_t slot_count);
    /**
     * @brief Creates a notification helper from a configuration descriptor.
     * @param[out] handle  Composite handle to initialize; must not be null or already valid.
     * @param[in]  cfg     Configuration; must not be null and its arrays must outlive the helper.
     * @return @c OSAL_OK on success; otherwise a canonical argument, initialization, resource, or backend error.
     */
    osal_result_t osal_c_notification_create_with_cfg(osal_notification_handle*       handle,
                                                      const osal_notification_config* cfg);
    /**
     * @brief Destroys an initialized notification helper.
     * @param[in,out] handle  Helper handle to destroy.
     * @return @c OSAL_OK on success, or a canonical invalid-argument, not-initialized, or backend error.
     */
    osal_result_t osal_c_notification_destroy(osal_notification_handle* handle);
    /**
     * @brief Applies a notification action to one slot.
     * @param[in,out] handle  Initialized notification handle.
     * @param[in]     value   Value used by the selected action.
     * @param[in]     action  Update action to apply; must be a defined enumerator.
     * @param[in]     index   Slot index in the caller-managed arrays.
     * @return @c OSAL_OK on success, @c OSAL_WOULD_BLOCK when no-overwrite finds a pending slot,
     *         or a canonical invalid-argument, not-initialized, or backend error.
     */
    osal_result_t osal_c_notification_notify(osal_notification_handle* handle, uint32_t value,
                                             osal_notification_action action, size_t index);
    /**
     * @brief Waits for one notification slot to become pending.
     * @param[in,out] handle         Initialized notification handle.
     * @param[in]     index          Slot index in the caller-managed arrays.
     * @param[out]    value_out      Optional output for the observed slot value; may be null.
     * @param[in]     clear_on_entry Bits cleared before waiting.
     * @param[in]     clear_on_exit  Bits cleared after the wait succeeds.
     * @param[in]     timeout        Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK on success, @c OSAL_TIMEOUT on expiry, or a canonical invalid-argument,
     *         not-initialized, or backend error.
     * @note The wait consumes the pending state on success.
     */
    osal_result_t osal_c_notification_wait(osal_notification_handle* handle, size_t index, uint32_t* value_out,
                                           uint32_t clear_on_entry, uint32_t clear_on_exit, osal_tick_t timeout);
    /**
     * @brief Clears selected bits in one notification slot without changing pending state.
     * @param[in,out] handle  Initialized notification handle.
     * @param[in]     bits    Bit mask to clear from the slot value.
     * @param[in]     index   Slot index in the caller-managed arrays.
     * @return @c OSAL_OK on success, or a canonical invalid-argument, not-initialized, or backend error.
     */
    osal_result_t osal_c_notification_clear(osal_notification_handle* handle, uint32_t bits, size_t index);
    /**
     * @brief Resets one notification slot to zero and not-pending.
     * @param[in,out] handle  Initialized notification handle.
     * @param[in]     index   Slot index in the caller-managed arrays.
     * @return @c OSAL_OK on success, or a canonical invalid-argument, not-initialized, or backend error.
     */
    osal_result_t osal_c_notification_reset(osal_notification_handle* handle, size_t index);
    /**
     * @brief Reports whether one notification slot is pending.
     * @param[in] handle  Notification handle to query.
     * @param[in] index   Slot index in the caller-managed arrays.
     * @return Nonzero when pending; zero when not pending or the handle/index is invalid.
     */
    int osal_c_notification_pending(const osal_notification_handle* handle, size_t index);
    /**
     * @brief Reads one slot value without consuming pending state.
     * @param[in] handle  Notification handle to query.
     * @param[in] index   Slot index in the caller-managed arrays.
     * @return Current slot value, or zero when the handle/index is invalid.
     */
    uint32_t osal_c_notification_peek(const osal_notification_handle* handle, size_t index);

    /* ======================================================================== */
    /* Work Queue                                                               */
    /* ======================================================================== */

    /**
     * @brief Creates and starts a work queue.
     * @param[out] handle      Handle to initialize; must not be null or already own a queue.
     * @param[in]  stack       Optional caller-owned worker-thread stack.
     * @param[in]  stack_bytes Size of @p stack in bytes.
     * @param[in]  depth       Maximum number of pending work items; must be nonzero.
     * @param[in]  name        Optional debug name; may be null.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, support, or backend error.
     * @pre A supplied stack remains alive for the work queue's lifetime.
     */
    osal_result_t osal_c_work_queue_create(osal_work_queue_handle* handle, void* stack, size_t stack_bytes,
                                           size_t depth, const char* name);

    /**
     * @brief Creates and starts a work queue from a read-only configuration.
     * @param[out] handle  Handle to initialize; must not be null or already own a queue.
     * @param[in]  cfg     Configuration; must not be null.
     * @return @c OSAL_OK on success; otherwise a canonical validation, resource, support, or backend error.
     * @pre A supplied stack remains alive for the work queue's lifetime.
     */
    osal_result_t osal_c_work_queue_create_with_cfg(osal_work_queue_handle* handle, const osal_work_queue_config* cfg);

    /**
     * @brief Destroys an initialized work queue.
     * @param[in,out] handle  Queue handle to destroy.
     * @return @c OSAL_OK on success; otherwise a canonical initialization or backend error.
     */
    osal_result_t osal_c_work_queue_destroy(osal_work_queue_handle* handle);

    /**
     * @brief Submits a callback from thread context.
     * @param[in,out] handle  Initialized work-queue handle.
     * @param[in]     func    Non-null callback to execute on the worker thread.
     * @param[in]     arg     Opaque argument passed to @p func.
     * @return @c OSAL_OK if submitted; otherwise a canonical queue-full, initialization, or backend error.
     * @pre @p arg remains valid until the submitted callback has completed.
     * @note The callback runs asynchronously in the work-queue thread.
     */
    osal_result_t osal_c_work_queue_submit(osal_work_queue_handle* handle, osal_c_work_func_t func, void* arg);

    /**
     * @brief Submits a callback from interrupt context.
     * @param[in,out] handle  Initialized work-queue handle.
     * @param[in]     func    Non-null callback to execute on the worker thread.
     * @param[in]     arg     Opaque argument passed to @p func.
     * @return @c OSAL_OK if submitted; otherwise the backend's canonical queue-full, ISR, initialization,
     *         or support error.
     * @pre Call only from ISR context and only when the backend supports ISR work-queue submission.
     * @note The callback runs asynchronously in the work-queue thread.
     */
    osal_result_t osal_c_work_queue_submit_from_isr(osal_work_queue_handle* handle, osal_c_work_func_t func, void* arg);

    /**
     * @brief Blocks until currently queued work has completed or the timeout expires.
     * @param[in,out] handle   Initialized work-queue handle.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK when drained, @c OSAL_TIMEOUT on expiry, or a canonical initialization/backend error.
     * @note Call from thread context, not from the queue's own worker callback.
     */
    osal_result_t osal_c_work_queue_flush(osal_work_queue_handle* handle, osal_tick_t timeout);

    /**
     * @brief Discards all pending work items without executing them.
     * @param[in,out] handle  Initialized work-queue handle.
     * @return @c OSAL_OK on success; otherwise a canonical initialization/backend error.
     */
    osal_result_t osal_c_work_queue_cancel_all(osal_work_queue_handle* handle);

    /**
     * @brief Returns the number of pending work items.
     * @param[in] handle  Work-queue handle to query.
     * @return Pending item count, or zero when the handle is invalid.
     */
    size_t osal_c_work_queue_pending(const osal_work_queue_handle* handle);

    /* ======================================================================== */
    /* Delayable Work                                                           */
    /* ======================================================================== */

    /**
     * @brief Creates a timer-to-work-queue delayed callback helper.
     * @param[out] handle  Composite handle to initialize; must not be null or already valid.
     * @param[in]  queue   Initialized work queue borrowed for the helper's lifetime.
     * @param[in]  func    Non-null callback executed on @p queue.
     * @param[in]  arg     Opaque pointer passed to @p func.
     * @param[in]  name    Optional debug name for the backing timer.
     * @return @c OSAL_OK on success, @c OSAL_NOT_SUPPORTED when timer expiry cannot be
     *         safely handed to the queue, or a canonical argument/initialization/backend error.
     * @pre The work queue remains alive until the helper is destroyed.
     * @pre @p arg remains valid until the submitted callback has completed.
     * @note Callback execution occurs in the queue's worker thread; timer expiry itself may run in ISR context.
     */
    osal_result_t osal_c_delayable_work_create(osal_delayable_work_handle* handle, osal_work_queue_handle* queue,
                                               osal_c_work_func_t func, void* arg, const char* name);
    /**
     * @brief Creates a delayable-work helper from a configuration descriptor.
     * @param[out] handle  Composite handle to initialize; must not be null or already valid.
     * @param[in]  cfg     Configuration; must not be null and its queue must outlive the helper.
     * @return @c OSAL_OK on success, @c OSAL_NOT_SUPPORTED when safe timer-to-queue handoff is unavailable,
     *         or a canonical argument/initialization/backend error.
     */
    osal_result_t osal_c_delayable_work_create_with_cfg(osal_delayable_work_handle*       handle,
                                                        const osal_delayable_work_config* cfg);
    /**
     * @brief Destroys an idle delayable-work helper.
     * @param[in,out] handle  Helper handle to destroy.
     * @return @c OSAL_OK on success, @c OSAL_WOULD_BLOCK when armed, queued, running, or stopping,
     *         or a canonical argument/initialization/backend error.
     */
    osal_result_t osal_c_delayable_work_destroy(osal_delayable_work_handle* handle);
    /**
     * @brief Arms the callback to run after a delay.
     * @param[in,out] handle      Initialized helper handle.
     * @param[in]     delay_ticks Delay in ticks; @c OSAL_NO_WAIT queues the callback immediately.
     * @return @c OSAL_OK on success, @c OSAL_ALREADY_EXISTS if not idle, or a canonical timer/queue error.
     */
    osal_result_t osal_c_delayable_work_schedule(osal_delayable_work_handle* handle, osal_tick_t delay_ticks);
    /**
     * @brief Replaces an armed delay with a new delay.
     * @param[in,out] handle      Initialized helper handle.
     * @param[in]     delay_ticks New delay in ticks; @c OSAL_NO_WAIT queues the callback immediately.
     * @return @c OSAL_OK on success, @c OSAL_WOULD_BLOCK if queued/running/stopping,
     *         or a canonical timer/queue error.
     */
    osal_result_t osal_c_delayable_work_reschedule(osal_delayable_work_handle* handle, osal_tick_t delay_ticks);
    /**
     * @brief Cancels an armed delay before it reaches the work queue.
     * @param[in,out] handle  Initialized helper handle.
     * @return @c OSAL_OK when idle or successfully cancelled, @c OSAL_WOULD_BLOCK when queued, running, or stopping,
     *         or a canonical timer error.
     */
    osal_result_t osal_c_delayable_work_cancel(osal_delayable_work_handle* handle);
    /**
     * @brief Waits until the helper becomes idle.
     * @param[in] handle   Initialized helper handle.
     * @param[in] timeout  Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK when idle, @c OSAL_TIMEOUT on expiry, or a deferred timer-to-queue submission error.
     * @note Call from thread context; this helper polls using the OSAL sleep API.
     */
    osal_result_t osal_c_delayable_work_flush(osal_delayable_work_handle* handle, osal_tick_t timeout);
    /**
     * @brief Reports whether the helper is currently timer-armed.
     * @param[in] handle  Helper handle to query.
     * @return Nonzero when waiting for timer expiry; zero when invalid or not armed.
     */
    int osal_c_delayable_work_scheduled(const osal_delayable_work_handle* handle);
    /**
     * @brief Reports whether the helper has outstanding work.
     * @param[in] handle  Helper handle to query.
     * @return Nonzero when armed, queued, running, or stopping; zero when invalid or idle.
     */
    int osal_c_delayable_work_pending(const osal_delayable_work_handle* handle);
    /**
     * @brief Reports whether the callback is currently executing.
     * @param[in] handle  Helper handle to query.
     * @return Nonzero while the callback is running on the work queue; zero otherwise or when invalid.
     */
    int osal_c_delayable_work_running(const osal_delayable_work_handle* handle);

    /* ======================================================================== */
    /* Memory Pool                                                              */
    /* ======================================================================== */

    /**
     * @brief Creates a fixed-size block pool over caller-owned storage.
     * @param[out] handle      Handle to initialize.
     * @param[in]  buffer      Writable backing storage.
     * @param[in]  buf_bytes   Total storage size in bytes.
     * @param[in]  block_size  Size of each block in bytes.
     * @param[in]  block_count Number of blocks.
     * @param[in]  name        Optional debug name; may be null.
     * @return @c OSAL_OK on success; otherwise the canonical validation, support, or backend result.
     * @pre The backing buffer remains alive and writable until pool destruction.
     */
    osal_result_t osal_c_memory_pool_create(osal_memory_pool_handle* handle, void* buffer, size_t buf_bytes,
                                            size_t block_size, size_t block_count, const char* name);

    /**
     * @brief Creates a memory pool from a read-only configuration.
     * @param[out] handle  Handle to initialize.
     * @param[in]  cfg     Configuration; must not be null and its storage must outlive the pool.
     * @return @c OSAL_OK on success; otherwise the canonical validation, support, or backend result.
     */
    osal_result_t osal_c_memory_pool_create_with_cfg(osal_memory_pool_handle*       handle,
                                                     const osal_memory_pool_config* cfg);

    /**
     * @brief Destroys an initialized memory pool.
     * @param[in,out] handle  Pool handle to destroy.
     * @return @c OSAL_OK on success; otherwise a canonical initialization, outstanding-resource, or backend error.
     */
    osal_result_t osal_c_memory_pool_destroy(osal_memory_pool_handle* handle);

    /**
     * @brief Allocates one block, waiting indefinitely until one is available.
     * @param[in,out] handle  Initialized pool handle.
     * @return Pointer to the allocated block, or null when the pool is invalid, exhausted, or an error occurs.
     * @note This allocation attempt does not wait for a block to become available.
     */
    void* osal_c_memory_pool_allocate(osal_memory_pool_handle* handle);

    /**
     * @brief Allocates one block, waiting up to the specified timeout.
     * @param[in,out] handle   Initialized pool handle.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return Pointer to the allocated block, or null on timeout or another error.
     * @note Call from thread context; this operation may block.
     */
    void* osal_c_memory_pool_allocate_timed(osal_memory_pool_handle* handle, osal_tick_t timeout);

    /**
     * @brief Returns a previously allocated block to its pool.
     * @param[in,out] handle  Initialized pool handle.
     * @param[in]     block   Block pointer obtained from this pool's allocation API.
     * @return @c OSAL_OK on success; otherwise a canonical invalid-argument, initialization, or backend error.
     * @pre @p block has not already been deallocated and belongs to @p handle.
     */
    osal_result_t osal_c_memory_pool_deallocate(osal_memory_pool_handle* handle, void* block);

    /**
     * @brief Returns the number of unallocated blocks.
     * @param[in] handle  Pool handle to query.
     * @return Available block count, or zero when the handle is invalid.
     */
    size_t osal_c_memory_pool_available(const osal_memory_pool_handle* handle);

    /* ======================================================================== */
    /* Read-Write Lock                                                          */
    /* ======================================================================== */

    /**
     * @brief Creates a reader-writer lock.
     * @param[out] handle  Handle to initialize.
     * @return @c OSAL_OK on success; otherwise a canonical resource, support, or backend error.
     */
    osal_result_t osal_c_rwlock_create(osal_rwlock_handle* handle);

    /**
     * @brief Destroys an initialized reader-writer lock.
     * @param[in,out] handle  Lock handle to destroy.
     * @return @c OSAL_OK on success; otherwise a canonical initialization or backend error.
     */
    osal_result_t osal_c_rwlock_destroy(osal_rwlock_handle* handle);

    /**
     * @brief Acquires shared (read) access, waiting up to the timeout.
     * @param[in,out] handle   Initialized lock handle.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK on acquisition, @c OSAL_TIMEOUT on expiry, or a canonical support/backend error.
     */
    osal_result_t osal_c_rwlock_read_lock(osal_rwlock_handle* handle, osal_tick_t timeout);

    /**
     * @brief Releases shared (read) access held by the calling thread.
     * @param[in,out] handle  Initialized lock handle.
     * @return @c OSAL_OK on success; otherwise a canonical ownership, initialization, or backend error.
     */
    osal_result_t osal_c_rwlock_read_unlock(osal_rwlock_handle* handle);

    /**
     * @brief Acquires exclusive (write) access, waiting up to the timeout.
     * @param[in,out] handle   Initialized lock handle.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK on acquisition, @c OSAL_TIMEOUT on expiry, or a canonical support/backend error.
     */
    osal_result_t osal_c_rwlock_write_lock(osal_rwlock_handle* handle, osal_tick_t timeout);

    /**
     * @brief Releases exclusive (write) access held by the calling thread.
     * @param[in,out] handle  Initialized lock handle.
     * @return @c OSAL_OK on success; otherwise a canonical ownership, initialization, or backend error.
     */
    osal_result_t osal_c_rwlock_write_unlock(osal_rwlock_handle* handle);

    /* ======================================================================== */
    /* Stream Buffer                                                            */
    /* ======================================================================== */

    /**
     * @brief Creates a byte-oriented stream buffer over caller-owned ring storage.
     * @param[out] handle        Handle to initialize.
     * @param[in]  buffer        Writable storage of at least @p capacity + 1 bytes.
     * @param[in]  capacity      Usable byte capacity, excluding the extra ring byte.
     * @param[in]  trigger_level Minimum receive threshold; must be in the range 1 through @p capacity.
     * @return @c OSAL_OK on success; otherwise a canonical validation, support, or backend error.
     * @pre The storage remains alive and writable until buffer destruction. Use one producer and one consumer.
     */
    osal_result_t osal_c_stream_buffer_create(osal_stream_buffer_handle* handle, void* buffer, size_t capacity,
                                              size_t trigger_level);

    /**
     * @brief Creates a stream buffer from a read-only configuration descriptor.
     * @param[out] handle  Handle to initialize.
     * @param[in]  cfg     Configuration; must not be null and its buffer must outlive the stream buffer.
     * @return @c OSAL_OK on success; otherwise a canonical validation, support, or backend error.
     */
    osal_result_t osal_c_stream_buffer_create_with_cfg(osal_stream_buffer_handle*       handle,
                                                       const osal_stream_buffer_config* cfg);

    /**
     * @brief Destroys an initialized stream buffer.
     * @param[in,out] handle  Buffer handle to destroy.
     * @return @c OSAL_OK on success; otherwise a canonical initialization or backend error.
     */
    osal_result_t osal_c_stream_buffer_destroy(osal_stream_buffer_handle* handle);

    /**
     * @brief Writes bytes from thread context, waiting up to the timeout for space.
     * @param[in,out] handle   Initialized stream-buffer handle.
     * @param[in]     data     Source bytes to copy.
     * @param[in]     len      Number of bytes to write.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK on success; otherwise a canonical timeout, initialization, or backend error.
     */
    osal_result_t osal_c_stream_buffer_send(osal_stream_buffer_handle* handle, const void* data, size_t len,
                                            osal_tick_t timeout);

    /**
     * @brief Writes bytes from interrupt context without waiting.
     * @param[in,out] handle  Initialized stream-buffer handle.
     * @param[in]     data    Source bytes to copy.
     * @param[in]     len     Number of bytes to write.
     * @return @c OSAL_OK on success; otherwise the backend's canonical ISR, capacity, initialization,
     *         or support error.
     * @pre Call only from ISR context and only when the backend supports ISR stream-buffer operations.
     */
    osal_result_t osal_c_stream_buffer_send_isr(osal_stream_buffer_handle* handle, const void* data, size_t len);

    /**
     * @brief Reads bytes from thread context, waiting up to the timeout for data.
     * @param[in,out] handle   Initialized stream-buffer handle.
     * @param[out]    buf      Writable destination buffer.
     * @param[in]     max_len  Maximum number of bytes to copy.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return Number of bytes read; zero when no bytes are returned before timeout or on failure.
     */
    size_t osal_c_stream_buffer_receive(osal_stream_buffer_handle* handle, void* buf, size_t max_len,
                                        osal_tick_t timeout);

    /**
     * @brief Reads bytes from interrupt context without waiting.
     * @param[in,out] handle   Initialized stream-buffer handle.
     * @param[out]    buf      Writable destination buffer.
     * @param[in]     max_len  Maximum number of bytes to copy.
     * @return Number of bytes read; zero when no bytes are available or on failure.
     * @pre Call only from ISR context and only when the backend supports ISR stream-buffer operations.
     */
    size_t osal_c_stream_buffer_receive_isr(osal_stream_buffer_handle* handle, void* buf, size_t max_len);

    /**
     * @brief Returns the number of bytes available to read.
     * @param[in] handle  Buffer handle to query.
     * @return Readable byte count, or zero when the handle is invalid.
     */
    size_t osal_c_stream_buffer_available(const osal_stream_buffer_handle* handle);

    /**
     * @brief Returns the number of bytes that can be written without waiting.
     * @param[in] handle  Buffer handle to query.
     * @return Free byte count, or zero when the handle is invalid.
     */
    size_t osal_c_stream_buffer_free_space(const osal_stream_buffer_handle* handle);

    /**
     * @brief Resets a stream buffer to empty and discards buffered bytes.
     * @param[in,out] handle  Initialized buffer handle.
     * @return @c OSAL_OK on success; otherwise a canonical initialization or backend error.
     */
    osal_result_t osal_c_stream_buffer_reset(osal_stream_buffer_handle* handle);

    /* ======================================================================== */
    /* Message Buffer                                                           */
    /* ======================================================================== */

    /**
     * @brief Creates a length-prefixed single-producer/single-consumer message buffer.
     * @param[out] handle    Handle to initialize.
     * @param[in]  buffer    Writable ring storage of at least @p capacity + 1 bytes.
     * @param[in]  capacity  Ring capacity including message-header overhead.
     * @return @c OSAL_OK on success; otherwise a canonical validation, support, or backend error.
     * @pre The storage remains alive and writable until buffer destruction. Use one producer and one consumer.
     */
    osal_result_t osal_c_message_buffer_create(osal_message_buffer_handle* handle, void* buffer, size_t capacity);

    /**
     * @brief Creates a message buffer from a read-only configuration descriptor.
     * @param[out] handle  Handle to initialize.
     * @param[in]  cfg     Configuration; must not be null and its buffer must outlive the message buffer.
     * @return @c OSAL_OK on success; otherwise a canonical validation, support, or backend error.
     */
    osal_result_t osal_c_message_buffer_create_with_cfg(osal_message_buffer_handle*       handle,
                                                        const osal_message_buffer_config* cfg);

    /**
     * @brief Destroys an initialized message buffer.
     * @param[in,out] handle  Buffer handle to destroy.
     * @return @c OSAL_OK on success; otherwise a canonical initialization or backend error.
     */
    osal_result_t osal_c_message_buffer_destroy(osal_message_buffer_handle* handle);

    /**
     * @brief Writes one complete message from thread context.
     * @param[in,out] handle   Initialized message-buffer handle.
     * @param[in]     msg      Message payload to copy.
     * @param[in]     len      Payload length in bytes.
     * @param[in]     timeout  Maximum wait in ticks for space; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return @c OSAL_OK on success; otherwise a canonical timeout, capacity, initialization, or backend error.
     * @pre Use only with one producer and one consumer.
     */
    osal_result_t osal_c_message_buffer_send(osal_message_buffer_handle* handle, const void* msg, size_t len,
                                             osal_tick_t timeout);

    /**
     * @brief Writes one complete message from interrupt context without waiting.
     * @param[in,out] handle  Initialized message-buffer handle.
     * @param[in]     msg     Message payload to copy.
     * @param[in]     len     Payload length in bytes.
     * @return @c OSAL_OK on success; otherwise the backend's canonical ISR, capacity, initialization,
     *         or support error.
     * @pre Call only from ISR context when supported; maintain the single-producer/single-consumer contract.
     */
    osal_result_t osal_c_message_buffer_send_isr(osal_message_buffer_handle* handle, const void* msg, size_t len);

    /**
     * @brief Reads one complete message from thread context.
     * @param[in,out] handle   Initialized message-buffer handle.
     * @param[out]    buf      Writable destination buffer.
     * @param[in]     max_len  Maximum payload size to copy.
     * @param[in]     timeout  Maximum wait in ticks; @c OSAL_WAIT_FOREVER waits indefinitely.
     * @return Received payload size, or zero on timeout/error. Oversized messages are truncated to @p max_len
     *         and excess payload bytes are discarded.
     * @pre Use only with one producer and one consumer.
     */
    size_t osal_c_message_buffer_receive(osal_message_buffer_handle* handle, void* buf, size_t max_len,
                                         osal_tick_t timeout);

    /**
     * @brief Reads one complete message from interrupt context without waiting.
     * @param[in,out] handle   Initialized message-buffer handle.
     * @param[out]    buf      Writable destination buffer.
     * @param[in]     max_len  Maximum payload size to copy.
     * @return Received payload size, or zero when no complete message fits or on failure.
     * @pre Call only from ISR context when supported; maintain the single-producer/single-consumer contract.
     */
    size_t osal_c_message_buffer_receive_isr(osal_message_buffer_handle* handle, void* buf, size_t max_len);

    /**
     * @brief Returns the payload size of the next queued message.
     * @param[in] handle  Buffer handle to query.
     * @return Next payload size, or zero when empty or invalid; framing bytes are excluded.
     */
    size_t osal_c_message_buffer_available(const osal_message_buffer_handle* handle);

    /**
     * @brief Returns the maximum additional payload that currently fits.
     * @param[in] handle  Buffer handle to query.
     * @return Maximum payload length that currently fits after accounting for framing, or zero when invalid.
     */
    size_t osal_c_message_buffer_free_space(const osal_message_buffer_handle* handle);

    /**
     * @brief Resets a message buffer to empty and discards all messages.
     * @param[in,out] handle  Initialized buffer handle.
     * @return @c OSAL_OK on success; otherwise a canonical initialization or backend error.
     */
    osal_result_t osal_c_message_buffer_reset(osal_message_buffer_handle* handle);

#ifdef __cplusplus
} /* extern "C" */
#endif

#undef OSAL_C_MUTABLE

#endif /* OSAL_C_H */
