// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoaExtendedModifierRules.h"
#include "StatSystem.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace coa::modifier
{
    namespace
    {
        bool Finite(double value)
        {
            return std::isfinite(value) && std::abs(value) <= ValueBudget;
        }
        bool Valid(Entity const& entity)
        {
            return entity.guid && entity.generation;
        }
        bool Empty(Mask const& mask)
        {
            return mask == Mask{};
        }
        bool Carrier(uint32_t effect)
        {
            switch (effect)
            {
                case 6: case 27: case 35: case 65: case 119: case 128:
                case 129: case 143: case 190: return true;
                default: return false;
            }
        }
        bool ForcedMissSpell(uint32_t spell)
        {
            switch (spell)
            {
                case 680494: case 681218: case 681219: case 681220:
                case 681221: case 681540: return true;
                default: return false;
            }
        }
        bool Coefficient(Field field)
        {
            return field == Field::AttackPowerCoefficient ||
                field == Field::SpellPowerCoefficient ||
                field == Field::RangedAttackPowerCoefficient;
        }
        bool SourceLess(SourceKey const& a, SourceKey const& b)
        {
            return std::tie(a.caster.guid, a.caster.generation, a.holder.guid,
                a.holder.generation, a.spell, a.slot, a.application) <
                std::tie(b.caster.guid, b.caster.generation, b.holder.guid,
                b.holder.generation, b.spell, b.slot, b.application);
        }
    }

    bool operator==(Entity const& a, Entity const& b)
    {
        return a.guid == b.guid && a.generation == b.generation;
    }
    bool operator==(SourceKey const& a, SourceKey const& b)
    {
        return a.caster == b.caster && a.holder == b.holder &&
            a.application == b.application && a.spell == b.spell && a.slot == b.slot;
    }
    bool MatchesFamily(uint32_t family, Mask const& mask,
        uint32_t candidateFamily, Mask const& candidateMask)
    {
        return family == candidateFamily && ((mask[0] & candidateMask[0]) ||
            (mask[1] & candidateMask[1]) || (mask[2] & candidateMask[2]));
    }

    ValueResult CalculateAmount(NativeAmount const& n, AmountContext const& c)
    {
        if (!c.level || c.level > 60 || c.comboPoints > 100 ||
            n.baseLevel > 255 || n.spellLevel > 255 || n.maxLevel > 255 ||
            (n.maxLevel && n.maxLevel < n.baseLevel) ||
            !Finite(n.perLevel) || !Finite(n.perCombo))
        {
            return {};
        }
        int32_t roll = 1;
        if (n.dieSides != 0 && n.dieSides != 1)
        {
            if (!c.dieRoll)
            {
                return {Status::MissingContext, 0};
            }
            roll = *c.dieRoll;
            if (roll < std::min(1, n.dieSides) || roll > std::max(1, n.dieSides))
            {
                return {};
            }
        }
        uint32_t level = std::max(c.level, n.baseLevel);
        if (n.maxLevel)
        {
            level = std::min(level, n.maxLevel);
        }
        double value = double(n.basePoints) + roll +
            std::trunc((double(level) - n.spellLevel) * n.perLevel) +
            std::trunc(c.comboPoints * n.perCombo);
        if (!Finite(value))
        {
            return {Status::Limit, 0};
        }
        if (n.needsNativeScalar)
        {
            if (!c.nativeScalar)
            {
                return {Status::MissingContext, 0};
            }
            if (!Finite(*c.nativeScalar) || *c.nativeScalar <= 0)
            {
                return {};
            }
            value = std::trunc(value * *c.nativeScalar);
        }
        return Finite(value) ? ValueResult{Status::Ready, value} : ValueResult{Status::Limit, 0};
    }

    bool SupportsAura(uint32_t aura)
    {
        switch (aura)
        {
            case 214: case 303: case 308: case 312: case 313: case 316:
            case 317: case 319: case 327: case 328: case 330: case 333:
            case 336: case 338: case 340: case 344: case 345: case 348:
            case 350: case 351: case 353: case 357: case 360: return true;
            default: return false;
        }
    }

    Field SpellModField(int32_t op, uint32_t source)
    {
        switch (op)
        {
            case 0: case 22: case 38: return Field::DamageDone;
            case 1: return Field::Duration;
            case 10: return Field::CastTime;
            case 11: return Field::Cooldown;
            case 14: return Field::Cost;
            case 32: return Field::AttackPowerCoefficient;
            case 34: return Field::TargetCount;
            case 37: return source == 504694 ? Field::CriticalDamageBonus : Field::CriticalHealingBonus;
            case 40: case 41: return Field::SpellPowerCoefficient;
            case 42: case 45: return Field::RangedAttackPowerCoefficient;
            default: return Field::Count;
        }
    }

    Replacement Evaluate(NativeModifier const& n, SourceKey const& key, SourceContext const& c)
    {
        Replacement out;
        out.m_source = key;
        const bool spellmod = n.aura == 107 || n.aura == 108;
        if ((!SupportsAura(n.aura) && !spellmod) || !Carrier(n.effect) ||
            (spellmod && SpellModField(n.miscA, n.spell) == Field::Count))
        {
            out.m_status = Status::Unsupported;
            return out;
        }
        if (!n.spell || n.slot >= 3 || key.spell != n.spell || key.slot != n.slot ||
            !key.application || !Valid(key.caster) || !Valid(key.holder) ||
            n.inputDigest == Digest{} || !Finite(n.amountLimit) || n.amountLimit <= 0 ||
            !c.stacks || c.stacks > std::max(1u, n.stackCap) || n.stackCap > 1000 ||
            (n.rankRoot == 0) != (n.rank == 0) ||
            n.selectedSpells.size() > MaxSelectedSpells ||
            std::any_of(n.selectedSpells.begin(), n.selectedSpells.end(), [](uint32_t id) { return !id; }) ||
            uint32_t(n.group) > uint32_t(Group::PartyHit))
        {
            return out;
        }
        if (!c.authorize || !c.authorize(c.data, key, n))
        {
            out.m_status = Status::Unauthorized;
            return out;
        }
        if (n.effect == 190 && (!(c.petOwner == key.caster) || key.holder == key.caster))
        {
            out.m_status = Status::MissingContext;
            return out;
        }
        if (n.equippedItemClass != -1)
        {
            if (!c.equipmentEligible)
            {
                out.m_status = Status::MissingContext;
                return out;
            }
            if (!*c.equipmentEligible)
            {
                out.m_status = Status::Inactive;
                return out;
            }
        }
        ValueResult value = CalculateAmount(n.amount, c.amount);
        if (value.status != Status::Ready)
        {
            out.m_status = value.status;
            return out;
        }
        out.m_binding = n;
        out.m_area = c.area;
        Contribution term;
        term.amount = value.value * c.stacks;
        auto needStat = [&](std::optional<double> stat, double percent)
        {
            if (!stat)
            {
                out.m_status = Status::MissingContext;
                return false;
            }
            if (!Finite(*stat) || *stat < 0)
            {
                out.m_status = Status::Invalid;
                return false;
            }
            term.amount = *stat * percent / 100.0 * c.stacks;
            return true;
        };
        auto school = [&](int32_t mask)
        {
            if (mask < 0 || mask > 127)
            {
                return false;
            }
            term.schoolMask = uint32_t(mask);
            return true;
        };
        if (spellmod)
        {
            if (Empty(n.effectMask))
            {
                out.m_status = Status::Unsupported;
                return out;
            }
            term.field = SpellModField(n.miscA, n.spell);
            term.arithmetic = n.aura == 107 ? Arithmetic::Flat : Arithmetic::SpellPercent;
            if (Coefficient(term.field) && n.aura == 107)
            {
                term.amount /= 100.0;
            }
            if (n.miscA == 22 || n.miscA == 40 || n.miscA == 45)
            {
                term.phase = Phase::Periodic;
            }
            if (n.miscA == 0 || n.miscA == 41 || n.miscA == 42)
            {
                term.phase = Phase::Direct;
            }
            term.nonPlayerOnly = n.miscA == 38;
            term.criticalOnly = n.miscA == 37;
            term.healingOnly = term.field == Field::CriticalHealingBonus;
            term.damageOnly = term.field == Field::CriticalDamageBonus || term.field == Field::DamageDone;
        }
        else
        {
            switch (n.aura)
            {
                case 333:
                    term.field = Field::HitChance;
                    term.matchMask = false; // The selected policy is all-hit, including MiscA2/23/32.
                    if (ForcedMissSpell(n.spell))
                    {
                        if (value.value != -100) { return out; }
                        term.flags = ForcedMiss;
                        term.arithmetic = Arithmetic::Flag;
                        term.damageOnly = true;
                    }
                    break;
                case 344:
                    term.field = Field::MeleeAttackPower;
                    if (n.spell != 578318)
                    {
                        Contribution ranged = term;
                        ranged.field = Field::RangedAttackPower;
                        out.m_terms.push_back(ranged);
                    }
                    break;
                case 345:
                    if (!school(n.miscA)) { return out; }
                    term.field = Field::DamageSpellPower;
                    if (n.spell == 301353 && !needStat(c.armorPenetrationRating, 100)) { return out; }
                    if (n.spell == 808008)
                    {
                        if (n.effect != 190 || !needStat(c.ownerHealingPower, 10)) { return out; }
                        term.field = Field::DamageDone;
                        term.arithmetic = Arithmetic::Percent;
                    }
                    out.m_terms.push_back(term);
                    term.field = n.spell == 808008 ? Field::HealingDone : Field::HealingSpellPower;
                    break;
                case 317:
                    if (!school(n.miscA)) { return out; }
                    term.field = n.spell == 681088 ? Field::AbsorbBase : Field::AbsorbCapacity;
                    term.arithmetic = Arithmetic::AbsorbPercent;
                    if (n.spell == 681088) { out.m_suppressSlots = 1; }
                    // 500706's genuine zero remains zero; no Insanity-derived coefficient.
                    break;
                case 319:
                    if (!school(n.miscA)) { return out; }
                    term.field = Field::HealingTaken;
                    term.scope = Scope::Target;
                    term.arithmetic = Arithmetic::Percent;
                    if (n.spell == 520326)
                    {
                        term.scope = Scope::Holder;
                        term.field = Field::DamageDone;
                        out.m_terms.push_back(term);
                        term.field = Field::HealingDone;
                    }
                    break;
                case 303: case 360:
                    if (n.miscA != 13 && n.miscA != 18 && n.miscA != 23 && n.miscA != 28)
                    {
                        out.m_status = Status::Unsupported;
                        return out;
                    }
                    if (!school(n.miscB)) { return out; }
                    term.field = n.aura == 303 ? Field::DamageDone : Field::HealingDone;
                    term.arithmetic = Arithmetic::Percent;
                    term.condition = uint32_t(n.miscA);
                    term.poisonAlternative = n.spell == 705050 || n.spell == 707850;
                    term.ownCircleAlternative = n.spell == 520034;
                    if ((n.spell == 300325 || n.spell == 520034) && n.selectedSpells.empty())
                    {
                        out.m_status = Status::MissingContext;
                        return out;
                    }
                    break;
                case 308: case 330: case 338:
                    term.scope = Scope::CasterTarget;
                    term.field = n.aura == 338 ? Field::Armor : Field::CritChance;
                    if (n.aura == 330 && !school(n.miscA)) { return out; }
                    if (n.aura == 338)
                    {
                        term.amount = -term.amount;
                        term.arithmetic = Arithmetic::Percent;
                    }
                    break;
                case 214: case 351:
                    if (!school(n.miscA)) { return out; }
                    if (n.aura == 214 && n.miscB != 0 && n.miscB != 1)
                    {
                        out.m_status = Status::Unsupported;
                        return out;
                    }
                    term.scope = n.aura == 351 || n.miscB == 1 ? Scope::CasterTarget : Scope::Target;
                    term.field = n.aura == 351 ? Field::HealingTaken : Field::DamageTaken;
                    term.arithmetic = Arithmetic::Percent;
                    term.phase = Phase::Periodic;
                    break;
                case 350:
                    if (!school(n.miscA)) { return out; }
                    term.field = Field::CritChance;
                    break;
                case 327: case 328:
                    if (n.miscB < 0 || n.miscB > 4 ||
                        (n.aura == 327 && (n.miscA < 0 || n.miscA > 4)) ||
                        (n.aura == 328 && n.miscA != 0 && n.miscA != -2))
                    {
                        out.m_status = Status::Unsupported;
                        return out;
                    }
                    if (!needStat(c.stats[n.miscB], value.value)) { return out; }
                    term.field = n.aura == 327 ? Field(int(Field::Strength) + n.miscA) :
                        n.miscA == 0 ? Field::MaxMana : Field::MaxHealth;
                    if (n.aura == 328)
                    {
                        auto provisioned = n.miscA == 0 ? c.manaProvisioned : c.healthProvisioned;
                        if (!provisioned)
                        {
                            out.m_status = Status::MissingContext;
                            return out;
                        }
                        if (!*provisioned)
                        {
                            out.m_status = Status::Inactive;
                            return out;
                        }
                        if (n.miscA == 0 && (n.spell == 560080 || n.spell == 561335 || n.spell == 705632))
                        {
                            if (!needStat(c.nativeManaFromIntellect, value.value)) { return out; }
                        }
                    }
                    break;
                case 312: case 313: case 316: case 353:
                    if (Empty(n.effectMask))
                    {
                        out.m_status = Status::Unsupported;
                        return out;
                    }
                    term.arithmetic = Arithmetic::Flag;
                    if (n.aura == 312) { term.field = Field::MinimumRange; term.flags = NoMinimumRange; }
                    if (n.aura == 313) { term.field = Field::CastWhileMoving; term.flags = MovingCast; }
                    if (n.aura == 316) { term.field = Field::PeriodicHaste; term.flags = HastedPeriodic; term.phase = Phase::Periodic; }
                    if (n.aura == 353) { term.field = Field::CastDuringChannel; term.flags = PreserveChannel; term.instantOnly = true; }
                    break;
                case 348:
                    term.field = Field::ChannelDefense;
                    term.arithmetic = Arithmetic::Flag;
                    term.flags = ChannelAvoidance;
                    break;
                case 336:
                    if (!c.area)
                    {
                        out.m_status = Status::MissingContext;
                        return out;
                    }
                    term.field = Field::AreaTargetBoundary;
                    term.scope = Scope::Area;
                    term.arithmetic = Arithmetic::Flag;
                    term.flags = BlockTarget;
                    break;
                case 357:
                    term.field = Field::Cost;
                    term.arithmetic = Arithmetic::SpellPercent;
                    term.amount = -term.amount;
                    term.instantOnly = true;
                    break;
                case 340:
                    if (value.value != 0) { return out; }
                    term.field = Field::Metadata;
                    break;
                default:
                    out.m_status = Status::Unsupported;
                    return out;
            }
        }
        out.m_terms.push_back(term);
        for (auto const& contribution : out.m_terms)
        {
            if (!Finite(contribution.amount) || std::abs(contribution.amount) > n.amountLimit)
            {
                out.m_terms.clear();
                out.m_status = Status::Limit;
                return out;
            }
        }
        out.m_status = Status::Ready;
        return out;
    }

    Status Ledger::Replace(Replacement const& replacement)
    {
        if (replacement.Result() != Status::Ready && replacement.Result() != Status::Inactive)
        {
            return replacement.Result();
        }
        auto entry = std::find_if(m_sources.begin(), m_sources.end(), [&](Replacement const& r)
        {
            auto const& a = r.Source();
            auto const& b = replacement.Source();
            return a.caster == b.caster && a.holder == b.holder && a.spell == b.spell && a.slot == b.slot;
        });
        if (entry != m_sources.end() && entry->Source().application > replacement.Source().application)
        {
            return Status::Stale;
        }
        if (replacement.Result() == Status::Inactive)
        {
            if (entry != m_sources.end()) { m_sources.erase(entry); }
            return Status::Inactive;
        }
        if (entry != m_sources.end())
        {
            *entry = replacement;
        }
        else
        {
            if (m_sources.size() >= MaxSources) { return Status::Limit; }
            m_sources.push_back(replacement);
            std::sort(m_sources.begin(), m_sources.end(), [](Replacement const& a, Replacement const& b)
            {
                return SourceLess(a.Source(), b.Source());
            });
        }
        return Status::Ready;
    }
    bool Ledger::Remove(SourceKey const& key)
    {
        auto entry = std::find_if(m_sources.begin(), m_sources.end(), [&](Replacement const& r)
        {
            return r.Source() == key;
        });
        if (entry == m_sources.end()) { return false; }
        m_sources.erase(entry);
        return true;
    }

    Result Ledger::Calculate(Query const& q, double base, double cap) const
    {
        Result result;
        if (uint32_t(q.field) >= uint32_t(Field::Count) || !Finite(base) || base < 0 ||
            !Finite(cap) || cap < 0 || q.schoolMask > 127 ||
            uint32_t(q.attack) > uint32_t(Attack::Spell) || !Valid(q.actor))
        {
            return result;
        }
        result.value = base;
        float product = 1.0f;
        StatSystem::PercentModifier percentages;
        std::vector<double> factors;
        double partyPower = 0, partyHit = 0;
        bool havePartyPower = false, havePartyHit = false;
        for (auto const& r : m_sources)
        {
            auto const& n = r.m_binding;
            auto const& key = r.m_source;
            if (n.rankRoot && std::any_of(m_sources.begin(), m_sources.end(), [&](Replacement const& other)
            {
                return other.m_source.caster == key.caster && other.m_source.holder == key.holder &&
                    other.m_binding.rankRoot == n.rankRoot && other.m_binding.slot == n.slot &&
                    (other.m_binding.rank > n.rank || (other.m_binding.rank == n.rank &&
                        other.m_source.application > key.application));
            }))
            {
                continue;
            }
            for (auto const& term : r.m_terms)
            {
                if (term.field != q.field ||
                    (term.scope == Scope::Holder && !(q.actor == key.holder)) ||
                    ((term.scope == Scope::Target || term.scope == Scope::CasterTarget) && !(q.target == key.holder)) ||
                    (term.scope == Scope::CasterTarget && !(q.actor == key.caster)) ||
                    (term.phase == Phase::Periodic && !q.periodic) ||
                    (term.phase == Phase::Direct && q.periodic) ||
                    (term.schoolMask && !(term.schoolMask & q.schoolMask)) ||
                    (term.matchMask && !Empty(n.effectMask) && !MatchesFamily(n.family, n.effectMask, q.family, q.familyFlags)) ||
                    (!n.selectedSpells.empty() && std::find(n.selectedSpells.begin(), n.selectedSpells.end(), q.spell) == n.selectedSpells.end()) ||
                    (term.instantOnly && !q.instant) || (term.criticalOnly && !q.critical) ||
                    (term.healingOnly && !q.healing) || (term.damageOnly && !q.damage))
                {
                    continue;
                }
                if (term.nonPlayerOnly)
                {
                    if (!q.playerOpponent) { return {Status::MissingContext}; }
                    if (*q.playerOpponent) { continue; }
                }
                if ((q.field == Field::AbsorbBase || q.field == Field::AbsorbCapacity) && !q.damageShield) { continue; }
                if (Coefficient(q.field))
                {
                    if (!q.hasNativeCoefficient) { return {Status::MissingContext}; }
                    if (!*q.hasNativeCoefficient || base == 0) { continue; }
                }
                if (n.aura == 357 && !q.manaCost) { continue; }
                if ((n.aura == 348 || n.aura == 353) && !q.channeling) { continue; }
                if (term.condition)
                {
                    if (!q.targetState || !q.targetState->maxHealth || q.targetState->health > q.targetState->maxHealth)
                    {
                        return {Status::MissingContext};
                    }
                    auto const& t = *q.targetState;
                    uint64_t health = uint64_t(t.health) * 100;
                    bool matches = (term.condition == 13 && health < uint64_t(t.maxHealth) * 35) ||
                        (term.condition == 23 && health > uint64_t(t.maxHealth) * 75) ||
                        (term.condition == 28 && health > uint64_t(t.maxHealth) * 80) ||
                        (term.condition == 18 && (t.bleeding || (term.poisonAlternative && t.poisoned))) ||
                        (term.ownCircleAlternative && t.ownTranquilCircle);
                    if (!matches) { continue; }
                }
                if (term.scope == Scope::Area)
                {
                    if (!q.areaState) { return {Status::MissingContext}; }
                    auto const& a = *q.areaState;
                    if (a.area != r.m_area || !a.targeted || !a.actorHostileToOwner || a.actorInside == a.targetInside) { continue; }
                }
                ++result.matched;
                result.flags |= term.flags;
                if (term.flags & ForcedMiss)
                {
                    if (!result.forcedMissSource) { result.forcedMissSource = key; }
                }
                if (n.group != Group::Independent && term.arithmetic == Arithmetic::Flat)
                {
                    double& strongest = n.group == Group::PartyPower ? partyPower : partyHit;
                    bool& present = n.group == Group::PartyPower ? havePartyPower : havePartyHit;
                    strongest = present ? std::max(strongest, term.amount) : term.amount;
                    present = true;
                    continue;
                }
                switch (term.arithmetic)
                {
                    case Arithmetic::Flat: result.flat += term.amount; break;
                    case Arithmetic::SpellPercent: case Arithmetic::AbsorbPercent:
                        result.additivePercent += term.amount; break;
                    case Arithmetic::Percent:
                        if (!percentages.Apply(product, float(term.amount), true) || !Finite(product))
                        {
                            return {Status::Limit};
                        }
                        factors.push_back(term.amount);
                        break;
                    case Arithmetic::Flag: break;
                }
            }
        }
        result.flat += partyPower + partyHit;
        // Stock spellmods add flat outside base * summed percent. Aura317 also sums
        // its percentages; ordinary independent aura multipliers retain their product.
        float summed = 1.0f;
        StatSystem::PercentModifier sum;
        if (!Finite(result.flat) || !Finite(result.additivePercent) ||
            !sum.Apply(summed, float(result.additivePercent), true))
        {
            return {Status::Limit};
        }
        result.multiplier = double(summed) * product;
        // Check the wide expression BEFORE float publication can round a budget
        // overrun back down (e.g. 1e9 + 25 rounds to 1e9 in a float).
        double checked = (base * summed + result.flat) * product;
        if (!Finite(checked)) { return {Status::Limit}; }
        float calculated = float(base);
        StatSystem::PercentModifier basePercent;
        if (!basePercent.Apply(calculated, float(result.additivePercent), true) ||
            !StatSystem::ApplyFlatModifier(calculated, float(result.flat), true))
        {
            return {Status::Limit};
        }
        StatSystem::PercentModifier independent;
        for (double factor : factors)
        {
            if (!independent.Apply(calculated, float(factor), true)) { return {Status::Limit}; }
        }
        double value = calculated;
        if (!Finite(value) || !Finite(result.multiplier)) { return {Status::Limit}; }
        double lower = q.field == Field::TargetCount ? 1.0 : 0.0;
        if (cap < lower) { return {}; }
        if (q.field == Field::HitChance || q.field == Field::CritChance) { cap = std::min(cap, 100.0); }
        result.value = std::clamp(value, lower, cap);
        if (q.field == Field::TargetCount) { result.value = std::floor(result.value); }
        if (result.flags & (ForcedMiss | NoMinimumRange)) { result.value = 0; }
        result.status = Status::Ready;
        return result;
    }

    ValueResult MissChance(double baseMiss, double hitBonus, Attack attack)
    {
        if (!Finite(baseMiss) || baseMiss < 0 || baseMiss > 100 ||
            !Finite(hitBonus) || uint32_t(attack) > uint32_t(Attack::Spell))
        {
            return {};
        }
        // Ascender chooses no unavoidable 1% spell miss; retain the caller's native
        // level/skill/dual-wield base miss, subtract percentage points, clamp 0..100.
        return {Status::Ready, std::clamp(baseMiss - hitBonus, 0.0, 100.0)};
    }
    AbsorbResult ConsumeShield(double capacity, double damage)
    {
        if (!Finite(capacity) || !Finite(damage) || capacity < 0 || damage < 0 ||
            capacity != std::floor(capacity) || damage != std::floor(damage))
        {
            return {};
        }
        uint32_t used = uint32_t(std::min(capacity, damage));
        return {Status::Ready, used, uint32_t(capacity) - used, uint32_t(damage) - used};
    }
    ValueResult HastedTickRemaining(double interval, double haste, double progress)
    {
        if (!Finite(interval) || interval <= 0 || !Finite(haste) || haste <= -100 ||
            !std::isfinite(progress) || progress < 0 || progress >= 1)
        {
            return {};
        }
        float multiplier = 1.0f;
        StatSystem::PercentModifier percent;
        if (!percent.Apply(multiplier, float(haste), true) || multiplier <= 0) { return {}; }
        double value = interval / multiplier * (1 - progress);
        return Finite(value) && value > 0 ? ValueResult{Status::Ready, value} : ValueResult{Status::Limit, 0};
    }
}
