// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COA_POWER_REQUIREMENTS_H
#define MANGOS_COA_POWER_REQUIREMENTS_H

#include "SpellResourceContext.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace coa
{
    // These are existing ordinary-player power slots, not aura-stack currencies.
    inline uint8_t PrimaryPowerBit(int64_t power)
    {
        switch (power)
        {
            case 0: case 1: case 2: case 3: case 6:
                return uint8_t(1u << power);
            default:
                return 0;
        }
    }

    struct PowerEffect
    {
        uint32_t effect = 0, targetA = 0, targetB = 0, child = 0, aura = 0;
        int32_t power = 0;
    };

    struct PowerSpell
    {
        int64_t power = 0;
        bool hasCost = false, passive = false;
        bool equipmentRequired = false, casterSourceOnly = false;
        bool channeled = false, selectedTriggerTarget = false;
        std::array<PowerEffect, 3> effects{};
    };

    // Finite actor identities, never a player's last selected GUID. Creature vs
    // player matters to effect64's equipment/caster-source-only decision.
    enum class PowerActor : uint8_t { None = 0, Owner = 1, OtherPlayer = 2, Creature = 4 };

    inline uint8_t PowerTargets(PowerEffect const& effect, PowerActor caster,
        uint8_t explicitTarget, bool& conditional)
    {
        auto select = [&](uint32_t code) -> uint8_t
        {
            switch (code)
            {
                case 1: case 32: case 87: // SELF / MINION / SELF2
                    return uint8_t(caster);
                case 6: case 25: case 77: // Explicit hostile target.
                    // An already-bound self target survives the executor's
                    // non_caster_target checks; do not reselect it as an enemy.
                    if (explicitTarget == 1) { return 1; }
                    return explicitTarget & (caster == PowerActor::Owner ? 6 : 7);
                case 21: case 35: case 45: case 57:
                    return explicitTarget;
                case 5: case 90: case 94: // Pet / noncombat pet / controlled vehicle.
                    return uint8_t(PowerActor::Creature);
                case 27: case 92: // Charmer/owner or summoner cannot be self.
                    conditional = true;
                    return caster == PowerActor::Owner ? 6 : 7;
                case 23: case 26: case 40: case 51: case 52: // Non-unit targets.
                    return 0;
                case 2: case 7: case 15: case 16: case 24: case 28: case 36:
                case 54: case 60: case 104: case 110:
                    conditional = true;
                    return caster == PowerActor::Owner ? 6 : 7;
                default:
                    // Area, master, script and effect-selected targets depend on
                    // the cast/event. Provision only reachable possible-owner paths.
                    conditional = true;
                    return 7;
            }
        };
        uint8_t targets;
        if (effect.targetA == 1 && (effect.targetB == 0 || effect.targetB == 18 || effect.targetB == 38))
        {
            targets = uint8_t(caster);
        }
        else if (!effect.targetA || effect.targetA == 18 ||
            (effect.targetA == 1 && (effect.targetB == 7 || effect.targetB == 65)))
        {
            targets = select(effect.targetB);
        }
        else if (!effect.targetB)
        {
            targets = select(effect.targetA);
        }
        else
        {
            targets = select(effect.targetA) | select(effect.targetB);
            conditional = true;
        }
        conditional |= targets && (targets & (targets - 1));
        return targets;
    }

    struct PowerRequirements
    {
        uint8_t powers = 0, conditional = 0;
        bool unresolved = false;
    };

    inline PowerRequirements RequiredPowers(int64_t display, int64_t starter,
        std::set<uint32_t> const& learned, std::function<PowerSpell(uint32_t)> const& resolve,
        SpellResourceContext::PowerCost rootCost = {})
    {
        PowerRequirements result{uint8_t(PrimaryPowerBit(display) | PrimaryPowerBit(starter)), 0, false};
        using State = std::tuple<uint32_t, PowerActor, uint8_t, bool, bool, bool>;
        std::set<State> seen;
        std::vector<State> pending;
        std::map<uint32_t, PowerSpell> records;
        auto record = [&](uint32_t id) -> PowerSpell const&
        {
            auto found = records.find(id);
            return found == records.end() ? records.emplace(id, resolve(id)).first->second : found->second;
        };
        auto add = [&](uint32_t id, PowerActor caster, uint8_t target,
            SpellResourceContext::PowerCost cost, bool conditional)
        {
            State state{id, caster, target, cost.item, cost.aura, conditional};
            if (caster != PowerActor::None && seen.insert(state).second)
            {
                if (seen.size() > 100000)
                {
                    throw std::runtime_error("CoA power context closure exceeds bound");
                }
                pending.push_back(state);
            }
        };
        for (auto id : learned)
        {
            add(id, PowerActor::Owner, record(id).passive ? 1 : 7, rootCost, false);
        }
        for (size_t i = 0; i < pending.size(); ++i)
        {
            auto const [id, caster, target, item, aura, uncertain] = pending[i];
            auto const& spell = record(id);
            SpellResourceContext::PowerCost cost{item, aura};
            if (caster == PowerActor::Owner && spell.hasCost && (cost.Checks() || cost.Debits()))
            {
                auto bit = PrimaryPowerBit(spell.power);
                result.powers |= bit;
                if (uncertain) { result.conditional |= bit; }
            }
            for (auto const& effect : spell.effects)
            {
                if (!effect.effect) { continue; } // Inactive native columns are not edges.
                bool conditional = uncertain;
                uint8_t targets = PowerTargets(effect, caster, target, conditional);
                if ((effect.effect == 30 || effect.effect == 137) && (targets & 1))
                {
                    auto bit = PrimaryPowerBit(effect.power);
                    result.powers |= bit;
                    if (conditional) { result.conditional |= bit; }
                }
                if (!effect.child || effect.effect == 36) { continue; } // Learned children are roots only if installed.

                using Kind = SpellResourceContext::Trigger;
                Kind kind;
                switch (effect.effect)
                {
                    case 64: kind = Kind::Direct; break;
                    case 142: kind = Kind::WithValue; break;
                    case 140: case 141: kind = Kind::Force; break;
                    case 32: kind = Kind::Missile; break;
                    case 151: kind = Kind::Ritual; break;
                    case 6:
                    {
                        if (effect.aura == 23 || effect.aura == 227 || effect.aura == 42 || effect.aura == 231)
                        {
                            auto const& child = record(effect.child);
                            bool proc = effect.aura == 42 || effect.aura == 231;
                            uint8_t recipients = child.selectedTriggerTarget && !proc ? 7 : targets;
                            for (auto recipient : {PowerActor::Owner, PowerActor::OtherPlayer, PowerActor::Creature})
                            {
                                if (!(recipients & uint8_t(recipient))) { continue; }
                                // Carried items originate from the spellbook owner;
                                // another recipient cannot resolve that item GUID.
                                add(effect.child, recipient, proc ? 7 : uint8_t(recipient),
                                    {proc && item && recipient == PowerActor::Owner, true}, true);
                            }
                            if (spell.channeled && effect.aura == 23)
                            {
                                add(effect.child, caster, targets, {false, true}, true);
                            }
                        }
                        else { result.unresolved = true; }
                        continue;
                    }
                    default:
                        result.unresolved = true;
                        continue;
                }
                auto const& child = record(effect.child);
                if (kind == Kind::Missile || ((kind == Kind::WithValue || kind == Kind::Ritual) && !targets))
                {
                    auto next = SpellResourceContext::ResolveTriggerContext(kind, caster, PowerActor::None,
                        caster != PowerActor::Creature, child.equipmentRequired, child.casterSourceOnly, cost);
                    add(effect.child, next.caster, 0, next.cost, conditional);
                    continue;
                }
                for (auto recipient : {PowerActor::Owner, PowerActor::OtherPlayer, PowerActor::Creature})
                {
                    if (!(targets & uint8_t(recipient))) { continue; }
                    auto next = SpellResourceContext::ResolveTriggerContext(kind, caster, recipient,
                        caster != PowerActor::Creature, child.equipmentRequired, child.casterSourceOnly, cost);
                    add(effect.child, next.caster, uint8_t(next.target), next.cost, conditional);
                }
            }
        }
        return result;
    }

    inline uint32_t RequiredBaseMana(uint8_t required, uint32_t nativeMana, uint32_t fallbackMana)
    {
        if (!(required & PrimaryPowerBit(0)))
        {
            return 0;
        }
        if (!nativeMana && !fallbackMana)
        {
            throw std::invalid_argument("CoA required mana pool has no base-mana data");
        }
        return nativeMana ? nativeMana : fallbackMana;
    }

    inline uint32_t PowerCapacity(uint8_t required, int64_t power, uint32_t baseMana)
    {
        if (!(required & PrimaryPowerBit(power)))
        {
            return 0;
        }
        switch (power)
        {
            case 0: return baseMana;
            case 1: case 6: return 1000;
            case 2: case 3: return 100;
            default: return 0;
        }
    }
}
#endif
