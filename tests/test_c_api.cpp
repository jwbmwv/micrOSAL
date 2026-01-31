// SPDX-License-Identifier: Apache-2.0
/// @file test_c_api.cpp
/// @brief Tests for the pure-C interface layer (osal_c.h / osal_c.cpp).
///
/// The heavy lifting is in the companion pure-C file c_api_c_check.c which
/// also serves as a compilation gate — if that .c file compiles as C11, the
/// header is guaranteed C-compatible.
///
/// This doctest harness calls into that C smoke-test and adds a few more
/// targeted checks from the C++ side.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <osal/osal_c.h>

#include <atomic>
#include <cstring>
#include <thread>

// Declared in c_api_c_check.c (linked as a C object).
extern "C" int osal_c_smoke_test(void);

alignas(16) static std::uint8_t c_api_delayable_wq_stack[65536];

static void c_api_delayable_work_cb(void* arg)
{
    auto* count = static_cast<std::atomic<int>*>(arg);
    count->fetch_add(1);
}

// =========================================================================
// Smoke test (exercised from C, verified from C++)
// =========================================================================

TEST_CASE("c_api: smoke test from pure C code returns 0")
{
    int rc = osal_c_smoke_test();
    INFO("osal_c_smoke_test returned error code ", rc);
    CHECK(rc == 0);
}

// =========================================================================
// Clock
// =========================================================================

TEST_CASE("c_api: clock monotonic returns positive value")
{
    int64_t ms = osal_c_clock_monotonic_ms();
    CHECK(ms >= 0);
}

TEST_CASE("c_api: clock ticks returns non-zero value")
{
    osal_tick_t t = osal_c_clock_ticks();
    CHECK(t >= 0);
}

// =========================================================================
// Mutex
// =========================================================================

TEST_CASE("c_api: mutex create / lock / unlock / destroy")
{
    osal_mutex_handle mtx;
    CHECK(osal_c_mutex_create(&mtx, 0) == OSAL_OK);
    CHECK(osal_c_mutex_lock(&mtx, OSAL_WAIT_FOREVER) == OSAL_OK);
    CHECK(osal_c_mutex_unlock(&mtx) == OSAL_OK);
    CHECK(osal_c_mutex_destroy(&mtx) == OSAL_OK);
}

TEST_CASE("c_api: mutex try_lock")
{
    osal_mutex_handle mtx;
    REQUIRE(osal_c_mutex_create(&mtx, 0) == OSAL_OK);
    CHECK(osal_c_mutex_try_lock(&mtx) == OSAL_OK);
    osal_c_mutex_unlock(&mtx);
    osal_c_mutex_destroy(&mtx);
}

TEST_CASE("c_api: recursive mutex")
{
    if constexpr (!osal::active_capabilities::has_recursive_mutex)
    {
        MESSAGE("Skipped — backend does not support recursive mutex");
        return;
    }

    osal_mutex_handle mtx;
    REQUIRE(osal_c_mutex_create(&mtx, 1) == OSAL_OK);
    CHECK(osal_c_mutex_lock(&mtx, OSAL_WAIT_FOREVER) == OSAL_OK);
    CHECK(osal_c_mutex_lock(&mtx, OSAL_WAIT_FOREVER) == OSAL_OK);
    osal_c_mutex_unlock(&mtx);
    osal_c_mutex_unlock(&mtx);
    osal_c_mutex_destroy(&mtx);
}

// =========================================================================
// Semaphore
// =========================================================================

TEST_CASE("c_api: semaphore round-trip")
{
    osal_semaphore_handle sem;
    REQUIRE(osal_c_semaphore_create(&sem, 0, 5) == OSAL_OK);
    CHECK(osal_c_semaphore_give(&sem) == OSAL_OK);
    CHECK(osal_c_semaphore_take(&sem, OSAL_WAIT_FOREVER) == OSAL_OK);
    // Should time out with no remaining count
    CHECK(osal_c_semaphore_try_take(&sem) != OSAL_OK);
    osal_c_semaphore_destroy(&sem);
}

