// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "CoaPowerRequirements.h"
#include "CoaState.h"
#include "PowerRules.h"

#include <array>
#include <limits>
#include <map>
#include <openssl/evp.h>
#include <sstream>

namespace
{
    using Spells = std::map<uint32_t, coa::PowerSpell>;
    uint8_t Required(std::set<uint32_t> const& learned, Spells const& spells)
    {
        return coa::RequiredPowers(3, 3, learned, [&](uint32_t id) { return spells.at(id); }).powers;
    }
    coa::PowerSpell Cost(int64_t power, bool passive = false)
    {
        coa::PowerSpell spell;
        spell.power = power;
        spell.hasCost = true;
        spell.passive = passive;
        spell.casterSourceOnly = passive;
        spell.effects[0] = {passive ? 6u : 2u, passive ? 1u : 6u, 0, 0, 0, 0};
        return spell;
    }
    coa::PowerSpell Trigger(uint32_t child, uint32_t effect = 64, uint32_t target = 1, uint32_t aura = 0)
    {
        coa::PowerSpell spell;
        spell.casterSourceOnly = target == 1;
        spell.effects[0] = {effect, target, 0, child, aura, 0};
        return spell;
    }
    coa::PowerSpell Gain(int32_t power, uint32_t target = 1)
    {
        coa::PowerSpell spell;
        spell.casterSourceOnly = target == 1;
        spell.effects[0] = {30, target, 0, 0, 0, power};
        return spell;
    }
}

TEST(CoaPowerRequirements_native_21_primary_contexts_do_not_create_unused_pools)
{
    // Independent DisplayPower sequence for native class IDs12..32.
    std::array<int, 21> display = {3,0,3,0,0,1,3,3,1,2,0,6,0,0,3,0,0,0,6,0,0};
    for (size_t index = 0; index < display.size(); ++index)
    {
        int starter = index == 8 ? -2 : index == 11 || index == 18 ? 0 : display[index];
        auto mask = coa::RequiredPowers(display[index], starter, {}, [](uint32_t) { return coa::PowerSpell{}; }).powers;
        for (int power = 0; power < 7; ++power)
        {
            uint32_t expected = power != display[index] && power != starter ? 0
                : power == 0 ? 120 : power == 1 || power == 6 ? 1000 : 100;
            CHECK_EQ(coa::PowerCapacity(mask, power, 120), expected);
        }
    }
}

TEST(CoaPowerRequirements_learned_costs_follow_checks_including_passive_activation)
{
    Spells spells = {{10, Cost(0)}, {11, Cost(2)}, {12, Cost(1, true)}, {13, {}}};
    CHECK_EQ(Required({}, spells), 8u);
    CHECK_EQ(Required({10}, spells), 9u);
    CHECK_EQ(Required({10,11}, spells), 13u);
    CHECK_EQ(Required({12,13}, spells), 10u);
    CHECK_EQ(Required({11}, spells), 12u); // Disable/unlearn last mana user.
    CHECK_EQ(Required({}, spells), 8u); // Respec removes the last extra user.
}

TEST(CoaPowerRequirements_direct_triggered_costs_need_the_actual_caster_pool)
{
    Spells spells = {{10, Trigger(11, 142)}, {11, Cost(0)}, {12, Cost(2)}};
    spells[11].effects[1] = {142,1,0,12,0,0};
    spells[12].effects[1] = {142,1,0,10,0,0};
    CHECK_EQ(Required({10}, spells), 13u);
    CHECK_EQ(Required({10,11}, spells), 13u);
    CHECK_EQ(Required({10,12}, spells), 13u);
}

