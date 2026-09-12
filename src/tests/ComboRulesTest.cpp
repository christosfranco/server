// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "ComboRules.h"

#include <limits>

TEST(ComboRules_signed_and_large_deltas_clamp_before_narrowing)
{
    CHECK_EQ(ComboRules::AddPoints(0, 1, true), 1u);
    CHECK_EQ(ComboRules::AddPoints(4, 1, true), 5u);
    CHECK_EQ(ComboRules::AddPoints(5, 1, true), 5u);
    CHECK_EQ(ComboRules::AddPoints(1, -1, true), 0u);
    CHECK_EQ(ComboRules::AddPoints(0, -1, true), 0u);
    for (int32_t amount : {127, 128, 255, 256, std::numeric_limits<int32_t>::max()})
    {
        CHECK_EQ(ComboRules::AddPoints(5, amount, true), 5u);
        CHECK_EQ(ComboRules::AddPoints(0, amount, false), 5u);
    }
    for (int32_t amount : {-127, -128, -129, -256, std::numeric_limits<int32_t>::min()})
    {
        CHECK_EQ(ComboRules::AddPoints(5, amount, true), 0u);
        CHECK_EQ(ComboRules::AddPoints(5, amount, false), 0u);
    }
}

TEST(ComboRules_generation_on_another_target_does_not_transfer_points)
{
    CHECK_EQ(ComboRules::AddPoints(5, 1, false), 1u);
    CHECK_EQ(ComboRules::AddPoints(5, -1, false), 0u);
    CHECK_EQ(ComboRules::AddPoints(0, 1, false), 1u);
    CHECK_EQ(ComboRules::AddPoints(3, -2, true), 1u);
}

TEST(ComboRules_zero_points_with_matching_target_cannot_finish)
{
    auto points = ComboRules::AddPoints(1, -1, true);
    CHECK(!ComboRules::CanFinish(true, false, false, points, true));
    CHECK(ComboRules::CanFinish(true, false, false, 1, true));
    CHECK(ComboRules::CanFinish(true, false, false, 5, true));
    CHECK(!ComboRules::CanFinish(true, false, false, 5, false));
    CHECK(!ComboRules::CanFinish(true, false, false, 0, false));
}

TEST(ComboRules_triggered_and_ignore_state_exemptions_are_unchanged)
{
    for (uint8_t points : {0, 1, 5})
    {
        for (bool matching : {false, true})
        {
            CHECK(ComboRules::CanFinish(true, true, false, points, matching));
            CHECK(ComboRules::CanFinish(true, false, true, points, matching));
            CHECK(ComboRules::CanFinish(false, false, false, points, matching));
        }
    }
}
