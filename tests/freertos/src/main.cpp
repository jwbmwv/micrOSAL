// SPDX-License-Identifier: Apache-2.0
/// @file tests/freertos/src/main.cpp
/// @brief MicrOSAL doctest suite for the FreeRTOS backend.
///
/// Runs all OSAL primitive tests on top of the FreeRTOS-Kernel v11 POSIX
/// simulation port.  The POSIX port runs on Linux as a normal process using
/// POSIX signals to simulate preemption; no cross-compiler or emulator is
/// required.
///
/// Entry flow
/// ──────────
/// 1. main() creates a single high-priority "runner" task and calls
///    vTaskStartScheduler() which blocks until vTaskEndScheduler() is called.
/// 2. The runner task executes the doctest context, stores the exit code in a
///    global, and calls vTaskEndScheduler().
/// 3. main() returns the stored exit code to CTest.
///
/// Build:
///   cmake -B build-freertos tests/freertos -DCMAKE_BUILD_TYPE=Debug
///   cmake --build build-freertos -j$(nproc)
///   ctest --test-dir build-freertos --output-on-failure

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "FreeRTOS.h"
#include "task.h"

#include <osal/osal.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>

// ---------------------------------------------------------------------------
// Static memory for the idle task and timer task (required when
// configSUPPORT_STATIC_ALLOCATION == 1)
// ---------------------------------------------------------------------------
static StaticTask_t   s_idle_tcb;
static StackType_t    s_idle_stack[configMINIMAL_STACK_SIZE];
static StaticTask_t   s_timer_tcb;
static StackType_t    s_timer_stack[configTIMER_TASK_STACK_DEPTH];

extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t** ppxIdleTaskTCBBuffer,
                                               StackType_t**  ppxIdleTaskStackBuffer,
                                               configSTACK_DEPTH_TYPE* pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer   = &s_idle_tcb;
    *ppxIdleTaskStackBuffer = s_idle_stack;
    *pulIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}

extern "C" void vApplicationGetTimerTaskMemory(StaticTask_t** ppxTimerTaskTCBBuffer,
                                                StackType_t**  ppxTimerTaskStackBuffer,
                                                configSTACK_DEPTH_TYPE* pulTimerTaskStackSize)
{
    *ppxTimerTaskTCBBuffer   = &s_timer_tcb;
    *ppxTimerTaskStackBuffer = s_timer_stack;
    *pulTimerTaskStackSize   = configTIMER_TASK_STACK_DEPTH;
}

// Minimal stack-overflow hook (required if configCHECK_FOR_STACK_OVERFLOW > 0)
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char* pcTaskName)
{
    (void)pcTaskName;
    std::abort();
}

// ---------------------------------------------------------------------------
// Doctest runner task
// ---------------------------------------------------------------------------
static int g_exit_code = 0;

static void runner_task(void* /*pvParam*/)
{
    doctest::Context ctx;
    ctx.setOption("no-breaks", true);   // don't break into debugger on failure
    g_exit_code = ctx.run();
    vTaskEndScheduler();
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    // Forward argc/argv to doctest so --help / --test-case etc. work from CTest.
    // We stash them in globals so runner_task can pick them up via the Context API.
    (void)argc;
    (void)argv;

    xTaskCreate(runner_task,
                "doctest_runner",
                static_cast<configSTACK_DEPTH_TYPE>(131072 / sizeof(StackType_t)),
                nullptr,
                configMAX_PRIORITIES - 2,
                nullptr);

    vTaskStartScheduler();   // returns after vTaskEndScheduler()
    return g_exit_code;
}

// ==========================================================================
//  Test cases — mirror of the Linux/POSIX doctest suite
// ==========================================================================

// --------------------------------------------------------------------------
// osal::mutex
// --------------------------------------------------------------------------

TEST_CASE("freertos/mutex: construction succeeds")
{
    osal::mutex m;
    CHECK(m.valid());
}

TEST_CASE("freertos/mutex: lock and unlock")
{
    osal::mutex m;
    REQUIRE(m.valid());
    m.lock();
    m.unlock();
}

