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
        // Bags-and-weapons plus, 24.2, a shared cloth chest and legs available
        // to every class/race pair. The final two rows are intentionally
        // level-1 ordinary quality with `AllowableClass`/`AllowableRace` fully
        // permissive, matching how a "Recruit's Shirt/Pants"-shaped shared
        // starter looks in the Item store.
        return {
            {25,2,7,21,43,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2092,2,15,13,173,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2362,4,6,14,433,0xffffffff,0xffffffff,1,1,0,1,true,false},
            {2504,2,2,15,45,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2508,2,3,26,46,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2512,6,2,24,0,0xffffffff,0xffffffff,1,5,1,1000,false,false},
            {2516,6,3,24,0,0xffffffff,0xffffffff,1,5,1,1000,false,false},
            // Shared cloth chest (INVTYPE_CHEST=5) and legs (INVTYPE_LEGS=7),
            // subclass 1 (cloth) so every class with the level-one cloth row
            // fits. Skill 0 -- level-one cloth on this DBC has no skill line
            // rank associated with the row, the same way stock 6117 Mail
            // Armor is a level-20 grant not level-1.
            {6116,4,1,5,0,0xffffffff,0xffffffff,1,1,1,1,true,false},
            {6119,4,1,7,0,0xffffffff,0xffffffff,1,1,1,1,true,false}
        };
    }
    std::vector<coa::StarterProficiency> Proficiencies()
    {
        // Native restrictions, including the deployed DBC's minimum skill rank one.
        // 24.2: an armour-proficiency row (spell 9078, Cloth, skill 415,
        // itemClass 4, subclass 1) is required for every class that plans a
        // cloth chest/legs starter; the row's associations mask makes it
        // native for every playable class, matching what the client shows on
        // the character sheet at level 1.
        return {{201,43,2,128,false,512,0,0,0,1,0,{{512,0xffffffff,0}}},
            {1180,173,2,32768,false,512,0,0,0,1,0,{{512,0xffffffff,0}}},
            {9116,433,4,64,false,1,0,0,0,1,0,{{1,0xffffffff,0}}},
            {264,45,2,4,false,16896,0,0,0,1,0,{{16896,0xffffffff,0}}},
            {266,46,2,8,false,4,0,0,0,1,0,{{4,0xffffffff,0}}},
            {674,118,-1,0,true,909,0,0,0,1,20,{{0xffffffff,0xffffffff,1}}},
            {370094,118,-1,0,true,4061165568u,0,0,0,1,0,{{0xffffffff,0xffffffff,1}}},
            {9078,415,4,2,false,0xffffffff,0,0,0,0,0,{{0xffffffff,0xffffffff,1}}}};
    }
    bool Throws(std::function<void()> action)
    {
        try { action(); } catch (std::invalid_argument const&) { return true; }
        return false;
    }
    // Erase the proficiency with a specific spell id. 24.2 added the cloth
    // armour row to Proficiencies() and pushed the pre-existing dual-wield
    // 370094 row off the end, so tests that used to `pop_back()` the last
    // row now name the row they mean.
    std::vector<coa::StarterProficiency> Without(uint32_t spell)
    {
        auto prof = Proficiencies();
        prof.erase(std::remove_if(prof.begin(), prof.end(),
            [&](coa::StarterProficiency const& p) { return p.spell == spell; }),
            prof.end());
        return prof;
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
    // 24.2: the plan now also carries a chest/legs starter for every class;
    // this test measures resource cost, not armour readiness, so it clears
    // the two new slots to keep its assertion strictly about mana and cost.
    plan.gear[4] = {};
    plan.gear[5] = {};
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

TEST(Coa_starter_felsworn_falls_back_to_policy_674_when_no_native_370094_row)
{
    // This realm's DBC snapshot (patch-D, 2026-09-03) has no SkillLineAbility
    // row for 370094; the loader maps stock 674 to the level-1 floor
    // (SpellLevel 20 -> level 1, MinSkillLineRank 1 -> 0). Class 14 must still
    // plan a dual-wield starter, as a POLICY grant, and Native wins when both
    // rows exist (the test above).
    auto prof = Without(370094);        // no native dual-wield row (24.2: was pop_back())
    for (auto& p : prof) { if (p.spell == 674) { p.level = 1; p.minimumSkill = 0; } }
    auto felsworn = coa::PlanStarter(contexts[2], 14, 1, Items(), prof);
    CHECK(felsworn.proficiencies.count(674));
    CHECK(felsworn.policyProficiencies.count(674));
    CHECK(felsworn.skills.count(118));
    CHECK(!felsworn.proficiencies.count(370094));
    auto both = Proficiencies();
    for (auto& p : both) { if (p.spell == 674) { p.level = 1; p.minimumSkill = 0; } }
    auto preferred = coa::PlanStarter(contexts[2], 14, 1, Items(), both);
    CHECK(preferred.proficiencies.count(370094));
    CHECK(!preferred.proficiencies.count(674));
    CHECK(!preferred.policyProficiencies.count(674));   // 1180 stays a policy grant either way
}

TEST(Coa_starter_missing_incompatible_or_restricted_sources_fail_closed)
{
    // Without 370094 AND without the 674 policy exception (level 20 in the
    // vanilla row here), Felsworn has no dual-wield source and PlanStarter
    // throws. 24.2 changed the .pop_back() this used to be to an explicit
    // erase because the cloth-armour row is now the last entry.
    auto prof = Without(370094);
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

// 24.2: every CoA class/race pair plans a chest and legs; the plan carries
// the cloth-armour proficiency for that slot so PrepareCoaStarterInfrastructure
// grants it before EquipCoaStarter runs; and StarterReady gates the runtime
// slot on the armour-proficiency mask.
TEST(Coa_starter_plans_chest_and_legs_for_every_class_and_gates_armor_proficiency)
{
    for (size_t index = 0; index < contexts.size(); ++index)
    {
        auto plan = coa::PlanStarter(contexts[index], uint32_t(index + 12), 1, Items(), Proficiencies());
        CHECK_EQ(plan.gear[4].id, 6116u);              // shared cloth chest
        CHECK_EQ(plan.gear[4].itemClass, 4u);
        CHECK_EQ(plan.gear[4].subclass, 1u);
        CHECK_EQ(plan.gear[4].inventoryType, 5u);
        CHECK_EQ(plan.gear[5].id, 6119u);              // shared cloth legs
        CHECK_EQ(plan.gear[5].itemClass, 4u);
        CHECK_EQ(plan.gear[5].subclass, 1u);
        CHECK_EQ(plan.gear[5].inventoryType, 7u);
        CHECK(plan.proficiencies.count(9078));         // cloth armour granted
        CHECK(plan.nativeProficiencies.count(9078));   // and it is native, not a policy grant
        CHECK(!plan.policyProficiencies.count(9078));
        // Armour proficiency drives the readiness check. The mask must at
        // least cover Cloth (armour subclass 1); a class that also carries a
        // shield in the off-hand slot needs the shield bit set as well, so
        // the "pass" mask is derived from the plan itself. The "fail" mask
        // omits Cloth specifically to prove the chest/legs gate closes.
        uint32_t needed = 0;
        for (auto const& item : plan.gear)
        {
            if (item.id && item.itemClass == 4 && item.subclass < 32)
            {
                needed |= (uint32_t(1) << item.subclass);
            }
        }
        CHECK(needed & (uint32_t(1) << 1));            // every class plans a cloth chest/legs
        uint32_t mana = coa::StarterBaseMana(contexts[index], 0, 100);
        uint32_t power = contexts[index].power == -2 ? 100
            : contexts[index].power == 1 || contexts[index].power == 6 ? 0
            : coa::StarterPowerCapacity(contexts[index], contexts[index].power, mana);
        // Removing the Cloth bit closes the chest/legs gate; every other
        // armour subclass the plan asked for stays granted.
        CHECK(!coa::StarterReady(plan, plan.gear, plan.skills, 0xffffffff, needed & ~(uint32_t(1) << 1),
            plan.dualWield, true, mana, 100, power, 200));
        CHECK(coa::StarterReady(plan, plan.gear, plan.skills, 0xffffffff, needed,
            plan.dualWield, true, mana, 100, power, 200));
    }
}

// The chest/legs slots are gated on FitsSlot: a shield in chest, a robe in
// legs, or a plate item without plate in the mask all trip StarterReady.
TEST(Coa_starter_armor_slots_fail_when_slot_content_or_proficiency_is_wrong)
{
    auto plan = coa::PlanStarter(contexts[0], 12, 1, Items(), Proficiencies());
    // Chest slot with a shield -> FitsSlot rejects (shield subclass 6, invType 14).
    auto swap = plan.gear;
    swap[4] = coa::StarterItem{2362,4,6,14,433,0xffffffff,0xffffffff,1,1,0,1,true,false};
    CHECK(!coa::StarterReady(plan, swap, plan.skills, 0xffffffff, (uint32_t(1) << 1),
        plan.dualWield, true, 100, 100, 100, 200));
    // Legs slot empty (id=0) -> StarterReady skips because plan.gear[5] is
    // still populated, but equipped[5] is zero: FitsSlot on the zero item
    // fails -- confirms the "occupied slot" branch is real, not only a plan
    // absence.
    swap = plan.gear;
    swap[5] = coa::StarterItem{};
    CHECK(!coa::StarterReady(plan, swap, plan.skills, 0xffffffff, (uint32_t(1) << 1),
        plan.dualWield, true, 100, 100, 100, 200));
    // A plate chest (subclass 4) with only cloth (bit 1) in the mask fails
    // even when the plan itself carried the cloth chest, because equipment is
    // what StarterReady reads.
    swap = plan.gear;
    swap[4] = coa::StarterItem{6117,4,4,5,0,0xffffffff,0xffffffff,1,1,1,1,true,false};
    CHECK(!coa::StarterReady(plan, swap, plan.skills, 0xffffffff, (uint32_t(1) << 1),
        plan.dualWield, true, 100, 100, 100, 200));
}

// When no cloth-armour proficiency row is reachable to the class, the planner
// does not synthesize one: 24.2 chose to keep armour Native-only for shared
// cloth, so an absent proficiency row simply leaves the slot empty (best
// effort) rather than granting a policy proficiency the character never had.
TEST(Coa_starter_armor_slot_stays_empty_when_no_native_cloth_proficiency_exists)
{
    auto prof = Without(9078);                          // drop the cloth-armour row
    auto plan = coa::PlanStarter(contexts[0], 12, 1, Items(), prof);
    CHECK_EQ(plan.gear[4].id, 0u);
    CHECK_EQ(plan.gear[5].id, 0u);
    CHECK(!plan.proficiencies.count(9078));
    // A player logging in without a cloth chest and without the proficiency is
    // still ready if the weapon plan is met -- armour is best-effort, not a
    // hard-fail on realms whose DBC snapshot has no shared cloth row.
    uint32_t mana = coa::StarterBaseMana(contexts[0], 0, 100);
    uint32_t power = coa::StarterPowerCapacity(contexts[0], contexts[0].power, mana);
    CHECK(coa::StarterReady(plan, plan.gear, plan.skills, 0xffffffff, 0, plan.dualWield,
        true, mana, 100, power, 0));
}

// The chest/legs items are ordinary shared rows: level-1, quality 1, itemLevel
// <= 5, no restrictions. Restrictions and above-level filters catch them the
// same way they catch weapons, so a level-2 or restricted item is silently
// dropped and the slot returns empty rather than shipping a bad item.
TEST(Coa_starter_armor_selection_respects_level_quality_and_restriction_gates)
{
    auto items = Items();
    // chest at index 7, legs at index 8; move chest above level 1.
    items[7].level = 2;
    auto plan = coa::PlanStarter(contexts[0], 12, 1, items, Proficiencies());
    CHECK_EQ(plan.gear[4].id, 0u);
    CHECK_EQ(plan.gear[5].id, 6119u);
    items = Items();
    items[8].restricted = true;
    plan = coa::PlanStarter(contexts[0], 12, 1, items, Proficiencies());
    CHECK_EQ(plan.gear[5].id, 0u);
    CHECK_EQ(plan.gear[4].id, 6116u);
    items = Items();
    items[7].quality = 2;
    plan = coa::PlanStarter(contexts[0], 12, 1, items, Proficiencies());
    CHECK_EQ(plan.gear[4].id, 0u);
}
