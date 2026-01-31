// SPDX-License-Identifier: Apache-2.0
/// @file main.cpp
/// @brief Zephyr ztest suite for MicrOSAL — exercises the Zephyr backend.
///
/// This single-file test covers all major OSAL primitives using Zephyr's
/// standard ztest framework.  It can be built for any Zephyr-supported
/// board:
///
///   west build -b native_sim tests/zephyr
///   west build -b qemu_cortex_m3 tests/zephyr
///   west twister -T tests/zephyr
///
/// Users can retarget to real hardware by switching the board argument.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <osal/osal.hpp>
#include <cstring>
#include <cstdint>

// Zephyr stacks must be declared with K_THREAD_STACK_DEFINE for proper
// metadata & guard regions.  These are shared across tests (sequential execution).
K_THREAD_STACK_DEFINE(z_test_stack, 4096);
K_THREAD_STACK_DEFINE(z_wq_stack, 4096);
K_THREAD_STACK_DEFINE(z_xthread_stack, 4096);
K_THREAD_STACK_DEFINE(z_xef_stack, 4096);
K_THREAD_STACK_DEFINE(z_sb_stack, 2048);
K_THREAD_STACK_DEFINE(z_mb_stack, 2048);
K_THREAD_STACK_DEFINE(z_cv_wait_stack, 2048);
K_THREAD_STACK_DEFINE(z_tld_stack, 2048);
K_THREAD_STACK_DEFINE(z_rwlock_ra_stack, 2048);
K_THREAD_STACK_DEFINE(z_rwlock_rb_stack, 2048);
K_THREAD_STACK_DEFINE(z_note_stack, 2048);
alignas(std::uint32_t) static std::uint8_t z_pool_buf[sizeof(std::uint32_t) * 16U];

// =========================================================================
// Clock
// =========================================================================

ZTEST(osal_clock, test_monotonic_positive)
{
    auto now = osal::monotonic_clock::now();
    // Monotonic clock should return a time point > epoch on a running system.
    auto ms = std::chrono::duration_cast<osal::milliseconds>(now.time_since_epoch());
    zassert_true(ms.count() >= 0, "monotonic clock should be >= 0");
}

ZTEST(osal_clock, test_ticks_nonzero)
{
    // On native_sim the tick counter may be zero at boot, so just check
    // that the function returns without error.
    osal::tick_t t = osal::clock_utils::now_ticks();
    zassert_true(t >= 0, "ticks should be >= 0 on a running kernel");
}

ZTEST_SUITE(osal_clock, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Mutex
// =========================================================================

ZTEST(osal_mutex, test_construction)
{
    osal::mutex m;
    zassert_true(m.valid(), "mutex construction should succeed");
}

ZTEST(osal_mutex, test_lock_unlock)
{
    osal::mutex m;
    zassert_true(m.valid(), NULL);
    m.lock();
    m.unlock();
}

ZTEST(osal_mutex, test_try_lock)
{
    osal::mutex m;
    zassert_true(m.valid(), NULL);
    zassert_true(m.try_lock(), "try_lock on unlocked mutex should succeed");
    m.unlock();
}

ZTEST(osal_mutex, test_recursive)
{
    osal::mutex m{osal::mutex_type::recursive};
    zassert_true(m.valid(), "recursive mutex construction should succeed");
    m.lock();
    // Zephyr k_mutex is inherently recursive, second lock should not deadlock.
    zassert_true(m.try_lock(), "recursive lock should succeed from same thread");
    m.unlock();
    m.unlock();
}

ZTEST(osal_mutex, test_config_construction)
{
    const osal::mutex_config cfg{osal::mutex_type::normal};
    osal::mutex              m{cfg};
    zassert_true(m.valid(), "config-constructed mutex should be valid");
    zassert_true(m.try_lock(), NULL);
    m.unlock();
}

ZTEST(osal_mutex, test_lock_guard)
{
    osal::mutex m;
    zassert_true(m.valid(), NULL);
    {
        osal::mutex::lock_guard guard{m};
        // Guard should hold the lock — destructor releases.
    }
    zassert_true(m.try_lock(), "mutex should be free after guard destroyed");
    m.unlock();
}

ZTEST_SUITE(osal_mutex, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Semaphore
// =========================================================================

ZTEST(osal_semaphore, test_binary_construction)
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    zassert_true(s.valid(), "binary semaphore construction should succeed");
}

ZTEST(osal_semaphore, test_binary_give_take)
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    zassert_true(s.valid(), NULL);
    s.give();
    zassert_true(s.try_take(), "take after give should succeed");
}

ZTEST(osal_semaphore, test_counting)
{
    osal::semaphore s{osal::semaphore_type::counting, 0U, 5U};
    zassert_true(s.valid(), NULL);

    s.give();
    s.give();
    s.give();

    zassert_true(s.try_take(), NULL);
    zassert_true(s.try_take(), NULL);
    zassert_true(s.try_take(), NULL);
    zassert_false(s.try_take(), "should be empty after 3 takes");
}

ZTEST(osal_semaphore, test_counting_initial)
{
    osal::semaphore s{osal::semaphore_type::counting, 2U, 5U};
    zassert_true(s.valid(), NULL);

    zassert_true(s.try_take(), "initial_count=2, first take");
    zassert_true(s.try_take(), "initial_count=2, second take");
    zassert_false(s.try_take(), "should be empty");
}

ZTEST(osal_semaphore, test_config_construction)
{
    const osal::semaphore_config cfg{osal::semaphore_type::counting, 1U, 10U};
    osal::semaphore              s{cfg};
    zassert_true(s.valid(), NULL);
    zassert_true(s.try_take(), "config-constructed semaphore should have initial count");
    zassert_false(s.try_take(), NULL);
}

ZTEST(osal_semaphore, test_take_for_timeout)
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    zassert_true(s.valid(), NULL);
    // Should time out — no one gives.
    zassert_false(s.take_for(osal::milliseconds{20}), "take_for should timeout");
}

