// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoaCombatIntegration.h"
#include "CoaStanceRules.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "ObjectLookup.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Log.h"
#include "Map.h"
#include <algorithm>
#include <atomic>
#include <limits>

using namespace coa::combat;

void Unit::AdvanceCombatEpoch()
{
    static std::atomic<uint64> next{1};
    m_combatEpoch = next.fetch_add(1, std::memory_order_relaxed);
    MANGOS_ASSERT(m_combatEpoch != 0);
}

namespace CoaCombatIntegration
{
    Identity Identify(Unit const& unit)
    {
        auto const& frame = unit.Where().CurrentFrame();
        return {unit.GetObjectGuid().GetRawValue(), unit.GetInstanceId(), unit.GetCombatEpoch(),
            unit.GetMapId(), frame.Id(), frame.IsDeck()};
    }
    Unit* Resolve(Unit& owner, Identity const& identity)
    {
        if (!owner.IsInWorld() || identity.map != owner.GetMapId() || identity.instance != owner.GetInstanceId())
        {
            return nullptr;
        }
        auto* unit = identity.guid == owner.GetObjectGuid().GetRawValue() ? &owner :
            owner.GetMap()->GetUnit(ObjectGuid(identity.guid));
        return unit && Identify(*unit) == identity ? unit : nullptr;
    }
    SpellCastResult CastResult(Status status)
    {
        switch (status)
        {
            case Status::Ready: return SPELL_CAST_OK;
            case Status::Insufficient: return SPELL_FAILED_NO_POWER;
            case Status::Unauthorized: return SPELL_FAILED_SPELL_UNAVAILABLE;
            case Status::Unsupported: return SPELL_FAILED_SPELL_UNAVAILABLE;
            case Status::Invalid: return SPELL_FAILED_BAD_TARGETS;
            case Status::Limit: return SPELL_FAILED_NOT_READY;
            case Status::Recursive: return SPELL_FAILED_SPELL_IN_PROGRESS;
            default: return SPELL_FAILED_TRY_AGAIN;
        }
    }
    bool IsCounterOnlyComponent(uint32 spell, uint32 slot, uint32 aura, int32 value)
    {
        return (spell == 500706 && slot == 1 && aura == 317 && value == 0) ||
            (spell == 803061 && aura == SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
}

bool Player::KnowsCoaCombatSpell(uint32 spell) const
{
    // 4039 is a CA entry. Its native committed spell is 300755, not spell4039.
    return HasSpell(spell) && (spell != 300755 || m_coaBuild.spells.count(300755));
}

bool Player::IsCoaCombatAuthorized(uint32 source, uint32 resource) const
{
    if (!IsCoaManaged() || !m_coaReady || m_coaFailed) { return false; }
    bool owned = m_coaCombatSpells.count(source) ||
        (source == 500728 && m_coaCombatSpells.count(500706)) ||
        (source == 804585 && m_coaCombatSpells.count(500149)) ||
        (source == 804097 && m_coaCombatSpells.count(500149)) ||
        (source == 804098 && m_coaCombatSpells.count(500149));
    if (source == 300755 || source == 900755) { owned = KnowsCoaCombatSpell(300755); }
    if (!owned) { return false; }
    return !resource || PolicyResourceEdge(source, resource);
}

Context Player::BuildCoaCombatContext() const
{
    Context c;
    c.actor = CoaCombatIntegration::Identify(*this);
    c.playerClass = getClass();
    c.nowMs = m_Events.CalculateTime(0);
    c.alive = IsAlive();
    c.inWorld = IsInWorld();
    c.inCombat = IsInCombat();
    // Ascender stable-input policy: equipment + flat rating auras, excluding ALL
    // rating-from-stat terms. Sampling total rating would feed this Intellect back.
    c.criticalStrikeRating = std::max<int32>(0, m_baseRatingValue[CR_CRIT_SPELL]);
    if (KnowsCoaCombatSpell(807749) && GetSpellAuraHolder(807749, GetObjectGuid())) { c.activeVow = 807749; }
    if (GetWeaponForAttack(BASE_ATTACK, true, false)) { c.weaponMask |= 1; }
    if (GetWeaponForAttack(OFF_ATTACK, true, false)) { c.weaponMask |= 2; }
    c.data = this;
    c.known = [](void const* data, uint32 spell)
    {
        return static_cast<Player const*>(data)->KnowsCoaCombatSpell(spell);
    };
    c.selfAura = [](void const* data, uint32 spell)
    {
        auto const* player = static_cast<Player const*>(data);
        return player->GetSpellAuraHolder(spell, player->GetObjectGuid()) != nullptr;
    };
    c.authorized = [](void const* data, uint32 source, uint32 resource)
    {
        return static_cast<Player const*>(data)->IsCoaCombatAuthorized(source, resource);
    };
    c.nativeSpell = [](void const* data, uint32 spell)
    {
        auto* info = sSpellStore.LookupEntry(spell);
        return NativeSpell{info != nullptr, info ? CalculateSpellDuration(info, static_cast<Player const*>(data)) : 0};
    };
    c.resolve = [](void const* data, Identity const& identity)
    {
        auto& owner = *const_cast<Player*>(static_cast<Player const*>(data));
        auto* unit = CoaCombatIntegration::Resolve(owner, identity);
        if (!unit) { return Resolution{}; }
        return Resolution{true, unit->IsAlive(), unit->IsInWorld(),
            InReach(owner, *unit, 100.0f) && HasLineOfSight(owner, *unit),
            owner.IsFriendlyTo(unit), CoaCombatIntegration::Identify(*unit)};
    };
    c.destinationValid = [](void const* data, Destination const& destination)
    {
        auto const& owner = *static_cast<Player const*>(data);
        Geometry::Vector3 point(destination.x, destination.y, destination.z);
        return owner.IsInWorld() && owner.IsAlive() && destination.owner == CoaCombatIntegration::Identify(owner) &&
            destination.phase == owner.GetPhaseMask() && owner.Where().WithinDist(point, 100.0f) && HasLineOfSight(owner, point);
    };
    return c;
}

void Player::SyncCoaCombatAuras()
{
    if (m_coaCombatPublishing) { return; }
    auto now = m_Events.CalculateTime(0);
    bool changed = false;
    auto identity = CoaCombatIntegration::Identify(*this);
    if (!m_coaCombat.actor.guid) { m_coaCombat.actor = identity; }
    if (m_coaCombat.actor == identity)
    {
        for (auto const& pair : GetSpellAuraHolderMap())
        {
            auto* holder = pair.second;
            if (!ControlledAura(holder->GetId()) || holder->GetCasterGuid() != GetObjectGuid() ||
                !holder->GetCastItemGuid().IsEmpty()) { continue; }
            bool found = false;
            for (size_t i = 0; i < m_coaCombat.auras.size; ++i)
            {
                auto const& a = m_coaCombat.auras.values[i];
                found = found || (a.spell == holder->GetId() && a.target == identity);
            }
            if (!found)
            {
                coa::combat::Aura a;
                a.spell = holder->GetId();
                a.target = a.caster = identity;
                a.stacks = int32(holder->GetStackAmount());
                a.charges = int32(holder->GetAuraCharges());
                a.expiresMs = holder->IsCoaControlled() ? holder->GetCoaExpiry() :
                    holder->GetAuraDuration() > 0 ? now + holder->GetAuraDuration() : 0;
                if (!m_coaCombat.auras.Push(a))
                {
                    sLog.outError("CoA native aura import exceeds bound for player %u", GetGUIDLow());
                    m_coaFailed = true;
                    return;
                }
                changed = true;
            }
        }
    }
    for (size_t i = 0; i < m_coaCombat.auras.size; ++i)
    {
        auto& a = m_coaCombat.auras.values[i];
        auto* target = CoaCombatIntegration::Resolve(*this, a.target);
        auto* holder = target ? target->GetSpellAuraHolder(a.spell, GetObjectGuid()) : nullptr;
        int32 stacks = holder ? int32(holder->GetStackAmount()) : 0;
        int32 charges = holder ? int32(holder->GetAuraCharges()) : 0;
        uint64 expiry = holder && holder->IsCoaControlled() ? holder->GetCoaExpiry() :
            holder && holder->GetAuraDuration() > 0 ? now + holder->GetAuraDuration() : 0;
        // The policy clock owns controlled finite expiry; external removal is an
        // explicit lifecycle event so Madness cleanup is not lost in synchronization.
        if ((!holder && a.stacks && a.spell == 803061) || (holder && a.spell == 807440 && !charges))
        {
            a.expiresMs = now ? now : 1;
            continue;
        }
        if (a.stacks != stacks || a.charges != charges || a.expiresMs != expiry)
        {
            a.stacks = stacks;
            a.charges = charges;
            a.expiresMs = expiry;
            changed = true;
        }
    }
    if (changed) { ++m_coaCombat.revision; }
}

bool Player::OnCoaCombatEvent(Event event)
{
    if (!IsCoaManaged() || !m_coaReady || m_coaFailed) { return false; }
    if (m_coaCombatInvalidating && event.kind != EventKind::Invalidate) { return false; }
    if (m_coaCombatPublishing)
    {
        if (!m_coaCombatDeferred.Push(event))
        {
            sLog.outError("CoA combat event queue limit for player %u", GetGUIDLow());
            return false;
        }
        return true;
    }
    SyncCoaCombatAuras();
    event.sequence = ++m_coaEventSequence;
    CoaCombatTransaction transaction(*this, event);
    bool applied = transaction.Prepare() == SPELL_CAST_OK && transaction.Publish();
    if (applied) { transaction.ReleaseCasts(); }
    if (!m_coaCombatDraining)
    {
        m_coaCombatDraining = true;
        size_t dispatched = 0;
        while (m_coaCombatDeferred.size && dispatched++ < 32)
        {
            auto next = m_coaCombatDeferred.values[0];
            for (size_t i = 1; i < m_coaCombatDeferred.size; ++i)
            {
                m_coaCombatDeferred.values[i - 1] = m_coaCombatDeferred.values[i];
            }
            --m_coaCombatDeferred.size;
            OnCoaCombatEvent(next);
        }
        if (m_coaCombatDeferred.size)
        {
            sLog.outError("CoA combat recursive event limit for player %u", GetGUIDLow());
            m_coaCombatDeferred.size = 0;
        }
        m_coaCombatDraining = false;
    }
    return applied;
}

void Player::UpdateCoaCombatRules()
{
    if (!IsInWorld() || !IsCoaManaged() || !m_coaReady) { return; }
    Event e;
    e.kind = EventKind::Tick;
    OnCoaCombatEvent(e);
}

void Player::InvalidateCoaCombatRules()
{
    if (m_coaCombatInvalidating) { return; }
    if (m_coaCombatPublishing)
    {
        Event e;
        e.kind = EventKind::Invalidate;
        m_coaCombatDeferred.Push(e);
        return;
    }
    Event e;
    e.kind = EventKind::Invalidate;
    m_coaCombatInvalidating = true;
    if (IsInWorld() && m_coaReady) { OnCoaCombatEvent(e); }
    uint64 order = m_coaCombat.nextOrder;
    uint64 revision = m_coaCombat.revision;
    m_coaCombat = {};
    m_coaCombat.nextOrder = order;
    m_coaCombat.revision = revision;
    m_coaCombat.clockMs = m_Events.CalculateTime(0);
    m_coaCombatDeferred.size = 0;
    if (m_coaIntellect)
    {
        int32 old = m_coaIntellect;
        m_coaIntellect = 0;
        HandleStatModifier(UNIT_MOD_STAT_INTELLECT, TOTAL_VALUE, float(old), false);
    }
    AdvanceCombatEpoch();
    m_coaCombatInvalidating = false;
}

void Player::EndCoaCombatCast()
{
    m_coaCombatPublishing = false;
    Event e;
    e.kind = EventKind::Refresh;
    OnCoaCombatEvent(e);
}

void Player::RefreshCoaCombatIntellect()
{
    if (m_coaCombatRefreshingStat || !IsInWorld() || !m_coaReady || !IsCoaManaged()) { return; }
    m_coaCombatRefreshingStat = true;
    Event e;
    e.kind = EventKind::Refresh;
    OnCoaCombatEvent(e);
    m_coaCombatRefreshingStat = false;
}

void Player::ApplyCoaStanceRule(uint32 spell, bool preActive)
{
    // class_resources.lua stance exclusivity, same sets and semantics: the
    // just-cast member applies through the normal path on this tick, every
    // other member leaves synchronously, and a recast of the active member
    // toggles it off. Runs after the cast's own effects (all members are
    // instant), so unlike the pre-effects Lua hook no delayed poll is needed.
    // Deliberately not CoA-managed-gated: the Lua applied to any caster and
    // no stock class can know these ids.
    auto const* set = coa::StanceSetForSpell(spell);
    if (!set) { return; }
    for (std::size_t i = 0; i < set->size; ++i)
    {
        if (set->members[i] != spell && HasAura(set->members[i])) { RemoveAurasDueToSpell(set->members[i]); }
    }
    if (preActive) { RemoveAurasDueToSpell(spell); }
}

CoaCombatTransaction::CoaCombatTransaction(Player& owner, Event event) : m_owner(owner), m_events{event} {}
CoaCombatTransaction::CoaCombatTransaction(Player& owner, std::vector<Event> events) : m_owner(owner), m_events(std::move(events)) {}
CoaCombatTransaction::~CoaCombatTransaction() = default;

void CoaCombatTransaction::ReservePower(int32 type, uint32 cost)
{
    if (type == POWER_HEALTH) { m_powerCosts[MAX_POWERS] += cost; }
    else if (type >= 0 && type < MAX_POWERS) { m_powerCosts[type] += cost; }
}

SpellCastResult CoaCombatTransaction::Prepare()
{
    if (m_owner.m_coaCombatPublishing) { return SPELL_FAILED_SPELL_IN_PROGRESS; }
    m_owner.SyncCoaCombatAuras();
    if (m_events.empty() || m_events.size() > 16) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
    m_context = m_owner.BuildCoaCombatContext();
    auto candidate = m_owner.m_coaCombat;
    std::vector<AuraDelta> deltas;
    std::vector<CastIntent> intents;
    for (auto& event : m_events)
    {
        if (!event.sequence) { event.sequence = ++m_owner.m_coaEventSequence; }
        auto plan = Evaluate(m_context, candidate, event);
        if (plan.Result() != Status::Ready) { return CoaCombatIntegration::CastResult(plan.Result()); }
        candidate = plan.Preview();
        for (size_t i = 0; i < plan.Deltas().size; ++i)
        {
            auto const& delta = plan.Deltas().values[i];
            auto found = std::find_if(deltas.begin(), deltas.end(), [&](AuraDelta const& old)
            {
                return old.after.spell == delta.after.spell && old.after.target == delta.after.target;
            });
            if (found != deltas.end()) { found->after = delta.after; }
            else { deltas.push_back(delta); }
        }
        for (size_t i = 0; i < plan.Casts().size; ++i) { intents.push_back(plan.Casts().values[i]); }
        if (deltas.size() > MaxAuras * 2 || intents.size() > MaxPending) { return SPELL_FAILED_NOT_READY; }
        m_plans.push_back(std::move(plan));
    }
    m_plan = m_plans.back();
    for (auto const& d : deltas)
    {
        auto* target = CoaCombatIntegration::Resolve(m_owner, d.after.target);
        if (!target && !d.after.stacks && d.after.target.guid == m_owner.GetObjectGuid().GetRawValue()) { target = &m_owner; }
        if (!target)
        {
            if (!d.after.stacks) { continue; }
            return SPELL_FAILED_BAD_TARGETS;
        }
        auto* previous = target->GetSpellAuraHolder(d.after.spell, m_owner.GetObjectGuid());
        if (previous && !previous->GetCastItemGuid().IsEmpty()) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
        std::unique_ptr<SpellAuraHolder> holder;
        if (d.after.stacks)
        {
            auto* info = sSpellStore.LookupEntry(d.after.spell);
            if (!info || !target->IsAlive() || target->IsImmuneToSpell(info, target == &m_owner))
            {
                return SPELL_FAILED_IMMUNE;
            }
            holder.reset(CreateSpellAuraHolder(info, target, &m_owner));
            holder->SetCoaControlled(m_owner.GetCombatEpoch());
            for (uint32 slot = 0; slot < MAX_EFFECT_INDEX; ++slot)
            {
                if (!IsAuraApplyEffect(info, SpellEffectIndex(slot))) { continue; }
                auto value = info->CalculateSimpleValue(SpellEffectIndex(slot));
                if (CoaCombatIntegration::IsCounterOnlyComponent(info->ID, slot, info->EffectAura[slot], value)) { continue; }
                if (info->EffectAura[slot] >= TOTAL_AURAS ||
                    target->IsImmuneToSpellEffect(info, SpellEffectIndex(slot), target == &m_owner))
                {
                    return SPELL_FAILED_IMMUNE;
                }
                auto* aura = CreateAura(info, SpellEffectIndex(slot), nullptr, holder.get(), target, &m_owner);
                aura->GetModifier()->m_amount *= d.after.stacks;
                holder->AddAura(aura, SpellEffectIndex(slot));
            }
            int32 duration = d.after.expiresMs ? int32(d.after.expiresMs - m_context.nowMs) : -1;
            holder->SetLoadedState(m_owner.GetObjectGuid(), ObjectGuid(), d.after.stacks,
                d.after.charges, duration, duration);
            if (target->GetVisibleAurasCount() >= MAX_AURAS && !previous) { return SPELL_FAILED_NOT_READY; }
        }
        m_holders.push_back({target, CoaCombatIntegration::Identify(*target), d.after.spell, previous, std::move(holder)});
    }
    for (auto const& intent : intents)
    {
        auto* info = sSpellStore.LookupEntry(intent.spell);
        if (!info) { return SPELL_FAILED_SPELL_UNAVAILABLE; }
        auto cast = std::make_unique<Spell>(&m_owner, info, true, ObjectGuid(intent.originalCaster.guid));
        auto result = cast->PrepareCoaLinkedCast(intent);
        if (result != SPELL_CAST_OK)
        {
            // A failed native fire preflight fizzles an existing job once. It
            // must not remain queued until immunity disappears and fire late.
            auto& pending = m_owner.m_coaCombat.pending;
            for (size_t i = 0; i < pending.size; ++i)
            {
                if (pending.values[i].sequence != intent.sequence || pending.values[i].dueMs > m_context.nowMs) { continue; }
                for (size_t j = i + 1; j < pending.size; ++j) { pending.values[j - 1] = pending.values[j]; }
                --pending.size;
                ++m_owner.m_coaCombat.revision;
                break;
            }
            return result;
        }
        ReservePower(cast->m_spellInfo->PowerType, cast->GetPowerCost());
        m_casts.push_back(std::move(cast));
    }
    for (size_t power = 0; power < MAX_POWERS; ++power)
    {
        if (m_powerCosts[power] > m_owner.GetPower(Powers(power))) { return SPELL_FAILED_NO_POWER; }
    }
    if (m_powerCosts[MAX_POWERS] >= m_owner.GetHealth() && m_powerCosts[MAX_POWERS]) { return SPELL_FAILED_NO_POWER; }
    m_ready = true;
    return SPELL_CAST_OK;
}

bool CoaCombatTransaction::Publish()
{
    if (!m_ready || m_owner.m_coaCombatPublishing) { return false; }
    m_owner.SyncCoaCombatAuras();
    for (auto const& change : m_holders)
    {
        if (CoaCombatIntegration::Resolve(m_owner, change.targetLife) != change.target ||
            change.target->GetSpellAuraHolder(change.spell, m_owner.GetObjectGuid()) != change.previous) { return false; }
    }
    auto candidate = m_owner.m_coaCombat;
    for (auto const& plan : m_plans)
    {
        if (Commit(m_owner.BuildCoaCombatContext(), plan, true, candidate) != Status::Ready) { return false; }
    }
    m_owner.m_coaCombat = candidate;
    m_ready = false;
    m_owner.m_coaCombatPublishing = true;
    // All holder maps and modifier lists are final before any modifier callback.
    for (auto& change : m_holders) { change.target->StageCoaAura(change.previous, change.replacement.get()); }
    for (auto& change : m_holders)
    {
        change.target->ActivateCoaAura(change.previous, change.replacement.release());
    }
    int32 replacement = m_plan.Intellect().replacement;
    int32 delta = replacement - m_owner.m_coaIntellect;
    m_owner.m_coaIntellect = replacement;
    if (delta) { m_owner.HandleStatModifier(UNIT_MOD_STAT_INTELLECT, TOTAL_VALUE, float(delta), true); }
    m_owner.m_coaCombatPublishing = false;
    // Actual holders serialize native spell IDs/flags/stacks/charges/durations.
    for (auto const& change : m_holders)
    {
        m_owner.SendAurasForTarget(change.target);
    }
    return true;
}

void CoaCombatTransaction::ReleaseCasts()
{
    for (auto& cast : m_casts)
    {
        if (cast) { cast.release()->ExecuteCoaLinkedCast(); }
    }
}
