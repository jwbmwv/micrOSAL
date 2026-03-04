// SPDX-License-Identifier: Apache-2.0
/// @file tests/nuttx/src/main.cpp
/// @brief MicrOSAL test suite for the Apache NuttX backend.
///
/// This file is built as a NuttX built-in application ("microsal_test") via
/// the NuttX CMake build system.  It can be run in two ways:
///
///   1. From NSH:
///        nsh> microsal_test
///
///   2. As the init task (fastest for CI):
///        Set CONFIG_INIT_ENTRYPOINT="microsal_test_main" and
///        CONFIG_INIT_STACKSIZE=65536 in the defconfig.
///
/// The NuttX sim target (sim/nsh on x86_64 Linux) executes the full NuttX
/// kernel as a Linux process, providing a genuine NuttX environment without
/// requiring a cross-compiler or emulator.
///
/// @note  doctest is NOT used here.  GCC's C++ locale headers (<iostream>,
///        <sstream>) are incompatible with NuttX's stripped system headers
///        in a sim/nsh build.  Instead a minimal printf-based framework is
///        inlined below.  All TEST_CASE bodies are unchanged from the
///        doctest version.
///
/// @note NuttX uses nxsem_*, nxmutex_*, and POSIX pthread_* internally;
///       all OSAL primitives are exercised through the nuttx_backend.cpp path.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin

#include <osal/osal.hpp>

#include <stdint.h>
#include <string.h>
#include <atomic>
#include <stdio.h>   /* printf — NuttX provides this without locale deps */

// ==========================================================================
//  Minimal NuttX-compatible test framework
//  (printf only — no iostream, sstream, or locale dependency)
// ==========================================================================

namespace nxtest {

struct TestCase { const char* name; void (*fn)(); };

static constexpr int MAX_TESTS = 128;
static TestCase g_tests[MAX_TESTS];
static int g_test_count          = 0;
static int g_current_checks_fail = 0;
static int g_total_passed        = 0;
static int g_total_failed        = 0;

/// Auto-registration helper — instantiated by each TEST_CASE macro.
struct Registrar {
    Registrar(const char* n, void(*f)()) {
        if (g_test_count < MAX_TESTS)
            g_tests[g_test_count++] = {n, f};
    }
};

inline int run_all() {
    printf("[microsal_test] Running %d test(s) ...\n\n", g_test_count);
    for (int i = 0; i < g_test_count; ++i) {
        g_current_checks_fail = 0;
        g_tests[i].fn();
        if (g_current_checks_fail == 0) {
            printf("  PASS  %s\n", g_tests[i].name);
            ++g_total_passed;
        } else {
            printf("  FAIL  %s  (%d check(s) failed)\n",
                   g_tests[i].name, g_current_checks_fail);
            ++g_total_failed;
        }
    }
    printf("\n[microsal_test] %d/%d passed", g_total_passed, g_test_count);
    if (g_total_failed)
        printf("  *** %d FAILED ***", g_total_failed);
    printf("\n");
    return g_total_failed == 0 ? 0 : 1;
}

} // namespace nxtest

// --------------------------------------------------------------------------
//  Macro API  (DROP-IN for doctest TEST_CASE / CHECK / REQUIRE / MESSAGE)
// --------------------------------------------------------------------------

#define NXTEST_CAT2_(a, b)  a##b
#define NXTEST_CAT_(a, b)   NXTEST_CAT2_(a, b)

/// Register and define a test case function.
#define TEST_CASE(name) \
    static void NXTEST_CAT_(nxtest_fn_, __LINE__)(); \
    static ::nxtest::Registrar NXTEST_CAT_(nxtest_reg_, __LINE__)( \
        name, NXTEST_CAT_(nxtest_fn_, __LINE__)); \
    static void NXTEST_CAT_(nxtest_fn_, __LINE__)()

/// Assertion that aborts the current test on failure.
#define REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            printf("    REQUIRE failed: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++::nxtest::g_current_checks_fail; \
            return; \
        } \
    } while (0)

/// Soft assertion — records failure but continues the test.
#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            printf("    CHECK failed: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++::nxtest::g_current_checks_fail; \
        } \
    } while (0)

#define CHECK_FALSE(expr)   CHECK(!(expr))

/// Informational message (replaces doctest MESSAGE macro).
#define MESSAGE(msg)    printf("    INFO: %s\n", msg)

// ==========================================================================
//  NuttX built-in application entry point.
// ==========================================================================

