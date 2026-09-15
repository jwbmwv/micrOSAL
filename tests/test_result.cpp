// SPDX-License-Identifier: Apache-2.0
/// @file test_result.cpp
/// @brief Tests for osal::result and osal::error_code.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/error.hpp>
#include <type_traits>
#include <utility>

namespace
{

template<typename Function>
concept status_success_callback = requires(osal::result status, Function&& function) {
    status.and_then(std::forward<Function>(function));
    status.transform(std::forward<Function>(function));
};

template<typename Function>
concept status_error_callback =
    requires(osal::result status, Function&& function) { status.or_else(std::forward<Function>(function)); };

}  // namespace

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

TEST_CASE("to_underlying: enum conversion")
{
    CHECK(osal::detail::to_underlying(osal::error_code::ok) == 0);
    CHECK(osal::detail::to_underlying(osal::error_code::timeout) == 1);
}

TEST_CASE("bounded_vector: basic storage operations")
{
    osal::detail::bounded_vector<int, 2> vec;
    CHECK(vec.empty());
    CHECK_FALSE(vec.full());
    CHECK(vec.capacity() == 2);

    CHECK(vec.push_back(10));
    CHECK(vec.push_back(20));
    CHECK_FALSE(vec.empty());
    CHECK(vec.full());
    CHECK(vec.size() == 2);
    CHECK_FALSE(vec.push_back(30));  // Excess fails

    int sum = 0;
    for (int val : vec)
    {
        sum += val;
    }
    CHECK(sum == 30);

    vec.clear();
    CHECK(vec.empty());
    CHECK(vec.size() == 0);
}

TEST_CASE("bounded_vector: checked resize preserves contents and initializes new values")
{
    osal::detail::bounded_vector<int, 3> values;
    const int                            first = 0;
    static_assert(std::is_same_v<decltype(values.push_back(first)), bool>);
    static_assert(std::is_same_v<decltype(values.resize(0U)), bool>);
    REQUIRE(values.push_back(first));
    REQUIRE(values.push_back(7));
    REQUIRE(values.resize(3U));
    CHECK(values.full());
    CHECK(values[0] == 0);
    CHECK(values[1] == 7);
    CHECK(values[2] == 0);

    CHECK_FALSE(values.push_back(first));
    CHECK_FALSE(values.resize(4U));
    REQUIRE(values.size() == 3U);
    CHECK(values[1] == 7);

    REQUIRE(values.resize(1U));
    REQUIRE(values.resize(3U));
    const auto& snapshot = values;
    CHECK(snapshot[1] == 0);
    CHECK(snapshot[2] == 0);
    CHECK(snapshot.data() == snapshot.begin());
    CHECK(snapshot.end() - snapshot.begin() == 3);
    values.clear();
    CHECK(values.begin() == values.end());
    REQUIRE(values.resize(1U));
    CHECK(values[0] == 0);
}

TEST_CASE("bounded_vector: zero capacity is always empty and full")
{
    osal::detail::bounded_vector<int, 0> values;
    const int                            item = 1;
    CHECK(values.empty());
    CHECK(values.full());
    CHECK(values.capacity() == 0U);
    CHECK_FALSE(values.push_back(item));
    CHECK_FALSE(values.push_back(1));
    CHECK_FALSE(values.resize(1U));
    CHECK(values.resize(0U));
    values.clear();
    CHECK(values.size() == 0U);
    CHECK(values.begin() == values.end());
}

TEST_CASE("result: status callback constraints and noexcept")
{
    const auto status_step      = []() noexcept -> osal::result { return {}; };
    const auto error_step       = [](osal::error_code code) noexcept -> osal::result { return code; };
    const auto throwing_step    = []() noexcept(false) -> osal::result { return {}; };
    const auto throwing_handler = [](osal::error_code code) noexcept(false) -> osal::result { return code; };
    static_assert(status_success_callback<decltype(status_step)>);
    static_assert(status_error_callback<decltype(error_step)>);
    static_assert(!status_success_callback<decltype([] { return 42; })>);
    static_assert(!status_success_callback<decltype([] {})>);
    static_assert(!status_success_callback<decltype([] { return osal::error_code::ok; })>);
    static_assert(!status_error_callback<decltype([](osal::error_code) { return 42; })>);
    static_assert(!status_error_callback<decltype([](osal::error_code) {})>);
    static_assert(noexcept(osal::ok().and_then(status_step)));
    static_assert(noexcept(osal::ok().transform(status_step)));
    static_assert(noexcept(osal::ok().or_else(error_step)));
    static_assert(!noexcept(osal::ok().and_then(throwing_step)));
    static_assert(!noexcept(osal::ok().transform(throwing_step)));
    static_assert(!noexcept(osal::ok().or_else(throwing_handler)));
    CHECK(osal::ok().and_then(status_step).ok());
}

