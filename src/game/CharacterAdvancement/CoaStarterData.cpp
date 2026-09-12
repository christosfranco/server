// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoaStarter.h"
#include "CoaCatalog.h"
#include "World.h"
#include "ObjectMgr.h"
#include "Item.h"
#include "DBCStores.h"
#include "SQLStorages.h"
#include "SpellMgr.h"
#include <cmath>
#include <algorithm>
#include <stdexcept>

void World::InitializeCoaStarters()
{
    std::set<uint32> outfitItems;
    for (uint32 i = 0; i < sCharStartOutfitStore.GetNumRows(); ++i)
    {
        if (auto outfit = sCharStartOutfitStore.LookupEntry(i))
        {
            for (auto item : outfit->ItemID)
            {
                if (item > 0)
                {
                    outfitItems.insert(uint32(item));
                }
            }
        }
    }
    std::vector<coa::StarterItem> items;
    // PLAN 23.2: Ascension's Item.dbc carries 38,898 items above 5,000,000
    // (max id 9,200,851) and the world DB mirrors ids from it (8210210), so
    // main's 5,000,000 guard, written against stock's id space, refused to
    // boot this realm. 16 Mi is the DBC index ceiling the core uses elsewhere.
    if (sItemStorage.GetMaxEntry() > 16777216)
    {
        throw std::runtime_error("CoA starter item-index bound exceeded");
    }
    for (uint32 id = 1; id < sItemStorage.GetMaxEntry(); ++id)
    {
        auto p = ObjectMgr::GetItemPrototype(id);
        if (!p || (p->Class != ITEM_CLASS_WEAPON && p->Class != ITEM_CLASS_ARMOR && p->Class != ITEM_CLASS_PROJECTILE) ||
            p->RequiredLevel > 1 || p->ItemLevel > 5 || p->Quality > ITEM_QUALITY_NORMAL)
        {
            continue;
        }
        bool restricted = p->RequiredSkill || p->RequiredSkillRank || p->RequiredSpell || p->RequiredHonorRank ||
            p->RequiredCityRank || p->RequiredReputationFaction || p->ScalingStatDistribution || p->ScalingStatValue ||
            p->StartQuest || p->RandomProperty || p->RandomSuffix || p->ItemSet || p->Area ||
            (p->Map && p->Map != uint32(-1)) || p->Duration || p->HolidayId || p->ItemLimitCategory ||
            (p->Flags2 & (ITEM_FLAG2_HORDE_ONLY | ITEM_FLAG2_ALLIANCE_ONLY)) || p->MaxCount == 1;
        for (auto const& spell : p->Spells)
        {
            restricted |= spell.SpellId != 0;
        }
        float damage = 0;
        for (auto const& d : p->Damage)
        {
            damage += d.DamageMax;
        }
        restricted |= (p->Class != ITEM_CLASS_ARMOR && (!std::isfinite(damage) || damage <= 0));
        if (restricted)
        {
            continue;
        }
        items.push_back({id, p->Class, p->SubClass, p->InventoryType, Item::GetSkill(p), p->AllowableClass,
            p->AllowableRace, p->RequiredLevel, p->ItemLevel, p->Quality, p->GetMaxStackSize(), outfitItems.count(id) != 0, false});
    }
    std::vector<coa::StarterProficiency> proficiencies;
    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        auto skill = sSkillLineAbilityStore.LookupEntry(i);
        auto spell = skill ? sSpellStore.LookupEntry(skill->Spell) : nullptr;
        if (!spell || !sSkillLineStore.LookupEntry(skill->SkillLine))
        {
            continue;
        }
        bool proficiency = false, dualWield = false, otherEffect = false;
        for (auto effect : spell->Effect)
        {
            proficiency |= effect == SPELL_EFFECT_PROFICIENCY;
            dualWield |= effect == SPELL_EFFECT_DUAL_WIELD;
            // PLAN 23.2: Blizzard's weapon-proficiency shape is
            // [SPELL_EFFECT_WEAPON(25), SPELL_EFFECT_PROFICIENCY(60), 0] in
            // stock Spell.dbc (49,839 rows) and unchanged in September's
            // realm layer (239,067 rows). Stock never reached this filter
            // (no CoA classes, compat off), so 25 is benign beside a
            // proficiency; any other co-effect still fails closed.
            otherEffect |= effect && effect != SPELL_EFFECT_PROFICIENCY &&
                effect != SPELL_EFFECT_DUAL_WIELD && effect != SPELL_EFFECT_WEAPON;
        }
        if (otherEffect || (!proficiency && !dualWield) || spell->ManaCost || spell->ManaCostPct ||
            spell->CasterAuraState || spell->TargetAuraState || spell->CasterAuraSpell || spell->TargetAuraSpell ||
            spell->ShapeshiftMask || spell->RequiresSpellFocus ||
            std::any_of(std::begin(spell->Reagent), std::end(spell->Reagent), [](int32 id) { return id > 0; }) ||
            std::any_of(std::begin(spell->Totem), std::end(spell->Totem), [](uint32 id) { return id != 0; }))
        {
            continue;
        }
        // PLAN 23.2: the starter kit is granted at level 1, while
        // MinSkillLineRank/SpellLevel govern trainer progression afterwards.
        // Blizzard's proficiency rows sit at the floor (rank 1: stock
        // SkillLineAbility 252, September's 10269), and September sets Dual
        // Wield (674) to SpellLevel 20 (stock 0) while the realm plays a
        // dual-wield Felsworn at 1 (22.4c). Map the floor into the planner's
        // contract; anything above it stays gated, and the
        // SkillRaceClassInfo level<=1 gate downstream still applies.
        uint32 const minimumSkill = skill->MinSkillLineRank <= 1 ? 0 : skill->MinSkillLineRank;
        uint32 const proficiencyLevel = std::min(spell->SpellLevel, 1u);
        coa::StarterProficiency candidate{spell->ID, skill->SkillLine, spell->EquippedItemClass,
            uint32(spell->EquippedItemSubclass), dualWield, skill->ClassMask, skill->RaceMask,
            skill->ExcludeClass, skill->ExcludeRace, minimumSkill, proficiencyLevel, {}};
        for (uint32 row = 0; row < sSkillRaceClassInfoStore.GetNumRows(); ++row)
        {
            auto access = sSkillRaceClassInfoStore.LookupEntry(row);
            if (access && access->skillId == candidate.skill)
            {
                candidate.associations.push_back({access->classMask, access->raceMask, access->reqLevel});
            }
        }
        proficiencies.push_back(std::move(candidate));
    }
    std::sort(proficiencies.begin(), proficiencies.end(), [](auto const& a, auto const& b) { return a.spell < b.spell; });
    for (uint32 cls = 12; cls <= 32; ++cls)
    {
        auto entry = sSpellStore.LookupEntry(m_coaCatalog->BaseSpell(cls, 1).spell);
        auto classEntry = sChrClassesStore.LookupEntry(cls);
        if (!entry || !classEntry || IsPassiveSpell(entry) || entry->CasterAuraState || entry->TargetAuraState ||
            entry->CasterAuraSpell || entry->TargetAuraSpell || entry->ShapeshiftMask || entry->RequiresSpellFocus)
        {
            throw std::runtime_error("CoA starter has unsupported native prerequisites for class " + std::to_string(cls));
        }
        for (auto reagent : entry->Reagent)
        {
            if (reagent > 0)
            {
                throw std::runtime_error("CoA starter requires reagents");
            }
        }
        std::array<coa::StarterEffect, 3> effects{};
        for (size_t i = 0; i < effects.size(); ++i)
        {
            bool damagingTrigger = false;
            if (auto trigger = sSpellStore.LookupEntry(entry->EffectTriggerSpell[i]))
            {
                for (auto effect : trigger->Effect)
                {
                    damagingTrigger |= effect == SPELL_EFFECT_SCHOOL_DAMAGE || effect == SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL ||
                        effect == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE || effect == SPELL_EFFECT_WEAPON_DAMAGE ||
                        effect == SPELL_EFFECT_NORMALIZED_WEAPON_DMG;
                }
            }
            effects[i] = {entry->Effect[i], entry->ImplicitTargetA[i], entry->EffectAura[i], damagingTrigger};
        }
        if (!coa::StarterHasDamage(effects))
        {
            throw std::runtime_error("CoA starter has no supported hostile-unit damage for class " + std::to_string(cls));
        }
        coa::StarterSpell spell{entry->ID, classEntry->DisplayPower, int32(entry->PowerType), entry->ManaCost,
            entry->ManaCostPct, entry->EquippedItemClass, uint32(entry->EquippedItemSubclass), uint32(entry->EquippedItemInvTypes),
            entry->HasAttribute(SPELL_ATTR_EX3_MAIN_HAND), entry->HasAttribute(SPELL_ATTR_EX3_REQ_OFFHAND),
            entry->HasAttribute(SPELL_ATTR_RANGED)};
        float rate = spell.power == 2 ? getConfig(CONFIG_FLOAT_RATE_POWER_FOCUS)
            : spell.power == 3 ? getConfig(CONFIG_FLOAT_RATE_POWER_ENERGY) : getConfig(CONFIG_FLOAT_RATE_POWER_MANA);
        if ((spell.power == 2 || spell.power == 3 || coa::StarterNeedsMana(spell)) && (!std::isfinite(rate) || rate <= 0))
        {
            throw std::runtime_error("CoA starter requires a positive base resource regeneration rate");
        }
        for (uint32 level = 1; level <= 60; ++level)
        {
            PlayerClassLevelInfo native{}, fallback{};
            sObjectMgr.GetPlayerClassLevelInfo(cls, level, &native);
            sObjectMgr.GetPlayerClassLevelInfo(CLASS_MAGE, level, &fallback);
            (void)coa::StarterBaseMana(spell, native.basemana, fallback.basemana);
        }
        bool hasRace = false;
        for (uint32 race = 1; race < MAX_RACES; ++race)
        {
            if (!sChrRacesStore.LookupEntry(race) || !sObjectMgr.GetPlayerInfo(race, cls))
            {
                continue;
            }
            try
            {
                m_coaStarters.emplace(std::make_pair(cls, race), coa::PlanStarter(spell, cls, race, items, proficiencies));
                hasRace = true;
            }
            catch (std::exception const& error)
            {
                throw std::runtime_error("CoA starter class " + std::to_string(cls) + " race " + std::to_string(race) + ": " + error.what());
            }
        }
        if (!hasRace)
        {
            throw std::runtime_error("CoA class has no supported starter race");
        }
    }
}
