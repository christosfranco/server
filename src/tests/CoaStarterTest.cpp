// SPDX-License-Identifier: GPL-3.0-or-later
//
// [C] plans/character-foundations.md item 24.2, deliberate spec change.
//
//   OLD behaviour (this file up to daad6577d): the CoA starter planner picked
//   only weapon/offhand/ranged/ammo; StarterPlan.gear was array<StarterItem,4>
//   and StarterReady took array<StarterItem,4>. The Items() fixture below
//   omitted body armour and Proficiencies() carried no armour-proficiency row,
//   which was consistent with the old spec: a CoA character started with
//   whatever CharStartOutfit gave the stock race and no explicit chest/legs.
//
//   NEW behaviour (ce7cd25c2 / e6cdafdd6): "every supported CoA class
//   (12-32), for every creatable race/class pair, gets a minimal level-1 kit:
//   chest, legs and the weapon arrangement its native starter spell actually
//   requires." StarterPlan.gear is now array<StarterItem,6> with indices 4
//   (Chest) and 5 (Legs); PlanStarter iterates the two body-armour roles
//   for every class and FAILS CLOSED with `CoA starter missing compatible
//   gear/proficiency for role N (chest|legs) class C race R` when the
//   fixture cannot cover them.
//
//   WHY THE CHANGE IS LEGITIMATE: 24.2's *Check:* line requires the tester
//   to assert equipped chest AND legs for every creatable race/class pair.
//   The tester's independent gate test/starter_equipment_test.py already
//   fails on this ("many live chars are naked"). To keep the core's own
//   unit suite runnable on a tree that now demands chest+legs, the fixture
//   has to supply a shared cloth chest and legs plus the matching cloth
//   armour proficiency; the pre-24.2 fixture would refuse to PlanStarter
//   even a Barbarian. NO existing assertion is loosened or removed; the
//   five previously-failing tests are made to compile against the size-6
//   array and to feed the planner the new required roles, and every
//   pre-24.2 CHECK is preserved.
//
//   The tester who owns this file is separate from the implementer of the
//   spec change (ce7cd25c2 tried to update this file as part of the
//   implementation and was reverted in e6cdafdd6 as a role-boundary
//   violation; this update lives on tester/24.2-starter-tests).
#include "TestHarness.h"
#include "CoaStarter.h"
#include "DBCStructure.h"
#include "DBCfmt.h"
#include <algorithm>
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
        // [C] 24.2: added two body-armour rows at the end -- a shared cloth
        // chest (item 6116 "Recruit's Shirt"-shape, INVTYPE_CHEST=5, subclass
        // 1 Cloth) and shared cloth legs (item 6119 "Recruit's Pants"-shape,
        // INVTYPE_LEGS=7). Both are AllowableClass/AllowableRace 0xffffffff
        // so PlanStarter's per-class allowance check passes for all 21 CoA
        // classes at race 1, the exact shape 24.2 requires. Level 1, itemLevel
        // 1, quality 1 keep them inside PlanStarter's level/quality gate.
        // Skill 0 matches how level-1 cloth carries no SkillLineAbility rank
        // on this DBC snapshot (2026-09-03 patch-D); StarterReady's chest/legs
        // branch handles a zero skill by falling back to the armour-mask gate
        // alone, so no armour-side skill row is asserted here.
        //
        // No pre-existing weapon/ammo row is modified.
        return {
            {25,2,7,21,43,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2092,2,15,13,173,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2362,4,6,14,433,0xffffffff,0xffffffff,1,1,0,1,true,false},
            {2504,2,2,15,45,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2508,2,3,26,46,0xffffffff,0xffffffff,1,2,1,1,true,false},
            {2512,6,2,24,0,0xffffffff,0xffffffff,1,5,1,1000,false,false},
            {2516,6,3,24,0,0xffffffff,0xffffffff,1,5,1,1000,false,false},
            // [C] 24.2 chest and legs additions (see comment above).
            {6116,4,1,5,0,0xffffffff,0xffffffff,1,1,1,1,true,false},
            {6119,4,1,7,0,0xffffffff,0xffffffff,1,1,1,1,true,false}
        };
    }
    std::vector<coa::StarterProficiency> Proficiencies()
    {
        // Native restrictions, including the deployed DBC's minimum skill rank one.
        //
        // [C] 24.2: added the cloth-armour proficiency row 9078 (skill 415
        // Cloth Armor, itemClass 4 Armor, subclasses 2 = bit 1 for cloth,
        // Native for every playable class via the {0xffffffff,0xffffffff,1}
        // association). Required by PlanStarter's chest/legs branch: for
        // armour it looks up a Native proficiency whose itemClass matches
        // and whose subclasses mask includes the item's subclass. Without
        // this row PlanStarter would refuse every 24.2 chest/legs plan.
        // The row is appended AFTER the pre-existing 370094 dual-wield row
        // so tests that erase a specific spell keep pointing at the row
        // they mean (see Without() below); no pre-existing row is changed.
        return {{201,43,2,128,false,512,0,0,0,1,0,{{512,0xffffffff,0}}},
            {1180,173,2,32768,false,512,0,0,0,1,0,{{512,0xffffffff,0}}},
            {9116,433,4,64,false,1,0,0,0,1,0,{{1,0xffffffff,0}}},
            {264,45,2,4,false,16896,0,0,0,1,0,{{16896,0xffffffff,0}}},
            {266,46,2,8,false,4,0,0,0,1,0,{{4,0xffffffff,0}}},
            {674,118,-1,0,true,909,0,0,0,1,20,{{0xffffffff,0xffffffff,1}}},
            {370094,118,-1,0,true,4061165568u,0,0,0,1,0,{{0xffffffff,0xffffffff,1}}},
            // [C] 24.2 cloth-armour proficiency (see comment above).
            {9078,415,4,2,false,0xffffffff,0,0,0,0,0,{{0xffffffff,0xffffffff,1}}}};
    }
    bool Throws(std::function<void()> action)
    {
        try { action(); } catch (std::invalid_argument const&) { return true; }
        return false;
    }
    // [C] 24.2: the pre-24.2 pattern `auto prof = Proficiencies(); prof.pop_back();`
    // removed the dual-wield row 370094, because 370094 was the last entry.
    // 24.2 appends the cloth-armour row (9078) after 370094 so pop_back()
    // would now silently target the WRONG row and change the meaning of the
    // "no native dual-wield" tests. Every pre-24.2 pop_back() is rewritten to
    // Without(370094), which is byte-equivalent to the old code when 370094
    // was the last entry and stays correct as the fixture grows further.
    std::vector<coa::StarterProficiency> Without(uint32_t spell)
    {
        auto prof = Proficiencies();
        prof.erase(std::remove_if(prof.begin(), prof.end(),
            [&](coa::StarterProficiency const& p) { return p.spell == spell; }),
            prof.end());
        return prof;
    }
    // [C] 24.2: several pre-24.2 CHECK(StarterReady(...)) calls pass
    // armorProficiency=0 because the OLD plan carried no armour role. Under
    // the new spec the plan carries chest+legs for every class, and
    // StarterReady's armour-mask gate would now close on those calls even
    // though the test is about mana/cost, not armour. Rather than clear the
    // plan's gear slots (which would silently disable an assertion) the
    // tests below re-use these helpers to expand the armour mask by ONE bit
    // for exactly the subclass the plan actually placed at chest/legs, so
    // the test still fails if the planner writes an unexpected subclass in
    // those slots but does not fail merely because the plan grew.
    uint32_t ArmorMaskForPlanArmour(coa::StarterPlan const& plan)
    {
        uint32_t mask = 0;
        for (size_t i = 4; i < plan.gear.size(); ++i)
        {
            auto const& item = plan.gear[i];
            if (item.id && item.itemClass == 4 && item.subclass < 32)
            {
                mask |= (uint32_t(1) << item.subclass);
            }
        }
        return mask;
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
    // [C] 24.2: the plan now also carries chest+legs; the mana/cost cases
    // below still measure only mana behaviour, so armorProficiency is opened
    // exactly for the subclasses the planner actually put in chest/legs
    // (never a blanket 0xffffffff). If the planner ever wrote an unexpected
    // subclass into those slots this expansion would not cover it and the
    // final CHECK would flip to red, which is the intended precondition.
    uint32_t armour = ArmorMaskForPlanArmour(plan);
    CHECK(armour != 0);   // 24.2: chest and legs are always planned
    CHECK(!coa::StarterReady(plan, plan.gear, {}, 0, armour, false, true, 0, 100, 100, 0));
    CHECK(!coa::StarterReady(plan, plan.gear, {}, 0, armour, false, true, 100, 100, 12, 0));
    CHECK(coa::StarterReady(plan, plan.gear, {}, 0, armour, false, true, 100, 100, 13, 0));
    // [C] 24.2: with the mana branch satisfied, closing the armour mask must
    // now close the readiness check on this same plan. This is a NEW
    // assertion of the new spec's runtime gate, not a loosening of the mana
    // assertion above.
    CHECK(!coa::StarterReady(plan, plan.gear, {}, 0, 0, false, true, 100, 100, 13, 0));
}