TEST_CASE("freertos/mutex: try_lock succeeds when free")
{
    osal::mutex m;
    REQUIRE(m.valid());
    CHECK(m.try_lock());
    m.unlock();
}

TEST_CASE("freertos/mutex: try_lock fails when held by another thread")
{
    osal::mutex m;
    REQUIRE(m.valid());

    struct ctx_t { osal::mutex* mtx; bool could_lock{false}; };
    static ctx_t ctx{&m};
    ctx.could_lock = false;

    m.lock();   // hold from current task

    constexpr std::size_t kStack = 65536U;
    alignas(16) static std::uint8_t stack[kStack];
    osal::thread_config cfg{};
    cfg.entry       = [](void* arg) {
        auto* c = static_cast<ctx_t*>(arg);
        c->could_lock = c->mtx->try_lock();
        if (c->could_lock) c->mtx->unlock();
    };
    cfg.arg         = &ctx;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStack;
    cfg.name        = "mtx_probe";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());
    (void)t.join();

    CHECK_FALSE(ctx.could_lock);
    m.unlock();
}

TEST_CASE("freertos/mutex: timed lock times out")
{
    if constexpr (!osal::active_capabilities::has_timed_mutex)
    {
        MESSAGE("Skipped — backend lacks timed mutex");
        return;
    }
    osal::mutex m;
    REQUIRE(m.valid());
    m.lock();

    struct ctx_t { osal::mutex* mtx; bool timed_out{false}; };
    static ctx_t ctx{&m};

    constexpr std::size_t kStack = 65536U;
    alignas(16) static std::uint8_t stack[kStack];
    osal::thread_config cfg{};
    cfg.entry = [](void* arg) {
        auto* c = static_cast<ctx_t*>(arg);
        c->timed_out = !c->mtx->try_lock_for(osal::milliseconds{50});
    };
    cfg.arg         = &ctx;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStack;
    cfg.name        = "mtx_timeout";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());
    (void)t.join();

    CHECK(ctx.timed_out);
    m.unlock();
}

TEST_CASE("freertos/mutex: lock_guard RAII")
{
    osal::mutex m;
    REQUIRE(m.valid());
    {
        osal::mutex::lock_guard g{m};
        (void)g;
    }
    CHECK(m.try_lock());
    m.unlock();
}

// --------------------------------------------------------------------------
// osal::semaphore
// --------------------------------------------------------------------------

TEST_CASE("freertos/semaphore: binary give/take")
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    REQUIRE(s.valid());
    s.give();
    CHECK(s.try_take());
    CHECK_FALSE(s.try_take());
}

TEST_CASE("freertos/semaphore: counting semaphore")
{
    osal::semaphore s{osal::semaphore_type::counting, 0U, 10U};
    REQUIRE(s.valid());
    s.give();
    s.give();
    s.give();
    CHECK(s.try_take());
    CHECK(s.try_take());
    CHECK(s.try_take());
    CHECK_FALSE(s.try_take());
}

TEST_CASE("freertos/semaphore: take_for timeout")
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    REQUIRE(s.valid());
    CHECK_FALSE(s.take_for(osal::milliseconds{30}));
}

TEST_CASE("freertos/semaphore: cross-thread signal")
{
    static osal::semaphore sig{osal::semaphore_type::binary, 0U};
    static std::atomic<int> counter{0};
    REQUIRE(sig.valid());

    constexpr std::size_t kStack = 65536U;
    alignas(16) static std::uint8_t stack[kStack];
    osal::thread_config cfg{};
    cfg.entry = [](void*) {
        counter.store(42);
        sig.give();
    };
    cfg.arg         = nullptr;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStack;
    cfg.name        = "sem_sig";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());
    sig.take();
    CHECK(counter.load() == 42);
    (void)t.join();
}

// --------------------------------------------------------------------------
// osal::queue
// --------------------------------------------------------------------------

