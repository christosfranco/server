// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "PowerRules.h"
#include "CoaPowerRequirements.h"
#include "SharedDefines.h"
#include "DBCEnums.h"
#include "UpdateFields.h"

#include <array>
#include <map>

namespace PowerTriggerFixture
{
    constexpr uint32 REGEN_TIME_FULL = 2000;
    constexpr uint32 REGEN_TIME_PRECISE = 500;
    constexpr uint32 TYPEID_PLAYER = 4;
    constexpr uint32 SPELLMOD_COST = 14;
    struct Item {};
    struct SpellEntry
    {
        uint32 ID = 0, PowerType = 0, ManaCost = 0, ManaCostPct = 0, SpellLevel = 1, SchoolMask = 1;
        uint32 Attributes = 0, AttributesEx = 0, AttributesExD = 0;
        uint32 Effect[3]{}, ImplicitTargetA[3]{}, ImplicitTargetB[3]{};
        int32 EquippedItemClass = -1;
        int32 EffectMiscValue[3]{};
        uint32 EffectTriggerSpell[3]{};
        bool HasAttribute(SpellAttributes a) const { return Attributes & a; }
        bool HasAttribute(SpellAttributesEx a) const { return AttributesEx & a; }
        bool HasAttribute(SpellAttributesEx4 a) const { return AttributesExD & a; }
    };
    struct Log
    {
        template<class... Args> void outError(Args...) {}
    } sLog;
    class Player;
    struct Unit
    {
        std::array<uint32, MAX_POWERS> current{}, maximum{};
        uint32 health = 300, maxHealth = 500, baseHealth = 200, baseMana = 100, manaUses = 0;
        uint32 GetTypeId() const { return TYPEID_PLAYER; }
        uint32 GetRegenTimer() const { return 2000; }
        uint32 GetHealth() const { return health; }
        uint32 GetMaxHealth() const { return maxHealth; }
        uint32 GetCreateHealth() const { return baseHealth; }
        uint32 GetCreateMana() const { return baseMana; }
        uint32 getLevel() const { return 1; }
        uint32 GetAttackTime(uint32) const { return 2000; }
        int32 GetInt32Value(uint32) const { return 0; }
        float GetFloatValue(uint32) const { return 0; }
        Player* GetSpellModOwner() { return nullptr; }
        uint32 GetPower(Powers p) const { return current.at(p); }
        uint32 GetMaxPower(Powers p) const { return maximum.at(p); }
        void SetHealth(uint32 value) { health = std::min(value, maxHealth); }
        void ApplyPowerMod(Powers p, uint32 value, bool apply)
        {
            current.at(p) = PowerRules::ApplyDelta(current.at(p), maximum.at(p),
                apply ? int64_t(value) : -int64_t(value));
        }
        void SetLastManaUse() { ++manaUses; }
    };
    class Player : public Unit
    {
    public:
        template<class T> void ApplySpellMod(uint32, uint32, T&) {}
        void RegenerateAll(uint32) {}
    };
    struct Spell
    {
        Unit* m_caster;
        SpellEntry const* m_spellInfo;
        Item* m_CastItem = nullptr;
        SpellEntry const* m_triggeredByAuraSpell = nullptr;
        bool m_IsTriggeredSpell = false;
        uint32 m_powerCost = 0;
        SpellSchoolMask m_spellSchoolMask = SPELL_SCHOOL_MASK_NORMAL;
        unsigned runeChecks = 0, runeDebits = 0;
        SpellCastResult CheckOrTakeRunePower(bool take)
        {
            take ? ++runeDebits : ++runeChecks;
            return SPELL_CAST_OK;
        }
        SpellCastResult CheckPower();
        void TakePower();
        static uint32 CalculatePowerCost(SpellEntry const*, Unit*, Spell const* = nullptr, Item* = nullptr);
    };
    inline SpellSchoolMask GetSpellSchoolMask(SpellEntry const* s) { return SpellSchoolMask(s->SchoolMask); }
#define DEBUG_LOG(...) ((void)0)
#include "PowerMethodBodies.inc"
#undef DEBUG_LOG

    coa::PowerSpell Cached(SpellEntry const& spell)
    {
        coa::PowerSpell use;
        use.power = spell.PowerType;
        use.hasCost = spell.ManaCost || spell.ManaCostPct;
        use.equipmentRequired = spell.EquippedItemClass >= 0;
        use.casterSourceOnly = IsSpellWithCasterSourceTargetsOnly(&spell);
        for (unsigned i = 0; i < 3; ++i)
        {
            use.effects[i] = {spell.Effect[i], spell.ImplicitTargetA[i], spell.ImplicitTargetB[i],
                spell.EffectTriggerSpell[i], 0, spell.EffectMiscValue[i]};
        }
        return use;
    }

