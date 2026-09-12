// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COA_STARTER_H
#define MANGOS_COA_STARTER_H
#include <array>
#include <cstdint>
#include <set>
#include <vector>

namespace coa
{
    struct StarterSpell
    {
        uint32_t id = 0, displayPower = 0;
        int32_t power = 0;
        uint32_t cost = 0, costPercent = 0;
        int32_t equipmentClass = -1;
        uint32_t subclasses = 0, inventoryTypes = 0;
        bool mainHand = false, offHand = false, ranged = false;
    };
    // Main hand, off hand, ranged weapon, ammunition. These are not client slots.
    enum class StarterSlot : uint8_t { MainHand, OffHand, Ranged, Ammo };
    struct StarterItem
    {
        uint32_t id = 0, itemClass = 0, subclass = 0, inventoryType = 0, skill = 0;
        uint32_t classes = 0, races = 0, level = 0, itemLevel = 0, quality = 0;
        uint32_t stack = 1;
        bool outfit = false, restricted = false;
    };
    struct StarterSkillAccess
    {
        uint32_t classes = 0, races = 0, level = 0;
    };
    struct StarterProficiency
    {
        uint32_t spell = 0, skill = 0;
        int32_t itemClass = -1;
        uint32_t subclasses = 0;
        bool dualWield = false;
        uint32_t classes = 0, races = 0, excludedClasses = 0, excludedRaces = 0;
        uint32_t minimumSkill = 0, level = 0;
        std::vector<StarterSkillAccess> associations;
    };
    enum class ProficiencyAccess { Denied, Native, Policy };
    ProficiencyAccess StarterProficiencyAccess(StarterProficiency const& proficiency, uint32_t playerClass, uint32_t race);
    struct StarterPlan
    {
        StarterSpell spell;
        std::array<StarterItem, 4> gear{};
        std::set<uint32_t> proficiencies, skills;
        std::set<uint32_t> policyProficiencies;
        std::set<uint32_t> nativeProficiencies;
        bool dualWield = false;
    };
    struct StarterEffect
    {
        uint32_t effect, target, aura;
        bool damagingTrigger;
    };
    bool StarterHasDamage(std::array<StarterEffect, 3> const& effects);
    bool StarterNeedsMana(StarterSpell const& spell);
    uint32_t StarterBaseMana(StarterSpell const& spell, uint32_t nativeMana, uint32_t fallbackMana);
    uint32_t StarterPowerCapacity(StarterSpell const& spell, int32_t power, uint32_t baseMana);
    float StarterManaRegenFloor(uint32_t baseMana);
    StarterPlan PlanStarter(StarterSpell const& spell, uint32_t playerClass, uint32_t race,
        std::vector<StarterItem> items, std::vector<StarterProficiency> const& proficiencies);
    // The full per-class starting book (research/ascension-reference, 21.1
    // capture). Empty for classes without a native book (stock, Hero 10).
    std::set<uint32_t> StarterSpells(uint32_t playerClass);
    // Same equipment/resource postcondition used by real Player creation and tests.
    bool StarterReady(StarterPlan const& plan, std::array<StarterItem, 4> const& equipped,
        std::set<uint32_t> const& skills, uint32_t weaponProficiency, uint32_t armorProficiency,
        bool dualWield, bool knowsSpell, uint32_t baseMana, uint32_t baseHealth,
        uint32_t currentPower, uint32_t ammoCount);
}
#endif
