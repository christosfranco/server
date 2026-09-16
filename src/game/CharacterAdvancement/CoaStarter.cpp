// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoaStarter.h"
#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace coa
{
    bool StarterHasDamage(std::array<StarterEffect, 3> const& effects)
    {
        for (auto const& e : effects)
        {
            if (e.target == 6 && ((e.effect == 2 || e.effect == 17 || e.effect == 31 || e.effect == 58 || e.effect == 121) ||
                (e.effect == 6 && (e.aura == 3 || e.aura == 53 || e.aura == 89)) ||
                (e.damagingTrigger && (e.effect == 64 || (e.effect == 6 && (e.aura == 23 || e.aura == 227))))))
            {
                return true;
            }
        }
        return false;
    }
    bool StarterProficiencyEffects(std::array<uint32_t, 3> const& effects)
    {
        bool proficiency = false, dualWield = false, weaponMarker = false;
        for (auto effect : effects)
        {
            if (effect && effect != 60 && effect != 40 && effect != 25)
            {
                return false;
            }
            proficiency |= effect == 60;
            dualWield |= effect == 40;
            weaponMarker |= effect == 25;
        }
        // Native weapon proficiencies pair effect 25's usability marker with 60.
        return (proficiency || dualWield) && (!weaponMarker || proficiency);
    }
    bool StarterNeedsMana(StarterSpell const& spell)
    {
        return spell.power == 0 || spell.displayPower == 0;
    }
    uint32_t StarterBaseMana(StarterSpell const& spell, uint32_t nativeMana, uint32_t fallbackMana)
    {
        if (!StarterNeedsMana(spell))
        {
            return nativeMana;
        }
        if (!nativeMana && !fallbackMana)
        {
            throw std::invalid_argument("CoA mana starter has no base-mana data");
        }
        return nativeMana ? nativeMana : fallbackMana;
    }
    uint32_t StarterPowerCapacity(StarterSpell const& spell, int32_t power, uint32_t baseMana)
    {
        switch (power)
        {
            case 0: return baseMana;
            case 1: return 1000;
            case 2: return spell.power == 2 || spell.displayPower == 2 ? 100 : 0;
            case 3: return 100;
            case 6: return spell.power == 6 || spell.displayPower == 6 ? 1000 : 0;
            default: return 0;
        }
    }
    float StarterManaRegenFloor(uint32_t baseMana)
    {
        // Explicit base-resource policy when custom class spirit tables are absent.
        return float(baseMana) * 0.02f;
    }
    namespace
    {
        bool Fits(StarterSpell const& spell, StarterItem const& item)
        {
            return item.id && item.subclass < 32 && item.inventoryType < 32 &&
                (spell.equipmentClass == -1 || spell.equipmentClass == int32_t(item.itemClass)) &&
                (!spell.subclasses || (spell.subclasses & (uint32_t(1) << item.subclass))) &&
                (!spell.inventoryTypes || (spell.inventoryTypes & (uint32_t(1) << item.inventoryType)));
        }
        bool FitsSlot(StarterItem const& item, StarterSlot slot, bool dualWield)
        {
            switch (slot)
            {
                case StarterSlot::MainHand:
                    return item.itemClass == 2 && (item.inventoryType == 13 || item.inventoryType == 21 ||
                        (!dualWield && item.inventoryType == 17));
                case StarterSlot::OffHand:
                    return dualWield ? item.itemClass == 2 && (item.inventoryType == 13 || item.inventoryType == 22)
                        : item.itemClass == 4 && item.subclass == 6 && item.inventoryType == 14;
                case StarterSlot::Ranged:
                    return item.itemClass == 2 && (item.subclass == 2 || item.subclass == 3 || item.subclass == 18) &&
                        (item.inventoryType == 15 || item.inventoryType == 26);
                case StarterSlot::Ammo:
                    return item.itemClass == 6 && item.inventoryType == 24;
                case StarterSlot::Chest:
                    // Cloth/Leather/Mail/Plate body armour. INVTYPE_CHEST=5 and
                    // INVTYPE_ROBE=20 both equip to EQUIPMENT_SLOT_CHEST.
                    return item.itemClass == 4 && item.subclass >= 1 && item.subclass <= 4 &&
                        (item.inventoryType == 5 || item.inventoryType == 20);
                case StarterSlot::Legs:
                    return item.itemClass == 4 && item.subclass >= 1 && item.subclass <= 4 &&
                        item.inventoryType == 7;
            }
            return false;
        }
    }
    StarterPlan PlanStarter(StarterSpell const& spell, uint32_t playerClass, uint32_t race,
        std::vector<StarterItem> items, std::vector<StarterProficiency> const& proficiencies)
    {
        if (!spell.id || !playerClass || playerClass > 32 || !race || race > 32 ||
            (spell.power != -2 && spell.power != 0 && spell.power != 1 && spell.power != 2 && spell.power != 3 && spell.power != 6) ||
            spell.displayPower > 6 || spell.costPercent > 100 ||
            (spell.equipmentClass != -1 && spell.equipmentClass != 2 && spell.equipmentClass != 4))
        {
            throw std::invalid_argument("CoA unsupported starter context");
        }
        StarterPlan plan;
        plan.spell = spell;
        for (auto const& proficiency : proficiencies)
        {
            if (StarterProficiencyAccess(proficiency, playerClass, race) == ProficiencyAccess::Native)
            {
                plan.nativeProficiencies.insert(proficiency.spell);
            }
        }
        plan.dualWield = spell.offHand;
        if (plan.dualWield)
        {
            // A NATIVE dual-wield row first (the level-zero 370094 where the
            // realm's SkillLineAbility.dbc carries it, docs/coa-advancement.md),
            // and only then a policy grant: the Sep-3 patch-D snapshot this
            // realm is staged from has no 370094 row at all, so class 14 keeps
            // the 674 exception below (22.4c/23.2) until the DBC set is
            // re-staged from an install that has one. Preferring Native means
            // the two never compete on a DBC that carries both.
            auto dw = std::find_if(proficiencies.begin(), proficiencies.end(), [&](StarterProficiency const& p)
                { return p.dualWield && StarterProficiencyAccess(p, playerClass, race) == ProficiencyAccess::Native; });
            if (dw == proficiencies.end())
            {
                dw = std::find_if(proficiencies.begin(), proficiencies.end(), [&](StarterProficiency const& p)
                    { return p.dualWield && StarterProficiencyAccess(p, playerClass, race) == ProficiencyAccess::Policy; });
            }
            if (dw == proficiencies.end())
            {
                throw std::invalid_argument("CoA starter requires native dual-wield proficiency");
            }
            plan.proficiencies.insert(dw->spell);
            plan.skills.insert(dw->skill);
            if (StarterProficiencyAccess(*dw, playerClass, race) == ProficiencyAccess::Policy)
            {
                plan.policyProficiencies.insert(dw->spell);
            }
        }
        std::sort(items.begin(), items.end(), [](StarterItem const& a, StarterItem const& b)
        {
            return std::make_tuple(!a.outfit, a.itemLevel, a.id) < std::make_tuple(!b.outfit, b.itemLevel, b.id);
        });
        for (size_t slot = 0; slot < plan.gear.size(); ++slot)
        {
            auto role = StarterSlot(slot);
            bool needed = role == StarterSlot::MainHand ? spell.mainHand || plan.dualWield ||
                    (spell.equipmentClass == 2 && !spell.ranged)
                : role == StarterSlot::OffHand ? plan.dualWield || spell.equipmentClass == 4
                : role == StarterSlot::Ranged ? spell.ranged
                : role == StarterSlot::Ammo ? spell.ranged
                : role == StarterSlot::Chest || role == StarterSlot::Legs;    // 24.2: always try
            if (!needed)
            {
                continue;
            }
            bool armour = role == StarterSlot::Chest || role == StarterSlot::Legs;
            bool found = false;
            for (auto const& item : items)
            {
                if (!item.id || item.restricted || item.level > 1 || item.itemLevel > 5 || item.quality > 1 ||
                    !(item.classes & (uint32_t(1) << (playerClass - 1))) ||
                    !(item.races & (uint32_t(1) << (race - 1))) || !FitsSlot(item, role, plan.dualWield))
                {
                    continue;
                }
                if (role == StarterSlot::Ammo)
                {
                    uint32_t ammoSubclass = plan.gear[2].subclass == 3 ? 3 : 2;
                    if (item.subclass != ammoSubclass || item.stack < 1)
                    {
                        continue;
                    }
                }
                else if (!armour && (!Fits(spell, item) || !item.skill))
                {
                    // Weapons must satisfy the starter spell's EquippedItemClass/
                    // Subclass/InventoryTypes mask and carry a skill row so the
                    // proficiency association below has something to key on.
                    // Armour body slots are independent of the spell's weapon
                    // mask; they only need a class-armour proficiency.
                    continue;
                }
                auto proficiency = proficiencies.end();
                if (role != StarterSlot::Ammo)
                {
                    // Armour proficiencies key on the item's Class/Subclass. The
                    // starter grants Native proficiencies only for body-armour
                    // slots so a bare-bones cloth chest never depends on a
                    // policy grant. Weapons keep the pre-24.2 rule: any Native
                    // or Policy proficiency the class can access.
                    proficiency = std::find_if(proficiencies.begin(), proficiencies.end(), [&](StarterProficiency const& p)
                    {
                        if (p.dualWield || !p.spell || p.itemClass != int32_t(item.itemClass) ||
                            item.subclass >= 32 || !(p.subclasses & (uint32_t(1) << item.subclass)))
                        {
                            return false;
                        }
                        if (!armour && p.skill != item.skill)
                        {
                            return false;
                        }
                        auto access = StarterProficiencyAccess(p, playerClass, race);
                        return armour ? access == ProficiencyAccess::Native
                                      : access != ProficiencyAccess::Denied;
                    });
                    if (proficiency == proficiencies.end())
                    {
                        continue;
                    }
                }
                plan.gear[slot] = item;
                if (role != StarterSlot::Ammo)
                {
                    plan.proficiencies.insert(proficiency->spell);
                    if (StarterProficiencyAccess(*proficiency, playerClass, race) == ProficiencyAccess::Policy)
                    {
                        plan.policyProficiencies.insert(proficiency->spell);
                    }
                    if (proficiency->skill)
                    {
                        plan.skills.insert(proficiency->skill);
                    }
                }
                found = true;
                break;
            }
            if (!found)
            {
                // Weapon roles remain hard requirements: no runnable class fits
                // the starter spell without them. Body armour is best-effort at
                // plan time -- if the DBC snapshot has no shared cloth chest a
                // class can wear, the character still boots and the login-time
                // repair retries when a compatible item is added to the data
                // set. 24.2 keeps that door open rather than refusing creation.
                if (!armour)
                {
                    throw std::invalid_argument("CoA starter missing compatible gear/proficiency for role " + std::to_string(slot));
                }
            }
        }
        return plan;
    }

    ProficiencyAccess StarterProficiencyAccess(StarterProficiency const& p, uint32_t playerClass, uint32_t race)
    {
        // Starter skills are initialized to rank one before proficiency spells.
        if (!p.spell || !p.skill || !playerClass || playerClass > 32 || !race || race > 32 || p.level > 1 || p.minimumSkill > 1)
        {
            return ProficiencyAccess::Denied;
        }
        uint32_t cls = uint32_t(1) << (playerClass - 1), rac = uint32_t(1) << (race - 1);
        if ((p.races && !(p.races & rac)) || (p.excludedRaces & rac) || (p.excludedClasses & cls))
        {
            return ProficiencyAccess::Denied;
        }
        // Ascender base-kit policy /1. Exact class/spell/skill/effect-mask pairs,
        // NOT a claim of vendor class permission. Only positive class masks may
        // be overridden; races, exclusions, minimum skill and levels still gate.
        bool exception = false;
        if (!p.dualWield && p.spell == 201 && p.skill == 43 && p.itemClass == 2 && p.subclasses == 128)
        {
            exception = playerClass == 12 || playerClass == 17 || playerClass == 19 || playerClass == 25 ||
                playerClass == 30 || playerClass == 32;
        }
        if (!p.dualWield && p.spell == 1180 && p.skill == 173 && p.itemClass == 2 && p.subclasses == 32768)
        {
            exception = playerClass == 14;
        }
        if (!p.dualWield && p.spell == 9116 && p.skill == 433 && p.itemClass == 4 && p.subclasses == 64)
        {
            exception = playerClass == 18;
        }
        if (!p.dualWield && p.spell == 264 && p.skill == 45 && p.itemClass == 2 && p.subclasses == 4)
        {
            exception = playerClass == 15 || playerClass == 21 || playerClass == 26;
        }
        if (!p.dualWield && p.spell == 266 && p.skill == 46 && p.itemClass == 2 && p.subclasses == 8)
        {
            exception = playerClass == 28;
        }
        // Felsworn dual wield (22.4c/23.2). main 833f88a dropped this in favour
        // of the native level-zero 370094 row; this realm's DBC snapshot has
        // none, and PlanStarter prefers a Native row when one exists, so the
        // grant is reached only where 370094 is absent. Stock 674 is SpellLevel
        // 20 in September's layer; the loader maps the level-1 floor.
        if (p.dualWield && p.spell == 674 && p.skill == 118 && p.itemClass == -1 && p.subclasses == 0)
        {
            exception = playerClass == 14;
        }
        bool nativeClass = !p.classes || (p.classes & cls);
        for (auto const& access : p.associations)
        {
            if (access.level > 1 || !(access.races & rac))
            {
                continue;
            }
            if (nativeClass && (access.classes & cls))
            {
                return ProficiencyAccess::Native;
            }
        }
        if (exception && std::any_of(p.associations.begin(), p.associations.end(), [&](StarterSkillAccess const& access)
            { return access.classes && access.level <= 1 && (access.races & rac); }))
        {
            return ProficiencyAccess::Policy;
        }
        return ProficiencyAccess::Denied;
    }
    bool StarterReady(StarterPlan const& plan, std::array<StarterItem, 6> const& equipped,
        std::set<uint32_t> const& skills, uint32_t weaponProficiency, uint32_t armorProficiency,
        bool dualWield, bool knowsSpell, uint32_t baseMana, uint32_t baseHealth,
        uint32_t currentPower, uint32_t ammoCount)
    {
        if (!knowsSpell || (plan.dualWield && !dualWield) || (StarterNeedsMana(plan.spell) && !baseMana))
        {
            return false;
        }
        uint32_t base = plan.spell.power == -2 ? baseHealth : plan.spell.power == 0 ? baseMana
            : StarterPowerCapacity(plan.spell, plan.spell.power, baseMana);
        uint64_t cost = plan.spell.cost + uint64_t(base) * plan.spell.costPercent / 100;
        if (currentPower < cost || (plan.spell.power == -2 && currentPower <= cost))
        {
            return false;
        }
        for (size_t slot = 0; slot < equipped.size(); ++slot)
        {
            if (!plan.gear[slot].id)
            {
                continue;
            }
            auto const& item = equipped[slot];
            auto role = StarterSlot(slot);
            if (!FitsSlot(item, role, plan.dualWield))
            {
                return false;
            }
            if (role == StarterSlot::Ammo)
            {
                if (!ammoCount || item.subclass != (equipped[2].subclass == 3 ? 3u : 2u))
                {
                    return false;
                }
            }
            else if (role == StarterSlot::Chest || role == StarterSlot::Legs)
            {
                // Armour proficiency is the sole runtime gate for body slots;
                // the starter spell's weapon mask does not apply. A skill row is
                // preferred but not universal for level-1 cloth on this DBC.
                if (!(armorProficiency & (uint32_t(1) << item.subclass)))
                {
                    return false;
                }
                if (plan.gear[slot].skill && !skills.count(item.skill))
                {
                    return false;
                }
            }
            else if (!Fits(plan.spell, item) || !skills.count(item.skill) ||
                !((item.itemClass == 2 ? weaponProficiency : armorProficiency) & (uint32_t(1) << item.subclass)))
            {
                return false;
            }
        }
        return true;
    }
}