TEST(CoaPowerRequirements_energize_uses_valid_native_slots_and_owner_destination)
{
    Spells spells = {{10, Gain(1)}};
    CHECK_EQ(Required({10}, spells), 10u);
    spells[10].effects[0] = {137,1,0,0,0,0};
    CHECK_EQ(Required({10}, spells), 9u);
    spells[10] = Gain(1, 6);
    CHECK_EQ(Required({10}, spells), 8u);
    for (uint32_t unsupported : {175u, 183u})
    {
        spells[10].effects[0] = {unsupported,1,0,0,0,1};
        CHECK_EQ(Required({10}, spells), 8u);
    }
    for (int power : {-2, -1, 4, 5, 7, 255, std::numeric_limits<int>::max()})
    {
        spells[10] = Gain(power);
        CHECK_EQ(Required({10}, spells), 8u);
        CHECK_EQ(coa::PowerCapacity(0xff, power, 100), 0u);
    }
    CHECK_EQ(coa::PrimaryPowerBit(0xfffffffeu), 0u); // Native health-cost encoding.
}

TEST(CoaPowerRequirements_hidden_rage_and_mana_gains_need_owned_paths)
{
    Spells spells = {{10, Trigger(11)}, {11, Gain(1)}, {12, Trigger(13)}, {13, Gain(0)}};
    CHECK_EQ(coa::RequiredPowers(0, 0, {10}, [&](uint32_t id) { return spells.at(id); }).powers, 3u);
    CHECK_EQ(Required({12}, spells), 9u);
    CHECK_EQ(Required({}, spells), 8u);
}

TEST(CoaPowerRequirements_mana_fallback_applies_only_to_a_required_pool)
{
    CHECK_EQ(coa::RequiredBaseMana(8, 200, 500), 0u);
    CHECK_EQ(coa::RequiredBaseMana(9, 200, 500), 200u);
    CHECK_EQ(coa::RequiredBaseMana(9, 0, 500), 500u);
    bool rejected = false;
    try { (void)coa::RequiredBaseMana(9, 0, 0); }
    catch (std::invalid_argument const&) { rejected = true; }
    CHECK(rejected);
}

TEST(CoaPowerRequirements_form_capacity_does_not_refill_or_remove_owned_hidden_pools)
{
    uint8_t const owned = 9; // Energy display with an actually learned mana cost.
    auto inBear = uint8_t(owned | coa::PrimaryPowerBit(1));
    CHECK_EQ(coa::PowerCapacity(inBear, 1, 100), 1000u);
    CHECK_EQ(coa::PowerCapacity(inBear, 0, 100), 100u);
    CHECK_EQ(coa::PowerCapacity(owned, 1, 100), 0u);
    CHECK_EQ(coa::PowerCapacity(owned, 0, 100), 100u);
    uint32_t energy = 37;
    for (unsigned i = 0; i < 100; ++i)
    {
        energy = PowerRules::ApplyDelta(energy, coa::PowerCapacity(inBear, 3, 100), 0);
        energy = PowerRules::ApplyDelta(energy, coa::PowerCapacity(owned, 3, 100), 0);
    }
    CHECK_EQ(energy, 37u);
}

TEST(CoaPowerRequirements_missing_native_dependency_fails_instead_of_inventing_power)
{
    Spells spells = {{10, Trigger(11)}};
    bool rejected = false;
    try { (void)Required({10}, spells); }
    catch (std::out_of_range const&) { rejected = true; }
    CHECK(rejected);
}

TEST(CoaPowerRequirements_enemy_child_is_not_an_owner_source_after_real_source_removal)
{
    Spells spells = {{10, Trigger(30, 64, 6)}, {20, Trigger(30, 64, 1)}, {30, Gain(1)}};
    CHECK_EQ(Required({10}, spells), 8u);
    CHECK_EQ(Required({10,20}, spells), 10u); // Same child reached in both actor contexts.
    CHECK_EQ(Required({10}, spells), 8u); // Last actual owner source is gone.
    spells[30].hasCost = true;
    spells[30].power = 6;
    CHECK_EQ(Required({10}, spells), 8u); // Enemy also pays its own child's cost.
    CHECK_EQ(Required({20}, spells), 74u);
}

