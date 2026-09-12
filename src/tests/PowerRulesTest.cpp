// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "PowerRules.h"

#include <array>
#include <cmath>
#include <limits>

TEST(PowerRules_signed_delta_clamps_before_any_narrowing)
{
    constexpr uint32_t Max = 0xffffffffu;
    CHECK_EQ(PowerRules::ApplyDelta(0, 100, 0), 0u);
    CHECK_EQ(PowerRules::ApplyDelta(100, 100, 0), 100u);
    CHECK_EQ(PowerRules::ApplyDelta(40, 100, -40), 0u);
    CHECK_EQ(PowerRules::ApplyDelta(40, 100, -41), 0u);
    CHECK_EQ(PowerRules::ApplyDelta(99, 100, 1), 100u);
    CHECK_EQ(PowerRules::ApplyDelta(99, 100, 2), 100u);
    CHECK_EQ(PowerRules::ApplyDelta(1, Max, 0x7fffffff), 0x80000000u);
    CHECK_EQ(PowerRules::ApplyDelta(Max, Max, 0x7fffffff), Max);
    CHECK_EQ(PowerRules::ApplyDelta(Max, Max, -int64_t(0x80000000u)), 0x7fffffffu);
    CHECK_EQ(PowerRules::ApplyDelta(100, Max, -int64_t(0x80000000u)), 0u);
    CHECK_EQ(PowerRules::ApplyDelta(100, Max, int64_t(Max)), Max);
    CHECK_EQ(PowerRules::ApplyDelta(Max, Max, -int64_t(Max)), 0u);
    CHECK_EQ(PowerRules::ApplyDelta(Max, Max, std::numeric_limits<int64_t>::max()), Max);
    CHECK_EQ(PowerRules::ApplyDelta(Max, Max, std::numeric_limits<int64_t>::min()), 0u);
    CHECK_EQ(PowerRules::ApplyDelta(Max, 100, 0), 100u);
    CHECK_EQ(PowerRules::ApplyDelta(100, 0, 1), 0u);
}

TEST(PowerRules_focus_six_per_second_survives_precise_cast_updates)
{
    double remainder = 0;
    uint32_t power = 0;
    for (unsigned i = 0; i < 4; ++i)
    {
        power = PowerRules::Regenerate(power, 100, 6.0, 750, remainder);
    }
    CHECK_EQ(power, 18u);
    CHECK_EQ(remainder, 0.0);
    power = PowerRules::Regenerate(60, 100, 6.0, 2000, remainder);
    CHECK_EQ(power, 72u);
    CHECK_EQ(PowerRules::Regenerate(98, 100, 6.0, 2000, remainder), 100u);
    CHECK_EQ(remainder, 0.0);
}

TEST(PowerRules_focus_policy_checks_each_multiplier_before_multiplication)
{
    CHECK_EQ(PowerRules::FocusRatePerSecond(1, 1), 6.0);
    CHECK_EQ(PowerRules::FocusRatePerSecond(2, 0.5), 6.0);
    CHECK_EQ(PowerRules::FocusRatePerSecond(0, 1), 0.0);
    CHECK_EQ(PowerRules::FocusRatePerSecond(1, -1), 0.0);
    CHECK_EQ(PowerRules::FocusRatePerSecond(-1, -1), 0.0);
    CHECK_EQ(PowerRules::FocusRatePerSecond(std::numeric_limits<double>::infinity(), 1), 0.0);
    CHECK_EQ(PowerRules::FocusRatePerSecond(1, std::numeric_limits<double>::quiet_NaN()), 0.0);
}

TEST(PowerRules_all_primary_regen_and_decay_rates_are_partition_invariant)
{
    // Mana test rates are explicit normal/interrupted fixtures, not class curves.
    for (double rate : {2.75, 0.625, 6.0, 10.0, -10.0, -15.0, 6.0 * 1.25 * 0.5})
    {
        double wholeRemainder = 0;
        auto whole = PowerRules::Regenerate(50000, 100000, rate, 12000, wholeRemainder);
        for (uint32_t partition : {1u, 7u, 50u, 200u, 750u, 2000u, 12000u})
        {
            double remainder = 0;
            uint32_t power = 50000;
            for (uint32_t elapsed = 0; elapsed < 12000;)
            {
                auto diff = std::min(partition, 12000 - elapsed);
                power = PowerRules::Regenerate(power, 100000, rate, diff, remainder);
                elapsed += diff;
            }
            CHECK_EQ(power, whole);
            CHECK(std::abs(remainder - wholeRemainder) < 1e-8);
            CHECK(std::abs(remainder) < 1.0);
        }
    }
}