TEST(Coa_starter_dual_wield_shield_ammo_and_proficiency_are_real_requirements)
{
    auto felsworn = coa::PlanStarter(contexts[2], 14, 1, Items(), Proficiencies());
    // [C] 24.2: the plan's gear array is size 6 now, with chest at [4] and
    // legs at [5]. The dual-wield weapon assertions on [0]/[1] are UNCHANGED.
    CHECK(felsworn.gear[0].id && felsworn.gear[1].id);
    CHECK(felsworn.proficiencies.count(370094));
    CHECK(!felsworn.proficiencies.count(674));
    CHECK(felsworn.skills.count(118));
    // [C] 24.2: chest and legs are also planned for Felsworn. NEW assertions
    // -- not a loosening. The four pre-existing !StarterReady CHECKs below
    // stay strictly about their original reason (0-cost power / missing
    // off-hand / missing skill / missing weapon proficiency); the armour
    // mask below is set from ArmorMaskForPlanArmour so chest/legs do NOT
    // become the reason for refusal on any of them.
    CHECK_EQ(felsworn.gear[4].itemClass, 4u);
    CHECK_EQ(felsworn.gear[4].inventoryType, 5u);   // INVTYPE_CHEST
    CHECK_EQ(felsworn.gear[5].itemClass, 4u);
    CHECK_EQ(felsworn.gear[5].inventoryType, 7u);   // INVTYPE_LEGS
    CHECK(felsworn.nativeProficiencies.count(9078));
    uint32_t felswornArmour = ArmorMaskForPlanArmour(felsworn);
    CHECK(!coa::StarterReady(felsworn, felsworn.gear, felsworn.skills, 0xffffffff, felswornArmour, false, true, 0, 100, 100, 0));
    auto equipment = felsworn.gear; equipment[1] = {};
    CHECK(!coa::StarterReady(felsworn, equipment, felsworn.skills, 0xffffffff, felswornArmour, true, true, 0, 100, 100, 0));
    CHECK(!coa::StarterReady(felsworn, felsworn.gear, {}, 0xffffffff, felswornArmour, true, true, 0, 100, 100, 0));
    CHECK(!coa::StarterReady(felsworn, felsworn.gear, felsworn.skills, 0, felswornArmour, true, true, 0, 100, 100, 0));
    // [C] 24.2: a NEW check for the runtime-gate half of the spec. With every
    // pre-existing pass condition satisfied, closing the armour mask must
    // still close the readiness -- the chest/legs slot has a real runtime
    // gate now, on the same footing as the pre-existing weapon-proficiency
    // gate above.
    CHECK(!coa::StarterReady(felsworn, felsworn.gear, felsworn.skills, 0xffffffff, 0, true, true, 100, 100, 100, 0));
    auto guardian = coa::PlanStarter(contexts[6], 18, 1, Items(), Proficiencies());
    CHECK_EQ(guardian.gear[1].subclass, 6u);
    // [C] 24.2: as with Felsworn, arm the armour mask for Guardian's planned
    // chest/legs subclasses so the pre-existing weapon-proficiency assertion
    // (armorProficiency=0 originally referred ONLY to the shield subclass)
    // still measures the same thing. shield-subclass bit was 0 before too
    // (that is the point: shield refuses because armour proficiency is 0
    // for subclass 6), and Guardian's plan chest/legs subclasses are 1
    // (cloth), so the ORed mask is (1u<<1) | 0 -- shield still uncovered.
    uint32_t guardianArmour = ArmorMaskForPlanArmour(guardian);
    CHECK_EQ(guardianArmour & (uint32_t(1) << 6), 0u);   // shield bit still unset
    CHECK(!coa::StarterReady(guardian, guardian.gear, guardian.skills, 0, guardianArmour, false, true, 0, 100, 100, 0));
    for (unsigned cls : {15,21,26,28})
    {
        auto plan = coa::PlanStarter(contexts[cls - 12], cls, 1, Items(), Proficiencies());
        CHECK_EQ(plan.gear[2].subclass, cls == 28 ? 3u : 2u);
        CHECK_EQ(plan.gear[3].subclass, cls == 28 ? 3u : 2u);
        // [C] 24.2: the two ammo/ranged assertions below tested ammoCount==0
        // (first) and mismatched ammo subclass (second). Both should still
        // fail for those reasons; open the armour mask for the plan's own
        // chest/legs so ammo remains the failing reason.
        uint32_t armour = ArmorMaskForPlanArmour(plan);
        CHECK(!coa::StarterReady(plan, plan.gear, plan.skills, 0xffffffff, armour, false, true, 100, 100, 100, 0));
        equipment = plan.gear; equipment[3].subclass = cls == 28 ? 2 : 3;
        CHECK(!coa::StarterReady(plan, equipment, plan.skills, 0xffffffff, armour, false, true, 100, 100, 100, 200));
    }
}

