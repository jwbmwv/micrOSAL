// SPDX-License-Identifier: Apache-2.0
/// @file test_irq_mask_guard.cpp
/// @brief Tests for osal::irq_mask_guard.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

static_assert(osal::irq_mask_guard::is_supported ==
              osal::supports_requirement<osal::support_requirement::irq_mask_guard>);

TEST_CASE("irq_mask_guard: construction reflects support")
{
    osal::irq_mask_guard guard;

    if constexpr (osal::irq_mask_guard::is_supported)
    {
        CHECK(guard.active());
    }
    else
    {
        CHECK_FALSE(guard.active());
    }
}

TEST_CASE("irq_mask_guard: nested scopes are safe")
{
    osal::irq_mask_guard outer;

    if constexpr (osal::irq_mask_guard::is_supported)
    {
        CHECK(outer.active());
        {
            osal::irq_mask_guard inner;
            CHECK(inner.active());
            CHECK(outer.active());
        }
        CHECK(outer.active());
    }
    else
    {
        CHECK_FALSE(outer.active());
        {
            osal::irq_mask_guard inner;
            CHECK_FALSE(inner.active());
        }
    }
}

TEST_CASE("irq_mask_guard: repeated acquire release cycles")
{
    for (int i = 0; i < 5; ++i)
    {
        osal::irq_mask_guard guard;
        if constexpr (osal::irq_mask_guard::is_supported)
        {
            CHECK(guard.active());
        }
        else
        {
            CHECK_FALSE(guard.active());
        }
    }
}
