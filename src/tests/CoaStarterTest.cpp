// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "CoaStarter.h"
#include "DBCStructure.h"
#include "DBCfmt.h"
#include <functional>
#include <stdexcept>

namespace
{
    // Independent expectations from reviewed JSON base_contexts, classes12..32.
    std::array<coa::StarterSpell, 21> const contexts = {{
        {801787,3,3,60,0,2,0,0,true,false,false},
        {807037,0,0,0,11,-1,0,0,false,false,false},
        {801901,3,3,45,0,2,173555,0,true,true,false},
        {804179,0,0,0,8,2,262156,0,false,false,true},
        {804020,0,0,0,18,-1,0,0,false,false,false},
        {500904,1,1,0,0,2,173555,0,true,false,false},
        {800311,3,3,45,0,4,64,0,false,false,false},
        {805406,3,3,40,0,2,173555,0,true,false,false},
        {500125,1,-2,0,7,-1,0,0,false,false,false},
        {500074,2,2,40,0,2,262148,0,false,false,true},
        {804418,0,0,0,10,-1,0,0,false,false,false},
        {801722,6,0,0,13,-1,0,0,false,false,false},
        {800790,0,0,0,11,-1,0,0,false,false,false},
        {500720,0,0,0,12,2,173555,0,true,false,false},
        {801972,3,3,40,0,2,262156,0,false,false,true},
        {800231,0,0,0,10,-1,0,0,false,false,false},
        {500549,0,0,1,5,2,8,0,false,false,true},
        {800869,0,0,0,13,-1,0,0,false,false,false},
        {500357,6,0,0,0,2,173555,0,true,false,false},
        {500402,0,0,0,12,-1,0,0,false,false,false},
        {707141,0,0,0,7,2,173555,0,true,false,false}
    }};
    std::vector<coa::StarterItem> Items()
    {
        return {
            {25,2,7,21,43,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2092,2,15,13,173,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2362,4,6,14,433,0xffffffff,0xffffffff,1,1,0,1,true,false},
            {2504,2,2,15,45,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2508,2,3,26,46,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2512,6,2,24,0,0xffffffff,0xffffffff,1,5,1,1000,false,false},
            {2516,6,3,24,0,0xffffffff,0xffffffff,1,5,1,1000,false,false}
        };
    }
    std::vector<coa::StarterProficiency> Proficiencies()
    {
        // Native restrictions, including the deployed DBC's minimum skill rank one.
        return {{201,43,2,128,false,512,0,0,0,1,0,{{512,0xffffffff,0}}},
            {1180,173,2,32768,false,512,0,0,0,1,0,{{512,0xffffffff,0}}},
            {9116,433,4,64,false,1,0,0,0,1,0,{{1,0xffffffff,0}}},
            {264,45,2,4,false,16896,0,0,0,1,0,{{16896,0xffffffff,0}}},
            {266,46,2,8,false,4,0,0,0,1,0,{{4,0xffffffff,0}}},
            {674,118,-1,0,true,909,0,0,0,1,20,{{0xffffffff,0xffffffff,1}}},
            {370094,118,-1,0,true,4061165568u,0,0,0,1,0,{{0xffffffff,0xffffffff,1}}}};
    }
    bool Throws(std::function<void()> action)
    {
        try { action(); } catch (std::invalid_argument const&) { return true; }
        return false;
    }
}

TEST(Coa_starter_all_21_contexts_have_runnable_equipment_and_resources)
{
    for (size_t index = 0; index < contexts.size(); ++index)
    {
        auto const& spell = contexts[index];
        auto plan = coa::PlanStarter(spell, uint32_t(index + 12), 1, Items(), Proficiencies());
        uint32_t mana = coa::StarterBaseMana(spell, 0, 100);
        uint32_t power = spell.power == -2 ? 100 : spell.power == 1 || spell.power == 6 ? 0
            : coa::StarterPowerCapacity(spell, spell.power, mana);
        CHECK(coa::StarterReady(plan, plan.gear, plan.skills, 0xffffffff, 0xffffffff, plan.dualWield,
            true, mana, 100, power, 200));
        if (spell.power == 2)
        {
            CHECK_EQ(power, 100u);
        }
    }
}

