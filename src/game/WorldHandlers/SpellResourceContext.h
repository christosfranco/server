// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_SPELL_RESOURCE_CONTEXT_H
#define MANGOS_SPELL_RESOURCE_CONTEXT_H

namespace SpellResourceContext
{
    struct PowerCost
    {
        bool item = false;
        bool aura = false;
        // Spell::cast checks even direct and aura-triggered spells. Only the
        // debit has an aura exemption; m_IsTriggeredSpell is not a cost waiver.
        bool Checks() const { return !item; }
        bool Debits() const { return !item && !aura; }
    };

    enum class Trigger { Direct, WithValue, Force, Missile, Ritual };

    template<class Actor>
    struct Cast
    {
        Actor caster, target;
        PowerCost cost;
    };

    // The same formal-caster choice is used by the real effect64 dispatcher and
    // the resource graph. Original-caster GUID is intentionally not the payer.
    template<class Actor>
    Cast<Actor> ResolveTriggerContext(Trigger kind, Actor caster, Actor target,
        bool playerCaster, bool equipmentRequired, bool casterSourceOnly, PowerCost parent)
    {
        switch (kind)
        {
            case Trigger::Direct:
                if (target == Actor{})
                {
                    return {Actor{}, target, {parent.item, false}};
                }
                if (!(equipmentRequired && playerCaster) && casterSourceOnly)
                {
                    caster = target;
                }
                return {caster, target, {parent.item, false}};
            case Trigger::Force:
                return {target, target, {false, false}};
            case Trigger::Missile:
                return {caster, Actor{}, {parent.item, false}};
            case Trigger::Ritual:
                return {caster, target, {false, false}};
            case Trigger::WithValue:
                return {caster, target, {parent.item, false}};
        }
        return {Actor{}, Actor{}, {}};
    }
}
#endif