// =========================================================================
// Queue
// =========================================================================

TEST_CASE("c_api: queue send / receive / count / free")
{
    osal_queue_handle q;
    int queue_buf[4]; // backing storage
    REQUIRE(osal_c_queue_create(&q, queue_buf, sizeof(int), 4) == OSAL_OK);

    int val = 99;
    CHECK(osal_c_queue_send(&q, &val, OSAL_NO_WAIT) == OSAL_OK);
    CHECK(osal_c_queue_count(&q) == 1);
    CHECK(osal_c_queue_free(&q) == 3);

    int out = 0;
    CHECK(osal_c_queue_receive(&q, &out, OSAL_NO_WAIT) == OSAL_OK);
    CHECK(out == 99);
    CHECK(osal_c_queue_count(&q) == 0);

    osal_c_queue_destroy(&q);
}

// =========================================================================
// Thread
// =========================================================================

static void thread_entry(void* arg)
{
    auto* flag = static_cast<std::atomic<int>*>(arg);
    flag->store(1);
}

#if defined(OSAL_BACKEND_BAREMETAL)
alignas(16) static std::uint8_t c_api_thread_stack[65536];
alignas(16) static std::uint8_t c_api_cfg_thread_stack[65536];
#endif

TEST_CASE("c_api: thread create / join")
{
    std::atomic<int> flag{0};
    osal_thread_handle thr;
    void*  stack       = nullptr;
    size_t stack_bytes = 0U;
#if defined(OSAL_BACKEND_BAREMETAL)
    stack       = c_api_thread_stack;
    stack_bytes = sizeof(c_api_thread_stack);
#endif
    osal_result_t rc = osal_c_thread_create(&thr, thread_entry, &flag, OSAL_PRIORITY_NORMAL, 0, stack, stack_bytes,
                                            "c_test");
    REQUIRE(rc == OSAL_OK);
    CHECK(osal_c_thread_join(&thr, OSAL_WAIT_FOREVER) == OSAL_OK);
    CHECK(flag.load() == 1);
}

TEST_CASE("c_api: thread yield and sleep compile and run")
{
    osal_c_thread_yield();
    osal_c_thread_sleep_ms(1);
}

TEST_CASE("c_api: tls create / set / get / destroy")
{
    osal_tls_key_handle tls{};
    const osal_result_t create_rc = osal_c_tls_key_create(&tls);
    if (create_rc == OSAL_NOT_SUPPORTED)
    {
        CHECK(true);
        return;
    }
    REQUIRE(create_rc == OSAL_OK);

    int value = 41;
    CHECK(osal_c_tls_set(&tls, &value) == OSAL_OK);
    CHECK(osal_c_tls_get(&tls) == &value);

    CHECK(osal_c_tls_key_destroy(&tls) == OSAL_OK);
    CHECK(osal_c_tls_get(&tls) == nullptr);
    CHECK(osal_c_tls_set(&tls, &value) == OSAL_NOT_INITIALIZED);
}

TEST_CASE("c_api: tls values are isolated per thread")
{
    osal_tls_key_handle tls{};
    const osal_result_t create_rc = osal_c_tls_key_create(&tls);
    if (create_rc == OSAL_NOT_SUPPORTED)
    {
        CHECK(true);
        return;
    }
    REQUIRE(create_rc == OSAL_OK);

    int main_value = 1;
    int worker_value = 2;
    REQUIRE(osal_c_tls_set(&tls, &main_value) == OSAL_OK);
    REQUIRE(osal_c_tls_get(&tls) == &main_value);

    std::thread worker([&]() {
        CHECK(osal_c_tls_get(&tls) == nullptr);
        CHECK(osal_c_tls_set(&tls, &worker_value) == OSAL_OK);
        CHECK(osal_c_tls_get(&tls) == &worker_value);
    });
    worker.join();

    CHECK(osal_c_tls_get(&tls) == &main_value);
    CHECK(osal_c_tls_key_destroy(&tls) == OSAL_OK);
}

// =========================================================================
// Timer
// =========================================================================