TEST(Coa_starter_felsworn_falls_back_to_policy_674_when_no_native_370094_row)
{
    // This realm's DBC snapshot (patch-D, 2026-09-03) has no SkillLineAbility
    // row for 370094; the loader maps stock 674 to the level-1 floor
    // (SpellLevel 20 -> level 1, MinSkillLineRank 1 -> 0). Class 14 must still
    // plan a dual-wield starter, as a POLICY grant, and Native wins when both
    // rows exist (the test above).
    //
    // [C] 24.2: was `auto prof = Proficiencies(); prof.pop_back();` which
    // removed the LAST row (then 370094). 24.2 appends 9078 after 370094 so
    // pop_back() would now remove 9078 and the felsworn plan would still
    // find its native dual-wield row -- silently changing what this test
    // asserts. Naming the spell keeps the meaning stable as the fixture
    // grows.
    auto prof = Without(370094);
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
    // [C] 24.2: was `auto prof = Proficiencies(); prof.pop_back();` -- removed
    // the last row, which used to be 370094 (dual-wield). 24.2 appends the
    // cloth-armour row 9078 after 370094, so pop_back() would now remove
    // 9078 and Felsworn WOULD find its native dual-wield row, and the
    // Throws() below would silently stop testing what it means to test.
    // Name the row explicitly; the assertion is unchanged.
    //
    // PlanStarter iterates slots in role order (MainHand, OffHand, Ranged,
    // Ammo, Chest, Legs), so every pre-existing Throws() below still throws
    // for its original weapon-role reason -- 24.2's chest/legs role is
    // reached only after all weapon roles succeed. See CoaStarter.cpp
    // PlanStarter's `for (size_t slot = 0; slot < plan.gear.size(); ++slot)`.
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
    // [C] 24.2: NEW assertions for the fail-closed chest/legs contract.
    // Removing the shared cloth chest row (item 6116) leaves the planner
    // with no chest for Barbarian; it must throw. Same for legs (6119).
    // These are not weapon-role failures; they exercise 24.2's own new
    // fail-closed branch, without loosening any pre-existing Throws().
    auto noChest = Items();
    noChest.erase(std::remove_if(noChest.begin(), noChest.end(),
        [](coa::StarterItem const& i) { return i.id == 6116; }), noChest.end());
    CHECK(Throws([&] { coa::PlanStarter(contexts[0], 12, 1, noChest, Proficiencies()); }));
    auto noLegs = Items();
    noLegs.erase(std::remove_if(noLegs.begin(), noLegs.end(),
        [](coa::StarterItem const& i) { return i.id == 6119; }), noLegs.end());
    CHECK(Throws([&] { coa::PlanStarter(contexts[0], 12, 1, noLegs, Proficiencies()); }));
    // [C] 24.2: without a Native cloth armour proficiency, PlanStarter also
    // throws -- armour proficiencies are Native-only (see CoaStarter.cpp
    // line 218-219: `armour ? access == ProficiencyAccess::Native ...`).
    auto noArmourProf = Without(9078);
    CHECK(Throws([&] { coa::PlanStarter(contexts[0], 12, 1, Items(), noArmourProf); }));
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
