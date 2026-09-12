// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "StatSystem.h"
#include "PowerRules.h"

#include <cmath>
#include <limits>
#include <map>

TEST(CoaStatMechanics_DeclaredPowerSurvivesDisplayChanges)
{
    for (int32 display = POWER_MANA; display < MAX_POWERS; ++display)
    {
        CHECK(StatSystem::CanModifyPower(POWER_MANA, Powers(display), true));
        CHECK(StatSystem::CanModifyPower(POWER_RUNIC_POWER, Powers(display), true));
    }
    CHECK(StatSystem::CanModifyPower(POWER_ENERGY, POWER_ENERGY, false));
    CHECK(!StatSystem::CanModifyPower(POWER_MANA, POWER_ENERGY, false));
}

TEST(CoaStatMechanics_InvalidPowerCannotIndexModifiers)
{
    for (int32 power : {int32(POWER_HEALTH), -1, int32(MAX_POWERS), 255,
        std::numeric_limits<int32>::max()})
    {
        CHECK(!StatSystem::CanModifyPower(power, POWER_MANA, true));
        CHECK(!StatSystem::CanModifyPower(power, POWER_MANA, false));
    }
}

TEST(CoaStatMechanics_HiddenManaPositiveAndNegativePercent)
{
    REQUIRE(StatSystem::CanModifyPower(POWER_MANA, POWER_RUNIC_POWER, true));
    const float intellectMana = StatSystem::CalculateManaBonusFromIntellect(40.0f);
    CHECK_EQ(intellectMana, 320.0f);
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 180.0f, 1.2f), 1800u);
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 180.0f, 0.8f), 1200u);
}

TEST(CoaStatMechanics_IntellectThresholdAndModifierOrder)
{
    CHECK_EQ(StatSystem::CalculateManaBonusFromIntellect(0.0f), 0.0f);
    CHECK_EQ(StatSystem::CalculateManaBonusFromIntellect(19.0f), 19.0f);
    CHECK_EQ(StatSystem::CalculateManaBonusFromIntellect(20.0f), 20.0f);
    CHECK_EQ(StatSystem::CalculateManaBonusFromIntellect(21.0f), 35.0f);

    // Intellect is already the fully modified stat. Its mana contribution is
    // outside base mana's percentage, but inside the final pool percentage.
    const float intellectMana = StatSystem::CalculateManaBonusFromIntellect(64.0f);
    CHECK_EQ(intellectMana, 680.0f);
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        200.0f, 1.25f, 100.0f, 1.5f), 3420u);
}

TEST(CoaStatMechanics_MultiplePercentModifiersKeepTheirProduct)
{
    const float intellectMana = StatSystem::CalculateManaBonusFromIntellect(40.0f);
    const float positiveAndNegative = 1.5f * 0.75f;
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 280.0f, positiveAndNegative), 1800u);
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 280.0f, 0.75f * 1.5f), 1800u);
}

TEST(CoaStatMechanics_ModifiersDoNotCreateAbsentPools)
{
    const float intellectMana = StatSystem::CalculateManaBonusFromIntellect(180.0f);
    CHECK_EQ(StatSystem::CalculateMaxPower(0, intellectMana,
        200.0f, 1.25f, 500.0f, 2.0f), 0u);
    CHECK_EQ(StatSystem::CalculateMaxPower(0, 0.0f,
        0.0f, 1.0f, 100.0f, 1.0f), 0u);
}

TEST(CoaStatMechanics_ZeroCapDoesNotPreventRemoval)
{
    const float intellectMana = StatSystem::CalculateManaBonusFromIntellect(40.0f);
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 0.0f, 0.0f), 0u);
    REQUIRE(StatSystem::CanModifyPower(POWER_MANA, POWER_RUNIC_POWER, true));
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 0.0f, 1.0f), 1320u);
}

TEST(CoaStatMechanics_PowerCapacityClampsBeforeIntegerConversion)
{
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, 0.0f,
        0.0f, 1.0f, -1001.0f, 1.0f), 0u);
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, 0.0f,
        0.0f, 1.0f, 0.0f, -1.0f), 0u);
    CHECK_EQ(StatSystem::CalculateMaxPower(100, 0.0f,
        0.0f, 1.0f, 0.75f, 1.0f), 100u);
    CHECK_EQ(StatSystem::CalculateMaxPower(std::numeric_limits<uint32>::max(),
        0.0f, 0.0f, 1.0f, 0.0f, 1.0f), std::numeric_limits<uint32>::max());
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, 0.0f,
        0.0f, 1.0f, 0.0f, std::numeric_limits<float>::quiet_NaN()), 0u);
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, 0.0f,
        0.0f, 1.0f, 0.0f, std::numeric_limits<float>::infinity()), 0u);
}