    TEST(PowerTriggerContext_direct_child_cache_covers_actual_check_and_debit)
    {
        SpellEntry child;
        child.PowerType = POWER_RAGE;
        child.ManaCost = 30;
        child.Effect[0] = 2;
        child.ImplicitTargetA[0] = 6;
        SpellEntry parent;
        parent.Effect[0] = 64;
        parent.ImplicitTargetA[0] = 6;
        parent.EffectTriggerSpell[0] = 11;
        std::map<uint32, coa::PowerSpell> records = {{10, Cached(parent)}, {11, Cached(child)}};
        auto required = coa::RequiredPowers(0, 0, {10}, [&](uint32 id) { return records.at(id); });
        CHECK_EQ(required.powers, 3u);
        Player owner;
        owner.maximum[POWER_RAGE] = coa::PowerCapacity(required.powers, 1, 100);
        owner.current[POWER_RAGE] = std::min(50u, owner.maximum[POWER_RAGE]);
        Spell cast{&owner, &child};
        cast.m_IsTriggeredSpell = true;
        cast.m_powerCost = Spell::CalculatePowerCost(&child, &owner);
        CHECK_EQ(cast.m_powerCost, 30u);
        auto checked = cast.CheckPower();
        CHECK_EQ(checked, SPELL_CAST_OK);
        if (checked == SPELL_CAST_OK)
        {
            cast.TakePower();
        }
        CHECK_EQ(owner.current[POWER_RAGE], 20u);
    }

    TEST(PowerTriggerContext_witchhunter_child_self_is_enemy_not_owner)
    {
        SpellEntry child;
        child.Effect[0] = child.Effect[1] = 3;
        child.Effect[2] = 30;
        child.EffectMiscValue[2] = 1;
        for (auto& target : child.ImplicitTargetA) { target = 1; }
        REQUIRE(IsSpellWithCasterSourceTargetsOnly(&child));
        Player owner, enemy;
        enemy.maximum[POWER_RAGE] = 1000;
        auto context = SpellResourceContext::ResolveTriggerContext(SpellResourceContext::Trigger::Direct,
            static_cast<Unit*>(&owner), static_cast<Unit*>(&enemy), true, false,
            IsSpellWithCasterSourceTargetsOnly(&child), {});
        context.caster->ApplyPowerMod(POWER_RAGE, 100, true);
        CHECK_EQ(owner.current[POWER_RAGE], 0u);
        CHECK_EQ(enemy.current[POWER_RAGE], 100u);
        SpellEntry parent;
        parent.Effect[1] = 64;
        parent.ImplicitTargetA[1] = 6;
        parent.EffectTriggerSpell[1] = 680235;
        std::map<uint32, coa::PowerSpell> records = {{804179, Cached(parent)}, {680235, Cached(child)}};
        CHECK_EQ(coa::RequiredPowers(0, 0, {804179}, [&](uint32 id) { return records.at(id); }).powers, 1u);
        CHECK_EQ(coa::RequiredPowers(0, 0, {680235}, [&](uint32 id) { return records.at(id); }).powers, 3u);
    }

    TEST(PowerTriggerContext_actual_cost_exemptions_keep_triggered_checks)
    {
        Player owner;
        owner.maximum[POWER_RAGE] = 1000;
        SpellEntry child;
        child.PowerType = POWER_RAGE;
        child.ManaCost = 30;
        Item item;
        for (bool triggered : {false, true})
        {
            for (bool aura : {false, true})
            {
                for (bool itemCast : {false, true})
                {
                    Spell cast{&owner, &child};
                    cast.m_IsTriggeredSpell = triggered;
                    cast.m_triggeredByAuraSpell = aura ? &child : nullptr;
                    cast.m_CastItem = itemCast ? &item : nullptr;
                    cast.m_powerCost = Spell::CalculatePowerCost(&child, &owner, &cast, cast.m_CastItem);
                    owner.current[POWER_RAGE] = 0;
                    CHECK_EQ(cast.CheckPower(), itemCast ? SPELL_CAST_OK : SPELL_FAILED_NO_POWER);
                    owner.current[POWER_RAGE] = 50;
                    REQUIRE(cast.CheckPower() == SPELL_CAST_OK);
                    cast.TakePower();
                    CHECK_EQ(owner.current[POWER_RAGE], itemCast || aura ? 50u : 20u);
                }
            }
        }
    }

