// SPDX-License-Identifier: Apache-2.0
/// @file test_message_buffer.cpp
/// @brief Tests for osal::message_buffer<N>.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("message_buffer: construction succeeds")
{
    osal::message_buffer<64> mb;
    CHECK(mb.valid());
}

TEST_CASE("message_buffer: starts empty")
{
    osal::message_buffer<64> mb;
    REQUIRE(mb.valid());
    CHECK(mb.empty());
    CHECK(mb.next_message_size() == 0U);
}

// ---------------------------------------------------------------------------
// Basic send / receive
// ---------------------------------------------------------------------------

TEST_CASE("message_buffer: send and receive single byte message")
{
    osal::message_buffer<64> mb;
    REQUIRE(mb.valid());

    const std::uint8_t tx = 0x5AU;
    REQUIRE(mb.try_send(&tx, 1U));
    CHECK_FALSE(mb.empty());
    CHECK(mb.next_message_size() == 1U);

    std::uint8_t rx = 0U;
    const std::size_t n = mb.try_receive(&rx, sizeof(rx));
    CHECK(n == 1U);
    CHECK(rx == 0x5AU);
    CHECK(mb.empty());
}

TEST_CASE("message_buffer: send and receive struct message")
{
    struct Packet
    {
        std::uint16_t id;
        std::uint32_t payload;
    };

    osal::message_buffer<64> mb;
    REQUIRE(mb.valid());

    const Packet tx{0x1234U, 0xDEADBEEFUL};
    REQUIRE(mb.try_send(&tx, sizeof(tx)));
    CHECK(mb.next_message_size() == sizeof(tx));

    Packet rx{};
    const std::size_t n = mb.try_receive(&rx, sizeof(rx));
    REQUIRE(n == sizeof(Packet));
    CHECK(rx.id == 0x1234U);
    CHECK(rx.payload == 0xDEADBEEFUL);
    CHECK(mb.empty());
}

// ---------------------------------------------------------------------------
// FIFO ordering
// ---------------------------------------------------------------------------

TEST_CASE("message_buffer: FIFO ordering of multiple messages")
{
    osal::message_buffer<128> mb;
    REQUIRE(mb.valid());

    for (std::uint8_t i = 1U; i <= 4U; ++i)
    {
        REQUIRE(mb.try_send(&i, 1U));
    }

    for (std::uint8_t expected = 1U; expected <= 4U; ++expected)
    {
        CHECK(mb.next_message_size() == 1U);
        std::uint8_t rx = 0U;
        const std::size_t n = mb.try_receive(&rx, sizeof(rx));
        CHECK(n == 1U);
        CHECK(rx == expected);
    }
    CHECK(mb.empty());
}

// ---------------------------------------------------------------------------
// try_receive on empty
// ---------------------------------------------------------------------------

TEST_CASE("message_buffer: try_receive on empty returns 0")
{
    osal::message_buffer<64> mb;
    REQUIRE(mb.valid());

    std::uint8_t buf[16]{};
    CHECK(mb.try_receive(buf, sizeof(buf)) == 0U);
}

// ---------------------------------------------------------------------------
// Capacity / free_space
// ---------------------------------------------------------------------------

TEST_CASE("message_buffer: free_space decreases after send")
{
    // N=32, kMsgHeaderBytes=2, so max payload per message = 30 bytes.
    // After sending 10-byte payload: ring uses 12 bytes (header + payload).
    osal::message_buffer<32> mb;
    REQUIRE(mb.valid());

    const std::uint8_t data[10]{};
    REQUIRE(mb.try_send(data, sizeof(data)));

    // free_space reflects remaining payload capacity.
    CHECK(mb.free_space() < 32U);
}

