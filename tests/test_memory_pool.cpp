// SPDX-License-Identifier: Apache-2.0
/// @file test_memory_pool.cpp
/// @brief Tests for osal::memory_pool (fixed-size block allocator).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <cstring>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("memory_pool: construction succeeds")
{
    alignas(std::size_t) static std::uint8_t buf[sizeof(std::size_t) * 8];
    osal::memory_pool pool{buf, sizeof(buf), sizeof(std::size_t), 8, "test"};
    CHECK(pool.valid());
}

TEST_CASE("memory_pool: config construction succeeds")
{
    alignas(std::size_t) static std::uint8_t buf[sizeof(std::size_t) * 8];
    const osal::memory_pool_config cfg{buf, sizeof(buf), sizeof(std::size_t), 8, "cfg_test"};
    osal::memory_pool pool{cfg};
    CHECK(pool.valid());
}

// ---------------------------------------------------------------------------
// block_size() accessor
// ---------------------------------------------------------------------------

TEST_CASE("memory_pool: block_size returns configured size")
{
    alignas(64) static std::uint8_t buf[64 * 4];
    osal::memory_pool pool{buf, sizeof(buf), 64, 4};
    REQUIRE(pool.valid());
    CHECK(pool.block_size() == 64U);
}

// ---------------------------------------------------------------------------
// available() initial count
// ---------------------------------------------------------------------------

TEST_CASE("memory_pool: available equals block_count at start")
{
    alignas(32) static std::uint8_t buf[32 * 4];
    osal::memory_pool pool{buf, sizeof(buf), 32, 4};
    REQUIRE(pool.valid());
    CHECK(pool.available() == 4U);
}

// ---------------------------------------------------------------------------
// allocate() basic
// ---------------------------------------------------------------------------

TEST_CASE("memory_pool: allocate returns non-null and decrements available")
{
    alignas(32) static std::uint8_t buf[32 * 4];
    osal::memory_pool pool{buf, sizeof(buf), 32, 4};
    REQUIRE(pool.valid());

    void* blk = pool.allocate();
    CHECK(blk != nullptr);
    CHECK(pool.available() == 3U);

    REQUIRE(pool.deallocate(blk).ok());
    CHECK(pool.available() == 4U);
}

// ---------------------------------------------------------------------------
// Exhaust the pool — allocate returns nullptr when empty
// ---------------------------------------------------------------------------

TEST_CASE("memory_pool: allocate returns nullptr when exhausted")
{
    alignas(32) static std::uint8_t buf[32 * 2];
    osal::memory_pool pool{buf, sizeof(buf), 32, 2};
    REQUIRE(pool.valid());

    void* b1 = pool.allocate();
    void* b2 = pool.allocate();
    REQUIRE(b1 != nullptr);
    REQUIRE(b2 != nullptr);
    CHECK(pool.available() == 0U);

    void* b3 = pool.allocate();
    CHECK(b3 == nullptr);

    pool.deallocate(b1);
    pool.deallocate(b2);
}

// ---------------------------------------------------------------------------
// All blocks are distinct and writable
// ---------------------------------------------------------------------------

TEST_CASE("memory_pool: all blocks are distinct and writable")
{
    constexpr std::size_t kCount = 8U;
    constexpr std::size_t kBlockSize = 64U;
    alignas(kBlockSize) static std::uint8_t buf[kBlockSize * kCount];
    osal::memory_pool pool{buf, sizeof(buf), kBlockSize, kCount};
    REQUIRE(pool.valid());

    void* blocks[kCount];
    for (std::size_t i = 0; i < kCount; ++i)
    {
        blocks[i] = pool.allocate();
        REQUIRE(blocks[i] != nullptr);
        // Write a sentinel byte pattern.
        std::memset(blocks[i], static_cast<int>(i + 1), kBlockSize);
    }

    // Verify blocks don't overlap by checking sentinel values intact.
    for (std::size_t i = 0; i < kCount; ++i)
    {
        auto* p = static_cast<const std::uint8_t*>(blocks[i]);
        bool ok = true;
        for (std::size_t j = 0; j < kBlockSize; ++j)
        {
            if (p[j] != static_cast<std::uint8_t>(i + 1))
            {
                ok = false;
                break;
            }
        }
        CHECK(ok);
    }

    for (std::size_t i = 0; i < kCount; ++i)
    {
        pool.deallocate(blocks[i]);
    }
    CHECK(pool.available() == kCount);
}