TEST(CoaPowerRequirements_nested_force_cast_and_conditional_owner_return)
{
    Spells spells = {{10, Trigger(20, 140, 6)}, {20, Trigger(30, 64, 1)}, {30, Gain(1)}};
    CHECK_EQ(Required({10}, spells), 8u); // Enemy forces itself, then triggers itself.
    spells[20] = Trigger(30, 140, 27); // A forced unit's owner is not known at reconciliation.
    auto conditional = coa::RequiredPowers(3, 3, {10}, [&](uint32_t id) { return spells.at(id); });
    CHECK_EQ(conditional.powers, 10u);
    CHECK_EQ(conditional.conditional, 2u);
    CHECK(!conditional.unresolved);
    spells[10] = Trigger(20, 140, 1); // Force owner; its master is some other actor, not self.
    CHECK_EQ(Required({10}, spells), 8u);
    spells[20] = Trigger(30, 64, 6);
    CHECK_EQ(Required({10}, spells), 10u); // Explicitly forced self is not reselected from target code6.
}

TEST(CoaPowerRequirements_item_aura_and_force_contexts_follow_real_cost_flags)
{
    Spells spells = {{10, Trigger(20, 142, 1)}, {20, Cost(1)}};
    auto required = [&](SpellResourceContext::PowerCost cost)
    {
        return coa::RequiredPowers(0, 0, {10}, [&](uint32_t id) { return spells.at(id); }, cost).powers;
    };
    CHECK_EQ(required({false,false}), 3u);
    CHECK_EQ(required({true,false}), 1u); // Item inherited by direct child: no check or debit.
    CHECK_EQ(required({false,true}), 3u); // Aura parent is not an aura context for its direct child.
    spells[10] = Trigger(20, 140, 1);
    CHECK_EQ(required({true,true}), 3u); // ForceCast explicitly drops item/aura provenance.
    spells[10] = Trigger(20, 6, 1, 42);
    CHECK_EQ(required({false,false}), 3u); // Proc child still checks power, despite free debit.
    CHECK_EQ(required({true,false}), 1u); // Own item-proc retains its item context.
    spells[10] = Trigger(20, 6, 1, 23);
    CHECK_EQ(required({true,false}), 3u); // Periodic trigger explicitly passes NULL item.
    spells[10] = Trigger(20, 6, 6, 227);
    CHECK_EQ(required({false,false}), 1u); // Periodic child belongs to the enemy recipient.
    spells = {{10, Trigger(20,64,6)}, {20, Trigger(30,6,1,42)},
        {30, Trigger(40,64,27)}, {40, Cost(1,true)}};
    CHECK_EQ(required({true,false}), 3u); // Enemy proc cannot resolve the owner's carried item.
}

TEST(CoaPowerRequirements_missile_and_with_value_keep_formal_caster)
{
    Spells spells = {{10, Trigger(20, 32, 22)}, {20, Trigger(30, 64, 1)}, {30, Cost(1)}};
    CHECK_EQ(Required({10}, spells), 10u); // No explicit child unit target does not erase its caster.
    spells[10] = Trigger(20, 142, 6);
    spells[20] = Gain(1);
    CHECK_EQ(Required({10}, spells), 10u); // Unlike effect64, effect142 does not delegate self-only children.
}

TEST(CoaPowerRequirements_unknown_and_inactive_trigger_columns_are_not_owner_edges)
{
    Spells spells = {{10, Trigger(999, 175)}};
    auto requirements = coa::RequiredPowers(3, 3, {10}, [&](uint32_t id) { return spells.at(id); });
    CHECK_EQ(requirements.powers, 8u);
    CHECK(requirements.unresolved); // Unknown dispatch does not justify an owner pool or child lookup.
    spells[10].effects[0].effect = 0;
    requirements = coa::RequiredPowers(3, 3, {10}, [&](uint32_t id) { return spells.at(id); });
    CHECK_EQ(requirements.powers, 8u);
    CHECK(!requirements.unresolved);
    spells[10] = Trigger(999, 36);
    CHECK_EQ(Required({10}, spells), 8u); // Learn link alone is not an installed spell.
}

