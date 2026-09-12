// SPDX-License-Identifier: GPL-3.0-or-later
#include "Spell.h"
#include "CoaCombatIntegration.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellResourceContext.h"
#include "ObjectLookup.h"
#include "Log.h"
#include "DynamicObject.h"
#include "Map.h"
#include <cmath>

using namespace coa::combat;

namespace
{
    bool CoalescedTimer(uint32 spell)
    {
        return spell == 500727 || spell == 500728 || spell == 900755 || spell == 803060;
    }
    bool NativeNeedsRules(SpellEntry const* info, uint32 depth = 0)
    {
        if (!info) { return false; }
        if (HasBindings(info->ID) || FindResource(info->ID) || FindGenerator(info->ID) ||
            info->ID == 804584 || CoalescedTimer(info->ID)) { return true; }
        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (info->Effect[i] >= TOTAL_SPELL_EFFECTS ||
                (IsAuraApplyEffect(info, SpellEffectIndex(i)) && info->EffectAura[i] >= TOTAL_AURAS)) { return true; }
            if ((info->Effect[i] == SPELL_EFFECT_TRIGGER_SPELL || info->Effect[i] == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE) &&
                depth < MaxRuleDepth && NativeNeedsRules(sSpellStore.LookupEntry(info->EffectTriggerSpell[i]), depth + 1)) { return true; }
        }
        return false;
    }
    Target Relation(Unit const& caster, Unit const& target)
    {
        return &caster == &target ? Target::Self : caster.IsFriendlyTo(&target) ? Target::Friendly : Target::Enemy;
    }
    bool NativeShapeSupported(SpellEntry const* info)
    {
        if (!info) { return false; }
        uint32 suppressed = 0;
        for (uint32 slot = 0; slot < MAX_EFFECT_INDEX; ++slot)
        {
            if (auto* binding = FindBinding(info->ID, slot, info->Effect[slot]))
            {
                if (binding->operation == Operation::Unsupported) { return false; }
                suppressed |= (1u << slot) | binding->suppressSlots;
            }
        }
        for (uint32 slot = 0; slot < MAX_EFFECT_INDEX; ++slot)
        {
            bool aura = IsAuraApplyEffect(info, SpellEffectIndex(slot));
            if (FindResource(info->ID) && !aura) { continue; }
            if (suppressed & (1u << slot)) { continue; }
            if (info->Effect[slot] >= TOTAL_SPELL_EFFECTS) { return false; }
            if (aura && info->EffectAura[slot] >= TOTAL_AURAS &&
                !CoaCombatIntegration::IsCounterOnlyComponent(info->ID, slot, info->EffectAura[slot],
                    info->CalculateSimpleValue(SpellEffectIndex(slot)))) { return false; }
        }
        return true;
    }
}

bool Spell::UsesCoaCombatRules() const
{
    Unit* owner = GetAffectiveCaster();
    return owner && owner->GetTypeId() == TYPEID_PLAYER && static_cast<Player*>(owner)->IsCoaManaged() && NativeNeedsRules(m_spellInfo);
}