TEST(Coa_starter_focus_capacity_and_ancillary_mana)
{
    auto const& ranger = contexts[9];
    CHECK_EQ(coa::StarterPowerCapacity(ranger, 2, 0), 100u);
    CHECK_EQ(coa::StarterPowerCapacity(contexts[0], 2, 0), 0u);
    auto const& necromancer = contexts[11];
    CHECK_EQ(necromancer.displayPower, 6u);
    CHECK(coa::StarterNeedsMana(necromancer));
    CHECK_EQ(coa::StarterBaseMana(necromancer, 0, 100), 100u);
    CHECK_EQ(coa::StarterBaseMana(necromancer, 120, 100), 120u);
    CHECK_EQ(coa::StarterPowerCapacity(necromancer, 0, 100), 100u);
    CHECK_EQ(coa::StarterPowerCapacity(necromancer, 6, 100), 1000u);
    CHECK_EQ(coa::StarterManaRegenFloor(100), 2.0f);
    CHECK(Throws([&] { coa::StarterBaseMana(necromancer, 0, 0); }));
    auto plan = coa::PlanStarter(necromancer, 23, 1, Items(), Proficiencies());
    CHECK(!coa::StarterReady(plan, plan.gear, {}, 0, 0, false, true, 0, 100, 100, 0));
    CHECK(!coa::StarterReady(plan, plan.gear, {}, 0, 0, false, true, 100, 100, 12, 0));
    CHECK(coa::StarterReady(plan, plan.gear, {}, 0, 0, false, true, 100, 100, 13, 0));
}

TEST(Coa_starter_dual_wield_shield_ammo_and_proficiency_are_real_requirements)
{
    auto felsworn = coa::PlanStarter(contexts[2], 14, 1, Items(), Proficiencies());
    CHECK(felsworn.gear[0].id && felsworn.gear[1].id);
    CHECK(felsworn.proficiencies.count(370094));
    CHECK(!felsworn.proficiencies.count(674));
    CHECK(felsworn.skills.count(118));
    CHECK(!coa::StarterReady(felsworn, felsworn.gear, felsworn.skills, 0xffffffff, 0, false, true, 0, 100, 100, 0));
    auto equipment = felsworn.gear; equipment[1] = {};
    CHECK(!coa::StarterReady(felsworn, equipment, felsworn.skills, 0xffffffff, 0, true, true, 0, 100, 100, 0));
    CHECK(!coa::StarterReady(felsworn, felsworn.gear, {}, 0xffffffff, 0, true, true, 0, 100, 100, 0));
    CHECK(!coa::StarterReady(felsworn, felsworn.gear, felsworn.skills, 0, 0, true, true, 0, 100, 100, 0));
    auto guardian = coa::PlanStarter(contexts[6], 18, 1, Items(), Proficiencies());
    CHECK_EQ(guardian.gear[1].subclass, 6u);
    CHECK(!coa::StarterReady(guardian, guardian.gear, guardian.skills, 0, 0, false, true, 0, 100, 100, 0));
    for (unsigned cls : {15,21,26,28})
    {
        auto plan = coa::PlanStarter(contexts[cls - 12], cls, 1, Items(), Proficiencies());
        CHECK_EQ(plan.gear[2].subclass, cls == 28 ? 3u : 2u);
        CHECK_EQ(plan.gear[3].subclass, cls == 28 ? 3u : 2u);
        CHECK(!coa::StarterReady(plan, plan.gear, plan.skills, 0xffffffff, 0, false, true, 100, 100, 100, 0));
        equipment = plan.gear; equipment[3].subclass = cls == 28 ? 2 : 3;
        CHECK(!coa::StarterReady(plan, equipment, plan.skills, 0xffffffff, 0, false, true, 100, 100, 100, 200));
    }
}

TEST(Coa_starter_missing_incompatible_or_restricted_sources_fail_closed)
{
    auto prof = Proficiencies(); prof.pop_back();
    CHECK(Throws([&] { coa::PlanStarter(contexts[2], 14, 1, Items(), prof); }));
    CHECK(Throws([&] { coa::PlanStarter(contexts[9], 21, 1, {}, Proficiencies()); }));
    auto items = Items(); items.erase(items.begin() + 3); // No bow, a gun cannot serve Ranger.
    CHECK(Throws([&] { coa::PlanStarter(contexts[9], 21, 1, items, Proficiencies()); }));
    items = Items(); items[3].classes = 1;
    CHECK(Throws([&] { coa::PlanStarter(contexts[9], 21, 1, items, Proficiencies()); }));
    items = Items(); items[3].races = 2;
    CHECK(Throws([&] { coa::PlanStarter(contexts[9], 21, 1, items, Proficiencies()); }));
    items = Items(); items[3].restricted = true;
    CHECK(Throws([&] { coa::PlanStarter(contexts[9], 21, 1, items, Proficiencies()); }));
    items = Items(); items[3].level = 2;
    CHECK(Throws([&] { coa::PlanStarter(contexts[9], 21, 1, items, Proficiencies()); }));
    auto spell = contexts[2]; spell.inventoryTypes = 1u << 17;
    CHECK(Throws([&] { coa::PlanStarter(spell, 14, 1, Items(), Proficiencies()); }));
}

TEST(Coa_starter_effectless_or_pet_commands_do_not_pass_damage_readiness)
{
    CHECK(!coa::StarterHasDamage({}));
    CHECK(!coa::StarterHasDamage({{{2,1,0,false},{},{}}}));
    CHECK(coa::StarterHasDamage({{{2,6,0,false},{},{}}}));
    CHECK(coa::StarterHasDamage({{{6,6,3,false},{},{}}}));
    CHECK(!coa::StarterHasDamage({{{64,6,0,false},{},{}}}));
    CHECK(coa::StarterHasDamage({{{64,6,0,true},{},{}}}));
}

