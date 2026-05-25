// SPDX-License-Identifier: Apache-2.0
/// @file test_wait_set.cpp
/// @brief Tests for osal::wait_set.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

#include <unistd.h>  // pipe(), close()

static_assert(osal::wait_set::is_supported == osal::active_capabilities::has_wait_set);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("wait_set: construction succeeds")
{
    osal::wait_set ws;
    CHECK(ws.valid());
}

// ---------------------------------------------------------------------------
// Backend-gated tests (only when has_wait_set == true)
// ---------------------------------------------------------------------------

TEST_CASE("wait_set: add returns ok when supported")
{
    if constexpr (!osal::active_capabilities::has_wait_set)
    {
        MESSAGE("Skipped — backend does not support wait_set");
        return;
    }

    osal::wait_set ws;
    REQUIRE(ws.valid());

    // Use a dummy fd (e.g. STDIN_FILENO on Linux/POSIX).
    auto r = ws.add(0, osal::wait_events::readable);
    CHECK(r.ok());
}

TEST_CASE("wait_set: remove returns ok when supported")
{
    if constexpr (!osal::active_capabilities::has_wait_set)
    {
        MESSAGE("Skipped — backend does not support wait_set");
        return;
    }

    osal::wait_set ws;
    REQUIRE(ws.valid());

    REQUIRE(ws.add(0, osal::wait_events::readable).ok());
    auto r = ws.remove(0);
    CHECK(r.ok());
}

TEST_CASE("wait_set: wait times out with no events")
{
    if constexpr (!osal::active_capabilities::has_wait_set)
    {
        MESSAGE("Skipped — backend does not support wait_set");
        return;
    }

    osal::wait_set ws;
    REQUIRE(ws.valid());

    // Use a pipe; the read end has no data pending — wait must time out.
    // Do NOT use fd 0 (stdin): in CI stdin is /dev/null or a closed pipe,
    // both of which are immediately readable (EOF), causing a false wakeup.
    int pipefd[2]{-1, -1};
    REQUIRE(::pipe(pipefd) == 0);
    REQUIRE(ws.add(pipefd[0], osal::wait_events::readable).ok());

    int         ready[4]{};
    std::size_t n_ready = 99;
    auto        r       = ws.wait(ready, 4, n_ready, osal::milliseconds{20});

    ::close(pipefd[0]);
    ::close(pipefd[1]);

    // Timeout is expected — nothing written to the pipe.
    CHECK_FALSE(r.ok());
    CHECK(n_ready == 0);
}

// ---------------------------------------------------------------------------
// Unsupported backend
// ---------------------------------------------------------------------------

TEST_CASE("wait_set: unsupported backend returns not_supported")
{
    if constexpr (osal::active_capabilities::has_wait_set)
    {
        MESSAGE("Skipped — backend supports wait_set, nothing to test here");
        return;
    }

    osal::wait_set ws;
    REQUIRE(ws.valid());

    CHECK(ws.add(0).code() == osal::error_code::not_supported);
    CHECK(ws.remove(0).code() == osal::error_code::not_supported);

    int         ready[4]{};
    std::size_t n_ready = 99;
    auto        r       = ws.wait(ready, 4, n_ready, osal::milliseconds{10});
    CHECK(r.code() == osal::error_code::not_supported);
    CHECK(n_ready == 0);
}