SpellCastResult Spell::CollectCoaCombatEvents(std::vector<Event>& events, uint32 depth)
{
    if (depth >= MaxRuleDepth || events.size() >= 16) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
    auto* original = GetAffectiveCaster();
    if (!original || original != m_caster || original->GetTypeId() != TYPEID_PLAYER) { return SPELL_FAILED_BAD_TARGETS; }
    auto& owner = *static_cast<Player*>(original);
    if (owner.CoaCombatPublishing()) { return SPELL_FAILED_SPELL_IN_PROGRESS; }
    if (m_CastItem) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
    if (!owner.IsCoaCombatAuthorized(m_spellInfo->ID)) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
    Event event;
    event.spell = m_spellInfo->ID;
    event.source = event.originalCaster = CoaCombatIntegration::Identify(owner);
    auto* selected = m_targets.getUnitTarget() ? m_targets.getUnitTarget() : &owner;
    event.target = CoaCombatIntegration::Identify(*selected);
    event.targetPolicy = Relation(owner, *selected);
    event.ruleDepth = m_coaRuleDepth;
    event.ancestors = m_coaAncestors;
    m_coaSuppressSlots = 0;

    if (CoalescedTimer(m_spellInfo->ID))
    {
        if (m_spellInfo->ID == 803060) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
        event.kind = EventKind::Refresh;
        events.push_back(event);
        m_coaSuppressSlots = 7;
        return SPELL_CAST_OK;
    }
    if (m_spellInfo->ID == 804584)
    {
        event.target = event.source;
        event.targetPolicy = Target::Self;
        events.push_back(event);
        m_coaSuppressSlots = 7;
        return SPELL_CAST_OK;
    }
    // Tooltip-only resource generators (class_resources.lua, no bound
    // 175/178/183 slot): the Cast carries empty effects and every native
    // slot executes normally. 804097 rides effect 175 past stock
    // TOTAL_SPELL_EFFECTS, so its slot is suppressed and the rule covers
    // the +2 instead (the 804098 binding is the +1 precedent).
    if (FindGenerator(m_spellInfo->ID))
    {
        event.target = event.source;
        event.targetPolicy = Target::Self;
        events.push_back(event);
        m_coaSuppressSlots = m_spellInfo->ID == 804097 ? 1 : 0;
        return SPELL_CAST_OK;
    }
    // A native aura application is one resource reward, not a replay of its
    // incidental damage/trigger/conversion slots. 500363's explicit175 entry
    // remains a five-fragment conversion when independently cast.
    if (FindResource(m_spellInfo->ID) &&
        (m_spellInfo->ID != 500363 || m_IsTriggeredSpell))
    {
        Unit* recipient = nullptr;
        for (uint32 slot = 0; slot < MAX_EFFECT_INDEX; ++slot)
        {
            if (!IsAuraApplyEffect(m_spellInfo, SpellEffectIndex(slot))) { continue; }
            recipient = m_spellInfo->ImplicitTargetA[slot] == TARGET_SELF ? &owner : m_targets.getUnitTarget();
            if (m_coaTargetsPrepared)
            {
                recipient = nullptr;
                for (auto const& hit : m_UniqueTargetInfo)
                {
                    if (!(hit.effectMask & (1u << slot))) { continue; }
                    if (recipient || hit.missCondition != SPELL_MISS_NONE) { return SPELL_FAILED_BAD_TARGETS; }
                    recipient = ObjectLookup::GetUnit(owner, hit.targetGUID);
                }
            }
            break;
        }
        if (!recipient) { return SPELL_FAILED_BAD_TARGETS; }
        event.target = CoaCombatIntegration::Identify(*recipient);
        event.targetPolicy = Relation(owner, *recipient);
        if ((m_spellInfo->ID == 804670 || m_spellInfo->ID == 804711) && owner.KnowsCoaCombatSpell(807693))
        {
            Event cap = event;
            cap.kind = EventKind::CapModifier;
            cap.spell = 807693;
            Effect input;
            input.slot = m_spellInfo->ID == 804670 ? 0 : 1;
            input.type = 175;
            input.child = m_spellInfo->ID;
            input.target = event.target;
            input.targetPolicy = event.targetPolicy;
            cap.effects.Push(input);
            events.push_back(cap);
        }
        event.kind = EventKind::ResourceGain;
        event.resource = m_spellInfo->ID;
        event.amount = 1;
        events.push_back(event);
        m_coaSuppressSlots = 7;
        return SPELL_CAST_OK;
    }

    for (uint32 slot = 0; slot < MAX_EFFECT_INDEX; ++slot)
    {
        auto* binding = FindBinding(m_spellInfo->ID, slot, m_spellInfo->Effect[slot]);
        if (binding) { m_coaSuppressSlots |= (1u << slot) | binding->suppressSlots; }
    }
    uint32 ownedSlots = m_coaSuppressSlots;
    for (uint32 slot = 0; slot < MAX_EFFECT_INDEX; ++slot)
    {
        auto effect = m_spellInfo->Effect[slot];
        auto* binding = FindBinding(m_spellInfo->ID, slot, effect);
        if (binding)
        {
            if (binding->operation == Operation::Unsupported) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
            if ((binding->operation == Operation::Delay || binding->operation == Operation::Duration ||
                binding->operation == Operation::SummonDuration || binding->operation == Operation::DelayAtDestination ||
                binding->operation == Operation::DurationAtDestination) &&
                !NativeShapeSupported(sSpellStore.LookupEntry(binding->child))) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
            bool atDestination = binding->operation == Operation::DelayAtDestination || binding->operation == Operation::DurationAtDestination;
            if (atDestination)
            {
                Destination location;
                location.owner = CoaCombatIntegration::Identify(owner);
                location.phase = owner.GetPhaseMask();
                location.mode = m_spellInfo->ID == 802631 || m_spellInfo->ID == 806415 ?
                    DestinationMode::PersistentArea : DestinationMode::Recipients;
                auto point = owner.Where().Pos();
                if (m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)
                {
                    point = Geometry::Vector3(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ);
                }
                else if (m_spellInfo->ID == 806039 && m_targets.getUnitTarget())
                {
                    if (!owner.Where().ShareFrame(m_targets.getUnitTarget()->Where())) { return SPELL_FAILED_BAD_TARGETS; }
                    point = m_targets.getUnitTarget()->Where().Pos();
                }
                location.x = point.x; location.y = point.y; location.z = point.z;
                if (location.mode == DestinationMode::Recipients && m_coaTargetsPrepared)
                {
                    for (auto const& hit : m_UniqueTargetInfo)
                    {
                        if (!(hit.effectMask & (1u << slot))) { continue; }
                        auto* unit = ObjectLookup::GetUnit(owner, hit.targetGUID);
                        if (!unit || hit.missCondition != SPELL_MISS_NONE ||
                            !location.recipients.Push(CoaCombatIntegration::Identify(*unit))) { return SPELL_FAILED_BAD_TARGETS; }
                    }
                }
                Effect input;
                input.slot = slot; input.type = effect; input.child = binding->child;
                input.calculatedValue = CalculateDamage(SpellEffectIndex(slot), &owner);
                input.location = location;
                if (location.mode == DestinationMode::PersistentArea) { m_coaLocation = location; }
                input.payload.nativeFlags = m_spellInfo->EffectMiscValue[slot];
                auto const& mask = m_spellInfo->EffectSpellClassMask[slot];
                auto const& family = m_spellInfo->SpellClassMask;
                input.payload.effectMask = {uint32(mask.Flags), uint32(mask.Flags >> 32), mask.Flags2};
                input.payload.familyMask = {uint32(family.Flags), uint32(family.Flags >> 32), family.Flags2};
                input.payload.procMask = m_spellInfo->ProcTypeMask;
                event.effects.Push(input);
                continue;
            }
            Unit* target = m_spellInfo->ImplicitTargetA[slot] == TARGET_SELF ? &owner : m_targets.getUnitTarget();
            if (m_coaTargetsPrepared)
            {
                target = nullptr;
                for (auto const& hit : m_UniqueTargetInfo)
                {
                    if (!(hit.effectMask & (1u << slot))) { continue; }
                    if (target || hit.missCondition != SPELL_MISS_NONE) { return SPELL_FAILED_BAD_TARGETS; }
                    target = ObjectLookup::GetUnit(owner, hit.targetGUID);
                }
            }
            if (!target || !CheckTarget(target, SpellEffectIndex(slot)) || !target->IsAlive() ||
                target->IsImmuneToSpellEffect(m_spellInfo, SpellEffectIndex(slot), target == &owner)) { return SPELL_FAILED_BAD_TARGETS; }
            Effect input;
            input.slot = slot;
            input.type = effect;
            input.child = m_spellInfo->EffectTriggerSpell[slot];
            input.calculatedValue = CalculateDamage(SpellEffectIndex(slot), target);
            input.target = CoaCombatIntegration::Identify(*target);
            input.targetPolicy = Relation(owner, *target);
            auto const& mask = m_spellInfo->EffectSpellClassMask[slot];
            auto const& family = m_spellInfo->SpellClassMask;
            input.payload.effectMask = {uint32(mask.Flags), uint32(mask.Flags >> 32), mask.Flags2};
            input.payload.familyMask = {uint32(family.Flags), uint32(family.Flags >> 32), family.Flags2};
            input.payload.procMask = m_spellInfo->ProcTypeMask;
            input.payload.nativeFlags = m_spellInfo->EffectMiscValue[slot];
            if (m_spellInfo->ID == 560233)
            {
                int32 amount = CalculateDamage(EFFECT_INDEX_0, target);
                input.payload.basePoints = {amount, amount, amount};
            }
            event.effects.Push(input);
            if (binding->operation == Operation::Cap) { event.kind = EventKind::CapModifier; }
            continue;
        }
        if (ownedSlots & (1u << slot)) { continue; }
        if (effect >= TOTAL_SPELL_EFFECTS ||
            (IsAuraApplyEffect(m_spellInfo, SpellEffectIndex(slot)) && m_spellInfo->EffectAura[slot] >= TOTAL_AURAS))
        {
            sLog.outError("CoA spell %u has unsupported companion slot %u effect %u aura %u",
                m_spellInfo->ID, slot, effect, m_spellInfo->EffectAura[slot]);
            return SPELL_FAILED_SPELL_UNAVAILABLE;
        }
        if (effect != SPELL_EFFECT_TRIGGER_SPELL && effect != SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE) { continue; }
        auto* child = sSpellStore.LookupEntry(m_spellInfo->EffectTriggerSpell[slot]);
        if (!NativeNeedsRules(child)) { continue; }
        Unit* target = m_spellInfo->ImplicitTargetA[slot] == TARGET_SELF ? &owner : m_targets.getUnitTarget();
        if (m_coaTargetsPrepared)
        {
            target = nullptr;
            for (auto const& hit : m_UniqueTargetInfo)
            {
                if (!(hit.effectMask & (1u << slot))) { continue; }
                if (target || hit.missCondition != SPELL_MISS_NONE) { return SPELL_FAILED_BAD_TARGETS; }
                target = ObjectLookup::GetUnit(owner, hit.targetGUID);
            }
        }
        auto context = SpellResourceContext::ResolveTriggerContext(
            effect == SPELL_EFFECT_TRIGGER_SPELL ? SpellResourceContext::Trigger::Direct : SpellResourceContext::Trigger::WithValue,
            m_caster, target, true, child->EquippedItemClass >= 0, IsSpellWithCasterSourceTargetsOnly(child),
            {m_CastItem != nullptr, m_triggeredByAuraSpell != nullptr});
        if (context.caster != &owner || !target) { return SPELL_FAILED_BAD_TARGETS; }
        // Coalesce only exact resource-only helpers; never suppress a child's
        // unrelated native damage/summon/script effects or change stock64 globally.
        Spell nested(context.caster, child, true, m_originalCasterGUID, m_spellInfo);
        nested.m_targets.setUnitTarget(target);
        nested.m_powerCost = CalculatePowerCost(child, context.caster, &nested, m_CastItem);
        if (nested.m_powerCost) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
        if (effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE)
        {
            int32 amount = CalculateDamage(SpellEffectIndex(slot), target);
            for (auto& bp : nested.m_currentBasePoints) { bp = amount; }
        }
        auto result = nested.CollectCoaCombatEvents(events, depth + 1);
        if (result != SPELL_CAST_OK) { return result; }
        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (child->Effect[i] && !(nested.m_coaSuppressSlots & (1u << i))) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
        }
        m_coaSuppressSlots |= 1u << slot;
    }
    if (event.effects.size) { events.push_back(event); }
    if (events.empty())
    {
        event.kind = EventKind::Refresh;
        events.push_back(event);
    }
    return SPELL_CAST_OK;
}