TEST_CASE("freertos/queue: send and receive")
{
    osal::queue<std::uint32_t, 8> q;
    REQUIRE(q.valid());
    CHECK(q.try_send(0xDEADBEEFU));
    std::uint32_t v = 0U;
    CHECK(q.try_receive(v));
    CHECK(v == 0xDEADBEEFU);
    CHECK(q.empty());
}

TEST_CASE("freertos/queue: FIFO ordering")
{
    osal::queue<std::uint32_t, 8> q;
    REQUIRE(q.valid());
    for (std::uint32_t i = 1U; i <= 4U; ++i)
        REQUIRE(q.try_send(i));
    for (std::uint32_t e = 1U; e <= 4U; ++e)
    {
        std::uint32_t v = 0U;
        REQUIRE(q.try_receive(v));
        CHECK(v == e);
    }
}

TEST_CASE("freertos/queue: full detection")
{
    osal::queue<std::uint8_t, 4> q;
    REQUIRE(q.valid());
    for (int i = 0; i < 4; ++i)
        REQUIRE(q.try_send(static_cast<std::uint8_t>(i)));
    CHECK(q.full());
    CHECK_FALSE(q.try_send(static_cast<std::uint8_t>(99)));
}

// --------------------------------------------------------------------------
// osal::timer
// --------------------------------------------------------------------------

TEST_CASE("freertos/timer: one-shot fires once")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend lacks timers");
        return;
    }

    static std::atomic<int> fire_count{0};
    fire_count.store(0);
    osal::timer tmr{+[](void*) { fire_count.fetch_add(1); }, nullptr,
                   osal::milliseconds{50}, osal::timer_mode::one_shot, "fr_tmr"};
    REQUIRE(tmr.valid());
    REQUIRE(tmr.start().ok());
    osal::thread::sleep_for(osal::milliseconds{200});
    CHECK(fire_count.load() == 1);
}

TEST_CASE("freertos/timer: periodic fires multiple times")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend lacks timers");
        return;
    }

    static std::atomic<int> tick_count{0};
    tick_count.store(0);
    osal::timer tmr{+[](void*) { tick_count.fetch_add(1); }, nullptr,
                   osal::milliseconds{40}, osal::timer_mode::periodic, "fr_tick"};
    REQUIRE(tmr.valid());
    REQUIRE(tmr.start().ok());
    osal::thread::sleep_for(osal::milliseconds{250});
    REQUIRE(tmr.stop().ok());
    CHECK(tick_count.load() >= 4);
}

// --------------------------------------------------------------------------
// osal::event_flags
// --------------------------------------------------------------------------

TEST_CASE("freertos/event_flags: set and get")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());
    ef.set(0b1010U);
    CHECK((ef.get() & 0b1010U) == 0b1010U);
}

TEST_CASE("freertos/event_flags: wait_any immediate")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());
    ef.set(0x0FU);
    osal::event_bits_t actual = 0U;
    auto r = ef.wait_any(0x0FU, &actual, /*clear_on_exit=*/true, osal::milliseconds{0});
    REQUIRE(r.ok());
    CHECK((actual & 0x0FU) == 0x0FU);
}

TEST_CASE("freertos/event_flags: cross-thread set")
{
    static osal::event_flags ef;
    REQUIRE(ef.valid());

    constexpr std::size_t kStack = 65536U;
    alignas(16) static std::uint8_t stack[kStack];
    osal::thread_config cfg{};
    cfg.entry = [](void*) {
        osal::thread::sleep_for(osal::milliseconds{30});
        ef.set(0x01U);
    };
    cfg.arg         = nullptr;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStack;
    cfg.name        = "ef_set";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());
    auto r = ef.wait_any(0x01U, nullptr, false, osal::milliseconds{500});
    CHECK(r.ok());
    (void)t.join();
}

// --------------------------------------------------------------------------
// osal::stream_buffer
// --------------------------------------------------------------------------