TEST(Coa_proficiency_sources_keep_class_race_skill_level_and_exclusion_gates)
{
    using Access = coa::ProficiencyAccess;
    auto p = Proficiencies()[0];
    CHECK(coa::StarterProficiencyAccess(p, 12, 1) == Access::Policy);
    CHECK(coa::StarterProficiencyAccess(p, 10, 1) == Access::Native);
    CHECK(coa::StarterProficiencyAccess(p, 13, 1) == Access::Denied);
    auto plan = coa::PlanStarter(contexts[0], 12, 1, Items(), Proficiencies());
    CHECK(plan.policyProficiencies.count(201));
    CHECK(!plan.nativeProficiencies.count(201));
    CHECK(!plan.nativeProficiencies.count(674));
    CHECK(!plan.proficiencies.count(674));
    CHECK(coa::StarterProficiencyAccess(Proficiencies()[5], 14, 1) == Access::Denied);
    CHECK(coa::StarterProficiencyAccess(Proficiencies()[6], 14, 1) == Access::Native);
    auto changed = p; changed.spell = 999201;
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Denied);
    changed = p; changed.subclasses |= 32768;
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Denied);
    changed = p; changed.races = 2;
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Denied);
    changed = p; changed.excludedRaces = 1;
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Denied);
    changed = p; changed.excludedClasses = uint32_t(1) << 11;
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Denied);
    changed = p; changed.minimumSkill = 0;
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Policy);
    changed.minimumSkill = 2;
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Denied);
    changed = p; changed.level = 2;
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Denied);
    changed = p; changed.associations.clear();
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Denied);
    changed = p; changed.associations[0].level = 2;
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Denied);
    changed = p; changed.associations[0].races = 2;
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Denied);
    changed = p; changed.classes = uint32_t(1) << 11; changed.associations[0].classes = changed.classes;
    CHECK(coa::StarterProficiencyAccess(changed, 12, 1) == Access::Native);
}

TEST(Coa_proficiency_loader_retains_native_exclusion_columns)
{
    CHECK_EQ(sizeof(SkillLineAbilityEntry), 12u * sizeof(uint32));
    CHECK_EQ(std::string(SkillLineAbilityfmt).size(), 14u);
    CHECK(SkillLineAbilityfmt[5] == 'i' && SkillLineAbilityfmt[6] == 'i');
    SkillLineAbilityEntry entry{};
    entry.ExcludeRace = 1; entry.ExcludeClass = 2048;
    CHECK_EQ(entry.ExcludeRace, 1u); CHECK_EQ(entry.ExcludeClass, 2048u);
}

TEST(Coa_starter_book_matches_the_reference_starting_set)
{
    // research/ascension-reference/<class>.json starting_spells[], verbatim:
    // per-class counts plus the combat-signature ids. Stock classes and Hero
    // 10 have no native book.
    struct Expect { uint32_t cls, count; std::vector<uint32_t> ids; };
    for (auto const& row : {
        Expect{12, 6, {801576}}, Expect{13, 4, {500017}}, Expect{14, 4, {800222, 801901}},
        Expect{15, 11, {500082, 804657, 803165}}, Expect{16, 5, {804020}}, Expect{17, 5, {801016}},
        Expect{18, 9, {500155}}, Expect{19, 7, {704572}}, Expect{20, 4, {500125, 800486}},
        Expect{21, 8, {500074, 805278, 803165}}, Expect{22, 4, {800852}}, Expect{23, 4, {500970}},
        Expect{24, 5, {800790, 805650}}, Expect{25, 9, {500720}}, Expect{26, 10, {800510, 674}},
        Expect{27, 6, {800611, 800764}}, Expect{28, 8, {500234}}, Expect{29, 6, {800869}},
        Expect{30, 7, {500357, 805684}}, Expect{31, 8, {500939, 800093}}, Expect{32, 7, {802202}},
    })
    {
        auto book = coa::StarterSpells(row.cls);
        CHECK_EQ(book.size(), size_t(row.count));
        for (auto id : row.ids) { CHECK(book.count(id)); }
    }
    for (uint32_t cls : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 0u, 33u})
    {
        CHECK(coa::StarterSpells(cls).empty());
    }
}

TEST(Coa_proficiency_accepts_native_weapon_marker_without_combat_effects)
{
    CHECK(coa::StarterProficiencyEffects({25, 60, 0}));
    CHECK(coa::StarterProficiencyEffects({60, 0, 0}));
    CHECK(coa::StarterProficiencyEffects({40, 0, 0}));
    CHECK(!coa::StarterProficiencyEffects({25, 60, 2}));
    CHECK(!coa::StarterProficiencyEffects({25, 40, 0}));
    CHECK(!coa::StarterProficiencyEffects({25, 0, 0}));
    CHECK(!coa::StarterProficiencyEffects({0, 0, 0}));
}