TEST(CoaStatMechanics_ModifierSnapshotsRecalculateWithoutDrift)
{
    const float intellectMana = StatSystem::CalculateManaBonusFromIntellect(40.0f);
    for (unsigned repeat = 0; repeat < 100; ++repeat)
    {
        // A flat gear bonus and a talent, then each removed independently.
        CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
            0.0f, 1.0f, 200.0f, 1.25f), 1900u);
        CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
            0.0f, 1.0f, 0.0f, 1.25f), 1650u);
        CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
            0.0f, 1.0f, 200.0f, 1.0f), 1520u);
        CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
            0.0f, 1.0f, 0.0f, 1.0f), 1320u);
    }
    CHECK_EQ(StatSystem::CalculateMaxPower(1000,
        StatSystem::CalculateManaBonusFromIntellect(60.0f),
        0.0f, 1.0f, 0.0f, 1.0f), 1620u);
}

TEST(CoaStatMechanics_VisiblePowerUsesTheSameModifierOrder)
{
    REQUIRE(StatSystem::CanModifyPower(POWER_ENERGY, POWER_ENERGY, true));
    CHECK_EQ(StatSystem::CalculateMaxPower(100, 0.0f,
        20.0f, 1.25f, 10.0f, 1.5f), 240u);
}

TEST(CoaStatMechanics_CapShrinkClampsCurrentWithoutRefill)
{
    const float intellectMana = StatSystem::CalculateManaBonusFromIntellect(40.0f);
    uint32 current = 1700;
    uint32 maximum = StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 200.0f, 1.25f);
    current = PowerRules::ApplyDelta(current, maximum, 0);
    CHECK_EQ(current, 1700u);

    REQUIRE(StatSystem::CanModifyPower(POWER_MANA, POWER_RUNIC_POWER, true));
    maximum = StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 0.0f, 1.0f);
    current = PowerRules::ApplyDelta(current, maximum, 0);
    CHECK_EQ(current, 1320u);

    maximum = StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 200.0f, 1.25f);
    current = PowerRules::ApplyDelta(current, maximum, 0);
    CHECK_EQ(current, 1320u);

    maximum = StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 0.0f, 0.0f);
    current = PowerRules::ApplyDelta(current, maximum, 0);
    CHECK_EQ(current, 0u);
    maximum = StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 0.0f, 1.0f);
    CHECK_EQ(PowerRules::ApplyDelta(current, maximum, 0), 0u);
}

TEST(CoaStatMechanics_ManaRegenUsesActualCapacityNotDisplay)
{
    REQUIRE(StatSystem::CanModifyPower(POWER_MANA, POWER_RUNIC_POWER, true));
    const auto hidden = StatSystem::CalculateManaRegen(1500, 30.0f, 12.0f, 50);
    CHECK_EQ(hidden.normal, 42.0f);
    CHECK_EQ(hidden.interrupted, 27.0f);

    const auto absent = StatSystem::CalculateManaRegen(0, 30.0f, 12.0f, 50);
    CHECK_EQ(absent.normal, 0.0f);
    CHECK_EQ(absent.interrupted, 0.0f);
    const auto restored = StatSystem::CalculateManaRegen(1500, 30.0f, 12.0f, 50);
    CHECK_EQ(restored.normal, 42.0f);
    CHECK_EQ(restored.interrupted, 27.0f);
}

TEST(CoaStatMechanics_ManaRegenInterruptionPreservesFlatBonus)
{
    const auto interrupted = StatSystem::CalculateManaRegen(1500, 30.0f, 12.0f, 0);
    CHECK_EQ(interrupted.normal, 42.0f);
    CHECK_EQ(interrupted.interrupted, 12.0f);
    const auto full = StatSystem::CalculateManaRegen(1500, 30.0f, 12.0f, 150);
    CHECK_EQ(full.interrupted, 42.0f);
}

