// SPDX-License-Identifier: Apache-2.0
/// @file test_stream_buffer.cpp
/// @brief Tests for osal::stream_buffer<N, TriggerLevel>.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

#include <cstdint>
#include <cstring>

TEST_CASE("stream_buffer: construction succeeds")
{
    osal::stream_buffer<64> sb;
    CHECK(sb.valid());
}

TEST_CASE("stream_buffer: starts empty")
{
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());
    CHECK(sb.empty());
    CHECK_FALSE(sb.full());
    CHECK(sb.available() == 0U);
    CHECK(sb.free_space() == 64U);
}

TEST_CASE("stream_buffer: send and receive single byte")
{
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());

    const std::uint8_t tx = 0xABU;
    REQUIRE(sb.try_send(&tx, 1U));
    CHECK(sb.available() == 1U);

    std::uint8_t rx = 0U;
    const std::size_t n = sb.try_receive(&rx, sizeof(rx));
    CHECK(n == 1U);
    CHECK(rx == 0xABU);
    CHECK(sb.empty());
}

TEST_CASE("stream_buffer: send and receive multi-byte block")
{
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());

    const char tx[] = "Hello, OSAL!";
    const std::size_t tx_len = sizeof(tx) - 1U;  // exclude NUL

    REQUIRE(sb.try_send(tx, tx_len));
    CHECK(sb.available() == tx_len);

    char rx[32]{};
    const std::size_t n = sb.try_receive(rx, sizeof(rx));
    CHECK(n == tx_len);
    CHECK(std::memcmp(tx, rx, tx_len) == 0);
    CHECK(sb.empty());
}

TEST_CASE("stream_buffer: try_receive on empty returns 0")
{
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());

    std::uint8_t buf[8]{};
    CHECK(sb.try_receive(buf, sizeof(buf)) == 0U);
}

TEST_CASE("stream_buffer: free_space decreases after send")
{
    osal::stream_buffer<16> sb;
    REQUIRE(sb.valid());

    const std::uint8_t data[8]{};
    REQUIRE(sb.try_send(data, 8U));
    CHECK(sb.available() == 8U);
    CHECK(sb.free_space() == 8U);
}

TEST_CASE("stream_buffer: partial receive leaves remainder")
{
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());

    const std::uint8_t tx[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    REQUIRE(sb.try_send(tx, 8U));

    // Read only 4 bytes.
    std::uint8_t rx[4]{};
    const std::size_t n = sb.try_receive(rx, 4U);
    CHECK(n == 4U);
    CHECK(rx[0] == 1U);
    CHECK(rx[3] == 4U);
    CHECK(sb.available() == 4U);

    // Read the rest.
    std::uint8_t rx2[4]{};
    const std::size_t n2 = sb.try_receive(rx2, 4U);
    CHECK(n2 == 4U);
    CHECK(rx2[0] == 5U);
    CHECK(rx2[3] == 8U);
    CHECK(sb.empty());
}

TEST_CASE("stream_buffer: full detection")
{
    osal::stream_buffer<8> sb;
    REQUIRE(sb.valid());

    const std::uint8_t data[8]{};
    REQUIRE(sb.try_send(data, 8U));
    CHECK(sb.full());
    CHECK_FALSE(sb.empty());

    // Cannot add another byte when full.
    const std::uint8_t one = 0U;
    CHECK_FALSE(sb.try_send(&one, 1U));
}

TEST_CASE("stream_buffer: reset clears all data")
{
    osal::stream_buffer<32> sb;
    REQUIRE(sb.valid());

    const std::uint8_t data[16]{};
    REQUIRE(sb.try_send(data, 16U));
    CHECK(sb.available() == 16U);

    REQUIRE(sb.reset().ok());
    CHECK(sb.empty());
    CHECK(sb.available() == 0U);
    CHECK(sb.free_space() == 32U);
}

TEST_CASE("stream_buffer: send and receive in multiple small chunks")
{
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());

    for (std::uint8_t i = 0U; i < 16U; ++i)
    {
        REQUIRE(sb.try_send(&i, 1U));
    }
    CHECK(sb.available() == 16U);

    for (std::uint8_t i = 0U; i < 16U; ++i)
    {
        std::uint8_t rx = 0xFFU;
        const std::size_t n = sb.try_receive(&rx, 1U);
        CHECK(n == 1U);
        CHECK(rx == i);
    }
    CHECK(sb.empty());
}

TEST_CASE("stream_buffer: trigger level 1 — receive unblocks on any byte")
{
    // Default TriggerLevel = 1: try_receive must return at least 1 byte when
    // any data is present.
    osal::stream_buffer<64, 1U> sb;
    REQUIRE(sb.valid());

    const std::uint8_t byte = 42U;
    REQUIRE(sb.try_send(&byte, 1U));

    std::uint8_t rx = 0U;
    CHECK(sb.try_receive(&rx, 1U) == 1U);
    CHECK(rx == 42U);
}