TEST(PowerRules_carry_belongs_to_each_pool_and_survives_spending)
{
    std::array<double, 7> carry{};
    CHECK_EQ(PowerRules::Regenerate(40, 100, 6, 100, carry[2]), 40u);
    CHECK_EQ(PowerRules::Regenerate(40, 100, 10, 100, carry[3]), 41u);
    CHECK_EQ(carry[3], 0.0);
    uint32_t focus = PowerRules::ApplyDelta(40, 100, -40);
    PowerRules::ClampRemainder(focus, 100, carry[2]);
    CHECK_EQ(PowerRules::Regenerate(focus, 100, 6, 100, carry[2]), 1u);
    CHECK(std::abs(carry[2] - 0.2) < 1e-12);
    CHECK_EQ(carry[0], 0.0);
    CHECK_EQ(carry[6], 0.0);
}

TEST(PowerRules_cap_floor_and_maximum_changes_do_not_bank_free_power)
{
    double remainder = 0;
    CHECK_EQ(PowerRules::Regenerate(99, 100, 6, 100, remainder), 99u);
    PowerRules::ClampRemainder(100, 100, remainder); // External energize reaches cap.
    CHECK_EQ(remainder, 0.0);
    CHECK_EQ(PowerRules::Regenerate(60, 100, 6, 100, remainder), 60u);
    PowerRules::ClampRemainder(50, 50, remainder); // Cap shrink.
    CHECK_EQ(remainder, 0.0);
    PowerRules::ClampRemainder(50, 100, remainder); // Cap growth does not refill.
    CHECK_EQ(PowerRules::Regenerate(50, 100, 0, 1000, remainder), 50u);
    CHECK_EQ(PowerRules::Regenerate(1, 1000, -15, 50, remainder), 1u);
    CHECK_EQ(PowerRules::Regenerate(1, 1000, -15, 50, remainder), 0u);
    CHECK_EQ(remainder, 0.0);
    CHECK_EQ(PowerRules::Regenerate(10, 1000, -15, 50, remainder), 10u);
    PowerRules::ClampRemainder(0, 1000, remainder); // Spend remaining rage/runic.
    CHECK_EQ(remainder, 0.0);
    CHECK_EQ(PowerRules::Regenerate(10, 0, 6, 1000, remainder), 0u);
    CHECK_EQ(remainder, 0.0);
}

TEST(PowerRules_paused_decay_and_mana_rate_changes_keep_only_earned_fraction)
{
    double remainder = 0;
    CHECK_EQ(PowerRules::Regenerate(50, 1000, -15, 50, remainder), 50u);
    // In combat / interrupt-regen pauses decay; there is no catch-up elapsed time.
    CHECK_EQ(PowerRules::Regenerate(50, 1000, 0, 60000, remainder), 50u);
    CHECK_EQ(PowerRules::Regenerate(50, 1000, -15, 50, remainder), 49u);
    CHECK_EQ(remainder, -0.5);
    remainder = 0;
    CHECK_EQ(PowerRules::Regenerate(50, 100, 2.75, 200, remainder), 50u);
    CHECK_EQ(PowerRules::Regenerate(50, 100, 0, 60000, remainder), 50u);
    CHECK_EQ(PowerRules::Regenerate(50, 100, 0.625, 800, remainder), 51u);
    CHECK(std::abs(remainder - 0.05) < 1e-12);
}

TEST(PowerRules_noop_recalculation_and_zero_elapsed_do_not_reset_carry)
{
    double remainder = 0;
    CHECK_EQ(PowerRules::Regenerate(40, 100, 6, 100, remainder), 40u);
    for (unsigned i = 0; i < 100; ++i)
    {
        PowerRules::ClampRemainder(40, 100, remainder);
        CHECK_EQ(PowerRules::Regenerate(40, 100, 6, 0, remainder), 40u);
    }
    CHECK_EQ(PowerRules::Regenerate(40, 100, 6, 100, remainder), 41u);
}