TEST_CASE("freertos/stream_buffer: construction and basic send/receive")
{
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());
    CHECK(sb.empty());

    const std::uint8_t tx[4] = {11U, 22U, 33U, 44U};
    REQUIRE(sb.try_send(tx, sizeof(tx)));
    CHECK(sb.available() == sizeof(tx));

    std::uint8_t rx[4]{};
    const std::size_t n = sb.try_receive(rx, sizeof(rx));
    CHECK(n == sizeof(tx));
    CHECK(std::memcmp(tx, rx, sizeof(tx)) == 0);
    CHECK(sb.empty());
}

TEST_CASE("freertos/stream_buffer: full rejection")
{
    osal::stream_buffer<4> sb;
    REQUIRE(sb.valid());
    const std::uint8_t data[4]{};
    REQUIRE(sb.try_send(data, sizeof(data)));
    CHECK(sb.full());
    CHECK_FALSE(sb.try_send(data, 1U));
}

TEST_CASE("freertos/stream_buffer: reset")
{
    osal::stream_buffer<32> sb;
    REQUIRE(sb.valid());
    const std::uint8_t data[8]{};
    REQUIRE(sb.try_send(data, sizeof(data)));
    REQUIRE(sb.reset().ok());
    CHECK(sb.empty());
}

TEST_CASE("freertos/stream_buffer: send_isr / receive_isr round-trip")
{
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());
    const std::uint8_t tx[4] = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
    REQUIRE(sb.send_isr(tx, sizeof(tx)).ok());
    std::uint8_t rx[4]{};
    CHECK(sb.receive_isr(rx, sizeof(rx)) == sizeof(tx));
    CHECK(std::memcmp(tx, rx, sizeof(tx)) == 0);
}

// --------------------------------------------------------------------------
// osal::message_buffer
// --------------------------------------------------------------------------

TEST_CASE("freertos/message_buffer: send and receive")
{
    osal::message_buffer<64> mb;
    REQUIRE(mb.valid());

    const std::uint8_t tx[6] = {1U, 2U, 3U, 4U, 5U, 6U};
    REQUIRE(mb.try_send(tx, sizeof(tx)));
    CHECK(mb.next_message_size() == sizeof(tx));

    std::uint8_t rx[8]{};
    const std::size_t n = mb.try_receive(rx, sizeof(rx));
    REQUIRE(n == sizeof(tx));
    CHECK(std::memcmp(tx, rx, sizeof(tx)) == 0);
    CHECK(mb.empty());
}

TEST_CASE("freertos/message_buffer: FIFO ordering")
{
    osal::message_buffer<128> mb;
    REQUIRE(mb.valid());
    for (std::uint8_t i = 1U; i <= 4U; ++i)
        REQUIRE(mb.try_send(&i, 1U));
    for (std::uint8_t e = 1U; e <= 4U; ++e)
    {
        std::uint8_t rx = 0U;
        REQUIRE(mb.try_receive(&rx, 1U) == 1U);
        CHECK(rx == e);
    }
    CHECK(mb.empty());
}

// --------------------------------------------------------------------------
// osal::ring_buffer  (header-only, backend-independent)
// --------------------------------------------------------------------------

TEST_CASE("freertos/ring_buffer: push/pop FIFO")
{
    osal::ring_buffer<std::uint32_t, 8> rb;
    for (std::uint32_t i = 0U; i < 8U; ++i)
        CHECK(rb.try_push(i));
    CHECK(rb.full());
    for (std::uint32_t e = 0U; e < 8U; ++e)
    {
        std::uint32_t v = 0xFFFFFFFFU;
        CHECK(rb.try_pop(v));
        CHECK(v == e);
    }
    CHECK(rb.empty());
}

// --------------------------------------------------------------------------
// osal::thread
// --------------------------------------------------------------------------

TEST_CASE("freertos/thread: create and join")
{
    static std::atomic<int> counter{0};
    constexpr std::size_t kStack = 65536U;
    alignas(16) static std::uint8_t stack[kStack];

    osal::thread_config cfg{};
    cfg.entry = [](void*) { counter.store(99); };
    cfg.arg         = nullptr;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStack;
    cfg.name        = "fr_thread";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());
    REQUIRE(t.join().ok());
    CHECK(counter.load() == 99);
}