TEST(CoaStatMechanics_CustomParryRequiresCapability)
{
    for (uint32 classId = 12; classId <= 32; ++classId)
    {
        CHECK_EQ(StatSystem::CalculateParryChance(classId, false,
            8.0f, 7.5f, 0.0f), 0.0f);
        CHECK_EQ(StatSystem::CalculateParryChance(classId, true,
            5.0f, 0.0f, 0.0f), 5.0f);
        CHECK_EQ(StatSystem::CalculateParryChance(classId, true,
            8.0f, 7.5f, 0.0f), 15.5f);
    }
}

TEST(CoaStatMechanics_StockParryRulesAreUnchanged)
{
    for (uint32 classId : {1u, 2u, 6u})
    {
        CHECK(std::abs(StatSystem::CalculateParryChance(classId, true,
            8.0f, 12.5f, 0.9560f) - 18.229656f) < 0.00001f);
    }
    for (uint32 classId : {3u, 4u, 7u})
    {
        CHECK(std::abs(StatSystem::CalculateParryChance(classId, true,
            8.0f, 12.5f, 0.9880f) - 19.640088f) < 0.00001f);
    }
    for (uint32 classId : {5u, 8u, 9u, 10u, 11u})
    {
        CHECK_EQ(StatSystem::CalculateParryChance(classId, true,
            8.0f, 12.5f, 0.9830f), 0.0f);
    }
}

TEST(CoaStatMechanics_ParryRemovalAndRespecSnapshots)
{
    CHECK_EQ(StatSystem::CalculateParryChance(32, true, 8.0f, 7.5f, 0.0f), 15.5f);
    CHECK_EQ(StatSystem::CalculateParryChance(32, true, 5.0f, 7.5f, 0.0f), 12.5f);
    CHECK_EQ(StatSystem::CalculateParryChance(32, true, 5.0f, 0.0f, 0.0f), 5.0f);
    CHECK_EQ(StatSystem::CalculateParryChance(32, false, 5.0f, 0.0f, 0.0f), 0.0f);
    CHECK_EQ(StatSystem::CalculateParryChance(32, true, -20.0f, 7.5f, 0.0f), 0.0f);
}

TEST(CoaStatMechanics_ParryRejectsInvalidClassOrNonfiniteResult)
{
    CHECK_EQ(StatSystem::CalculateParryChance(0, true, 8.0f, 7.5f, 0.0f), 0.0f);
    CHECK_EQ(StatSystem::CalculateParryChance(33, true, 8.0f, 7.5f, 0.0f), 0.0f);
    CHECK_EQ(StatSystem::CalculateParryChance(32, true, 8.0f,
        std::numeric_limits<float>::infinity(), 0.0f), 0.0f);
}

TEST(CoaStatMechanics_TwoSuppressorsRemainZeroUntilLastRemoval)
{
    StatSystem::PercentModifier modifier;
    float value = 1.0f;
    REQUIRE(modifier.Apply(value, 25.0f, true));
    REQUIRE(modifier.Apply(value, 20.0f, true));
    CHECK_EQ(value, 1.5f);
    REQUIRE(modifier.Apply(value, -100.0f, true));
    CHECK_EQ(value, 0.0f);
    REQUIRE(modifier.Apply(value, -100.0f, true));
    CHECK_EQ(value, 0.0f);
    REQUIRE(modifier.Apply(value, -100.0f, false));
    CHECK_EQ(value, 0.0f);
    REQUIRE(modifier.Apply(value, -100.0f, false));
    CHECK_EQ(value, 1.5f);
    REQUIRE(modifier.Apply(value, 25.0f, false));
    CHECK_EQ(value, 1.2f);
    REQUIRE(modifier.Apply(value, 20.0f, false));
    CHECK_EQ(value, 1.0f);
}

TEST(CoaStatMechanics_OverSuppressionNeverCreatesNegativeFactors)
{
    StatSystem::PercentModifier modifier;
    float value = 0.5f;
    REQUIRE(modifier.Apply(value, -150.0f, true));
    REQUIRE(modifier.Apply(value, -100.0f, true));
    REQUIRE(modifier.Apply(value, 50.0f, true));
    REQUIRE(modifier.Apply(value, -50.0f, true));
    CHECK_EQ(value, 0.0f);
    REQUIRE(modifier.Apply(value, -150.0f, false));
    CHECK_EQ(value, 0.0f);
    REQUIRE(modifier.Apply(value, -100.0f, false));
    CHECK_EQ(value, 0.375f);
    REQUIRE(modifier.Apply(value, 50.0f, false));
    REQUIRE(modifier.Apply(value, -50.0f, false));
    CHECK_EQ(value, 0.5f);
}