extern "C" int microsal_test_main(int /*argc*/, char** /*argv*/)
{
    return ::nxtest::run_all();
}

// ==========================================================================
//  Test cases
// ==========================================================================

// --------------------------------------------------------------------------
// osal::mutex
// --------------------------------------------------------------------------

TEST_CASE("nuttx/mutex: construction and lock/unlock")
{
    osal::mutex m;
    REQUIRE(m.valid());
    m.lock();
    m.unlock();
}

TEST_CASE("nuttx/mutex: try_lock succeeds when free")
{
    osal::mutex m;
    REQUIRE(m.valid());
    CHECK(m.try_lock());
    m.unlock();
}

TEST_CASE("nuttx/mutex: lock_guard RAII releases on scope exit")
{
    osal::mutex m;
    REQUIRE(m.valid());
    {
        osal::mutex::lock_guard g{m};
        (void)g;
    }
    // After guard destruction we must be able to re-lock.
    CHECK(m.try_lock());
    m.unlock();
}

TEST_CASE("nuttx/mutex: timed lock times out when held")
{
    if constexpr (!osal::active_capabilities::has_timed_mutex)
    {
        MESSAGE("Skipped — backend lacks timed mutex");
        return;
    }

    osal::mutex m;
    REQUIRE(m.valid());
    m.lock();   // hold by current thread

    struct ctx_t { osal::mutex* mtx; bool timed_out{false}; };
    static ctx_t ctx{&m};

    constexpr std::size_t kStack = 65536U;
    alignas(16) static std::uint8_t stack[kStack];
    osal::thread_config cfg{};
    cfg.entry = [](void* arg) {
        auto* c = static_cast<ctx_t*>(arg);
        c->timed_out = !c->mtx->try_lock_for(osal::milliseconds{40});
    };
    cfg.arg         = &ctx;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStack;
    cfg.name        = "mtx_to";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());
    (void)t.join();

    CHECK(ctx.timed_out);
    m.unlock();
}

// --------------------------------------------------------------------------
// osal::semaphore
// --------------------------------------------------------------------------

TEST_CASE("nuttx/semaphore: binary give/take")
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    REQUIRE(s.valid());
    s.give();
    CHECK(s.try_take());
    CHECK_FALSE(s.try_take());
}

TEST_CASE("nuttx/semaphore: counting semaphore give/take")
{
    osal::semaphore s{osal::semaphore_type::counting, 0U, 8U};
    REQUIRE(s.valid());
    for (int i = 0; i < 3; ++i)
        s.give();
    for (int i = 0; i < 3; ++i)
        CHECK(s.try_take());
    CHECK_FALSE(s.try_take());
}

TEST_CASE("nuttx/semaphore: take_for timeout fires")
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    REQUIRE(s.valid());
    CHECK_FALSE(s.take_for(osal::milliseconds{30}));
}

TEST_CASE("nuttx/semaphore: cross-thread signal")
{
    static osal::semaphore sig{osal::semaphore_type::binary, 0U};
    static std::atomic<int> val{0};
    REQUIRE(sig.valid());

    constexpr std::size_t kStack = 65536U;
    alignas(16) static std::uint8_t stack[kStack];
    osal::thread_config cfg{};
    cfg.entry = [](void*) { val.store(77); sig.give(); };
    cfg.arg         = nullptr;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStack;
    cfg.name        = "sig_t";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());
    sig.take();
    CHECK(val.load() == 77);
    (void)t.join();
}

// --------------------------------------------------------------------------
// osal::queue
// --------------------------------------------------------------------------

TEST_CASE("nuttx/queue: FIFO send and receive")
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
    CHECK(q.empty());
}

TEST_CASE("nuttx/queue: full detection")
{
    osal::queue<std::uint8_t, 4> q;
    REQUIRE(q.valid());
    for (int i = 0; i < 4; ++i)
        REQUIRE(q.try_send(static_cast<std::uint8_t>(i)));
    CHECK(q.full());
    CHECK_FALSE(q.try_send(static_cast<std::uint8_t>(99U)));
}

// --------------------------------------------------------------------------
// osal::timer
// --------------------------------------------------------------------------