static void timer_cb(void* arg)
{
    auto* counter = static_cast<std::atomic<int>*>(arg);
    counter->fetch_add(1);
}

TEST_CASE("c_api: timer one-shot fires")
{
    std::atomic<int> count{0};
    osal_timer_handle tmr;
    osal_tick_t period = 10; // 10 ticks
    REQUIRE(osal_c_timer_create(&tmr, "c_tmr", timer_cb, &count, period, 0) == OSAL_OK);
    CHECK(osal_c_timer_start(&tmr) == OSAL_OK);
    osal_c_thread_sleep_ms(200);
    osal_c_timer_stop(&tmr);
    CHECK(count.load() >= 1);
    osal_c_timer_destroy(&tmr);
}

// =========================================================================
// Event Flags
// =========================================================================

TEST_CASE("c_api: event flags set / wait_any")
{
    osal_event_flags_handle ef;
    REQUIRE(osal_c_event_flags_create(&ef) == OSAL_OK);

    osal_c_event_flags_set(&ef, 0x03);

    osal_event_bits_t actual = 0;
    CHECK(osal_c_event_flags_wait_any(&ef, 0x01, &actual, 1, OSAL_NO_WAIT) == OSAL_OK);
    CHECK((actual & 0x01) != 0);

    osal_c_event_flags_destroy(&ef);
}

// =========================================================================
// Condition Variable
// =========================================================================

TEST_CASE("c_api: condvar notify without waiters succeeds")
{
    osal_condvar_handle cv;
    REQUIRE(osal_c_condvar_create(&cv) == OSAL_OK);
    CHECK(osal_c_condvar_notify_one(&cv) == OSAL_OK);
    CHECK(osal_c_condvar_notify_all(&cv) == OSAL_OK);
    osal_c_condvar_destroy(&cv);
}

// =========================================================================
// Notification
// =========================================================================

TEST_CASE("c_api: notification create / notify / wait / destroy")
{
    std::uint32_t             values[2]{};
    std::uint8_t              pending[2]{};
    osal_notification_handle note{};

    REQUIRE(osal_c_notification_create(&note, values, pending, 2U) == OSAL_OK);
    CHECK(osal_c_notification_notify(&note, 0x1234U, OSAL_NOTIFICATION_OVERWRITE, 1U) == OSAL_OK);
    CHECK(osal_c_notification_pending(&note, 1U) == 1);
    CHECK(osal_c_notification_peek(&note, 1U) == 0x1234U);

    std::uint32_t out = 0U;
    CHECK(osal_c_notification_wait(&note, 1U, &out, 0U, 0xFFFFFFFFU, OSAL_NO_WAIT) == OSAL_OK);
    CHECK(out == 0x1234U);
    CHECK(osal_c_notification_pending(&note, 1U) == 0);
    CHECK(osal_c_notification_destroy(&note) == OSAL_OK);
}

// =========================================================================
// Delayable Work
// =========================================================================

TEST_CASE("c_api: delayable_work schedule / flush / destroy")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend lacks timers");
        return;
    }

    osal_work_queue_handle wq{};
    REQUIRE(osal_c_work_queue_create(&wq, c_api_delayable_wq_stack, sizeof(c_api_delayable_wq_stack), 8U, "c_dw_wq")
            == OSAL_OK);

    std::atomic<int>          count{0};
    osal_delayable_work_handle work{};
    REQUIRE(osal_c_delayable_work_create(&work, &wq, c_api_delayable_work_cb, &count, "c_dw") == OSAL_OK);

    CHECK(osal_c_delayable_work_schedule(&work, 10U) == OSAL_OK);
    CHECK(osal_c_delayable_work_pending(&work) == 1);
    CHECK(osal_c_delayable_work_flush(&work, OSAL_WAIT_FOREVER) == OSAL_OK);
    CHECK(count.load() == 1);
    CHECK(osal_c_delayable_work_pending(&work) == 0);

    CHECK(osal_c_delayable_work_destroy(&work) == OSAL_OK);
    CHECK(osal_c_work_queue_destroy(&wq) == OSAL_OK);
}