TEST_CASE("stream_buffer: cross-thread send/receive")
{
    static osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());

    auto producer = [](void*)
    {
        for (std::uint8_t i = 1U; i <= 8U; ++i)
        {
            (void)sb.send(&i, 1U);
        }
    };

    constexpr std::size_t kStackSize = 65536U;
    alignas(16) static std::uint8_t stack[kStackSize];

    osal::thread_config cfg{};
    cfg.entry       = producer;
    cfg.arg         = nullptr;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = kStackSize;
    cfg.name        = "sb_prod";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());

    for (std::uint8_t expected = 1U; expected <= 8U; ++expected)
    {
        std::uint8_t rx = 0U;
        const std::size_t n = sb.receive(&rx, 1U);
        CHECK(n == 1U);
        CHECK(rx == expected);
    }

    (void)t.join();
}

// ---------------------------------------------------------------------------
// ISR send / receive (task-context callable)
// ---------------------------------------------------------------------------

TEST_CASE("stream_buffer: send_isr and receive_isr round-trip")
{
    // send_isr / receive_isr are non-blocking.  On backends with
    // has_isr_stream_buffer == true they use the dedicated ISR API; on others
    // they fall back to the non-blocking semaphore path.  Either way the API
    // must be callable from task context and must produce correct data.
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());

    const std::uint8_t tx[4] = {0x10U, 0x20U, 0x30U, 0x40U};
    REQUIRE(sb.send_isr(tx, sizeof(tx)).ok());
    CHECK(sb.available() == sizeof(tx));

    std::uint8_t rx[4]{};
    const std::size_t n = sb.receive_isr(rx, sizeof(rx));
    CHECK(n == sizeof(tx));
    CHECK(std::memcmp(tx, rx, sizeof(tx)) == 0);
    CHECK(sb.empty());
}

TEST_CASE("stream_buffer: send_isr single byte")
{
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());

    const std::uint8_t byte = 0xBEU;
    REQUIRE(sb.send_isr(&byte, 1U).ok());
    CHECK(sb.available() == 1U);

    std::uint8_t rx = 0U;
    CHECK(sb.receive_isr(&rx, 1U) == 1U);
    CHECK(rx == 0xBEU);
}

TEST_CASE("stream_buffer: receive_isr on empty buffer returns 0")
{
    osal::stream_buffer<64> sb;
    REQUIRE(sb.valid());

    std::uint8_t buf[8]{};
    CHECK(sb.receive_isr(buf, sizeof(buf)) == 0U);
}

TEST_CASE("stream_buffer: send_isr on full buffer fails")
{
    osal::stream_buffer<4> sb;
    REQUIRE(sb.valid());

    // Fill the buffer exactly.
    const std::uint8_t data[4]{};
    REQUIRE(sb.send_isr(data, sizeof(data)).ok());
    CHECK(sb.full());

    // A further non-blocking write must fail — no space available.
    const std::uint8_t one = 0U;
    CHECK_FALSE(sb.send_isr(&one, 1U).ok());
}

// ---------------------------------------------------------------------------
// Trigger level > 1
// ---------------------------------------------------------------------------

TEST_CASE("stream_buffer: trigger level 4 — try_receive below threshold returns 0")
{
    // TriggerLevel=4: try_receive must return 0 while fewer than 4 bytes are present.
    osal::stream_buffer<64, 4U> sb;
    REQUIRE(sb.valid());

    // Write 3 bytes — deliberately below TriggerLevel.
    const std::uint8_t three[3] = {1U, 2U, 3U};
    REQUIRE(sb.try_send(three, sizeof(three)));
    CHECK(sb.available() == 3U);

    // try_receive must not return data (below trigger threshold).
    std::uint8_t rx[8]{};
    CHECK(sb.try_receive(rx, sizeof(rx)) == 0U);

    // The 3 bytes must still be pending.
    CHECK(sb.available() == 3U);
}

TEST_CASE("stream_buffer: trigger level 4 — try_receive at threshold returns data")
{
    osal::stream_buffer<64, 4U> sb;
    REQUIRE(sb.valid());

    // Write exactly TriggerLevel bytes.
    const std::uint8_t four[4] = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
    REQUIRE(sb.try_send(four, sizeof(four)));
    CHECK(sb.available() == 4U);

    // Now try_receive must return all 4 bytes.
    std::uint8_t rx[8]{};
    const std::size_t n = sb.try_receive(rx, sizeof(rx));
    CHECK(n == 4U);
    CHECK(rx[0] == 0xAAU);
    CHECK(rx[3] == 0xDDU);
    CHECK(sb.empty());
}

TEST_CASE("stream_buffer: trigger level 4 — try_receive above threshold returns data")
{
    osal::stream_buffer<64, 4U> sb;
    REQUIRE(sb.valid());

    // Write more bytes than TriggerLevel.
    const std::uint8_t six[6] = {1U, 2U, 3U, 4U, 5U, 6U};
    REQUIRE(sb.try_send(six, sizeof(six)));
    CHECK(sb.available() == 6U);

    // All available bytes are returned.
    std::uint8_t rx[8]{};
    const std::size_t n = sb.try_receive(rx, sizeof(rx));
    CHECK(n == 6U);
    CHECK(rx[0] == 1U);
    CHECK(rx[5] == 6U);
    CHECK(sb.empty());
}