SpellCastResult Spell::CheckCoaCombatRules()
{
    if (!UsesCoaCombatRules()) { return SPELL_CAST_OK; }
    std::vector<Event> events;
    auto result = CollectCoaCombatEvents(events);
    if (result != SPELL_CAST_OK) { return result; }
    CoaCombatTransaction preview(*static_cast<Player*>(m_caster), std::move(events));
    if (SpellResourceContext::PowerCost{m_CastItem != nullptr, m_triggeredByAuraSpell != nullptr}.Debits())
    {
        preview.ReservePower(m_spellInfo->PowerType, m_powerCost);
    }
    return preview.Prepare();
}

SpellCastResult Spell::PrepareCoaCombatRules()
{
    if (!UsesCoaCombatRules()) { return SPELL_CAST_OK; }
    if (m_coaPreparedOwner.guid && m_coaPreparedOwner != CoaCombatIntegration::Identify(*m_caster)) { return SPELL_FAILED_BAD_TARGETS; }
    for (size_t i = 0; i < m_coaTargetLives.size; ++i)
    {
        auto* target = CoaCombatIntegration::Resolve(*m_caster, m_coaTargetLives.values[i]);
        if (!target || !target->IsAlive() || target->IsImmuneToSpell(m_spellInfo, target == m_caster) ||
            (target != m_caster && !m_caster->IsFriendlyTo(target) && target->IsImmuneToDamage(GetSpellSchoolMask(m_spellInfo))))
        {
            return SPELL_FAILED_IMMUNE;
        }
    }
    for (auto const& target : m_UniqueTargetInfo)
    {
        if (target.missCondition != SPELL_MISS_NONE || !target.effectMask) { return SPELL_FAILED_BAD_TARGETS; }
    }
    std::vector<Event> events;
    auto result = CollectCoaCombatEvents(events);
    if (result != SPELL_CAST_OK) { return result; }
    m_coaTransaction = std::make_unique<CoaCombatTransaction>(*static_cast<Player*>(m_caster), std::move(events));
    if (SpellResourceContext::PowerCost{m_CastItem != nullptr, m_triggeredByAuraSpell != nullptr}.Debits())
    {
        m_coaTransaction->ReservePower(m_spellInfo->PowerType, m_powerCost);
    }
    return m_coaTransaction->Prepare();
}