TEST(CoaStatMechanics_OrdinaryPercentFactorsKeepStockProductSemantics)
{
    StatSystem::PercentModifier modifier;
    float value = 0.5f;
    REQUIRE(modifier.Apply(value, 5.0f, true));
    CHECK_EQ(value, 0.525f);
    REQUIRE(modifier.Apply(value, -20.0f, true));
    CHECK_EQ(value, 0.42f);
    REQUIRE(modifier.Apply(value, 50.0f, true));
    CHECK_EQ(value, 0.63f);
    REQUIRE(modifier.Apply(value, 5.0f, false));
    CHECK_EQ(value, 0.6f);
    REQUIRE(modifier.Apply(value, -20.0f, false));
    CHECK_EQ(value, 0.75f);
    REQUIRE(modifier.Apply(value, 50.0f, false));
    CHECK_EQ(value, 0.5f);
}

TEST(CoaStatMechanics_PercentBucketsSuppressIndependently)
{
    StatSystem::PercentModifier baseModifier;
    StatSystem::PercentModifier totalModifier;
    float base = 1.0f;
    float total = 1.0f;
    REQUIRE(baseModifier.Apply(base, -100.0f, true));
    REQUIRE(totalModifier.Apply(total, 25.0f, true));
    CHECK_EQ(base, 0.0f);
    CHECK_EQ(total, 1.25f);
    REQUIRE(totalModifier.Apply(total, -150.0f, true));
    REQUIRE(baseModifier.Apply(base, -100.0f, false));
    CHECK_EQ(base, 1.0f);
    CHECK_EQ(total, 0.0f);
    REQUIRE(totalModifier.Apply(total, -150.0f, false));
    CHECK_EQ(total, 1.25f);
}

TEST(CoaStatMechanics_SuppressedRestackPreservesOtherFactors)
{
    StatSystem::PercentModifier modifier;
    float value = 1.0f;
    REQUIRE(modifier.Apply(value, 10.0f, true));
    for (unsigned repeat = 0; repeat < 10000; ++repeat)
    {
        REQUIRE(modifier.Apply(value, 30.0f, true));
        REQUIRE(modifier.Apply(value, -100.0f, true));
        REQUIRE(modifier.Apply(value, 30.0f, false));
        REQUIRE(modifier.Apply(value, 50.0f, true));
        REQUIRE(modifier.Apply(value, -100.0f, false));
        CHECK_EQ(value, 1.65f);
        REQUIRE(modifier.Apply(value, 50.0f, false));
        CHECK_EQ(value, 1.1f);
    }
    REQUIRE(modifier.Apply(value, 10.0f, false));
    CHECK_EQ(value, 1.0f);
}

TEST(CoaStatMechanics_PercentExtremeProductsRecoverAfterRemoval)
{
    for (float amount : {-99.0f, 300.0f, std::numeric_limits<float>::max()})
    {
        StatSystem::PercentModifier modifier;
        float value = 0.5f;
        REQUIRE(modifier.Apply(value, 25.0f, true));
        for (unsigned count = 0; count < 2048; ++count)
        {
            REQUIRE(modifier.Apply(value, amount, true));
            CHECK(std::isfinite(value));
        }
        CHECK_EQ(value, amount < 0.0f ? 0.0f : std::numeric_limits<float>::max());
        REQUIRE(modifier.Apply(value, -100.0f, true));
        for (unsigned count = 0; count < 2048; ++count)
        {
            REQUIRE(modifier.Apply(value, amount, false));
            CHECK_EQ(value, 0.0f);
        }
        REQUIRE(modifier.Apply(value, -100.0f, false));
        CHECK_EQ(value, 0.625f);
        REQUIRE(modifier.Apply(value, 25.0f, false));
        CHECK_EQ(value, 0.5f);
    }
}