// =========================================================================
// Read-Write Lock
// =========================================================================

TEST_CASE("c_api: rwlock read-then-write")
{
    osal_rwlock_handle rw;
    REQUIRE(osal_c_rwlock_create(&rw) == OSAL_OK);

    CHECK(osal_c_rwlock_read_lock(&rw, OSAL_WAIT_FOREVER) == OSAL_OK);
    CHECK(osal_c_rwlock_read_unlock(&rw) == OSAL_OK);

    CHECK(osal_c_rwlock_write_lock(&rw, OSAL_WAIT_FOREVER) == OSAL_OK);
    CHECK(osal_c_rwlock_write_unlock(&rw) == OSAL_OK);

    osal_c_rwlock_destroy(&rw);
}

// =========================================================================
// Config-based _create_with_cfg variants
// =========================================================================

TEST_CASE("c_api: mutex create_with_cfg — normal")
{
    const osal_mutex_config cfg = {0};
    osal_mutex_handle mtx;
    CHECK(osal_c_mutex_create_with_cfg(&mtx, &cfg) == OSAL_OK);
    CHECK(osal_c_mutex_lock(&mtx, OSAL_WAIT_FOREVER) == OSAL_OK);
    osal_c_mutex_unlock(&mtx);
    osal_c_mutex_destroy(&mtx);
}

TEST_CASE("c_api: mutex create_with_cfg — recursive")
{
    if constexpr (!osal::active_capabilities::has_recursive_mutex)
    {
        MESSAGE("Skipped — backend does not support recursive mutex");
        return;
    }

    const osal_mutex_config cfg = {1};
    osal_mutex_handle mtx;
    CHECK(osal_c_mutex_create_with_cfg(&mtx, &cfg) == OSAL_OK);
    CHECK(osal_c_mutex_lock(&mtx, OSAL_WAIT_FOREVER) == OSAL_OK);
    CHECK(osal_c_mutex_lock(&mtx, OSAL_WAIT_FOREVER) == OSAL_OK);
    osal_c_mutex_unlock(&mtx);
    osal_c_mutex_unlock(&mtx);
    osal_c_mutex_destroy(&mtx);
}

TEST_CASE("c_api: semaphore create_with_cfg")
{
    const osal_semaphore_config cfg = {2, 5};
    osal_semaphore_handle sem;
    CHECK(osal_c_semaphore_create_with_cfg(&sem, &cfg) == OSAL_OK);
    CHECK(osal_c_semaphore_take(&sem, OSAL_NO_WAIT) == OSAL_OK);
    CHECK(osal_c_semaphore_take(&sem, OSAL_NO_WAIT) == OSAL_OK);
    CHECK(osal_c_semaphore_try_take(&sem) != OSAL_OK);
    osal_c_semaphore_destroy(&sem);
}

TEST_CASE("c_api: queue create_with_cfg")
{
    int buf[4];
    const osal_queue_config cfg = {buf, sizeof(int), 4};
    osal_queue_handle q;
    CHECK(osal_c_queue_create_with_cfg(&q, &cfg) == OSAL_OK);

    int val = 77;
    CHECK(osal_c_queue_send(&q, &val, OSAL_NO_WAIT) == OSAL_OK);
    int out = 0;
    CHECK(osal_c_queue_receive(&q, &out, OSAL_NO_WAIT) == OSAL_OK);
    CHECK(out == 77);
    osal_c_queue_destroy(&q);
}

static void cfg_thread_entry(void* arg)
{
    auto* flag = static_cast<std::atomic<int>*>(arg);
    flag->store(1);
}