TEST_CASE("result: status-only and_then, transform, or_else")
{
    // and_then on ok
    osal::result ok_res;
    bool         called = false;
    auto         res1   = ok_res.and_then(
        [&]()
        {
            called = true;
            return osal::result{osal::error_code::timeout};
        });
    CHECK(called);
    CHECK(res1.code() == osal::error_code::timeout);

    // and_then on error (short-circuits)
    bool         err_called = false;
    osal::result err_res{osal::error_code::invalid_argument};
    auto         res_skip = err_res.and_then(
        [&]()
        {
            err_called = true;
            return osal::result{osal::error_code::ok};
        });
    CHECK_FALSE(err_called);
    CHECK(res_skip.code() == osal::error_code::invalid_argument);

    // transform on ok
    bool xform_called = false;
    auto res_xform    = ok_res.transform(
        [&]()
        {
            xform_called = true;
            return osal::result{osal::error_code::already_exists};
        });
    CHECK(xform_called);
    CHECK(res_xform.code() == osal::error_code::already_exists);

    xform_called                 = false;
    const auto skipped_transform = err_res.transform(
        [&]() noexcept -> osal::result
        {
            xform_called = true;
            return {};
        });
    CHECK_FALSE(xform_called);
    CHECK(skipped_transform.code() == osal::error_code::invalid_argument);

    // or_else on error
    bool or_called = false;
    auto res2      = err_res.or_else(
        [&](osal::error_code c)
        {
            or_called = true;
            CHECK(c == osal::error_code::invalid_argument);
            return osal::result{osal::error_code::ok};
        });
    CHECK(or_called);
    CHECK(res2.ok());

    // or_else on ok (short-circuits)
    bool ok_or_called = false;
    auto res_ok_or    = ok_res.or_else(
        [&](osal::error_code)
        {
            ok_or_called = true;
            return osal::result{osal::error_code::timeout};
        });
    CHECK_FALSE(ok_or_called);
    CHECK(res_ok_or.ok());
}

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
TEST_CASE("result: to_expected conversion")
{
    osal::result ok_res;
    auto         exp_ok = ok_res.to_expected();
    CHECK(exp_ok.has_value());

    osal::result err_res{osal::error_code::timeout};
    auto         exp_err = err_res.to_expected();
    REQUIRE_FALSE(exp_err.has_value());
    CHECK(exp_err.error() == osal::error_code::timeout);
}
#endif

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
TEST_CASE("result: expected value chains preserve failure without invoking callbacks")
{
    const osal::result failure{osal::error_code::invalid_argument};
    bool               called = false;
    const auto         next   = [&called]() noexcept -> std::expected<osal::error_code, osal::error_code>
    {
        called = true;
        return osal::error_code::ok;
    };
    static_assert(!status_success_callback<decltype(next)>);
    const auto propagated = failure.to_expected().and_then(next);
    CHECK_FALSE(called);
    REQUIRE_FALSE(propagated.has_value());
    CHECK(propagated.error() == osal::error_code::invalid_argument);

    const auto success = osal::ok().to_expected().and_then(next);
    CHECK(called);
    REQUIRE(success.has_value());
    CHECK(*success == osal::error_code::ok);

    const auto transformed = osal::ok().to_expected().transform([] { return 42; });
    REQUIRE(transformed.has_value());
    CHECK(*transformed == 42);

    called             = false;
    const auto skipped = failure.to_expected().transform([&called] { called = true; });
    CHECK_FALSE(called);
    REQUIRE_FALSE(skipped.has_value());
    CHECK(skipped.error() == osal::error_code::invalid_argument);
}
#endif