TEST(CoaStatMechanics_PercentRejectsNonfiniteWithoutMutation)
{
    StatSystem::PercentModifier modifier;
    float value = 1.0f;
    REQUIRE(modifier.Apply(value, 25.0f, true));
    REQUIRE(modifier.Apply(value, -100.0f, true));
    for (float amount : {std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    {
        CHECK(!modifier.Apply(value, amount, true));
        CHECK(!modifier.Apply(value, amount, false));
        CHECK_EQ(value, 0.0f);
    }
    REQUIRE(modifier.Apply(value, -100.0f, false));
    CHECK_EQ(value, 1.25f);
    REQUIRE(modifier.Apply(value, 25.0f, false));
    CHECK_EQ(value, 1.0f);
    CHECK(!modifier.Apply(value, -100.0f, false));
    CHECK(!modifier.Apply(value, 25.0f, false));
    CHECK_EQ(value, 1.0f);
}

TEST(CoaStatMechanics_PercentKeepsInitialZeroAndOffhandBaselines)
{
    for (float initial : {0.0f, 0.5f, 1.0f, 100.0f})
    {
        StatSystem::PercentModifier modifier;
        float value = initial;
        for (unsigned repeat = 0; repeat < 100; ++repeat)
        {
            REQUIRE(modifier.Apply(value, 25.0f, true));
            CHECK_EQ(value, initial * 1.25f);
            REQUIRE(modifier.Apply(value, -100.0f, true));
            REQUIRE(modifier.Apply(value, 25.0f, false));
            CHECK_EQ(value, 0.0f);
            REQUIRE(modifier.Apply(value, -100.0f, false));
            CHECK_EQ(value, initial);
        }
    }
}

TEST(CoaStatMechanics_FlatModifiersStayAdditiveAndRejectNonfinite)
{
    float value = 100.0f;
    REQUIRE(StatSystem::ApplyFlatModifier(value, -150.0f, true));
    CHECK_EQ(value, -50.0f);
    REQUIRE(StatSystem::ApplyFlatModifier(value, -150.0f, false));
    CHECK_EQ(value, 100.0f);
    for (float amount : {std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    {
        CHECK(!StatSystem::ApplyFlatModifier(value, amount, true));
        CHECK(!StatSystem::ApplyFlatModifier(value, amount, false));
        CHECK_EQ(value, 100.0f);
    }
    value = std::numeric_limits<float>::max();
    CHECK(!StatSystem::ApplyFlatModifier(value, value, true));
    CHECK_EQ(value, std::numeric_limits<float>::max());
}

TEST(CoaStatMechanics_HiddenManaSuppressionRestoresLiveModifierProduct)
{
    StatSystem::PercentModifier modifier;
    float percent = 1.0f;
    const float intellectMana = StatSystem::CalculateManaBonusFromIntellect(40.0f);
    REQUIRE(modifier.Apply(percent, 25.0f, true));
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 200.0f, percent), 1900u);
    REQUIRE(modifier.Apply(percent, -100.0f, true));
    REQUIRE(modifier.Apply(percent, -150.0f, true));
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 200.0f, percent), 0u);
    REQUIRE(StatSystem::CanModifyPower(POWER_MANA, POWER_RUNIC_POWER, true));
    REQUIRE(modifier.Apply(percent, -100.0f, false));
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 200.0f, percent), 0u);
    REQUIRE(modifier.Apply(percent, -150.0f, false));
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 200.0f, percent), 1900u);
    REQUIRE(modifier.Apply(percent, 25.0f, false));
    CHECK_EQ(StatSystem::CalculateMaxPower(1000, intellectMana,
        0.0f, 1.0f, 0.0f, percent), 1320u);
}

TEST(CoaStatMechanics_ParrySourcesRequireOwnedExactNativeEffect)
{
    struct SpellRecord { uint32 Effect[3]; };
    std::map<uint32, SpellRecord> records = {
        {1, {{SPELL_EFFECT_PROFICIENCY, 0, 0}}},
        {2, {{0, SPELL_EFFECT_PARRY, 0}}},
        {3, {{SPELL_EFFECT_PARRY + 256, 0, 0}}},
        {4, {{SPELL_EFFECT_LEARN_SPELL, 0, 0}}}};
    std::map<uint32, bool> known = {{1, true}, {2, false}, {3, true}, {4, true}, {5, true}};
    const auto canParry = [&]
    {
        return StatSystem::HasParrySource(known,
            [&](uint32 id) { return known.at(id); },
            [&](uint32 id) -> SpellRecord const*
            {
                auto found = records.find(id);
                return found == records.end() ? nullptr : &found->second;
            });
    };
    CHECK(!canParry());
    known.at(2) = true;
    CHECK(canParry());
    known.at(2) = false;
    CHECK(!canParry());
    known.erase(2);
    CHECK(!canParry());
}