TEST_CASE("nuttx/timer: one-shot fires exactly once")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend lacks software timers");
        return;
    }

    static std::atomic<int> count{0};
    count.store(0);
    osal::timer tmr{+[](void*) { count.fetch_add(1); }, nullptr,
                   osal::milliseconds{50}, osal::timer_mode::one_shot, "nx_tmr"};
    REQUIRE(tmr.valid());
    REQUIRE(tmr.start().ok());
    osal::thread::sleep_for(osal::milliseconds{200});
    CHECK(count.load() == 1);
}

TEST_CASE("nuttx/timer: periodic fires multiple times")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend lacks software timers");
        return;
    }

    static std::atomic<int> ticks{0};
    ticks.store(0);
    osal::timer tmr{+[](void*) { ticks.fetch_add(1); }, nullptr,
                   osal::milliseconds{40}, osal::timer_mode::periodic, "nx_tick"};
    REQUIRE(tmr.valid());
    REQUIRE(tmr.start().ok());
    osal::thread::sleep_for(osal::milliseconds{250});
    REQUIRE(tmr.stop().ok());
    CHECK(ticks.load() >= 4);
}

// --------------------------------------------------------------------------
// osal::condvar
// --------------------------------------------------------------------------

TEST_CASE("nuttx/condvar: cross-thread wait+notify")
{
    static osal::mutex m;
    static osal::condvar cv;
    REQUIRE(m.valid());
    REQUIRE(cv.valid());
    static bool ready_flag{false};

    constexpr std::size_t kStack = 65536U;
    alignas(16) static std::uint8_t stack[kStack];
    osal::thread_config cfg{};
    cfg.entry = [](void*) {
        osal::thread::sleep_for(osal::milliseconds{30});
        {
            osal::mutex::lock_guard lk{m};
            ready_flag = true;
        }
        cv.notify_one();
    };
    cfg.arg         = nullptr;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStack;
    cfg.name        = "cv_nx";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());

    {
        osal::mutex::lock_guard lk{m};
        cv.wait(m, [] { return ready_flag; });
    }
    CHECK(ready_flag);
    (void)t.join();
}

TEST_CASE("nuttx/condvar: wait_for timeout")
{
    osal::mutex m;
    osal::condvar cv;
    REQUIRE(m.valid());
    REQUIRE(cv.valid());

    m.lock();
    const bool signalled = cv.wait_for(m, osal::milliseconds{30}, [] { return false; });
    m.unlock();
    CHECK_FALSE(signalled);
}

// --------------------------------------------------------------------------
// osal::rwlock
// --------------------------------------------------------------------------

TEST_CASE("nuttx/rwlock: basic read and write lock")
{
    if constexpr (!osal::active_capabilities::has_native_rwlock)
    {
        MESSAGE("Skipped — backend lacks native rwlock");
        return;
    }
    osal::rwlock rw;
    REQUIRE(rw.valid());
    rw.read_lock();
    rw.read_unlock();
    rw.write_lock();
    rw.write_unlock();
}

TEST_CASE("nuttx/rwlock: RAII write guard")
{
    if constexpr (!osal::active_capabilities::has_native_rwlock)
    {
        MESSAGE("Skipped — backend lacks native rwlock");
        return;
    }
    osal::rwlock rw;
    REQUIRE(rw.valid());
    {
        rw.write_lock();
        rw.write_unlock();
    }
    // Must be lockable again after guard release.
    CHECK(rw.write_lock().ok());
    rw.write_unlock();
}

// --------------------------------------------------------------------------
// osal::stream_buffer
// --------------------------------------------------------------------------

TEST_CASE("nuttx/stream_buffer: basic send/receive")
{
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());
    CHECK(sb.empty());

    const std::uint8_t tx[4] = {0x11U, 0x22U, 0x33U, 0x44U};
    REQUIRE(sb.try_send(tx, sizeof(tx)));
    CHECK(sb.available() == sizeof(tx));

    std::uint8_t rx[4]{};
    CHECK(sb.try_receive(rx, sizeof(rx)) == sizeof(tx));
    CHECK(std::memcmp(tx, rx, sizeof(tx)) == 0);
    CHECK(sb.empty());
}

TEST_CASE("nuttx/stream_buffer: reset")
{
    osal::stream_buffer<32> sb;
    REQUIRE(sb.valid());
    const std::uint8_t data[8]{};
    REQUIRE(sb.try_send(data, sizeof(data)));
    REQUIRE(sb.reset().ok());
    CHECK(sb.empty());
}

// --------------------------------------------------------------------------
// osal::message_buffer
// --------------------------------------------------------------------------

