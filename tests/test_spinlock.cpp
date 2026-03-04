// SPDX-License-Identifier: Apache-2.0
/// @file test_spinlock.cpp
/// @brief Tests for osal::spinlock.
///
/// On backends where has_spinlock == false every operation must return
/// error_code::not_supported (or false for try_lock).  On backends where
/// has_spinlock == true (currently only Zephyr) the native primitive is
/// exercised.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("spinlock: construction")
{
    osal::spinlock sl;
    if constexpr (osal::active_capabilities::has_spinlock)
    {
        CHECK(sl.valid());
    }
    else
    {
        // On stub backends valid() returns false (create returns not_supported)
        CHECK_FALSE(sl.valid());
    }
}

// ---------------------------------------------------------------------------
// Lock / unlock round-trip
// ---------------------------------------------------------------------------

TEST_CASE("spinlock: lock and unlock")
{
    osal::spinlock sl;
    if constexpr (osal::active_capabilities::has_spinlock)
    {
        REQUIRE(sl.valid());
        CHECK(sl.lock().ok());
        sl.unlock();  // must not crash
    }
    else
    {
        CHECK(sl.lock() == osal::error_code::not_supported);
        sl.unlock();  // must be a no-op, not crash
    }
}

// ---------------------------------------------------------------------------
// try_lock
// ---------------------------------------------------------------------------

TEST_CASE("spinlock: try_lock")
{
    osal::spinlock sl;
    if constexpr (osal::active_capabilities::has_spinlock)
    {
        REQUIRE(sl.valid());
        CHECK(sl.try_lock());
        sl.unlock();
    }
    else
    {
        CHECK_FALSE(sl.try_lock());
    }
}

// ---------------------------------------------------------------------------
// RAII lock_guard
// ---------------------------------------------------------------------------

TEST_CASE("spinlock: lock_guard acquires and releases")
{
    osal::spinlock sl;
    // On both supported and stub backends, lock_guard must not crash.
    {
        osal::spinlock::lock_guard lg{sl};
        // lock held here (or not_supported path — still safe)
    }
    // After destructor the lock must be released; a second guard must work.
    {
        osal::spinlock::lock_guard lg2{sl};
        (void)lg2;
    }
}

// ---------------------------------------------------------------------------
// Multiple sequential lock/unlock cycles
// ---------------------------------------------------------------------------

TEST_CASE("spinlock: sequential lock/unlock cycles")
{
    osal::spinlock sl;
    for (int i = 0; i < 5; ++i)
    {
        if constexpr (osal::active_capabilities::has_spinlock)
        {
            CHECK(sl.lock().ok());
        }
        else
        {
            CHECK(sl.lock() == osal::error_code::not_supported);
        }
        sl.unlock();
    }
}