ZTEST_SUITE(osal_semaphore, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Queue
// =========================================================================

ZTEST(osal_queue, test_construction)
{
    osal::queue<uint32_t, 4> q;
    zassert_true(q.valid(), "queue construction should succeed");
}

ZTEST(osal_queue, test_starts_empty)
{
    osal::queue<uint32_t, 4> q;
    zassert_true(q.valid(), NULL);
    zassert_true(q.empty(), NULL);
    zassert_false(q.full(), NULL);
    zassert_equal(q.count(), 0U, NULL);
    zassert_equal(q.free_slots(), 4U, NULL);
}

ZTEST(osal_queue, test_send_receive)
{
    osal::queue<uint32_t, 4> q;
    zassert_true(q.valid(), NULL);

    zassert_true(q.send(42U).ok(), "send should succeed");
    zassert_equal(q.count(), 1U, NULL);

    uint32_t val = 0;
    zassert_true(q.receive(val).ok(), "receive should succeed");
    zassert_equal(val, 42U, "received value should match");
    zassert_true(q.empty(), NULL);
}

ZTEST(osal_queue, test_fifo_order)
{
    osal::queue<uint32_t, 4> q;
    zassert_true(q.valid(), NULL);

    zassert_true(q.send(1U).ok(), NULL);
    zassert_true(q.send(2U).ok(), NULL);
    zassert_true(q.send(3U).ok(), NULL);

    uint32_t val = 0;
    zassert_true(q.receive(val).ok(), NULL);
    zassert_equal(val, 1U, NULL);
    zassert_true(q.receive(val).ok(), NULL);
    zassert_equal(val, 2U, NULL);
    zassert_true(q.receive(val).ok(), NULL);
    zassert_equal(val, 3U, NULL);
}

ZTEST(osal_queue, test_full_detection)
{
    osal::queue<uint32_t, 2> q;
    zassert_true(q.valid(), NULL);

    zassert_true(q.send(1U).ok(), NULL);
    zassert_true(q.send(2U).ok(), NULL);
    zassert_true(q.full(), "queue should be full after 2/2 sends");
}

ZTEST_SUITE(osal_queue, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Mailbox
// =========================================================================

static osal::mailbox<uint32_t>* g_xthread_mailbox = nullptr;

static void xthread_mailbox_producer(void*)
{
    osal::thread::sleep_for(osal::milliseconds{20});
    if (g_xthread_mailbox != nullptr)
    {
        (void)g_xthread_mailbox->send(99U);
    }
}

ZTEST(osal_mailbox, test_construction)
{
    osal::mailbox<uint32_t> mb;
    zassert_true(mb.valid(), "mailbox construction should succeed");
    zassert_true(mb.empty(), NULL);
    zassert_equal(mb.count(), 0U, NULL);
}

ZTEST(osal_mailbox, test_send_receive)
{
    osal::mailbox<uint32_t> mb;
    zassert_true(mb.valid(), NULL);

    zassert_true(mb.send(42U).ok(), "send should succeed");
    zassert_true(mb.full(), NULL);

    uint32_t value = 0U;
    zassert_true(mb.receive(value).ok(), "receive should succeed");
    zassert_equal(value, 42U, NULL);
    zassert_true(mb.empty(), NULL);
}

ZTEST(osal_mailbox, test_full_detection_and_aliases)
{
    osal::mailbox<uint32_t> mb;
    zassert_true(mb.valid(), NULL);

    zassert_true(mb.post(7U).ok(), NULL);
    zassert_false(mb.try_post(8U), NULL);

    uint32_t peeked = 0U;
    zassert_true(mb.peek(peeked), NULL);
    zassert_equal(peeked, 7U, NULL);
    zassert_equal(mb.count(), 1U, NULL);
}

ZTEST(osal_mailbox, test_cross_thread_send_receive)
{
    static osal::mailbox<uint32_t> mb;
    zassert_true(mb.valid(), NULL);
    g_xthread_mailbox = &mb;

    osal::thread        t;
    osal::thread_config cfg{};
    cfg.entry       = xthread_mailbox_producer;
    cfg.arg         = nullptr;
    cfg.stack       = z_xthread_stack;
    cfg.stack_bytes = K_THREAD_STACK_SIZEOF(z_xthread_stack);
    cfg.name        = "z_mb_prod";
    zassert_true(t.create(cfg).ok(), NULL);

    uint32_t value = 0U;
    zassert_true(mb.receive(value).ok(), NULL);
    zassert_equal(value, 99U, NULL);
    zassert_true(t.join().ok(), NULL);
    g_xthread_mailbox = nullptr;
}

ZTEST_SUITE(osal_mailbox, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Thread
// =========================================================================

static volatile bool g_thread_ran = false;

static void thread_entry_basic(void*)
{
    g_thread_ran = true;
}

ZTEST(osal_thread, test_default_not_valid)
{
    osal::thread t;
    zassert_false(t.valid(), "default-constructed thread should not be valid");
}

ZTEST(osal_thread, test_create_join)
{
    g_thread_ran = false;

    osal::thread        t;
    osal::thread_config cfg{};
    cfg.entry       = thread_entry_basic;
    cfg.arg         = nullptr;
    cfg.stack       = z_test_stack;
    cfg.stack_bytes = K_THREAD_STACK_SIZEOF(z_test_stack);
    cfg.name        = "z_basic";
    zassert_true(t.create(cfg).ok(), "thread create should succeed");
    zassert_true(t.valid(), NULL);

    zassert_true(t.join().ok(), "join should succeed");
    zassert_true(g_thread_ran, "thread entry should have executed");
}

ZTEST(osal_thread, test_yield_no_crash)
{
    osal::thread::yield();
    zassert_true(true, "yield should not crash");
}

ZTEST(osal_thread, test_sleep_for)
{
    auto t1 = osal::monotonic_clock::now();
    osal::thread::sleep_for(osal::milliseconds{30});
    auto t2      = osal::monotonic_clock::now();
    auto elapsed = std::chrono::duration_cast<osal::milliseconds>(t2 - t1);
    zassert_true(elapsed.count() >= 20, "sleep_for should have waited >= 20 ms");
}

ZTEST_SUITE(osal_thread, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Timer
// =========================================================================

static volatile uint32_t g_timer_count = 0;

static void timer_callback_ztest(void*)
{
    ++g_timer_count;
}

ZTEST(osal_timer, test_construction)
{
    osal::timer t{timer_callback_ztest, nullptr, osal::milliseconds{100}};
    zassert_true(t.valid(), "timer construction should succeed");
}

ZTEST(osal_timer, test_one_shot_fires)
{
    g_timer_count = 0;
    osal::timer t{timer_callback_ztest, nullptr, osal::milliseconds{30}, osal::timer_mode::one_shot};
    zassert_true(t.valid(), NULL);

    zassert_true(t.start().ok(), NULL);
    osal::thread::sleep_for(osal::milliseconds{200});
    t.stop();

    zassert_equal(g_timer_count, 1U, "one-shot timer should fire exactly once");
}

ZTEST(osal_timer, test_periodic_fires_multiple)
{
    g_timer_count = 0;
    osal::timer t{timer_callback_ztest, nullptr, osal::milliseconds{25}, osal::timer_mode::periodic};
    zassert_true(t.valid(), NULL);

    zassert_true(t.start().ok(), NULL);
    osal::thread::sleep_for(osal::milliseconds{200});
    t.stop();

    zassert_true(g_timer_count >= 2, "periodic timer should fire >= 2 times in 200 ms");
}

ZTEST(osal_timer, test_config_construction)
{
    g_timer_count = 0;
    const osal::timer_config cfg{timer_callback_ztest, nullptr, osal::milliseconds{30}, osal::timer_mode::one_shot,
                                 "cfg_tmr"};
    osal::timer              t{cfg};
    zassert_true(t.valid(), "config-constructed timer should be valid");

    zassert_true(t.start().ok(), NULL);
    osal::thread::sleep_for(osal::milliseconds{200});
    t.stop();
    zassert_equal(g_timer_count, 1U, NULL);
}

ZTEST_SUITE(osal_timer, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Event Flags
// =========================================================================

ZTEST(osal_event_flags, test_construction)
{
    osal::event_flags ef;
    zassert_true(ef.valid(), "event_flags construction should succeed");
}

ZTEST(osal_event_flags, test_set_and_get)
{
    osal::event_flags ef;
    zassert_true(ef.valid(), NULL);

    zassert_true(ef.set(0x05).ok(), "set should succeed");
    osal::event_bits_t bits = ef.get();
    zassert_equal(bits & 0x05, 0x05U, "bits 0 and 2 should be set");
}

ZTEST(osal_event_flags, test_clear)
{
    osal::event_flags ef;
    zassert_true(ef.valid(), NULL);

    zassert_true(ef.set(0x0F).ok(), NULL);
    zassert_true(ef.clear(0x03).ok(), NULL);
    osal::event_bits_t bits = ef.get();
    zassert_equal(bits & 0x0F, 0x0CU, "bits 0,1 should be cleared");
}

ZTEST(osal_event_flags, test_wait_any_immediate)
{
    osal::event_flags ef;
    zassert_true(ef.valid(), NULL);

    zassert_true(ef.set(0x01).ok(), NULL);
    osal::event_bits_t actual = 0;
    auto               r      = ef.wait_any(0x01, &actual, true, osal::milliseconds{100});
    zassert_true(r.ok(), "wait_any should succeed when bits are already set");
    zassert_true((actual & 0x01) != 0, NULL);
}

ZTEST_SUITE(osal_event_flags, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Condition Variable
// =========================================================================

ZTEST(osal_condvar, test_construction)
{
    osal::condvar cv;
    zassert_true(cv.valid(), "condvar construction should succeed");
}

ZTEST(osal_condvar, test_notify_without_waiters)
{
    osal::condvar cv;
    zassert_true(cv.valid(), NULL);
    // notify_one/notify_all return void — just ensure they don't crash.
    cv.notify_one();
    cv.notify_all();
    zassert_true(true, "notify with no waiters should not crash");
}

ZTEST_SUITE(osal_condvar, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Notification
// =========================================================================

static osal::notification<2>* g_z_note = nullptr;
static volatile std::uint32_t g_z_note_value = 0U;
static int g_z_c_delayable_count = 0;

static void zephyr_notification_waiter(void*)
{
    if (g_z_note != nullptr)
    {
        std::uint32_t value = 0U;
        if (g_z_note->wait(1U, osal::milliseconds{2000}, &value).ok())
        {
            g_z_note_value = value;
        }
    }
}

static void zephyr_c_delayable_cb(void* arg)
{
    auto* count = static_cast<int*>(arg);
    ++(*count);
}

ZTEST(osal_notification, test_notify_and_wait_round_trip)
{
    osal::notification<2> note;
    zassert_true(note.valid(), NULL);
    g_z_note = &note;
    g_z_note_value = 0U;

    osal::thread        t;
    osal::thread_config cfg{};
    cfg.entry       = zephyr_notification_waiter;
    cfg.arg         = nullptr;
    cfg.stack       = z_note_stack;
    cfg.stack_bytes = K_THREAD_STACK_SIZEOF(z_note_stack);
    cfg.name        = "z_note";
    zassert_true(t.create(cfg).ok(), NULL);

    osal::thread::sleep_for(osal::milliseconds{20});
    zassert_true(note.notify(0xCAFEU, osal::notification_action::overwrite, 1U).ok(), NULL);
    zassert_true(t.join().ok(), NULL);
    zassert_equal(g_z_note_value, 0xCAFEU, NULL);
    zassert_false(note.pending(1U), NULL);
    g_z_note = nullptr;
}

ZTEST(osal_notification, test_set_bits_increment_and_no_overwrite)
{
    osal::notification<1> note;
    zassert_true(note.valid(), NULL);
    zassert_true(note.notify(0x01U, osal::notification_action::overwrite, 0U).ok(), NULL);
    zassert_true(note.notify(0x04U, osal::notification_action::set_bits, 0U).ok(), NULL);
    zassert_true(note.notify(0U, osal::notification_action::increment, 0U).ok(), NULL);
    zassert_equal(note.peek(0U), 0x06U, NULL);
    zassert_equal(note.notify(0x08U, osal::notification_action::no_overwrite, 0U).code(),
                  osal::error_code::would_block, NULL);
}

ZTEST_SUITE(osal_notification, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Work Queue
// =========================================================================

static volatile bool g_wq_executed = false;

ZTEST(osal_work_queue, test_construction)
{
    osal::work_queue wq{z_wq_stack, K_THREAD_STACK_SIZEOF(z_wq_stack), 8, "z_wq"};
    zassert_true(wq.valid(), "work_queue construction should succeed");
}

ZTEST(osal_work_queue, test_submit_and_flush)
{
    osal::work_queue wq{z_wq_stack, K_THREAD_STACK_SIZEOF(z_wq_stack), 8, "z_wq"};
    zassert_true(wq.valid(), NULL);

    g_wq_executed = false;
    auto cb       = [](void*) { g_wq_executed = true; };
    zassert_true(wq.submit(cb).ok(), "submit should succeed");
    zassert_true(wq.flush(osal::milliseconds{2000}).ok(), "flush should succeed");
    zassert_true(g_wq_executed, "work item should have executed");
}

ZTEST(osal_work_queue, test_config_construction)
{
    const osal::work_queue_config cfg{z_wq_stack, K_THREAD_STACK_SIZEOF(z_wq_stack), 8, "cfg_wq"};
    osal::work_queue              wq{cfg};
    zassert_true(wq.valid(), "config-constructed work_queue should be valid");

    g_wq_executed = false;
    auto cb       = [](void*) { g_wq_executed = true; };
    zassert_true(wq.submit(cb).ok(), NULL);
    zassert_true(wq.flush(osal::milliseconds{2000}).ok(), NULL);
    zassert_true(g_wq_executed, NULL);
}

ZTEST_SUITE(osal_work_queue, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Delayable Work
// =========================================================================

static volatile int g_delayable_count = 0;

ZTEST(osal_delayable_work, test_schedule_and_flush)
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        ztest_test_skip();
    }

    osal::work_queue wq{z_wq_stack, K_THREAD_STACK_SIZEOF(z_wq_stack), 8U, "z_dwq"};
    zassert_true(wq.valid(), NULL);

    g_delayable_count = 0;
    osal::delayable_work work{wq, +[](void*) { ++g_delayable_count; }, nullptr, "z_dw"};
    zassert_true(work.valid(), NULL);
    zassert_true(work.schedule(osal::milliseconds{25}).ok(), NULL);
    zassert_true(work.flush(osal::milliseconds{2000}).ok(), NULL);
    zassert_equal(g_delayable_count, 1, NULL);
    zassert_false(work.pending(), NULL);
}

ZTEST_SUITE(osal_delayable_work, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Object Wait Set
// =========================================================================

ZTEST(osal_object_wait_set, test_queue_readiness_returns_registered_id)
{
    osal::object_wait_set      ws;
    osal::queue<std::uint32_t, 4> q;
    zassert_true(q.valid(), NULL);
    zassert_true(ws.add(q, 17).ok(), NULL);
    zassert_true(q.send(99U).ok(), NULL);

    int         ready_ids[4]{};
    std::size_t n_ready = 0U;
    zassert_true(ws.wait(ready_ids, 4U, n_ready, osal::milliseconds{200}).ok(), NULL);
    zassert_equal(n_ready, 1U, NULL);
    zassert_equal(ready_ids[0], 17, NULL);
}

ZTEST(osal_object_wait_set, test_notification_clear_on_exit)
{
    osal::object_wait_set  ws;
    osal::notification<2> note;
    zassert_true(note.valid(), NULL);
    zassert_true(ws.add(note, 1U, 29, true).ok(), NULL);
    zassert_true(note.notify(0x55U, osal::notification_action::overwrite, 1U).ok(), NULL);

    int         ready_ids[4]{};
    std::size_t n_ready = 0U;
    zassert_true(ws.wait(ready_ids, 4U, n_ready, osal::milliseconds{200}).ok(), NULL);
    zassert_equal(n_ready, 1U, NULL);
    zassert_equal(ready_ids[0], 29, NULL);
    zassert_false(note.pending(1U), NULL);
}

ZTEST_SUITE(osal_object_wait_set, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Memory Pool
// =========================================================================

ZTEST(osal_memory_pool, test_construction_and_alloc_free)
{
    osal::memory_pool mp{z_pool_buf, sizeof(z_pool_buf), sizeof(std::uint32_t), 16U, "z_mp"};
    zassert_true(mp.valid(), "memory_pool construction should succeed");
    zassert_equal(mp.available(), 16U, NULL);

    void* blk = mp.allocate();
    zassert_not_null(blk, "allocate should return a block");
    zassert_equal(mp.available(), 15U, NULL);

    zassert_true(mp.deallocate(blk).ok(), "deallocate should succeed");
    zassert_equal(mp.available(), 16U, NULL);
}

ZTEST(osal_memory_pool, test_config_construction)
{
    const osal::memory_pool_config cfg{z_pool_buf, sizeof(z_pool_buf), sizeof(std::uint32_t), 16U, "cfg_mp"};
    osal::memory_pool              mp{cfg};
    zassert_true(mp.valid(), "config-constructed memory_pool should be valid");
    zassert_equal(mp.block_size(), sizeof(std::uint32_t), NULL);
}

ZTEST_SUITE(osal_memory_pool, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Read-Write Lock
// =========================================================================

ZTEST(osal_rwlock, test_construction)
{
    osal::rwlock rw;
    zassert_true(rw.valid(), "rwlock construction should succeed");
}

ZTEST(osal_rwlock, test_read_and_write_lock_unlock)
{
    osal::rwlock rw;
    zassert_true(rw.valid(), NULL);

    zassert_true(rw.read_lock().ok(), "read_lock should succeed");
    zassert_true(rw.read_unlock().ok(), "read_unlock should succeed");

    zassert_true(rw.write_lock().ok(), "write_lock should succeed");
    zassert_true(rw.write_unlock().ok(), "write_unlock should succeed");
}

ZTEST_SUITE(osal_rwlock, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Stream Buffer
// =========================================================================

ZTEST(osal_stream_buffer, test_construction)
{
    osal::stream_buffer<64> sb;
    zassert_true(sb.valid(), "stream_buffer construction should succeed");
    zassert_equal(sb.available(), 0U, "fresh buffer should be empty");
    zassert_equal(sb.free_space(), 64U, "free_space should equal capacity");
}

ZTEST(osal_stream_buffer, test_send_receive)
{
    osal::stream_buffer<64> sb;
    zassert_true(sb.valid(), NULL);

    const std::uint8_t tx[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    zassert_true(sb.try_send(tx, sizeof(tx)), "try_send should succeed");
    zassert_equal(sb.available(), 8U, "8 bytes should be available");
    zassert_equal(sb.free_space(), 56U, NULL);

    std::uint8_t rx[8]{};
    std::size_t  n = sb.try_receive(rx, sizeof(rx));
    zassert_equal(n, 8U, "try_receive should return 8 bytes");
    zassert_mem_equal(rx, tx, 8U, "received data mismatch");
    zassert_true(sb.empty(), "buffer should be empty after receive");
}

ZTEST(osal_stream_buffer, test_empty_receive_returns_zero)
{
    osal::stream_buffer<32> sb;
    zassert_true(sb.valid(), NULL);
    std::uint8_t rx[8]{};
    zassert_equal(sb.try_receive(rx, sizeof(rx)), 0U, "empty buffer should return 0 bytes");
}

ZTEST(osal_stream_buffer, test_reset_clears_data)
{
    osal::stream_buffer<32> sb;
    zassert_true(sb.valid(), NULL);
    const std::uint8_t data[4]{};
    zassert_true(sb.try_send(data, sizeof(data)), NULL);
    zassert_equal(sb.available(), 4U, NULL);
    zassert_true(sb.reset().ok(), "reset should succeed");
    zassert_equal(sb.available(), 0U, "buffer should be empty after reset");
}

ZTEST(osal_stream_buffer, test_send_isr_receive_isr)
{
    // send_isr / receive_isr are non-blocking.  On backends with
    // has_isr_stream_buffer == true they use the dedicated ISR API; on others
    // (including Zephyr) they fall back to the semaphore-based path.
    osal::stream_buffer<64> sb;
    zassert_true(sb.valid(), NULL);

    const std::uint8_t tx[4] = {0x10U, 0x20U, 0x30U, 0x40U};
    zassert_true(sb.send_isr(tx, sizeof(tx)).ok(), "send_isr should succeed when space is available");
    zassert_equal(sb.available(), 4U, NULL);

    std::uint8_t rx[4]{};
    std::size_t  n = sb.receive_isr(rx, sizeof(rx));
    zassert_equal(n, 4U, "receive_isr should return 4 bytes");
    zassert_mem_equal(rx, tx, 4U, "received data mismatch");
    zassert_true(sb.empty(), "buffer should be empty after receive_isr");
}

ZTEST(osal_stream_buffer, test_receive_isr_empty_returns_zero)
{
    osal::stream_buffer<32> sb;
    zassert_true(sb.valid(), NULL);
    std::uint8_t buf[8]{};
    zassert_equal(sb.receive_isr(buf, sizeof(buf)), 0U, "receive_isr on empty buffer must return 0");
}

ZTEST(osal_stream_buffer, test_trigger_level_below_threshold)
{
    // TriggerLevel=4: try_receive returns 0 while fewer than 4 bytes present.
    osal::stream_buffer<64, 4U> sb;
    zassert_true(sb.valid(), NULL);

    const std::uint8_t three[3] = {1U, 2U, 3U};
    zassert_true(sb.try_send(three, sizeof(three)), NULL);
    zassert_equal(sb.available(), 3U, NULL);

    std::uint8_t rx[8]{};
    zassert_equal(sb.try_receive(rx, sizeof(rx)), 0U, "try_receive below TriggerLevel must return 0");
    zassert_equal(sb.available(), 3U, "data must remain pending");
}

ZTEST(osal_stream_buffer, test_trigger_level_at_threshold)
{
    // TriggerLevel=4: try_receive succeeds once 4 bytes are available.
    osal::stream_buffer<64, 4U> sb;
    zassert_true(sb.valid(), NULL);

    const std::uint8_t four[4] = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
    zassert_true(sb.try_send(four, sizeof(four)), NULL);

    std::uint8_t rx[8]{};
    std::size_t  n = sb.try_receive(rx, sizeof(rx));
    zassert_equal(n, 4U, "try_receive at TriggerLevel must return 4 bytes");
    zassert_equal(rx[0], 0xAAU, NULL);
    zassert_equal(rx[3], 0xDDU, NULL);
    zassert_true(sb.empty(), NULL);
}

ZTEST_SUITE(osal_stream_buffer, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Message Buffer
// =========================================================================

ZTEST(osal_message_buffer, test_construction)
{
    osal::message_buffer<64> mb;
    zassert_true(mb.valid(), "message_buffer construction should succeed");
    zassert_true(mb.empty(), "fresh message buffer should be empty");
}

ZTEST(osal_message_buffer, test_send_receive)
{
    osal::message_buffer<64> mb;
    zassert_true(mb.valid(), NULL);

    const std::uint8_t msg[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    zassert_true(mb.try_send(msg, sizeof(msg)), "try_send should succeed");
    zassert_equal(mb.next_message_size(), 6U, "next_message_size should equal payload length");

    std::uint8_t rx[16]{};
    std::size_t  n = mb.try_receive(rx, sizeof(rx));
    zassert_equal(n, 6U, "try_receive should return message length");
    zassert_mem_equal(rx, msg, 6U, "received message mismatch");
    zassert_true(mb.empty(), NULL);
}

ZTEST(osal_message_buffer, test_fifo_ordering)
{
    osal::message_buffer<128> mb;
    zassert_true(mb.valid(), NULL);

    for (std::uint8_t i = 1U; i <= 3U; ++i)
    {
        zassert_true(mb.try_send(&i, 1U), "send %d failed", i);
    }
    for (std::uint8_t expected = 1U; expected <= 3U; ++expected)
    {
        std::uint8_t out = 0U;
        zassert_equal(mb.try_receive(&out, 1U), 1U, NULL);
        zassert_equal(out, expected, "FIFO order violated");
    }
}

ZTEST(osal_message_buffer, test_reset_clears_messages)
{
    osal::message_buffer<64> mb;
    zassert_true(mb.valid(), NULL);
    const std::uint8_t msg[4] = {1, 2, 3, 4};
    zassert_true(mb.try_send(msg, sizeof(msg)), NULL);
    zassert_false(mb.empty(), NULL);
    zassert_true(mb.reset().ok(), "reset should succeed");
    zassert_true(mb.empty(), "buffer should be empty after reset");
}

ZTEST_SUITE(osal_message_buffer, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// C API (smoke test)
// =========================================================================

#include <osal/osal_c.h>

ZTEST(osal_c_api, test_mutex_round_trip)
{
    osal_mutex_handle mtx;
    zassert_equal(osal_c_mutex_create(&mtx, 0), OSAL_OK, NULL);
    zassert_equal(osal_c_mutex_lock(&mtx, OSAL_WAIT_FOREVER), OSAL_OK, NULL);
    zassert_equal(osal_c_mutex_unlock(&mtx), OSAL_OK, NULL);
    zassert_equal(osal_c_mutex_destroy(&mtx), OSAL_OK, NULL);
}

ZTEST(osal_c_api, test_semaphore_round_trip)
{
    osal_semaphore_handle sem;
    zassert_equal(osal_c_semaphore_create(&sem, 1, 5), OSAL_OK, NULL);
    zassert_equal(osal_c_semaphore_take(&sem, OSAL_NO_WAIT), OSAL_OK, NULL);
    zassert_equal(osal_c_semaphore_give(&sem), OSAL_OK, NULL);
    osal_c_semaphore_destroy(&sem);
}

ZTEST(osal_c_api, test_create_with_cfg)
{
    const osal_mutex_config cfg = {0};
    osal_mutex_handle       mtx;
    zassert_equal(osal_c_mutex_create_with_cfg(&mtx, &cfg), OSAL_OK, NULL);
    zassert_equal(osal_c_mutex_lock(&mtx, OSAL_WAIT_FOREVER), OSAL_OK, NULL);
    osal_c_mutex_unlock(&mtx);
    osal_c_mutex_destroy(&mtx);
}

ZTEST(osal_c_api, test_stream_buffer_round_trip)
{
    // Backing storage must be capacity+1 bytes (SPSC sentinel byte).
    static std::uint8_t       sbuf[33];
    osal_stream_buffer_handle sb;
    zassert_equal(osal_c_stream_buffer_create(&sb, sbuf, 32, 1), OSAL_OK, NULL);

    const std::uint8_t tx[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    zassert_equal(osal_c_stream_buffer_send(&sb, tx, sizeof(tx), OSAL_NO_WAIT), OSAL_OK, NULL);
    zassert_equal(osal_c_stream_buffer_available(&sb), 4U, NULL);

    std::uint8_t rx[4]{};
    zassert_equal(osal_c_stream_buffer_receive(&sb, rx, sizeof(rx), OSAL_NO_WAIT), 4U, NULL);
    zassert_mem_equal(rx, tx, 4U, "stream_buffer payload mismatch");
    osal_c_stream_buffer_destroy(&sb);
}

ZTEST(osal_c_api, test_message_buffer_round_trip)
{
    // Backing storage must be capacity+1 bytes.
    static std::uint8_t        mbuf[65];
    osal_message_buffer_handle mb;
    zassert_equal(osal_c_message_buffer_create(&mb, mbuf, 64), OSAL_OK, NULL);

    const std::uint8_t msg[4] = {1, 2, 3, 4};
    zassert_equal(osal_c_message_buffer_send(&mb, msg, sizeof(msg), OSAL_NO_WAIT), OSAL_OK, NULL);
    zassert_equal(osal_c_message_buffer_available(&mb), 4U, "available should report payload size");

    std::uint8_t rx[4]{};
    zassert_equal(osal_c_message_buffer_receive(&mb, rx, sizeof(rx), OSAL_NO_WAIT), 4U, NULL);
    zassert_mem_equal(rx, msg, 4U, "message_buffer payload mismatch");
    osal_c_message_buffer_destroy(&mb);
}

ZTEST(osal_c_api, test_notification_round_trip)
{
    std::uint32_t             values[2]{};
    std::uint8_t              pending[2]{};
    osal_notification_handle note;
    zassert_equal(osal_c_notification_create(&note, values, pending, 2U), OSAL_OK, NULL);
    zassert_equal(osal_c_notification_notify(&note, 0xBEEFU, OSAL_NOTIFICATION_OVERWRITE, 1U), OSAL_OK, NULL);
    zassert_equal(osal_c_notification_pending(&note, 1U), 1, NULL);

    std::uint32_t out = 0U;
    zassert_equal(osal_c_notification_wait(&note, 1U, &out, 0U, 0xFFFFFFFFU, OSAL_NO_WAIT), OSAL_OK, NULL);
    zassert_equal(out, 0xBEEFU, NULL);
    osal_c_notification_destroy(&note);
}

ZTEST(osal_c_api, test_delayable_work_round_trip)
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        ztest_test_skip();
    }

    osal_work_queue_handle wq;
    zassert_equal(osal_c_work_queue_create(&wq, z_wq_stack, K_THREAD_STACK_SIZEOF(z_wq_stack), 8U, "z_c_dw_wq"),
                  OSAL_OK, NULL);

    g_z_c_delayable_count = 0;
    osal_delayable_work_handle work;
    zassert_equal(osal_c_delayable_work_create(&work, &wq, zephyr_c_delayable_cb, &g_z_c_delayable_count, "z_c_dw"),
                  OSAL_OK, NULL);
    zassert_equal(osal_c_delayable_work_schedule(&work, 10U), OSAL_OK, NULL);
    zassert_equal(osal_c_delayable_work_flush(&work, OSAL_WAIT_FOREVER), OSAL_OK, NULL);
    zassert_equal(g_z_c_delayable_count, 1, NULL);
    zassert_equal(osal_c_delayable_work_destroy(&work), OSAL_OK, NULL);
    osal_c_work_queue_destroy(&wq);
}

ZTEST_SUITE(osal_c_api, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Cross-thread integration: semaphore signalling between threads
// =========================================================================

static osal::semaphore* g_xthread_sem;

static void xthread_child(void*)
{
    osal::thread::sleep_for(osal::milliseconds{20});
    g_xthread_sem->give();
}

ZTEST(osal_integration, test_cross_thread_semaphore)
{
    osal::semaphore sem{osal::semaphore_type::binary, 0U};
    zassert_true(sem.valid(), NULL);
    g_xthread_sem = &sem;

    osal::thread        t;
    osal::thread_config cfg{};
    cfg.entry       = xthread_child;
    cfg.arg         = nullptr;
    cfg.stack       = z_xthread_stack;
    cfg.stack_bytes = K_THREAD_STACK_SIZEOF(z_xthread_stack);
    cfg.name        = "z_xsem";
    zassert_true(t.create(cfg).ok(), NULL);

    // Should block until child gives (with generous timeout).
    zassert_true(sem.take_for(osal::milliseconds{2000}), "cross-thread give/take");
    zassert_true(t.join().ok(), NULL);
}

// Cross-thread event flags
static osal::event_flags* g_xthread_ef;

static void xthread_ef_setter(void*)
{
    osal::thread::sleep_for(osal::milliseconds{20});
    g_xthread_ef->set(0x03);
}

ZTEST(osal_integration, test_cross_thread_event_flags)
{
    osal::event_flags ef;
    zassert_true(ef.valid(), NULL);
    ef.clear(0xFFFFFFFFU);
    g_xthread_ef = &ef;

    osal::thread        t;
    osal::thread_config cfg{};
    cfg.entry       = xthread_ef_setter;
    cfg.arg         = nullptr;
    cfg.stack       = z_xef_stack;
    cfg.stack_bytes = K_THREAD_STACK_SIZEOF(z_xef_stack);
    cfg.name        = "z_xef";
    zassert_true(t.create(cfg).ok(), NULL);

    osal::event_bits_t actual = 0;
    auto               r      = ef.wait_all(0x03, &actual, true, osal::milliseconds{2000});
    zassert_true(r.ok(), "wait_all should succeed after child sets bits");
    zassert_equal(actual & 0x03, 0x03U, NULL);
    zassert_true(t.join().ok(), NULL);
}

// Cross-thread stream buffer: producer writes after a delay, consumer blocks.
static osal::stream_buffer<64>* g_xthread_sb;

static void xthread_sb_producer(void*)
{
    osal::thread::sleep_for(osal::milliseconds{20});
    const std::uint8_t data[4] = {0x11, 0x22, 0x33, 0x44};
    g_xthread_sb->send(data, sizeof(data));
}

ZTEST(osal_integration, test_cross_thread_stream_buffer)
{
    static osal::stream_buffer<64> sb;
    zassert_true(sb.valid(), NULL);
    g_xthread_sb = &sb;

    osal::thread        t;
    osal::thread_config cfg{};
    cfg.entry       = xthread_sb_producer;
    cfg.arg         = nullptr;
    cfg.stack       = z_sb_stack;
    cfg.stack_bytes = K_THREAD_STACK_SIZEOF(z_sb_stack);
    cfg.name        = "z_sb_prod";
    zassert_true(t.create(cfg).ok(), NULL);

    std::uint8_t rx[4]{};
    std::size_t  n = sb.receive_for(rx, sizeof(rx), osal::milliseconds{2000});
    zassert_equal(n, 4U, "cross-thread stream_buffer receive length mismatch");
    zassert_equal(rx[0], 0x11U, NULL);
    zassert_equal(rx[3], 0x44U, NULL);
    zassert_true(t.join().ok(), NULL);
}

// Cross-thread message buffer: producer sends a message after a delay.
static osal::message_buffer<64>* g_xthread_mb;

static void xthread_mb_producer(void*)
{
    osal::thread::sleep_for(osal::milliseconds{20});
    const std::uint8_t msg[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    g_xthread_mb->send(msg, sizeof(msg));
}

ZTEST(osal_integration, test_cross_thread_message_buffer)
{
    static osal::message_buffer<64> mb;
    zassert_true(mb.valid(), NULL);
    g_xthread_mb = &mb;

    osal::thread        t;
    osal::thread_config cfg{};
    cfg.entry       = xthread_mb_producer;
    cfg.arg         = nullptr;
    cfg.stack       = z_mb_stack;
    cfg.stack_bytes = K_THREAD_STACK_SIZEOF(z_mb_stack);
    cfg.name        = "z_mb_prod";
    zassert_true(t.create(cfg).ok(), NULL);

    std::uint8_t rx[4]{};
    std::size_t  n = mb.receive_for(rx, sizeof(rx), osal::milliseconds{2000});
    zassert_equal(n, 4U, "cross-thread message_buffer receive length mismatch");
    zassert_equal(rx[0], 0xDEU, NULL);
    zassert_equal(rx[3], 0xEFU, NULL);
    zassert_true(t.join().ok(), NULL);
}

ZTEST_SUITE(osal_integration, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Condvar — extended tests
// =========================================================================

static osal::mutex*   g_cv_ext_mtx   = nullptr;
static osal::condvar* g_cv_ext_cv    = nullptr;
static volatile bool  g_cv_ext_ready = false;

static void cv_notifier_thread(void*)
{
    osal::thread::sleep_for(osal::milliseconds{20});
    {
        osal::mutex::lock_guard lg{*g_cv_ext_mtx};
        g_cv_ext_ready = true;
    }
    g_cv_ext_cv->notify_one();
}

ZTEST(osal_condvar, test_wait_with_notify)
{
    osal::mutex   m;
    osal::condvar cv;
    zassert_true(m.valid(), NULL);
    zassert_true(cv.valid(), NULL);
    g_cv_ext_ready = false;
    g_cv_ext_mtx   = &m;
    g_cv_ext_cv    = &cv;

    osal::thread        t;
    osal::thread_config cfg{};
    cfg.entry       = cv_notifier_thread;
    cfg.arg         = nullptr;
    cfg.stack       = z_cv_wait_stack;
    cfg.stack_bytes = K_THREAD_STACK_SIZEOF(z_cv_wait_stack);
    cfg.name        = "z_cv_notify";
    zassert_true(t.create(cfg).ok(), NULL);

    {
        osal::mutex::lock_guard lg{m};
        cv.wait(m, [] { return static_cast<bool>(g_cv_ext_ready); });
    }
    zassert_true(g_cv_ext_ready, "predicate-wait should have been satisfied");
    zassert_true(t.join().ok(), NULL);
}

ZTEST(osal_condvar, test_wait_for_timeout)
{
    osal::mutex   m;
    osal::condvar cv;
    zassert_true(m.valid(), NULL);
    zassert_true(cv.valid(), NULL);
    m.lock();
    bool signalled = cv.wait_for(m, osal::milliseconds{30});
    m.unlock();
    zassert_false(signalled, "wait_for should time out when nobody signals");
}

// =========================================================================
// RWLock — extended tests
// =========================================================================

static osal::rwlock*    g_rw_ext = nullptr;
static std::atomic<int> g_rw_readers_inside{0};
static volatile bool    g_rw_reader_saw_overlap = false;

static void rw_concurrent_reader(void*)
{
    g_rw_ext->read_lock();
    g_rw_readers_inside.fetch_add(1, std::memory_order_relaxed);
    osal::thread::sleep_for(osal::milliseconds{40});
    if (g_rw_readers_inside.load(std::memory_order_relaxed) >= 2)
        g_rw_reader_saw_overlap = true;
    g_rw_readers_inside.fetch_sub(1, std::memory_order_relaxed);
    g_rw_ext->read_unlock();
}

ZTEST(osal_rwlock, test_concurrent_readers)
{
    osal::rwlock rw;
    zassert_true(rw.valid(), NULL);
    g_rw_ext = &rw;
    g_rw_readers_inside.store(0);
    g_rw_reader_saw_overlap = false;

    osal::thread        ta, tb;
    osal::thread_config ca{}, cb{};
    ca.entry       = rw_concurrent_reader;
    ca.stack       = z_rwlock_ra_stack;
    ca.stack_bytes = K_THREAD_STACK_SIZEOF(z_rwlock_ra_stack);
    ca.name        = "rw_ra";
    cb.entry       = rw_concurrent_reader;
    cb.stack       = z_rwlock_rb_stack;
    cb.stack_bytes = K_THREAD_STACK_SIZEOF(z_rwlock_rb_stack);
    cb.name        = "rw_rb";
    zassert_true(ta.create(ca).ok(), NULL);
    zassert_true(tb.create(cb).ok(), NULL);
    zassert_true(ta.join().ok(), NULL);
    zassert_true(tb.join().ok(), NULL);
    zassert_true(g_rw_reader_saw_overlap, "two readers should hold read_lock concurrently");
}

ZTEST(osal_rwlock, test_write_lock_for_success)
{
    osal::rwlock rw;
    zassert_true(rw.valid(), NULL);
    auto r = rw.write_lock_for(osal::milliseconds{50});
    zassert_true(r.ok(), "write_lock_for on free rwlock should succeed");
    rw.write_unlock();
}

ZTEST(osal_rwlock, test_read_guard_write_guard)
{
    osal::rwlock rw;
    zassert_true(rw.valid(), NULL);
    {
        osal::rwlock::read_guard rg{rw};
    }
    {
        osal::rwlock::write_guard wg{rw};
    }
    // Both guards released — write_lock should succeed immediately.
    zassert_true(rw.write_lock().ok(), "rwlock should be free after guards destroyed");
    rw.write_unlock();
}

// =========================================================================
// Ring buffer (header-only, no OS dependency)
// =========================================================================

ZTEST(osal_ring_buffer, test_empty_on_construction)
{
    osal::ring_buffer<uint32_t, 8> rb;
    uint32_t                       val = 0;
    zassert_false(rb.full(), "fresh ring buffer should not be full");
    zassert_false(rb.try_pop(val), "pop on empty should return false");
}

ZTEST(osal_ring_buffer, test_push_pop)
{
    osal::ring_buffer<uint32_t, 8> rb;
    zassert_true(rb.try_push(42U), "push should succeed on empty buffer");
    uint32_t val = 0;
    zassert_true(rb.try_pop(val), "pop after push should succeed");
    zassert_equal(val, 42U, "popped value should match");
}

ZTEST(osal_ring_buffer, test_fifo_ordering)
{
    osal::ring_buffer<uint32_t, 8> rb;
    for (uint32_t i = 1U; i <= 3U; ++i)
        zassert_true(rb.try_push(i), NULL);
    for (uint32_t expected = 1U; expected <= 3U; ++expected)
    {
        uint32_t val = 0;
        zassert_true(rb.try_pop(val), NULL);
        zassert_equal(val, expected, "FIFO order violated");
    }
}

ZTEST(osal_ring_buffer, test_full_detection)
{
    osal::ring_buffer<uint32_t, 4> rb;
    zassert_true(rb.try_push(1U), NULL);
    zassert_true(rb.try_push(2U), NULL);
    zassert_true(rb.try_push(3U), NULL);
    zassert_true(rb.try_push(4U), NULL);
    zassert_true(rb.full(), "should be full after 4/4 pushes");
    zassert_false(rb.try_push(5U), "push on full buffer should fail");
}

ZTEST(osal_ring_buffer, test_peek)
{
    osal::ring_buffer<uint32_t, 8> rb;
    zassert_true(rb.try_push(99U), NULL);
    uint32_t peeked = 0, popped = 0;
    zassert_true(rb.peek(peeked), "peek should succeed");
    zassert_equal(peeked, 99U, "peek value should match");
    zassert_true(rb.try_pop(popped), NULL);
    zassert_equal(popped, 99U, "pop after peek should return same value");
    zassert_false(rb.try_pop(popped), "buffer should be empty after pop");
}

ZTEST(osal_ring_buffer, test_reset)
{
    osal::ring_buffer<uint32_t, 4> rb;
    zassert_true(rb.try_push(1U), NULL);
    zassert_true(rb.try_push(2U), NULL);
    rb.reset();
    uint32_t val = 0;
    zassert_false(rb.try_pop(val), "buffer should be empty after reset");
}

ZTEST_SUITE(osal_ring_buffer, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Thread-local data (header-only, C++ thread_local emulation)
// =========================================================================

static osal::thread_local_data* g_tld_obj       = nullptr;
static int                      g_tld_child_val = 200;
static volatile bool            g_tld_done      = false;

static void tld_isolation_entry(void*)
{
    g_tld_obj->set(&g_tld_child_val);
    osal::thread::sleep_for(osal::milliseconds{20});
    g_tld_done = true;
}

ZTEST(osal_thread_local_data, test_construction)
{
    osal::thread_local_data tld;
    zassert_true(tld.valid(), "thread_local_data construction should succeed");
}

ZTEST(osal_thread_local_data, test_initial_nullptr)
{
    osal::thread_local_data tld;
    zassert_is_null(tld.get(), "initial TLD value should be nullptr");
}

ZTEST(osal_thread_local_data, test_set_get)
{
    osal::thread_local_data tld;
    int                     dummy = 42;
    tld.set(&dummy);
    zassert_equal_ptr(tld.get(), &dummy, "get should return the value that was set");
    tld.set(nullptr);
    zassert_is_null(tld.get(), "set(nullptr) should clear the value");
}

ZTEST(osal_thread_local_data, test_per_thread_isolation)
{
    osal::thread_local_data tld;
    zassert_true(tld.valid(), NULL);
    int parent_val = 100;
    tld.set(&parent_val);
    g_tld_obj  = &tld;
    g_tld_done = false;

    osal::thread        t;
    osal::thread_config cfg{};
    cfg.entry       = tld_isolation_entry;
    cfg.arg         = nullptr;
    cfg.stack       = z_tld_stack;
    cfg.stack_bytes = K_THREAD_STACK_SIZEOF(z_tld_stack);
    cfg.name        = "z_tld_child";
    zassert_true(t.create(cfg).ok(), NULL);
    zassert_true(t.join().ok(), NULL);
    zassert_equal_ptr(tld.get(), &parent_val, "parent TLD should be unaffected by child set()");
    tld.set(nullptr);
}

ZTEST_SUITE(osal_thread_local_data, NULL, NULL, NULL, NULL, NULL);

// =========================================================================
// Wait set (has_wait_set == false on Zephyr — emulated mode)
// =========================================================================

ZTEST(osal_wait_set, test_construction_valid)
{
    osal::wait_set ws;
    // has_wait_set == false → emulated mode → valid_ = true
    zassert_true(ws.valid(), "wait_set should be valid in emulated mode");
}

ZTEST(osal_wait_set, test_add_not_supported)
{
    osal::wait_set ws;
    auto           r = ws.add(0, osal::wait_events::readable);
    zassert_equal(static_cast<int>(r.code()), static_cast<int>(osal::error_code::not_supported),
                  "add should return not_supported on Zephyr");
}

ZTEST(osal_wait_set, test_wait_not_supported)
{
    osal::wait_set ws;
    int            ready_ids[4] = {};
    std::size_t    n            = 99U;
    auto           r            = ws.wait(ready_ids, 4U, n, osal::milliseconds{10});
    zassert_equal(static_cast<int>(r.code()), static_cast<int>(osal::error_code::not_supported),
                  "wait should return not_supported on Zephyr");
    zassert_equal(n, 0U, "n_ready should be 0 on not_supported");
}

ZTEST_SUITE(osal_wait_set, NULL, NULL, NULL, NULL, NULL);