TEST_CASE("nuttx/message_buffer: send/receive and FIFO order")
{
    osal::message_buffer<64> mb;
    REQUIRE(mb.valid());
    for (std::uint8_t i = 1U; i <= 3U; ++i)
        REQUIRE(mb.try_send(&i, 1U));
    for (std::uint8_t e = 1U; e <= 3U; ++e)
    {
        std::uint8_t rx = 0U;
        REQUIRE(mb.try_receive(&rx, 1U) == 1U);
        CHECK(rx == e);
    }
    CHECK(mb.empty());
}

// --------------------------------------------------------------------------
// osal::ring_buffer  (header-only)
// --------------------------------------------------------------------------

TEST_CASE("nuttx/ring_buffer: push/pop FIFO")
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

TEST_CASE("nuttx/thread: create and join")
{
    static std::atomic<int> counter{0};
    constexpr std::size_t kStack = 65536U;
    alignas(16) static std::uint8_t stack[kStack];

    osal::thread_config cfg{};
    cfg.entry = [](void*) { counter.store(55); };
    cfg.arg         = nullptr;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStack;
    cfg.name        = "nx_t";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());
    REQUIRE(t.join().ok());
    CHECK(counter.load() == 55);
}

TEST_CASE("nuttx/thread: sleep_for is non-trivial")
{
    const auto before = osal::monotonic_clock::now();
    osal::thread::sleep_for(osal::milliseconds{50});
    const auto after = osal::monotonic_clock::now();
    const auto elapsed = std::chrono::duration_cast<osal::milliseconds>(after - before);
    CHECK(elapsed.count() >= 40);
}

// --------------------------------------------------------------------------
// osal::thread_local_data
//
// Note: NuttX pthreads within the same task group share pthread TLS storage
// (pthread_getspecific can return the parent's value in a newly spawned
// thread).  This is a known NuttX kernel limitation for sim/nsh builds.
// We test only single-thread set/get correctness here; cross-thread
// isolation is covered by the LINUX/POSIX CTest suite.
// --------------------------------------------------------------------------

TEST_CASE("nuttx/thread_local_data: set and get in same thread")
{
    osal::thread_local_data tld;
    REQUIRE(tld.valid());

    // Before any set(), get() should return nullptr.
    CHECK(tld.get() == nullptr);

    // After set(), get() returns the correct value in the same thread.
    void* const sentinel = reinterpret_cast<void*>(static_cast<uintptr_t>(0xdeadbeefU));
    REQUIRE(tld.set(sentinel).ok());
    CHECK(tld.get() == sentinel);

    // Overwrite with a different value.
    void* const sentinel2 = reinterpret_cast<void*>(static_cast<uintptr_t>(0xcafeU));
    REQUIRE(tld.set(sentinel2).ok());
    CHECK(tld.get() == sentinel2);

    // Two independent keys do not alias each other.
    osal::thread_local_data tld2;
    REQUIRE(tld2.valid());
    CHECK(tld2.get() == nullptr);
    REQUIRE(tld2.set(sentinel).ok());
    CHECK(tld.get() == sentinel2);  // tld unchanged
    CHECK(tld2.get() == sentinel);  // tld2 has its own value
}

// --------------------------------------------------------------------------
// result / error_code  (header-only)
// --------------------------------------------------------------------------

TEST_CASE("nuttx/result: ok and error distinguishable")
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

TEST_CASE("nuttx/memory_pool: alloc and free round-trip")
{
    alignas(std::uint32_t) static std::uint8_t storage[sizeof(std::uint32_t) * 4U];
    osal::memory_pool pool{storage, sizeof(storage), sizeof(std::uint32_t), 4U};
    REQUIRE(pool.valid());
    CHECK(pool.available() == 4U);

    void* p = pool.allocate();
    REQUIRE(p != nullptr);
    CHECK(pool.available() == 3U);
    pool.deallocate(p);
    CHECK(pool.available() == 4U);
}

TEST_CASE("nuttx/memory_pool: try_allocate returns nullptr when empty")
{
    alignas(std::uint32_t) static std::uint8_t storage[sizeof(std::uint32_t) * 2U];
    osal::memory_pool pool{storage, sizeof(storage), sizeof(std::uint32_t), 2U};
    REQUIRE(pool.valid());

    void* a = pool.allocate();
    void* b = pool.allocate();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(pool.allocate() == nullptr);  // exhausted

    pool.deallocate(a);
    pool.deallocate(b);
}