    TEST(PowerTriggerContext_aura_child_is_checked_but_not_debited_and_percent_cost_keeps_base)
    {
        SpellEntry parent, child;
        parent.Effect[0] = 6;
        parent.ImplicitTargetA[0] = 1;
        parent.EffectTriggerSpell[0] = 11;
        child.PowerType = POWER_MANA;
        child.ManaCostPct = 30;
        child.Effect[0] = 2;
        child.ImplicitTargetA[0] = 6;
        auto cachedParent = Cached(parent);
        cachedParent.effects[0].aura = 42;
        std::map<uint32, coa::PowerSpell> records = {{10,cachedParent},{11,Cached(child)}};
        auto required = coa::RequiredPowers(3, 3, {10}, [&](uint32 id) { return records.at(id); });
        CHECK_EQ(required.powers, 9u);
        Player owner;
        owner.baseMana = coa::RequiredBaseMana(required.powers, 100, 200);
        owner.maximum[POWER_MANA] = 300;
        Spell cast{&owner, &child};
        cast.m_IsTriggeredSpell = true;
        cast.m_triggeredByAuraSpell = &parent;
        cast.m_powerCost = Spell::CalculatePowerCost(&child, &owner);
        CHECK_EQ(cast.m_powerCost, 30u); // Not free due to a missing base pool; not 30% of max300.
        owner.current[POWER_MANA] = 29;
        CHECK_EQ(cast.CheckPower(), SPELL_FAILED_NO_POWER);
        owner.current[POWER_MANA] = 30;
        REQUIRE(cast.CheckPower() == SPELL_CAST_OK);
        cast.TakePower();
        CHECK_EQ(owner.current[POWER_MANA], 30u);
        CHECK_EQ(owner.manaUses, 0u);
    }

    TEST(PowerTriggerContext_equipment_player_branch_and_nested_force_are_exact)
    {
        using namespace SpellResourceContext;
        Player owner, other;
        Unit* ownerPtr = &owner;
        Unit* otherPtr = &other;
        auto player = ResolveTriggerContext(Trigger::Direct, ownerPtr, otherPtr, true, true, true, {});
        CHECK(player.caster == ownerPtr); // Existing equipment guard keeps a player's formal caster.
        auto creature = ResolveTriggerContext(Trigger::Direct, otherPtr, ownerPtr, false, true, true, {});
        CHECK(creature.caster == ownerPtr); // Same native row with an NPC caster uses the else branch.
        auto forced = ResolveTriggerContext(Trigger::Force, ownerPtr, otherPtr, true, false, false, {true,true});
        CHECK(forced.caster == otherPtr && forced.target == otherPtr);
        CHECK(forced.cost.Checks() && forced.cost.Debits());
        auto returned = ResolveTriggerContext(Trigger::Force, forced.caster, ownerPtr, true, false, false, forced.cost);
        CHECK(returned.caster == ownerPtr && returned.target == ownerPtr);
        owner.maximum[POWER_RAGE] = 1000;
        owner.current[POWER_RAGE] = 50;
        SpellEntry child;
        child.PowerType = POWER_RAGE;
        child.ManaCost = 30;
        Spell cast{returned.caster, &child};
        cast.m_IsTriggeredSpell = true;
        cast.m_powerCost = Spell::CalculatePowerCost(&child, returned.caster);
        REQUIRE(cast.CheckPower() == SPELL_CAST_OK);
        cast.TakePower();
        CHECK_EQ(owner.current[POWER_RAGE], 20u);
        CHECK_EQ(other.current[POWER_RAGE], 0u);
    }

    TEST(PowerTriggerContext_target_changes_and_missing_targets_do_not_change_spellbook_ownership)
    {
        using namespace SpellResourceContext;
        Player owner, first, second;
        SpellEntry child, parent;
        child.Effect[0] = 30;
        child.ImplicitTargetA[0] = 1;
        child.EffectMiscValue[0] = 1;
        parent.Effect[0] = 64;
        parent.ImplicitTargetA[0] = 6;
        parent.EffectTriggerSpell[0] = 11;
        std::map<uint32, coa::PowerSpell> records = {{10,Cached(parent)},{11,Cached(child)}};
        for (Unit* target : {static_cast<Unit*>(&first), static_cast<Unit*>(&second), static_cast<Unit*>(nullptr)})
        {
            auto context = ResolveTriggerContext(Trigger::Direct, static_cast<Unit*>(&owner), target,
                true, false, IsSpellWithCasterSourceTargetsOnly(&child), {});
            CHECK(context.caster == target);
            CHECK_EQ(coa::RequiredPowers(0, 0, {10}, [&](uint32 id) { return records.at(id); }).powers, 1u);
        }
        first.health = 0; // A past target dying does not become spellbook ownership.
        CHECK_EQ(coa::RequiredPowers(0, 0, {10}, [&](uint32 id) { return records.at(id); }).powers, 1u);
    }
}