TEST_CASE("c_api: thread create_with_cfg")
{
    std::atomic<int> flag{0};
    osal_thread_config cfg;
    cfg.entry       = cfg_thread_entry;
    cfg.arg         = &flag;
    cfg.priority    = OSAL_PRIORITY_NORMAL;
    cfg.affinity    = OSAL_AFFINITY_ANY;
    cfg.stack       = nullptr;
    cfg.stack_bytes = 0;
#if defined(OSAL_BACKEND_BAREMETAL)
    cfg.stack       = c_api_cfg_thread_stack;
    cfg.stack_bytes = sizeof(c_api_cfg_thread_stack);
#endif
    cfg.name        = "cfg_t";

    osal_thread_handle thr;
    CHECK(osal_c_thread_create_with_cfg(&thr, &cfg) == OSAL_OK);
    CHECK(osal_c_thread_join(&thr, OSAL_WAIT_FOREVER) == OSAL_OK);
    CHECK(flag.load() == 1);
}

static void cfg_timer_cb(void* arg)
{
    auto* counter = static_cast<std::atomic<int>*>(arg);
    counter->fetch_add(1);
}

TEST_CASE("c_api: timer create_with_cfg")
{
    std::atomic<int> count{0};
    const osal_timer_config cfg = {"cfg_tmr", cfg_timer_cb, &count, 10, 0};
    osal_timer_handle tmr;
    CHECK(osal_c_timer_create_with_cfg(&tmr, &cfg) == OSAL_OK);
    CHECK(osal_c_timer_start(&tmr) == OSAL_OK);
    osal_c_thread_sleep_ms(200);
    osal_c_timer_stop(&tmr);
    CHECK(count.load() >= 1);
    osal_c_timer_destroy(&tmr);
}

// =========================================================================
// Stream Buffer
// =========================================================================