TEST(PowerRules_largest_ticks_rates_and_values_are_bounded)
{
    constexpr uint32_t Max = 0xffffffffu;
    double remainder = 0;
    CHECK_EQ(PowerRules::Regenerate(0, Max, 1000, Max, remainder), Max);
    CHECK_EQ(PowerRules::Regenerate(Max, Max, -1000, Max, remainder), 0u);
    CHECK_EQ(PowerRules::Regenerate(0, Max, std::numeric_limits<float>::max(), Max, remainder), Max);
    CHECK_EQ(PowerRules::Regenerate(Max, Max, -std::numeric_limits<float>::max(), Max, remainder), 0u);
    CHECK_EQ(PowerRules::Regenerate(0, Max, std::numeric_limits<double>::max(), Max, remainder), Max);
    CHECK_EQ(PowerRules::Regenerate(Max, Max, -std::numeric_limits<double>::max(), Max, remainder), 0u);
    for (double invalid : {std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        remainder = 0.5;
        CHECK_EQ(PowerRules::Regenerate(40, 100, invalid, Max, remainder), 40u);
        CHECK_EQ(remainder, 0.5);
    }
}

TEST(PowerRules_tiny_rates_still_accumulate)
{
    double remainder = 0;
    for (unsigned i = 0; i < 1000; ++i)
    {
        CHECK_EQ(PowerRules::Regenerate(0, 100, 1e-8, 1, remainder), 0u);
    }
    CHECK(std::abs(remainder - 1e-8) < 1e-20);
}

TEST(PowerRules_death_and_revival_discard_fraction_and_elapsed_dead_time)
{
    double carry[7] = {};
    uint32_t timer = 1900;
    CHECK_EQ(PowerRules::Regenerate(40, 100, 6, 100, carry[2]), 40u);
    CHECK_EQ(PowerRules::Regenerate(100, 1000, -15, 50, carry[6]), 100u);
    PowerRules::ResetRegeneration(carry, timer, 2000); // Actual death-state hook.
    for (double remainder : carry)
    {
        CHECK_EQ(remainder, 0.0);
    }
    CHECK_EQ(timer, 2000u);
    timer = 0; // The ordinary update countdown can expire while dead.
    PowerRules::ResetRegeneration(carry, timer, 2000); // Actual revival hook.
    CHECK_EQ(PowerRules::Regenerate(40, 100, 6, 2000 - timer, carry[2]), 40u);
    timer -= 100;
    CHECK_EQ(PowerRules::Regenerate(40, 100, 6, 2000 - timer, carry[2]), 40u);
    CHECK(std::abs(carry[2] - 0.6) < 1e-12);
}

TEST(PowerRules_modify_return_delta_remains_signed_at_uint32_extremes)
{
    constexpr uint32_t Max = 0xffffffffu;
    for (uint32_t current : {0u, 1u, 0x7fffffffu, 0x80000000u, Max})
    {
        for (int32_t delta : {0, 1, -1, std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::max()})
        {
            auto next = PowerRules::ApplyDelta(current, Max, delta);
            int64_t applied = int64_t(next) - current;
            CHECK(applied >= std::numeric_limits<int32_t>::min());
            CHECK(applied <= std::numeric_limits<int32_t>::max());
            CHECK(delta < 0 ? applied >= delta && applied <= 0 : applied <= delta && applied >= 0);
        }
    }
}

TEST(PowerRules_unsigned_costs_and_float_gains_do_not_change_sign)
{
    constexpr uint32_t Max = 0xffffffffu;
    CHECK_EQ(PowerRules::ApplyDelta(Max, Max, -int64_t(0x80000000u)), 0x7fffffffu);
    CHECK_EQ(PowerRules::ApplyDelta(Max, Max, -int64_t(Max)), 0u);
    CHECK_EQ(PowerRules::ApplyDelta(Max, Max, -int64_t(Max - 1)), 1u); // Health cost leaves one.
    CHECK_EQ(PowerRules::ApplyDelta(100, 100, -int64_t(Max)), 0u);
    CHECK_EQ(PowerRules::ApplyGain(100, 1000, 12.9), 112u);
    CHECK_EQ(PowerRules::ApplyGain(100, 1000, 0.9), 100u);
    CHECK_EQ(PowerRules::ApplyGain(0, Max, double(Max) * 10), Max);
    CHECK_EQ(PowerRules::ApplyGain(100, 1000, -12), 100u);
    CHECK_EQ(PowerRules::ApplyGain(100, 1000, std::numeric_limits<double>::infinity()), 100u);
    CHECK_EQ(PowerRules::ApplyGain(100, 1000, std::numeric_limits<double>::quiet_NaN()), 100u);
}
