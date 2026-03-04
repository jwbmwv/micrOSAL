/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file osal_c.h
 * @brief Pure-C interface for MicrOSAL
 * @details Allows C code to use all major OSAL primitives — mutex, semaphore,
 *          queue, thread, timer, event flags, clock, condvar, work queue,
 *          memory pool, and read-write lock — without requiring a C++ compiler.
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
#ifndef OSAL_C_H
#define OSAL_C_H

#include <stddef.h>
#include <stdint.h>

#if ((defined(OSAL_CFG_TICK_TYPE_U16) ? 1 : 0) + (defined(OSAL_CFG_TICK_TYPE_U32) ? 1 : 0) + \
     (defined(OSAL_CFG_TICK_TYPE_U64) ? 1 : 0)) > 1
#error "OSAL: only one of OSAL_CFG_TICK_TYPE_U16/U32/U64 may be defined"
#endif

#ifdef __cplusplus
extern "C"
{
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

    /** @brief Thread entry function. */
    typedef void (*osal_c_thread_entry_t)(void* arg);

    /** @brief Timer callback function. */
    typedef void (*osal_c_timer_callback_t)(void* arg);

    /** @brief Work queue item callback function. */
    typedef void (*osal_c_work_func_t)(void* arg);

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
     * All handles are layout-compatible with osal::active_traits::*_handle_t
     * (which is `struct { void* native; }`).
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

    /** @brief C TLS key handle. */
    typedef struct
    {
        uint8_t key;   /**< Internal key index. */
        uint8_t valid; /**< Non-zero when the key is active. */
    } osal_tls_key_handle;

    /* ======================================================================== */
    /* Clock                                                                    */
    /* ======================================================================== */

    /** @brief Returns monotonic time in milliseconds since an arbitrary epoch. */
    int64_t osal_c_clock_monotonic_ms(void);

    /** @brief Returns system (wall-clock) time in milliseconds since Unix epoch. */
    int64_t osal_c_clock_system_ms(void);

    /** @brief Returns the RTOS tick count. */
    osal_tick_t osal_c_clock_ticks(void);

    /** @brief Returns the duration of one RTOS tick in microseconds. */
    uint32_t osal_c_clock_tick_period_us(void);

    /* ======================================================================== */
    /* Mutex                                                                    */
    /* ======================================================================== */

    osal_result_t osal_c_mutex_create(osal_mutex_handle* handle, int recursive);
    osal_result_t osal_c_mutex_create_with_cfg(osal_mutex_handle* handle, const osal_mutex_config* cfg);
    osal_result_t osal_c_mutex_destroy(osal_mutex_handle* handle);
    osal_result_t osal_c_mutex_lock(osal_mutex_handle* handle, osal_tick_t timeout);
    osal_result_t osal_c_mutex_try_lock(osal_mutex_handle* handle);
    osal_result_t osal_c_mutex_unlock(osal_mutex_handle* handle);

    /* ======================================================================== */
    /* Semaphore                                                                */
    /* ======================================================================== */

    osal_result_t osal_c_semaphore_create(osal_semaphore_handle* handle, unsigned int initial_count,
                                          unsigned int max_count);
    osal_result_t osal_c_semaphore_create_with_cfg(osal_semaphore_handle* handle, const osal_semaphore_config* cfg);
    osal_result_t osal_c_semaphore_destroy(osal_semaphore_handle* handle);
    osal_result_t osal_c_semaphore_give(osal_semaphore_handle* handle);
    osal_result_t osal_c_semaphore_give_isr(osal_semaphore_handle* handle);
    osal_result_t osal_c_semaphore_take(osal_semaphore_handle* handle, osal_tick_t timeout);
    osal_result_t osal_c_semaphore_try_take(osal_semaphore_handle* handle);

    /* ======================================================================== */
    /* Queue                                                                    */
    /* ======================================================================== */

    /**
     * @brief Create a message queue.
     * @param handle     Output handle.
     * @param buffer     Caller-supplied buffer (must be >= item_size * capacity).
     * @param item_size  Size of each item in bytes.
     * @param capacity   Maximum number of items.
     */
    osal_result_t osal_c_queue_create(osal_queue_handle* handle, void* buffer, size_t item_size, size_t capacity);
    osal_result_t osal_c_queue_create_with_cfg(osal_queue_handle* handle, const osal_queue_config* cfg);
    osal_result_t osal_c_queue_destroy(osal_queue_handle* handle);
    osal_result_t osal_c_queue_send(osal_queue_handle* handle, const void* item, osal_tick_t timeout);
    osal_result_t osal_c_queue_send_isr(osal_queue_handle* handle, const void* item);
    osal_result_t osal_c_queue_receive(osal_queue_handle* handle, void* item, osal_tick_t timeout);
    osal_result_t osal_c_queue_receive_isr(osal_queue_handle* handle, void* item);
    osal_result_t osal_c_queue_peek(osal_queue_handle* handle, void* item, osal_tick_t timeout);
    size_t        osal_c_queue_count(const osal_queue_handle* handle);
    size_t        osal_c_queue_free(const osal_queue_handle* handle);

    /* ======================================================================== */
    /* Thread                                                                   */
    /* ======================================================================== */

    /**
     * @brief Create and start a thread.
     * @param handle       Output handle.
     * @param entry        Thread entry function.
     * @param arg          Argument passed to entry.
     * @param priority     Thread priority (0–255).
     * @param affinity     CPU affinity mask (0 = any).
     * @param stack        Caller-supplied stack buffer.
     * @param stack_bytes  Size of stack buffer in bytes.
     * @param name         Debug name (may be NULL).
     */
    osal_result_t osal_c_thread_create(osal_thread_handle* handle, osal_c_thread_entry_t entry, void* arg,
                                       osal_priority_t priority, osal_affinity_t affinity, void* stack,
                                       size_t stack_bytes, const char* name);
    osal_result_t osal_c_thread_create_with_cfg(osal_thread_handle* handle, const osal_thread_config* cfg);
    osal_result_t osal_c_thread_join(osal_thread_handle* handle, osal_tick_t timeout);
    osal_result_t osal_c_thread_detach(osal_thread_handle* handle);
    osal_result_t osal_c_thread_set_priority(osal_thread_handle* handle, osal_priority_t priority);
    osal_result_t osal_c_thread_set_affinity(osal_thread_handle* handle, osal_affinity_t affinity);
    osal_result_t osal_c_thread_suspend(osal_thread_handle* handle);
    osal_result_t osal_c_thread_resume(osal_thread_handle* handle);
    void          osal_c_thread_yield(void);
    void          osal_c_thread_sleep_ms(uint32_t ms);

    /* ======================================================================== */
    /* Thread-Local Data                                                        */
    /* ======================================================================== */

    /** @brief Allocate a thread-local key. */
    osal_result_t osal_c_tls_key_create(osal_tls_key_handle* handle);
    /** @brief Release a thread-local key. */
    osal_result_t osal_c_tls_key_destroy(osal_tls_key_handle* handle);
    /**
     * @brief Set the calling thread's value for @p handle.
     * @return @c OSAL_OK on success, @c OSAL_NOT_INITIALIZED if key is invalid.
     */
    osal_result_t osal_c_tls_set(osal_tls_key_handle* handle, void* value);
    /**
     * @brief Get the calling thread's value for @p handle (or NULL).
     * @return Stored pointer for this thread, or @c NULL when unset/invalid.
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
     * @brief Create a timer (does not start it).
     * @param handle       Output handle.
     * @param name         Debug name (may be NULL).
     * @param callback     Function called on expiry.
     * @param arg          Argument forwarded to callback.
     * @param period_ticks Timer period in RTOS ticks.
     * @param auto_reload  Non-zero for periodic, zero for one-shot.
     */
    osal_result_t osal_c_timer_create(osal_timer_handle* handle, const char* name, osal_c_timer_callback_t callback,
                                      void* arg, osal_tick_t period_ticks, int auto_reload);
    osal_result_t osal_c_timer_create_with_cfg(osal_timer_handle* handle, const osal_timer_config* cfg);
    osal_result_t osal_c_timer_destroy(osal_timer_handle* handle);
    osal_result_t osal_c_timer_start(osal_timer_handle* handle);
    osal_result_t osal_c_timer_stop(osal_timer_handle* handle);
    osal_result_t osal_c_timer_reset(osal_timer_handle* handle);
    osal_result_t osal_c_timer_set_period(osal_timer_handle* handle, osal_tick_t new_period_ticks);
    int           osal_c_timer_is_active(const osal_timer_handle* handle);

    /* ======================================================================== */
    /* Event Flags                                                              */
    /* ======================================================================== */

    osal_result_t     osal_c_event_flags_create(osal_event_flags_handle* handle);
    osal_result_t     osal_c_event_flags_destroy(osal_event_flags_handle* handle);
    osal_result_t     osal_c_event_flags_set(osal_event_flags_handle* handle, osal_event_bits_t bits);
    osal_result_t     osal_c_event_flags_clear(osal_event_flags_handle* handle, osal_event_bits_t bits);
    osal_event_bits_t osal_c_event_flags_get(const osal_event_flags_handle* handle);
    osal_result_t     osal_c_event_flags_wait_any(osal_event_flags_handle* handle, osal_event_bits_t wait_bits,
                                                  osal_event_bits_t* actual_bits, int clear_on_exit, osal_tick_t timeout);
    osal_result_t     osal_c_event_flags_wait_all(osal_event_flags_handle* handle, osal_event_bits_t wait_bits,
                                                  osal_event_bits_t* actual_bits, int clear_on_exit, osal_tick_t timeout);
    osal_result_t     osal_c_event_flags_set_isr(osal_event_flags_handle* handle, osal_event_bits_t bits);

    /* ======================================================================== */
    /* Condition Variable                                                       */
    /* ======================================================================== */

    osal_result_t osal_c_condvar_create(osal_condvar_handle* handle);
    osal_result_t osal_c_condvar_destroy(osal_condvar_handle* handle);
    /**
     * @brief Atomically unlock mutex and block until signalled, then re-lock.
     * @param handle   Condvar handle.
     * @param mutex    Mutex handle — must be locked by the calling thread.
     * @param timeout  Timeout in ticks.
     */
    osal_result_t osal_c_condvar_wait(osal_condvar_handle* handle, osal_mutex_handle* mutex, osal_tick_t timeout);
    osal_result_t osal_c_condvar_notify_one(osal_condvar_handle* handle);
    osal_result_t osal_c_condvar_notify_all(osal_condvar_handle* handle);

    /* ======================================================================== */
    /* Work Queue                                                               */
    /* ======================================================================== */

    /**
     * @brief Create and start a work queue.
     * @param handle       Output handle.
     * @param stack        Caller-supplied stack for the worker thread.
     * @param stack_bytes  Size of stack in bytes.
     * @param depth        Maximum number of pending work items.
     * @param name         Debug name (may be NULL).
     */
    osal_result_t osal_c_work_queue_create(osal_work_queue_handle* handle, void* stack, size_t stack_bytes,
                                           size_t depth, const char* name);
    osal_result_t osal_c_work_queue_create_with_cfg(osal_work_queue_handle* handle, const osal_work_queue_config* cfg);
    osal_result_t osal_c_work_queue_destroy(osal_work_queue_handle* handle);
    osal_result_t osal_c_work_queue_submit(osal_work_queue_handle* handle, osal_c_work_func_t func, void* arg);
    osal_result_t osal_c_work_queue_submit_from_isr(osal_work_queue_handle* handle, osal_c_work_func_t func, void* arg);
    osal_result_t osal_c_work_queue_flush(osal_work_queue_handle* handle, osal_tick_t timeout);
    osal_result_t osal_c_work_queue_cancel_all(osal_work_queue_handle* handle);
    size_t        osal_c_work_queue_pending(const osal_work_queue_handle* handle);

    /* ======================================================================== */
    /* Memory Pool                                                              */
    /* ======================================================================== */

    /**
     * @brief Create a fixed-size block pool.
     * @param handle       Output handle.
     * @param buffer       Caller-supplied backing storage.
     * @param buf_bytes    Size of buffer in bytes.
     * @param block_size   Size of each block in bytes.
     * @param block_count  Number of blocks.
     * @param name         Debug name (may be NULL).
     */
    osal_result_t osal_c_memory_pool_create(osal_memory_pool_handle* handle, void* buffer, size_t buf_bytes,
                                            size_t block_size, size_t block_count, const char* name);
    osal_result_t osal_c_memory_pool_create_with_cfg(osal_memory_pool_handle*       handle,
                                                     const osal_memory_pool_config* cfg);
    osal_result_t osal_c_memory_pool_destroy(osal_memory_pool_handle* handle);
    void*         osal_c_memory_pool_allocate(osal_memory_pool_handle* handle);
    void*         osal_c_memory_pool_allocate_timed(osal_memory_pool_handle* handle, osal_tick_t timeout);
    osal_result_t osal_c_memory_pool_deallocate(osal_memory_pool_handle* handle, void* block);
    size_t        osal_c_memory_pool_available(const osal_memory_pool_handle* handle);

    /* ======================================================================== */
    /* Read-Write Lock                                                          */
    /* ======================================================================== */

    osal_result_t osal_c_rwlock_create(osal_rwlock_handle* handle);
    osal_result_t osal_c_rwlock_destroy(osal_rwlock_handle* handle);
    osal_result_t osal_c_rwlock_read_lock(osal_rwlock_handle* handle, osal_tick_t timeout);
    osal_result_t osal_c_rwlock_read_unlock(osal_rwlock_handle* handle);
    osal_result_t osal_c_rwlock_write_lock(osal_rwlock_handle* handle, osal_tick_t timeout);
    osal_result_t osal_c_rwlock_write_unlock(osal_rwlock_handle* handle);

    /* ======================================================================== */
    /* Stream Buffer                                                            */
    /* ======================================================================== */

    /**
     * @brief Create a byte-oriented stream buffer.
     * @param handle        Output handle.
     * @param buffer        Caller-supplied ring storage — must be @p capacity+1 bytes.
     * @param capacity      Usable byte capacity (N).
     * @param trigger_level Minimum bytes available before receive unblocks (1..N).
     */
    osal_result_t osal_c_stream_buffer_create(osal_stream_buffer_handle* handle, void* buffer, size_t capacity,
                                              size_t trigger_level);
    osal_result_t osal_c_stream_buffer_create_with_cfg(osal_stream_buffer_handle*       handle,
                                                       const osal_stream_buffer_config* cfg);
    osal_result_t osal_c_stream_buffer_destroy(osal_stream_buffer_handle* handle);
    osal_result_t osal_c_stream_buffer_send(osal_stream_buffer_handle* handle, const void* data, size_t len,
                                            osal_tick_t timeout);
    osal_result_t osal_c_stream_buffer_send_isr(osal_stream_buffer_handle* handle, const void* data, size_t len);
    size_t        osal_c_stream_buffer_receive(osal_stream_buffer_handle* handle, void* buf, size_t max_len,
                                               osal_tick_t timeout);
    size_t        osal_c_stream_buffer_receive_isr(osal_stream_buffer_handle* handle, void* buf, size_t max_len);
    size_t        osal_c_stream_buffer_available(const osal_stream_buffer_handle* handle);
    size_t        osal_c_stream_buffer_free_space(const osal_stream_buffer_handle* handle);
    osal_result_t osal_c_stream_buffer_reset(osal_stream_buffer_handle* handle);

    /* ======================================================================== */
    /* Message Buffer                                                           */
    /* ======================================================================== */

    /**
     * @brief Create a message-oriented buffer (length-prefixed, SPSC).
     * @param handle    Output handle.
     * @param buffer    Caller-supplied ring storage — must be @p capacity+1 bytes.
     * @param capacity  Total ring byte capacity including @c OSAL_MSG_HEADER_BYTES
     *                  overhead per message.  Maximum single-message payload is
     *                  @p capacity - @c OSAL_MSG_HEADER_BYTES bytes.
     */
    osal_result_t osal_c_message_buffer_create(osal_message_buffer_handle* handle, void* buffer, size_t capacity);
    osal_result_t osal_c_message_buffer_create_with_cfg(osal_message_buffer_handle*       handle,
                                                        const osal_message_buffer_config* cfg);
    osal_result_t osal_c_message_buffer_destroy(osal_message_buffer_handle* handle);
    osal_result_t osal_c_message_buffer_send(osal_message_buffer_handle* handle, const void* msg, size_t len,
                                             osal_tick_t timeout);
    osal_result_t osal_c_message_buffer_send_isr(osal_message_buffer_handle* handle, const void* msg, size_t len);
    size_t        osal_c_message_buffer_receive(osal_message_buffer_handle* handle, void* buf, size_t max_len,
                                                osal_tick_t timeout);
    size_t        osal_c_message_buffer_receive_isr(osal_message_buffer_handle* handle, void* buf, size_t max_len);
    /** @brief Payload size of the next queued message (0 if empty). */
    size_t        osal_c_message_buffer_available(const osal_message_buffer_handle* handle);
    size_t        osal_c_message_buffer_free_space(const osal_message_buffer_handle* handle);
    osal_result_t osal_c_message_buffer_reset(osal_message_buffer_handle* handle);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OSAL_C_H */