TEST(CoaPowerRequirements_relog_respec_and_plan_identity_use_committed_spells)
{
    std::ostringstream text;
    text << "ASCENDER_COA_AUTHORIZATION\t1\t60\t10\nCLASS\t12\t14\nCLASS\t13\t15\n";
    for (unsigned cls : {12,13})
    {
        for (unsigned level = 1; level <= 60; ++level)
        {
            text << "BUDGET\t" << cls << '\t' << level << '\t' << (level < 10 ? 0 : 1) << "\t0\n";
        }
        text << "BASESPELL\t" << cls << '\t' << 900 + cls << "\t1\t1\n";
    }
    for (unsigned entry : {100,101,102,103})
    {
        text << "ENTRY\t" << entry << "\t12\t1\t0\t10\t" << (entry % 2) << "\t0\t1\t0\t0\t0\t0\t0\t0\t0\t0\n"
            << "SPELL\t" << entry << "\t1\t" << 1000 + entry << "\t1\t10\n";
    }
    text << "SPEC\t1\t12\t100\t101\nSPEC\t2\t12\t102\t103\n"
        "OWNER\t100\t1\nOWNER\t101\t1\nOWNER\t102\t2\nOWNER\t103\t2\n";
    auto bytes = text.str();
    unsigned char hash[32];
    size_t hashLength = sizeof(hash);
    REQUIRE(EVP_Q_digest(nullptr, "SHA256", nullptr, bytes.data(), bytes.size(), hash, &hashLength) == 1);
    auto catalog = coa::Catalog::Parse(bytes, testing::BytesToHex(hash, hashLength));
    struct MemoryStore : coa::Store
    {
        coa::State durable;
        coa::LoadStatus Load(uint32_t guid, coa::State& state) override
        {
            state = durable;
            return guid == durable.guid ? coa::LoadStatus::Found : coa::LoadStatus::Missing;
        }
        bool Commit(coa::State const&, coa::State const& next) override
        {
            durable = next;
            return true;
        }
    } store;
    coa::State state;
    state.guid = 42;
    coa::Build build;
    std::string error;
    Spells spells = {{912, Cost(3)}, {1100, {}}, {1101, Cost(0)},
        {1102, {}}, {1103, Cost(2)}, {9901, Cost(6)}};
    auto requirements = [&](coa::Build const& installed)
    {
        auto roots = installed.spells;
        roots.insert(9901); // Independently learned racial/profession remains owned.
        return Required(roots, spells);
    };
    REQUIRE(coa::Replace(*catalog, store, state, 12, 60, {{100,1}}, 100, build, error) == coa::ApplyStatus::Applied);
    CHECK_EQ(requirements(build), 73u); // Energy + mana + independent runic.
    coa::State loaded;
    REQUIRE(store.Load(42, loaded) == coa::LoadStatus::Found);
    coa::Build restored;
    REQUIRE(coa::ValidateLoaded(*catalog, loaded, 42, 12, 60, restored));
    CHECK_EQ(requirements(restored), 73u);
    CHECK(!coa::ValidateLoaded(*catalog, loaded, 43, 12, 60, restored));
    CHECK(!coa::ValidateLoaded(*catalog, loaded, 42, 13, 60, restored));
    loaded.catalogRevision = "wrong-native-catalog";
    CHECK(!coa::ValidateLoaded(*catalog, loaded, 42, 12, 60, restored));
    REQUIRE(coa::Replace(*catalog, store, state, 12, 60, {{102,1}}, 200, build, error) == coa::ApplyStatus::Applied);
    CHECK_EQ(requirements(build), 76u); // Old mana user removed, focus user learned.
    REQUIRE(coa::Replace(*catalog, store, state, 12, 60, {}, 300, build, error) == coa::ApplyStatus::Applied);
    CHECK_EQ(requirements(build), 72u); // Independent ownership survives reset.
    REQUIRE(coa::ValidateLoaded(*catalog, store.durable, 42, 12, 60, restored));
    CHECK_EQ(requirements(restored), 72u);
}
