// SPDX-License-Identifier: Apache-2.0
/// @file test_embedded_adapters.cpp
/// @brief Value-copy, capacity, peek, and persistent-flag adapter regressions.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <array>
#include <atomic>
#include <cstdint>

namespace
{

template<std::size_t Size>
void check_queue_value_copy()
{
    struct guarded_item
    {
        std::uint8_t                   before{0xa5};
        std::array<std::uint8_t, Size> value{};
        std::uint8_t                   after{0x5a};
    };
    struct guarded_storage
    {
        std::uint8_t before{0xa5};
        alignas(std::max_align_t) std::array<std::uint8_t, Size * 3U> bytes{};
        std::uint8_t after{0x5a};
    } storage;

    osal::active_traits::queue_handle_t handle{};
    REQUIRE(osal_queue_create(&handle, storage.bytes.data(), Size, 3U).ok());
    guarded_item source;
    source.value.fill(0x37);
    REQUIRE(osal_queue_send(&handle, source.value.data(), osal::NO_WAIT).ok());
    source.value.fill(0x19);
    REQUIRE(osal_queue_send(&handle, source.value.data(), osal::NO_WAIT).ok());
    source.value.fill(0x82);
    REQUIRE(osal_queue_send(&handle, source.value.data(), osal::NO_WAIT).ok());
    CHECK_FALSE(osal_queue_send(&handle, source.value.data(), osal::NO_WAIT).ok());
    CHECK(osal_queue_count(&handle) == 3U);
    CHECK(osal_queue_free(&handle) == 0U);

    guarded_item destination;
    const auto   peek = osal_queue_peek(&handle, destination.value.data(), osal::NO_WAIT);
    CHECK((peek.ok() || peek.code() == osal::error_code::not_supported));
    CHECK(osal_queue_count(&handle) == 3U);
    if (peek.ok())
    {
        for (const auto byte : destination.value)
        {
            CHECK(byte == 0x37);
        }
    }
    else
    {
        for (const auto byte : destination.value)
        {
            CHECK(byte == 0U);
        }
    }
    for (const std::uint8_t expected : {0x37, 0x19, 0x82})
    {
        REQUIRE(osal_queue_receive(&handle, destination.value.data(), osal::WAIT_FOREVER).ok());
        for (const auto byte : destination.value)
        {
            CHECK(byte == expected);
        }
        CHECK(destination.before == 0xa5);
        CHECK(destination.after == 0x5a);
    }
    CHECK(source.before == 0xa5);
    CHECK(source.after == 0x5a);
    CHECK(storage.before == 0xa5);
    CHECK(storage.after == 0x5a);
    CHECK(osal_queue_count(&handle) == 0U);
    CHECK(osal_queue_free(&handle) == 3U);
    REQUIRE(osal_queue_destroy(&handle).ok());
}

}  // namespace

TEST_CASE("embedded adapters: exact byte-sized queue copies and bounded capacity")
{
    check_queue_value_copy<1>();
    check_queue_value_copy<3>();
#if !defined(OSAL_BACKEND_CMSIS_RTOS)
    check_queue_value_copy<5>();
#endif
}

TEST_CASE("embedded adapters: pre-set flags and selective clearing")
{
    osal::event_flags flags;
    REQUIRE(flags.valid());
    REQUIRE(flags.set(0x0f).ok());
    REQUIRE(flags.clear(0x02).ok());
    CHECK(flags.get() == 0x0d);
    osal::event_bits_t actual{};
    REQUIRE(flags.wait_any(0x03, &actual, false, osal::milliseconds{0}).ok());
    CHECK((actual & 0x03) == 0x01);
    CHECK(flags.get() == 0x0d);
    REQUIRE(flags.wait_all(0x0c, &actual, true, osal::milliseconds{0}).ok());
    CHECK((actual & 0x0c) == 0x0c);
    CHECK(flags.get() == 0x01);
    CHECK_FALSE(flags.wait_all(0x03, &actual, false, osal::milliseconds{0}).ok());
    CHECK(flags.get() == 0x01);
}

#if defined(OSAL_BACKEND_EMBOS) || defined(OSAL_BACKEND_CHIBIOS)
namespace
{

struct callback_destroy_state
{
    osal::active_traits::timer_handle_t handle{};
    std::atomic<unsigned>               calls{0U};
    std::atomic<bool>                   preserved{true};
};

void attempt_callback_destroy(void* arg)
{
    auto*      state     = static_cast<callback_destroy_state*>(arg);
    void*      native    = state->handle.native;
    const auto destroyed = osal_timer_destroy(&state->handle);
    if (destroyed.code() != osal::error_code::not_supported || state->handle.native != native)
    {
        state->preserved.store(false);
    }
    state->calls.fetch_add(1U);
}

}  // namespace

TEST_CASE("embedded adapters: callback destruction preserves periodic timer ownership")
{
    callback_destroy_state state;
    REQUIRE(osal_timer_create(&state.handle, "destroy-guard", attempt_callback_destroy, &state,
                              osal::clock_utils::ms_to_ticks(osal::milliseconds{10}), true)
                .ok());
    REQUIRE(osal_timer_start(&state.handle).ok());
    for (unsigned i = 0U; i < 200U && state.calls.load() < 2U; ++i)
    {
        osal::thread::sleep_for(osal::milliseconds{1});
    }
    CHECK(state.calls.load() >= 2U);
    CHECK(state.preserved.load());
    osal::result destroyed{osal::error_code::not_supported};
    for (unsigned i = 0U; i < 200U && !destroyed.ok(); ++i)
    {
        destroyed = osal_timer_destroy(&state.handle);
        if (!destroyed.ok())
        {
            osal::thread::sleep_for(osal::milliseconds{1});
        }
    }
    REQUIRE(destroyed.ok());
    CHECK(state.handle.native == nullptr);
}
#endif