bool Spell::PublishCoaCombatRules()
{
    return !m_coaTransaction || m_coaTransaction->Publish();
}

SpellCastResult Spell::PrepareCoaLinkedCast(CastIntent const& intent)
{
    if (intent.source != CoaCombatIntegration::Identify(*m_caster) || intent.originalCaster != intent.source) { return SPELL_FAILED_BAD_TARGETS; }
    m_coaLinkedPrepared = true;
    m_coaPreparedOwner = intent.source;
    m_coaRuleDepth = intent.ruleDepth;
    m_coaAncestors = intent.ancestors;
    if (auto* location = std::get_if<Destination>(&intent.location))
    {
        if (m_caster->GetTypeId() != TYPEID_PLAYER) { return SPELL_FAILED_BAD_TARGETS; }
        auto context = static_cast<Player*>(m_caster)->BuildCoaCombatContext();
        if (!context.destinationValid(context.data, *location)) { return SPELL_FAILED_BAD_TARGETS; }
        m_coaLocation = *location;
        m_targets.setDestination(location->x, location->y, location->z);
        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (intent.payload.basePoints[i]) { m_currentBasePoints[i] = *intent.payload.basePoints[i]; }
            if (location->mode == DestinationMode::PersistentArea && m_spellInfo->Effect[i] &&
                m_spellInfo->Effect[i] != SPELL_EFFECT_PERSISTENT_AREA_AURA) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
            if (location->mode == DestinationMode::PersistentArea && m_spellInfo->Effect[i])
            {
                auto* radius = sSpellRadiusStore.LookupEntry(m_spellInfo->EffectRadiusIndex[i]);
                if (!radius || GetSpellRadius(radius) <= 0 || GetSpellRadius(radius) > 100) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
            }
        }
        m_powerCost = CalculatePowerCost(m_spellInfo, m_caster, this);
        if (m_powerCost) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
        auto result = CheckCast(false);
        if (result != SPELL_CAST_OK) { return result; }
        result = CheckRange(false);
        if (result != SPELL_CAST_OK) { return result; }
        for (size_t n = 0; n < location->recipients.size; ++n)
        {
            auto* unit = CoaCombatIntegration::Resolve(*m_caster, location->recipients.values[n]);
            if (!unit || !unit->IsAlive()) { continue; }
            if (m_caster->IsFriendlyTo(unit) || !InReach(*m_caster, *unit, 100.0f) || !HasLineOfSight(*m_caster, *unit)) { continue; }
            if (unit->IsImmuneToSpell(m_spellInfo, unit == m_caster)) { return SPELL_FAILED_IMMUNE; }
            for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                if (!m_spellInfo->Effect[i]) { continue; }
                if (!CheckTarget(unit, SpellEffectIndex(i)) || unit->IsImmuneToSpellEffect(m_spellInfo, SpellEffectIndex(i), false))
                {
                    return SPELL_FAILED_IMMUNE;
                }
                AddUnitTarget(unit, SpellEffectIndex(i));
            }
            if (!m_coaTargetLives.Push(CoaCombatIntegration::Identify(*unit))) { return SPELL_FAILED_BAD_TARGETS; }
        }
        for (auto const& hit : m_UniqueTargetInfo)
        {
            if (hit.missCondition != SPELL_MISS_NONE) { return SPELL_FAILED_IMMUNE; }
        }
        m_coaTargetsPrepared = true;
        m_duration = intent.durationMs ? int32(intent.durationMs) : CalculateSpellDuration(m_spellInfo, m_caster);
        if (location->mode == DestinationMode::PersistentArea && (m_duration <= 0 || m_duration > int32(MaxDelayMs)))
        {
            return SPELL_FAILED_SPELL_UNAVAILABLE;
        }
        if (location->mode == DestinationMode::PersistentArea)
        {
            for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                if (!m_spellInfo->Effect[i]) { continue; }
                float radius = GetSpellRadius(sSpellRadiusStore.LookupEntry(m_spellInfo->EffectRadiusIndex[i]));
                if (auto* modOwner = m_caster->GetSpellModOwner()) { modOwner->ApplySpellMod(m_spellInfo->ID, SPELLMOD_RADIUS, radius); }
                if (!std::isfinite(radius) || radius <= 0 || radius > 100) { return SPELL_FAILED_BAD_TARGETS; }
                auto area = std::make_unique<DynamicObject>();
                if (!area->Create(m_caster->GetMap()->GenerateLocalLowGuid(HIGHGUID_DYNAMICOBJECT), m_caster,
                    m_spellInfo->ID, SpellEffectIndex(i), location->x, location->y, location->z,
                    m_duration, radius, DYNAMIC_OBJECT_AREA_SPELL)) { return SPELL_FAILED_BAD_TARGETS; }
                area->SetCoaOwnerEpoch(location->owner.generation);
                m_coaPreparedAreas[i] = std::move(area);
            }
        }
        if (intent.durationMs) { m_coaDuration = intent.durationMs; }
        prepareDataForTriggerSystem();
        if (m_coaRuleDepth) { m_canTrigger = false; }
        return SPELL_CAST_OK;
    }
    auto* target = CoaCombatIntegration::Resolve(*m_caster, intent.target);
    if (!target || !target->IsAlive() || target->IsImmuneToSpell(m_spellInfo, target == m_caster)) { return SPELL_FAILED_IMMUNE; }
    if (intent.kind == IntentKind::Fulfillment && !InMeleeReach(*m_caster, *target)) { return SPELL_FAILED_OUT_OF_RANGE; }
    m_coaPreparedOwner = intent.source;
    m_coaRuleDepth = intent.ruleDepth;
    m_coaAncestors = intent.ancestors;
    m_targets.setUnitTarget(target);
    for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (intent.payload.basePoints[i]) { m_currentBasePoints[i] = *intent.payload.basePoints[i]; }
        if (m_spellInfo->Effect[i] && (!CheckTarget(target, SpellEffectIndex(i)) ||
            target->IsImmuneToSpellEffect(m_spellInfo, SpellEffectIndex(i), target == m_caster))) { return SPELL_FAILED_IMMUNE; }
    }
    m_powerCost = CalculatePowerCost(m_spellInfo, m_caster, this);
    auto result = CheckCast(false);
    if (result != SPELL_CAST_OK) { return result; }
    result = CheckRange(false);
    if (result != SPELL_CAST_OK) { return result; }
    FillTargetMap();
    m_coaTargetsPrepared = true;
    for (auto const& hit : m_UniqueTargetInfo)
    {
        if (hit.missCondition != SPELL_MISS_NONE || !hit.effectMask) { return SPELL_FAILED_IMMUNE; }
        auto* unit = ObjectLookup::GetUnit(*m_caster, hit.targetGUID);
        if (!unit || !m_coaTargetLives.Push(CoaCombatIntegration::Identify(*unit))) { return SPELL_FAILED_BAD_TARGETS; }
    }
    m_duration = intent.durationMs ? int32(intent.durationMs) : CalculateSpellDuration(m_spellInfo, m_caster);
    if (intent.durationMs) { m_coaDuration = intent.durationMs; }
    m_coaLinkedPrepared = true;
    prepareDataForTriggerSystem();
    if (m_coaRuleDepth) { m_canTrigger = false; }
    return SPELL_CAST_OK;
}

