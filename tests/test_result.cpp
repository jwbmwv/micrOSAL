// SPDX-License-Identifier: Apache-2.0
/// @file test_result.cpp
/// @brief Tests for osal::result and osal::error_code.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/error.hpp>

// ---------------------------------------------------------------------------
// error_code values are distinct
// ---------------------------------------------------------------------------

TEST_CASE("error_code: all values are distinct")
{
    // Compile-time check that each named code has its expected integral value.
    CHECK(static_cast<int>(osal::error_code::ok) == 0);
    CHECK(static_cast<int>(osal::error_code::timeout) == 1);
    CHECK(static_cast<int>(osal::error_code::would_block) == 2);
    CHECK(static_cast<int>(osal::error_code::invalid_argument) == 3);
    CHECK(static_cast<int>(osal::error_code::not_supported) == 4);
    CHECK(static_cast<int>(osal::error_code::out_of_resources) == 5);
    CHECK(static_cast<int>(osal::error_code::permission_denied) == 6);
    CHECK(static_cast<int>(osal::error_code::already_exists) == 7);
    CHECK(static_cast<int>(osal::error_code::not_initialized) == 8);
    CHECK(static_cast<int>(osal::error_code::overflow) == 9);
    CHECK(static_cast<int>(osal::error_code::underflow) == 10);
    CHECK(static_cast<int>(osal::error_code::deadlock_detected) == 11);
    CHECK(static_cast<int>(osal::error_code::not_owner) == 12);
    CHECK(static_cast<int>(osal::error_code::isr_invalid) == 13);
    CHECK(static_cast<int>(osal::error_code::unknown) == 255);
}

// ---------------------------------------------------------------------------
// result default construction
// ---------------------------------------------------------------------------

TEST_CASE("result: default is ok")
{
    osal::result r;
    CHECK(r.ok());
    CHECK(r.code() == osal::error_code::ok);
    CHECK(static_cast<bool>(r));
}

// ---------------------------------------------------------------------------
// result from error_code
// ---------------------------------------------------------------------------

TEST_CASE("result: constructed from error_code")
{
    osal::result r{osal::error_code::timeout};
    CHECK_FALSE(r.ok());
    CHECK(r.code() == osal::error_code::timeout);
    CHECK_FALSE(static_cast<bool>(r));
}

TEST_CASE("result: constructed from error_code::ok is ok")
{
    osal::result r{osal::error_code::ok};
    CHECK(r.ok());
    CHECK(r.code() == osal::error_code::ok);
    CHECK(static_cast<bool>(r));
}

// ---------------------------------------------------------------------------
// Implicit conversion from error_code
// ---------------------------------------------------------------------------

TEST_CASE("result: implicit conversion from error_code")
{
    // Verifies that error_code converts implicitly (non-explicit ctor).
    auto fn = []() -> osal::result { return osal::error_code::would_block; };
    auto r  = fn();
    CHECK_FALSE(r.ok());
    CHECK(r.code() == osal::error_code::would_block);
}

// ---------------------------------------------------------------------------
// operator== / operator!=
// ---------------------------------------------------------------------------

TEST_CASE("result: operator== with result")
{
    osal::result a;
    osal::result b;
    CHECK(a == b);

    osal::result c{osal::error_code::timeout};
    CHECK_FALSE(a == c);
}

TEST_CASE("result: operator!= with result")
{
    osal::result a;
    osal::result b{osal::error_code::timeout};
    CHECK(a != b);
    CHECK_FALSE(a != osal::result{});
}

TEST_CASE("result: operator== with error_code")
{
    osal::result a;
    CHECK(a == osal::error_code::ok);
    CHECK_FALSE(a == osal::error_code::timeout);

    osal::result b{osal::error_code::not_supported};
    CHECK(b == osal::error_code::not_supported);
    CHECK_FALSE(b == osal::error_code::ok);
}

TEST_CASE("result: operator!= with error_code")
{
    osal::result a;
    CHECK(a != osal::error_code::timeout);
    CHECK_FALSE(a != osal::error_code::ok);
}

// ---------------------------------------------------------------------------
// ok() free function
// ---------------------------------------------------------------------------

TEST_CASE("ok() free function returns ok result")
{
    auto r = osal::ok();
    CHECK(r.ok());
    CHECK(r.code() == osal::error_code::ok);
}