// ---------------------------------------------------------------------------
// allocate_for — returns block within timeout
// ---------------------------------------------------------------------------

TEST_CASE("memory_pool: allocate_for succeeds when block is available")
{
    alignas(32) static std::uint8_t buf[32];
    osal::memory_pool pool{buf, sizeof(buf), 32, 1};
    REQUIRE(pool.valid());

    void* blk = pool.allocate_for(osal::milliseconds{100});
    CHECK(blk != nullptr);
    pool.deallocate(blk);
}

// ---------------------------------------------------------------------------
// allocate_for — times out when pool is exhausted
// ---------------------------------------------------------------------------

TEST_CASE("memory_pool: allocate_for times out when pool is empty")
{
    alignas(32) static std::uint8_t buf[32];
    osal::memory_pool pool{buf, sizeof(buf), 32, 1};
    REQUIRE(pool.valid());

    void* b1 = pool.allocate();
    REQUIRE(b1 != nullptr);

    const auto t0 = osal::monotonic_clock::now();
    void* blk = pool.allocate_for(osal::milliseconds{40});
    const auto elapsed = std::chrono::duration_cast<osal::milliseconds>(osal::monotonic_clock::now() - t0);

    CHECK(blk == nullptr);
    CHECK(elapsed.count() >= 30);

    pool.deallocate(b1);
}

// ---------------------------------------------------------------------------
// deallocate — reuse after free
// ---------------------------------------------------------------------------

TEST_CASE("memory_pool: freed block can be reallocated")
{
    alignas(32) static std::uint8_t buf[32];
    osal::memory_pool pool{buf, sizeof(buf), 32, 1};
    REQUIRE(pool.valid());

    void* b1 = pool.allocate();
    REQUIRE(b1 != nullptr);
    CHECK(pool.available() == 0U);

    pool.deallocate(b1);
    CHECK(pool.available() == 1U);

    void* b2 = pool.allocate();
    CHECK(b2 != nullptr);
    CHECK(pool.available() == 0U);
    pool.deallocate(b2);
}

// ---------------------------------------------------------------------------
// Cross-thread: producer frees, consumer gets block via allocate_for
// ---------------------------------------------------------------------------

TEST_CASE("memory_pool: allocate_for unblocks when block is freed by another thread")
{
    alignas(32) static std::uint8_t buf[32];
    static osal::memory_pool pool{buf, sizeof(buf), 32, 1};
    static volatile void* freed_block = nullptr;

    // Pre-allocate the only block.
    void* held = pool.allocate();
    REQUIRE(held != nullptr);
    freed_block = nullptr;

    struct ctx_t { void* block; };
    static ctx_t ctx{held};

    // Releaser thread — frees after 50 ms.
    auto releaser = [](void* arg) {
        auto* c = static_cast<ctx_t*>(arg);
        osal::thread::sleep_for(osal::milliseconds{50});
        freed_block = c->block;
        pool.deallocate(c->block);
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread t;
    osal::thread_config cfg{};
    cfg.entry       = releaser;
    cfg.arg         = &ctx;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "releaser";
    REQUIRE(t.create(cfg).ok());

    void* blk = pool.allocate_for(osal::milliseconds{500});
    CHECK(blk != nullptr);

    REQUIRE(t.join().ok());
    if (blk) pool.deallocate(blk);
}