TEST(CoaStatMechanics_ParryRemovalKeepsIndependentQuestRaceAndSkillSources)
{
    struct SpellRecord { uint32 Effect[3]; };
    const SpellRecord parry = {{SPELL_EFFECT_PARRY, 0, 0}};
    // Talent, quest, racial and skill grants are equally valid known sources.
    std::map<uint32, bool> known = {{1, true}, {2, true}, {3, true}, {4, true}};
    const auto canParry = [&]
    {
        return StatSystem::HasParrySource(known,
            [&](uint32 id) { return known.at(id); },
            [&](uint32) { return &parry; });
    };
    CHECK(canParry());
    for (uint32 id : {1u, 2u, 3u})
    {
        known.erase(id);
        CHECK(canParry());
    }
    known.erase(4);
    CHECK(!canParry());
}

TEST(CoaStatMechanics_ParryLearnedChildrenAndRankReplacement)
{
    struct SpellRecord { uint32 Effect[3]; };
    const SpellRecord parent = {{SPELL_EFFECT_LEARN_SPELL, 0, 0}};
    const SpellRecord parry = {{0, 0, SPELL_EFFECT_PARRY}};
    std::map<uint32, bool> known = {{1, true}, {2, true}, {3, false}};
    const auto canParry = [&]
    {
        return StatSystem::HasParrySource(known,
            [&](uint32 id) { return known.at(id); },
            [&](uint32 id) { return id == 1 ? &parent : &parry; });
    };
    CHECK(canParry());
    known.at(3) = true;
    known.erase(2);
    CHECK(canParry());
    known.erase(1);
    CHECK(canParry());
    known.erase(3);
    CHECK(!canParry());
    known[1] = true;
    CHECK(!canParry());
    known[2] = true;
    CHECK(canParry());
}

TEST(CoaStatMechanics_BaseShieldBlockSuppressorsRestoreRemainingBuff)
{
    float values[BASEMOD_END][MOD_END] = {{0, 1}, {0, 1}, {0, 1}, {0, 1}};
    StatSystem::PercentModifier percentages[BASEMOD_END]{};
    REQUIRE(StatSystem::SetBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, FLAT_MOD, 200.0f));
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, 50.0f, true));
    const auto block = [&]
    {
        return StatSystem::CalculateShieldBlockValue(values[SHIELD_BLOCK_VALUE][FLAT_MOD],
            60.0f, values[SHIELD_BLOCK_VALUE][PCT_MOD]);
    };
    CHECK_EQ(block(), 330u);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, -100.0f, true));
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, -100.0f, true));
    CHECK_EQ(block(), 0u);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, -100.0f, false));
    CHECK_EQ(block(), 0u);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, -100.0f, false));
    CHECK_EQ(block(), 330u);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, 50.0f, false));
    CHECK_EQ(block(), 220u);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, FLAT_MOD, 200.0f, false));
    CHECK_EQ(block(), 20u);
}

TEST(CoaStatMechanics_BaseCritAndBlockUseTheirOwnBaselines)
{
    float values[BASEMOD_END][MOD_END] = {{0, 1}, {0, 1}, {0, 1}, {0, 1}};
    StatSystem::PercentModifier percentages[BASEMOD_END]{};
    for (BaseModGroup group : {CRIT_PERCENTAGE, RANGED_CRIT_PERCENTAGE, OFFHAND_CRIT_PERCENTAGE})
    {
        REQUIRE(StatSystem::SetBaseModifier(values, percentages, group, PCT_MOD, 0.0f));
        REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, group, PCT_MOD, 50.0f, true));
        REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, group, FLAT_MOD, 2.0f, true));
        CHECK_EQ(values[group][FLAT_MOD] + values[group][PCT_MOD], 2.0f);
        REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, group, PCT_MOD, 50.0f, false));
        CHECK_EQ(values[group][PCT_MOD], 0.0f);
        REQUIRE(StatSystem::SetBaseModifier(values, percentages, group, PCT_MOD, 5.0f));
        CHECK_EQ(values[group][FLAT_MOD] + values[group][PCT_MOD], 7.0f);
    }
    CHECK_EQ(values[SHIELD_BLOCK_VALUE][FLAT_MOD], 0.0f);
    CHECK_EQ(values[SHIELD_BLOCK_VALUE][PCT_MOD], 1.0f);
}