void Spell::ExecuteCoaLinkedCast()
{
    if (m_coaPreparedOwner != CoaCombatIntegration::Identify(*m_caster) || !m_caster->IsAlive())
    {
        delete this;
        return;
    }
    if (auto* location = std::get_if<Destination>(&m_coaLocation))
    {
        auto context = static_cast<Player*>(m_caster)->BuildCoaCombatContext();
        if (!context.destinationValid(context.data, *location)) { delete this; return; }
    }
    m_caster->m_Events.AddEvent(new SpellEvent(this), m_caster->m_Events.CalculateTime(1));
    cast(true);
}

void Spell::ReportCoaDirectSpell(Unit* target, uint32 amount, bool healing, bool critical)
{
    if (m_IsTriggeredSpell || m_coaRuleDepth || !amount || !target || m_caster->GetTypeId() != TYPEID_PLAYER ||
        GetAffectiveCaster() != m_caster) { return; }
    auto& owner = *static_cast<Player*>(m_caster);
    if (!owner.IsCoaManaged() || !owner.HasSpell(m_spellInfo->ID)) { return; }
    if (!m_coaCastId) { m_coaCastId = owner.NextCoaCastId(); }
    Event e;
    e.kind = EventKind::DirectSpell;
    e.castId = m_coaCastId;
    e.spell = m_spellInfo->ID;
    e.source = e.originalCaster = CoaCombatIntegration::Identify(owner);
    e.target = CoaCombatIntegration::Identify(*target);
    e.targetPolicy = Relation(owner, *target);
    e.direct = true;
    e.critical = critical;
    e.damage = !healing;
    e.healing = healing;
    e.effectiveAmount = amount;
    e.resolvedHit = true;
    e.nativeEventMask = m_procAttacker;
    owner.OnCoaCombatEvent(e);
}