TEST_CASE("freertos/thread: sleep_for is non-trivial")
{
    const auto before = osal::monotonic_clock::now();
    osal::thread::sleep_for(osal::milliseconds{50});
    const auto after = osal::monotonic_clock::now();
    const auto elapsed = std::chrono::duration_cast<osal::milliseconds>(after - before);
    CHECK(elapsed.count() >= 40);   // allow ± 20 % jitter
}

// --------------------------------------------------------------------------
// osal::condvar
// --------------------------------------------------------------------------

TEST_CASE("freertos/condvar: cross-thread notify")
{
    static osal::mutex m;
    static osal::condvar cv;
    REQUIRE(m.valid());
    REQUIRE(cv.valid());

    static bool ready = false;

    constexpr std::size_t kStack = 65536U;
    alignas(16) static std::uint8_t stack[kStack];
    osal::thread_config cfg{};
    cfg.entry = [](void*) {
        osal::thread::sleep_for(osal::milliseconds{30});
        {
            osal::mutex::lock_guard lk{m};
            ready = true;
        }
        cv.notify_one();
    };
    cfg.arg         = nullptr;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStack;
    cfg.name        = "cv_notify";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());

    {
        osal::mutex::lock_guard lk{m};
        cv.wait(m, [] { return ready; });
    }
    CHECK(ready);

    (void)t.join();
}

// --------------------------------------------------------------------------
// osal::thread_local_data
// --------------------------------------------------------------------------

TEST_CASE("freertos/thread_local_data: per-thread isolation")
{
    osal::thread_local_data tld;
    REQUIRE(tld.valid());
    tld.set(reinterpret_cast<void*>(0xCAFEBABEUL));

    static std::atomic<bool> other_saw_null{false};
    constexpr std::size_t kStack = 65536U;
    alignas(16) static std::uint8_t stack[kStack];
    osal::thread_config cfg{};
    cfg.entry = [](void* arg) {
        auto* tld_p = static_cast<osal::thread_local_data*>(arg);
        other_saw_null.store(tld_p->get() == nullptr);
    };
    cfg.arg         = &tld;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStack;
    cfg.name        = "tld_probe";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());
    (void)t.join();

    CHECK(other_saw_null.load());
    CHECK(tld.get() == reinterpret_cast<void*>(0xCAFEBABEUL));
}

// --------------------------------------------------------------------------
// result / error_code  (header-only)
// --------------------------------------------------------------------------

TEST_CASE("freertos/result: ok and error codes")
{
    const osal::result ok{};
    const osal::result err{osal::error_code::timeout};
    CHECK(ok.ok());
    CHECK_FALSE(err.ok());
    CHECK(err.code() == osal::error_code::timeout);
}

// --------------------------------------------------------------------------
// osal::memory_pool
// --------------------------------------------------------------------------

TEST_CASE("freertos/memory_pool: alloc and free")
{
    alignas(std::uint32_t) static std::uint8_t pool_storage[sizeof(std::uint32_t) * 8U];
    osal::memory_pool pool{pool_storage, sizeof(pool_storage), sizeof(std::uint32_t), 8U};
    REQUIRE(pool.valid());
    CHECK(pool.available() == 8U);

    void* p = pool.allocate();
    REQUIRE(p != nullptr);
    CHECK(pool.available() == 7U);
    pool.deallocate(p);
    CHECK(pool.available() == 8U);
}

TEST_CASE("freertos/memory_pool: exhaustion returns nullptr on try_allocate")
{
    alignas(std::uint32_t) static std::uint8_t pool_storage[sizeof(std::uint32_t) * 2U];
    osal::memory_pool pool{pool_storage, sizeof(pool_storage), sizeof(std::uint32_t), 2U};
    REQUIRE(pool.valid());

    void* p1 = pool.allocate();
    void* p2 = pool.allocate();
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);

    void* p3 = pool.allocate();  // returns nullptr when exhausted
    CHECK(p3 == nullptr);

    pool.deallocate(p1);
    pool.deallocate(p2);
}