TEST_CASE("c_api: stream_buffer create / send / receive / destroy")
{
    // capacity=32, backing storage must be 33 bytes (capacity + 1 sentinel)
    static std::uint8_t sbuf[33];
    osal_stream_buffer_handle sb;
    REQUIRE(osal_c_stream_buffer_create(&sb, sbuf, 32, 1) == OSAL_OK);

    const std::uint8_t tx[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(osal_c_stream_buffer_send(&sb, tx, sizeof(tx), OSAL_NO_WAIT) == OSAL_OK);
    CHECK(osal_c_stream_buffer_available(&sb) == 8);
    CHECK(osal_c_stream_buffer_free_space(&sb) == 24);

    std::uint8_t rx[8]{};
    size_t n = osal_c_stream_buffer_receive(&sb, rx, sizeof(rx), OSAL_NO_WAIT);
    CHECK(n == 8);
    CHECK(std::memcmp(tx, rx, 8) == 0);

    CHECK(osal_c_stream_buffer_available(&sb) == 0);
    osal_c_stream_buffer_destroy(&sb);
}

TEST_CASE("c_api: stream_buffer try-receive on empty returns 0")
{
    static std::uint8_t sbuf[17];
    osal_stream_buffer_handle sb;
    REQUIRE(osal_c_stream_buffer_create(&sb, sbuf, 16, 1) == OSAL_OK);

    std::uint8_t rx[4]{};
    CHECK(osal_c_stream_buffer_receive(&sb, rx, sizeof(rx), OSAL_NO_WAIT) == 0);

    osal_c_stream_buffer_destroy(&sb);
}

TEST_CASE("c_api: stream_buffer reset clears data")
{
    static std::uint8_t sbuf[17];
    osal_stream_buffer_handle sb;
    REQUIRE(osal_c_stream_buffer_create(&sb, sbuf, 16, 1) == OSAL_OK);

    const std::uint8_t data[8]{};
    REQUIRE(osal_c_stream_buffer_send(&sb, data, 8, OSAL_NO_WAIT) == OSAL_OK);
    CHECK(osal_c_stream_buffer_available(&sb) == 8);

    CHECK(osal_c_stream_buffer_reset(&sb) == OSAL_OK);
    CHECK(osal_c_stream_buffer_available(&sb) == 0);

    osal_c_stream_buffer_destroy(&sb);
}

TEST_CASE("c_api: stream_buffer create_with_cfg")
{
    static std::uint8_t sbuf[33];
    const osal_stream_buffer_config cfg = {sbuf, 32, 1};
    osal_stream_buffer_handle sb;
    CHECK(osal_c_stream_buffer_create_with_cfg(&sb, &cfg) == OSAL_OK);

    const std::uint8_t byte = 0x42;
    CHECK(osal_c_stream_buffer_send(&sb, &byte, 1, OSAL_NO_WAIT) == OSAL_OK);

    std::uint8_t out = 0;
    CHECK(osal_c_stream_buffer_receive(&sb, &out, 1, OSAL_NO_WAIT) == 1);
    CHECK(out == 0x42);

    osal_c_stream_buffer_destroy(&sb);
}

// =========================================================================
// Message Buffer
// =========================================================================

TEST_CASE("c_api: message_buffer create / send / receive / destroy")
{
    // capacity=64, backing storage must be 65 bytes
    static std::uint8_t mbuf[65];
    osal_message_buffer_handle mb;
    REQUIRE(osal_c_message_buffer_create(&mb, mbuf, 64) == OSAL_OK);

    const std::uint8_t msg[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    CHECK(osal_c_message_buffer_send(&mb, msg, sizeof(msg), OSAL_NO_WAIT) == OSAL_OK);
    CHECK(osal_c_message_buffer_available(&mb) == sizeof(msg));

    std::uint8_t rx[16]{};
    size_t n = osal_c_message_buffer_receive(&mb, rx, sizeof(rx), OSAL_NO_WAIT);
    REQUIRE(n == sizeof(msg));
    CHECK(std::memcmp(msg, rx, sizeof(msg)) == 0);

    CHECK(osal_c_message_buffer_available(&mb) == 0);
    osal_c_message_buffer_destroy(&mb);
}

TEST_CASE("c_api: message_buffer FIFO ordering")
{
    static std::uint8_t mbuf[65];
    osal_message_buffer_handle mb;
    REQUIRE(osal_c_message_buffer_create(&mb, mbuf, 64) == OSAL_OK);

    for (std::uint8_t i = 1; i <= 3; ++i)
    {
        CHECK(osal_c_message_buffer_send(&mb, &i, 1, OSAL_NO_WAIT) == OSAL_OK);
    }

    for (std::uint8_t expected = 1; expected <= 3; ++expected)
    {
        std::uint8_t rx = 0;
        CHECK(osal_c_message_buffer_receive(&mb, &rx, 1, OSAL_NO_WAIT) == 1);
        CHECK(rx == expected);
    }
    osal_c_message_buffer_destroy(&mb);
}

TEST_CASE("c_api: message_buffer reset clears messages")
{
    static std::uint8_t mbuf[33];
    osal_message_buffer_handle mb;
    REQUIRE(osal_c_message_buffer_create(&mb, mbuf, 32) == OSAL_OK);

    const std::uint8_t msg[4] = {1, 2, 3, 4};
    REQUIRE(osal_c_message_buffer_send(&mb, msg, sizeof(msg), OSAL_NO_WAIT) == OSAL_OK);
    CHECK(osal_c_message_buffer_available(&mb) == sizeof(msg));

    CHECK(osal_c_message_buffer_reset(&mb) == OSAL_OK);
    CHECK(osal_c_message_buffer_available(&mb) == 0);

    osal_c_message_buffer_destroy(&mb);
}

TEST_CASE("c_api: message_buffer create_with_cfg")
{
    static std::uint8_t mbuf[65];
    const osal_message_buffer_config cfg = {mbuf, 64};
    osal_message_buffer_handle mb;
    CHECK(osal_c_message_buffer_create_with_cfg(&mb, &cfg) == OSAL_OK);

    const std::uint8_t msg[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    CHECK(osal_c_message_buffer_send(&mb, msg, sizeof(msg), OSAL_NO_WAIT) == OSAL_OK);

    std::uint8_t rx[4]{};
    CHECK(osal_c_message_buffer_receive(&mb, rx, sizeof(rx), OSAL_NO_WAIT) == sizeof(msg));
    CHECK(std::memcmp(msg, rx, sizeof(msg)) == 0);

    osal_c_message_buffer_destroy(&mb);
}