TEST_CASE("message_buffer: max_message_size constant is correct")
{
    // kMsgHeaderBytes == sizeof(osal_mb_length_t) == 2 by default.
    CHECK(osal::message_buffer<64>::max_message_size == 64U - kMsgHeaderBytes);
    CHECK(osal::message_buffer<10>::max_message_size == 10U - kMsgHeaderBytes);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST_CASE("message_buffer: reset clears all queued messages")
{
    osal::message_buffer<64> mb;
    REQUIRE(mb.valid());

    const std::uint8_t data[4] = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
    REQUIRE(mb.try_send(data, sizeof(data)));
    CHECK_FALSE(mb.empty());

    REQUIRE(mb.reset().ok());
    CHECK(mb.empty());
    CHECK(mb.next_message_size() == 0U);
}

// ---------------------------------------------------------------------------
// Oversized receive (truncation)
// ---------------------------------------------------------------------------

TEST_CASE("message_buffer: receive truncates excess bytes silently")
{
    osal::message_buffer<64> mb;
    REQUIRE(mb.valid());

    const std::uint8_t tx[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    REQUIRE(mb.try_send(tx, sizeof(tx)));

    // Supply a smaller buffer — only get 4 bytes back; excess discarded.
    std::uint8_t rx[4]{};
    const std::size_t n = mb.try_receive(rx, sizeof(rx));
    CHECK(n == 4U);
    CHECK(rx[0] == 1U);
    CHECK(rx[3] == 4U);
    // Message is consumed; buffer should be empty after truncated read.
    CHECK(mb.empty());
}

// ---------------------------------------------------------------------------
// Variable-length messages
// ---------------------------------------------------------------------------

TEST_CASE("message_buffer: variable-length messages interleaved")
{
    osal::message_buffer<128> mb;
    REQUIRE(mb.valid());

    const std::uint8_t short_msg[2]  = {0x01U, 0x02U};
    const std::uint8_t long_msg[6]   = {0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU};
    const std::uint8_t medium_msg[4] = {0x10U, 0x20U, 0x30U, 0x40U};

    REQUIRE(mb.try_send(short_msg,  sizeof(short_msg)));
    REQUIRE(mb.try_send(long_msg,   sizeof(long_msg)));
    REQUIRE(mb.try_send(medium_msg, sizeof(medium_msg)));

    // Receive and verify each in order.
    std::uint8_t rx[16]{};

    CHECK(mb.next_message_size() == sizeof(short_msg));
    std::size_t n = mb.try_receive(rx, sizeof(rx));
    REQUIRE(n == sizeof(short_msg));
    CHECK(std::memcmp(rx, short_msg, sizeof(short_msg)) == 0);

    CHECK(mb.next_message_size() == sizeof(long_msg));
    n = mb.try_receive(rx, sizeof(rx));
    REQUIRE(n == sizeof(long_msg));
    CHECK(std::memcmp(rx, long_msg, sizeof(long_msg)) == 0);

    CHECK(mb.next_message_size() == sizeof(medium_msg));
    n = mb.try_receive(rx, sizeof(rx));
    REQUIRE(n == sizeof(medium_msg));
    CHECK(std::memcmp(rx, medium_msg, sizeof(medium_msg)) == 0);

    CHECK(mb.empty());
}

// ---------------------------------------------------------------------------
// Cross-thread
// ---------------------------------------------------------------------------

TEST_CASE("message_buffer: cross-thread send/receive")
{
    struct Msg
    {
        std::uint32_t seq;
        std::uint8_t  data[4];
    };

    static osal::message_buffer<256> mb;
    REQUIRE(mb.valid());

    auto producer = [](void*)
    {
        for (std::uint32_t i = 0U; i < 4U; ++i)
        {
            Msg m{i, {static_cast<std::uint8_t>(i), 0U, 0U, 0U}};
            (void)mb.send(&m, sizeof(m));
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
    cfg.name        = "mb_prod";

    osal::thread t;
    REQUIRE(t.create(cfg).ok());

    for (std::uint32_t expected = 0U; expected < 4U; ++expected)
    {
        Msg rx{};
        const std::size_t n = mb.receive(&rx, sizeof(rx));
        CHECK(n == sizeof(Msg));
        CHECK(rx.seq == expected);
        CHECK(rx.data[0] == static_cast<std::uint8_t>(expected));
    }

    (void)t.join();
}