TEST(CoaStatMechanics_BasePercentageReplacementDiscardsThePreviousStack)
{
    float values[BASEMOD_END][MOD_END] = {{0, 1}, {0, 1}, {0, 1}, {0, 1}};
    StatSystem::PercentModifier percentages[BASEMOD_END]{};
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, 50.0f, true));
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, -100.0f, true));
    REQUIRE(StatSystem::SetBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, 2.0f));
    CHECK_EQ(values[SHIELD_BLOCK_VALUE][PCT_MOD], 2.0f);
    CHECK(!StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, -100.0f, false));
    CHECK(!StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, 50.0f, false));
    CHECK_EQ(values[SHIELD_BLOCK_VALUE][PCT_MOD], 2.0f);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, 50.0f, true));
    CHECK_EQ(values[SHIELD_BLOCK_VALUE][PCT_MOD], 3.0f);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, 50.0f, false));
    CHECK_EQ(values[SHIELD_BLOCK_VALUE][PCT_MOD], 2.0f);
}

TEST(CoaStatMechanics_BaseCritPreservesSignedNativeBaseline)
{
    float values[BASEMOD_END][MOD_END]{};
    StatSystem::PercentModifier percentages[BASEMOD_END]{};
    REQUIRE(StatSystem::SetBaseModifier(values, percentages, CRIT_PERCENTAGE, FLAT_MOD, 2.0f));
    REQUIRE(StatSystem::SetBaseModifier(values, percentages, CRIT_PERCENTAGE, PCT_MOD, -1.0f));
    CHECK_EQ(values[CRIT_PERCENTAGE][FLAT_MOD] + values[CRIT_PERCENTAGE][PCT_MOD], 1.0f);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, CRIT_PERCENTAGE, PCT_MOD, 50.0f, true));
    CHECK_EQ(values[CRIT_PERCENTAGE][PCT_MOD], -1.5f);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, CRIT_PERCENTAGE, PCT_MOD, -100.0f, true));
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, CRIT_PERCENTAGE, PCT_MOD, -150.0f, true));
    CHECK_EQ(values[CRIT_PERCENTAGE][PCT_MOD], 0.0f);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, CRIT_PERCENTAGE, PCT_MOD, -100.0f, false));
    CHECK_EQ(values[CRIT_PERCENTAGE][PCT_MOD], 0.0f);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, CRIT_PERCENTAGE, PCT_MOD, -150.0f, false));
    CHECK_EQ(values[CRIT_PERCENTAGE][PCT_MOD], -1.5f);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, CRIT_PERCENTAGE, PCT_MOD, 50.0f, false));
    CHECK_EQ(values[CRIT_PERCENTAGE][PCT_MOD], -1.0f);
}

TEST(CoaStatMechanics_UnitAndBaseReplacementShareAccumulatorPolicy)
{
    float values[BASEMOD_END][MOD_END] = {{0, 1}, {0, 1}, {0, 1}, {0, 1}};
    StatSystem::PercentModifier percentages[BASEMOD_END]{};
    StatSystem::PercentModifier unitPercentage;
    float unitValue = 0.5f;
    REQUIRE(unitPercentage.Apply(unitValue, -150.0f, true));
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, -150.0f, true));
    REQUIRE(unitPercentage.Set(unitValue, 0.75f));
    REQUIRE(StatSystem::SetBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, 0.75f));
    CHECK(!unitPercentage.Apply(unitValue, -150.0f, false));
    CHECK(!StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, -150.0f, false));
    REQUIRE(unitPercentage.Apply(unitValue, 100.0f, true));
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, 100.0f, true));
    CHECK_EQ(unitValue, 1.5f);
    CHECK_EQ(values[SHIELD_BLOCK_VALUE][PCT_MOD], 1.5f);
}

TEST(CoaStatMechanics_BaseInvalidInputsLeaveValuesAndCountsUnchanged)
{
    float values[BASEMOD_END][MOD_END] = {{2, 3}, {4, 5}, {6, 7}, {200, 1}};
    StatSystem::PercentModifier percentages[BASEMOD_END]{};
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, 50.0f, true));
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, -100.0f, true));
    for (BaseModGroup group : {BASEMOD_END, BaseModGroup(std::numeric_limits<uint32>::max())})
    {
        CHECK(!StatSystem::ApplyBaseModifier(values, percentages, group, PCT_MOD, 50.0f, true));
        CHECK(!StatSystem::ApplyBaseModifier(values, percentages, group, PCT_MOD, -100.0f, false));
        CHECK(!StatSystem::SetBaseModifier(values, percentages, group, PCT_MOD, 1.0f));
    }
    for (BaseModType type : {BaseModType(MOD_END), BaseModType(std::numeric_limits<uint32>::max())})
    {
        CHECK(!StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, type, -100.0f, false));
        CHECK(!StatSystem::SetBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, type, 1.0f));
    }
    for (BaseModType type : {FLAT_MOD, PCT_MOD})
    {
        for (float amount : {std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        {
            CHECK(!StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, type, amount, true));
            CHECK(!StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, type, amount, false));
            CHECK(!StatSystem::SetBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, type, amount));
        }
    }
    CHECK(!StatSystem::SetBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, -1.0f));
    CHECK_EQ(values[CRIT_PERCENTAGE][FLAT_MOD], 2.0f);
    CHECK_EQ(values[CRIT_PERCENTAGE][PCT_MOD], 3.0f);
    CHECK_EQ(values[RANGED_CRIT_PERCENTAGE][FLAT_MOD], 4.0f);
    CHECK_EQ(values[RANGED_CRIT_PERCENTAGE][PCT_MOD], 5.0f);
    CHECK_EQ(values[OFFHAND_CRIT_PERCENTAGE][FLAT_MOD], 6.0f);
    CHECK_EQ(values[OFFHAND_CRIT_PERCENTAGE][PCT_MOD], 7.0f);
    CHECK_EQ(values[SHIELD_BLOCK_VALUE][FLAT_MOD], 200.0f);
    CHECK_EQ(values[SHIELD_BLOCK_VALUE][PCT_MOD], 0.0f);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, -100.0f, false));
    CHECK_EQ(values[SHIELD_BLOCK_VALUE][PCT_MOD], 1.5f);
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, SHIELD_BLOCK_VALUE, PCT_MOD, 50.0f, false));
    CHECK_EQ(values[SHIELD_BLOCK_VALUE][PCT_MOD], 1.0f);
}

TEST(CoaStatMechanics_BaseFractionalRestacksRetainProductAndIdentity)
{
    float values[BASEMOD_END][MOD_END]{};
    StatSystem::PercentModifier percentages[BASEMOD_END]{};
    REQUIRE(StatSystem::SetBaseModifier(values, percentages, OFFHAND_CRIT_PERCENTAGE, PCT_MOD, 2.5f));
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, OFFHAND_CRIT_PERCENTAGE, PCT_MOD, 5.5f, true));
    for (unsigned repeat = 0; repeat < 10000; ++repeat)
    {
        REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, OFFHAND_CRIT_PERCENTAGE, PCT_MOD, 50.0f, true));
        REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, OFFHAND_CRIT_PERCENTAGE, PCT_MOD, -25.0f, true));
        CHECK_EQ(values[OFFHAND_CRIT_PERCENTAGE][PCT_MOD], 2.9671875f);
        REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, OFFHAND_CRIT_PERCENTAGE, PCT_MOD, 50.0f, false));
        CHECK_EQ(values[OFFHAND_CRIT_PERCENTAGE][PCT_MOD], 1.978125f);
        REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, OFFHAND_CRIT_PERCENTAGE, PCT_MOD, -25.0f, false));
        CHECK_EQ(values[OFFHAND_CRIT_PERCENTAGE][PCT_MOD], 2.6375f);
    }
    REQUIRE(StatSystem::ApplyBaseModifier(values, percentages, OFFHAND_CRIT_PERCENTAGE, PCT_MOD, 5.5f, false));
    CHECK_EQ(values[OFFHAND_CRIT_PERCENTAGE][PCT_MOD], 2.5f);
}

TEST(CoaStatMechanics_ShieldBlockClampsBeforeUnsignedConversion)
{
    CHECK_EQ(StatSystem::CalculateShieldBlockValue(0.0f, 0.0f, 1.0f), 0u);
    CHECK_EQ(StatSystem::CalculateShieldBlockValue(200.0f, 60.0f, 0.0f), 0u);
    CHECK_EQ(StatSystem::CalculateShieldBlockValue(200.0f, 60.0f, 0.75f), 165u);
    CHECK_EQ(StatSystem::CalculateShieldBlockValue(std::numeric_limits<float>::max(), 20.0f, 1.0f),
        std::numeric_limits<uint32>::max());
    CHECK_EQ(StatSystem::CalculateShieldBlockValue(200.0f, 60.0f,
        std::numeric_limits<float>::infinity()), 0u);
    CHECK_EQ(StatSystem::CalculateShieldBlockValue(200.0f, 60.0f,
        std::numeric_limits<float>::quiet_NaN()), 0u);
}
