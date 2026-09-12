// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoaValueTransferRules.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace coa::transfer
{
    namespace
    {
        // An explicit private generated artifact is required, never loose runtime JSON.
#ifdef COA_VALUE_TRANSFER_CATALOG_HEADER
#include COA_VALUE_TRANSFER_CATALOG_HEADER
#else
#error "Generate and pin COA_VALUE_TRANSFER_CATALOG_HEADER with coa_value_transfer_catalog.py"
#endif

        bool Valid(Identity const& id)
        {
            return id.guid && id.generation;
        }
        bool SameFrame(Identity const& a, Identity const& b)
        {
            return a.map == b.map && a.instance == b.instance &&
                a.frame == b.frame && a.deck == b.deck;
        }
        bool Live(UnitFacts const& f, Identity const& expected, Identity const& anchor)
        {
            return Valid(expected) && f.identity == expected && SameFrame(anchor, expected) &&
                f.alive && f.inWorld && f.inRange && f.phase && f.los &&
                f.health && f.maxHealth && f.health <= f.maxHealth;
        }
        bool NativeReady(Context const& c, uint32_t spell)
        {
            auto n = FindNative(spell);
            if (!n || !c.native)
            {
                return false;
            }
            auto facts = c.native(c.data, spell);
            return facts.dependenciesVerified && facts.digest == n->digest;
        }
        bool Incoming(Selector s)
        {
            return s == Selector::DamageTaken || s == Selector::DirectDamageTaken ||
                s == Selector::MagicDamageTaken || s == Selector::PhysicalDamageTaken ||
                s == Selector::MeleeDamageTaken || s == Selector::HealReceived ||
                s == Selector::AbsorbUsed;
        }
        bool Pet(Selector s)
        {
            return s == Selector::PetDamage || s == Selector::PetDamageOrHeal ||
                s == Selector::PetAutoCritical;
        }
        bool Matches(Selector s, Event const& e)
        {
            bool damage = e.damage > 0, heal = e.healing > 0;
            bool direct = !e.periodic, crit = e.outcome == Outcome::Critical;
            switch (s)
            {
                case Selector::None: return false;
                case Selector::Damage: case Selector::DamageTaken: case Selector::PetDamage: return damage;
                case Selector::DirectDamage: case Selector::DirectDamageTaken: return damage && direct;
                case Selector::CriticalDamage: return damage && crit;
                case Selector::DirectCriticalDamage: return damage && direct && crit;
                case Selector::Heal: case Selector::HealReceived: return heal;
                case Selector::CriticalHeal: return heal && crit;
                case Selector::PeriodicCriticalHeal: return heal && crit && e.periodic;
                case Selector::Overheal: return e.overheal > 0;
                case Selector::DamageOrHeal: return (damage || heal) && direct && !e.autoAttack;
                case Selector::PetDamageOrHeal: return damage || heal;
                case Selector::MagicDamageTaken: return damage && direct && (e.school & 126);
                case Selector::PhysicalDamage: case Selector::PhysicalDamageTaken: return damage && (e.school & 1);
                case Selector::PhysicalNatureCritical: return damage && crit && (e.school & 9);
                case Selector::MeleeDamage: case Selector::MeleeDamageTaken: return damage && e.melee && direct;
                case Selector::AutoDamage: return damage && e.autoAttack && direct;
                case Selector::AutoCritical: return damage && e.autoAttack && direct && crit;
                case Selector::MeleeAuto: return damage && e.autoAttack && e.melee && direct;
                case Selector::RangedAuto: return damage && e.autoAttack && e.ranged && direct;
                case Selector::PeriodicDamage: return damage && e.periodic;
                case Selector::AbsorbUsed: return e.absorbed > 0;
                case Selector::PetAutoCritical: return damage && e.autoAttack && e.melee && direct && crit;
            }
            return false;
        }
        bool OwnedPet(UnitFacts const& f, Identity const& owner, Cohort cohort)
        {
            return !f.player && f.creature && f.worldBindingVerified && f.aiReady &&
                f.controller == owner && f.originalOwner == owner &&
                (cohort == Cohort::OwnedSummon ? f.cohort != Cohort::None : f.cohort == cohort);
        }
        bool NamedEdge(Binding const& b, Event const& e, AuraKey const& key)
        {
            auto const& parents = e.lineage.parents;
            if (!parents.size || parents.size > MaxDepth || e.lineage.terminal)
            {
                return false;
            }
            auto const& last = parents.values[parents.size - 1];
            if (last.caster != key.caster || last.holder != key.holder)
            {
                return false;
            }
            return (b.spell == 705087 && last.spell == 680276 && e.spell == 520571) ||
                (b.spell == 705397 && last.spell == 803997 && e.spell == 804474) ||
                (b.spell == 707479 && last.spell == 504750 && e.spell == 503864) ||
                (b.spell == 704884 && last.spell == 301209 && e.spell == 301211) ||
                (b.spell == 504750 && last.spell == 504750 && e.spell == 503864 && parents.size < MaxDepth);
        }
        bool OutgoingEdge(uint32_t spell)
        {
            return spell == 680276 || spell == 803997 || spell == 504750 || spell == 301209;
        }
        uint32_t EventMask(Event const& e, bool incoming)
        {
            if (e.periodic)
            {
                return incoming ? 0x80000 : 0x40000;
            }
            if (e.healing || e.overheal)
            {
                return incoming ? 0x8000 : 0x4000;
            }
            uint32_t flags = e.autoAttack ? (e.ranged ? 0x40 : 0x4) :
                e.melee ? 0x10 : e.ranged ? 0x100 : 0x10000;
            return incoming ? (flags << 1) | 0x100000 : flags;
        }
        Status Remember(State& state, Event const& e, bool onceCast, bool& castSeen)
        {
            if (state.histories.size > MaxActors)
            {
                return Status::Invalid;
            }
            History* history = nullptr;
            for (std::size_t i = 0; i < state.histories.size; ++i)
            {
                if (state.histories.values[i].actor == e.actor)
                {
                    history = &state.histories.values[i];
                }
            }
            if (!history)
            {
                History h;
                h.actor = e.actor;
                if (!state.histories.Push(h))
                {
                    return Status::Limit;
                }
                history = &state.histories.values[state.histories.size - 1];
            }
            auto& h = *history;
            if (h.size > HistorySize || h.next >= HistorySize)
            {
                return Status::Invalid;
            }
            if (e.id <= h.eventFloor || (e.castId && e.castId <= h.castFloor))
            {
                return Status::Duplicate;
            }
            for (std::size_t i = 0; i < h.size; ++i)
            {
                if (h.fifo[i].event == e.id)
                {
                    return Status::Duplicate;
                }
                castSeen |= e.castId && h.fifo[i].cast == e.castId;
            }
            if (castSeen && onceCast)
            {
                return Status::Duplicate;
            }
            if (h.size == HistorySize)
            {
                h.eventFloor = std::max(h.eventFloor, h.fifo[h.next].event);
                h.castFloor = std::max(h.castFloor, h.fifo[h.next].cast);
            }
            else
            {
                ++h.size;
            }
            h.fifo[h.next] = {e.id, e.castId};
            h.next = (h.next + 1) % HistorySize;
            return Status::Ready;
        }
        bool SameIntent(Intent const& a, Intent const& b)
        {
            if (!(a.source == b.source) || !(a.lineage.root == b.lineage.root) ||
                a.lineage.terminal != b.lineage.terminal || a.lineage.parents.size != b.lineage.parents.size)
            {
                return false;
            }
            for (std::size_t i = 0; i < a.lineage.parents.size; ++i)
            {
                if (!(a.lineage.parents.values[i] == b.lineage.parents.values[i]))
                {
                    return false;
                }
            }
            return a.anchor == b.anchor && a.ground.valid == b.ground.valid &&
                a.ground.frame == b.ground.frame && a.ground.coordinates == b.ground.coordinates &&
                std::tie(a.actor, a.originalCaster, a.target, a.child, a.slot, a.valueSlots,
                a.companionSlots, a.kind, a.schedule, a.amount, a.previousContribution,
                a.ticks, a.periodMs, a.followupHealPercent, a.expiresMs, a.allowCritical,
                a.suppressSourceBonuses) ==
                std::tie(b.actor, b.originalCaster, b.target, b.child, b.slot, b.valueSlots,
                b.companionSlots, b.kind, b.schedule, b.amount, b.previousContribution,
                b.ticks, b.periodMs, b.followupHealPercent, b.expiresMs, b.allowCritical,
                b.suppressSourceBonuses);
        }
    }

    bool operator==(AuraKey const& a, AuraKey const& b)
    {
        return a.spell == b.spell && a.slot == b.slot && a.caster == b.caster &&
            a.holder == b.holder && a.application == b.application;
    }
    bool operator==(Root const& a, Root const& b)
    {
        return a.actor == b.actor && a.event == b.event;
    }
    Binding const* FindBinding(uint32_t spell, uint32_t slot)
    {
        for (auto const& b : Bindings)
        {
            if (b.spell == spell && b.slot == slot)
            {
                return &b;
            }
        }
        return nullptr;
    }
    std::size_t BindingCount() { return std::size(Bindings); }
    Binding const* BindingAt(std::size_t i) { return i < BindingCount() ? &Bindings[i] : nullptr; }
    NativeRecord const* FindNative(uint32_t spell)
    {
        auto it = std::lower_bound(std::begin(NativeRecords), std::end(NativeRecords), spell,
            [](NativeRecord const& n, uint32_t id) { return n.spell < id; });
        return it != std::end(NativeRecords) && it->spell == spell ? it : nullptr;
    }
    int32_t Percent(uint64_t input, uint64_t numerator, uint32_t denominator)
    {
        if (!input || !numerator || !denominator)
        {
            return 0;
        }
        constexpr uint64_t cap = INT32_MAX;
        // Compare before multiplying, so even hostile uint64 inputs saturate safely.
        if (input > cap * denominator / numerator)
        {
            return INT32_MAX;
        }
        return int32_t(input * numerator / denominator);
    }
    std::optional<int32_t> NativeSignedValue(double value, bool signedAllowed)
    {
        if (!std::isfinite(value) || (!signedAllowed && value < 0))
        {
            return std::nullopt;
        }
        return int32_t(std::clamp(std::floor(value), double(INT32_MIN), double(INT32_MAX)));
    }
    int32_t TickValue(Intent const& intent, uint32_t tick)
    {
        if (intent.amount < 0 || !intent.ticks || intent.ticks > MaxTicks || tick >= intent.ticks)
        {
            return 0;
        }
        if (intent.schedule != Schedule::Total)
        {
            return intent.amount;
        }
        return intent.amount / int32_t(intent.ticks) + (tick < uint32_t(intent.amount) % intent.ticks ? 1 : 0);
    }
    bool StockProcMatches(ProcData const& p, uint32_t flags, uint32_t extra,
                          uint32_t school, uint32_t family, std::array<uint32_t, 3> const& mask)
    {
        if (!(p.flags & flags) || (p.school && !(p.school & school)) ||
            (p.family && p.family != family))
        {
            return false;
        }
        if ((p.mask[0] || p.mask[1] || p.mask[2]) &&
            !(p.mask[0] & mask[0]) && !(p.mask[1] & mask[1]) && !(p.mask[2] & mask[2]))
        {
            return false;
        }
        if (p.extra)
        {
            return (p.extra & 0x10000) || (p.extra & extra);
        }
        if ((p.flags & 0xc0000) && (extra & 0x40000))
        {
            return false;
        }
        return (extra & (0x80000 | 3)) != 0;
    }

    Plan Prepare(Context const& c, State const& state, Event const& e, ChainBudget const& chain)
    {
        Plan p;
        p.m_next = state;
        p.m_event = e;
        p.m_revision = state.revision;
        p.m_chainRevision = chain.revision;
        p.m_nowMs = c.nowMs;
        auto reject = [&](Status status)
        {
            p.m_status = status;
            p.m_next = state;
            p.m_intents = {};
            p.m_suppressSlots = p.m_manaDebit = p.m_budgetDebit = 0;
            return p;
        };
        auto primary = FindBinding(c.aura.spell, c.aura.slot);
        if (!primary || c.aura.slot != primary->budgetSlot)
        {
            return reject(Status::Unsupported);
        }
        auto const& b = *primary;
        if (!b.defined)
        {
            return reject(Status::SourceGap);
        }
        if (b.childContractGap)
        {
            return reject(Status::Dependency);
        }
        if (!Valid(c.aura.caster) || !Valid(c.aura.holder) || !c.aura.application ||
            c.playerClass != b.playerClass || !c.authorized ||
            !c.authorized(c.data, c.aura, b.playerClass) || !c.resolve ||
            (!b.holderMayDiffer && c.aura.holder != c.aura.caster))
        {
            return reject(Status::Unauthorized);
        }
        if (!state.active || (state.initialized && !(state.aura == c.aura)) ||
            c.nowMs < state.clockMs || state.revision == UINT64_MAX || chain.revision == UINT64_MAX ||
            state.contributions.size > 3 || state.attackers.size > MaxRecipients)
        {
            return reject(Status::Stale);
        }
        if (!NativeReady(c, b.spell))
        {
            return reject(Status::NativeMismatch);
        }
        if (!e.trustedIds || !e.id || !Valid(e.actor) || !Valid(e.victim) ||
            !Valid(e.originalCaster) || e.origin == Origin::Untrusted ||
            e.damage > MaxInput || e.healing > MaxInput || e.overheal > MaxInput || e.absorbed > MaxInput ||
            (e.damage && (e.healing || e.overheal)) || (e.periodic && e.autoAttack) ||
            (e.melee && e.ranged) || e.lineage.parents.size > MaxDepth ||
            !Valid(e.lineage.root.actor) || !e.lineage.root.event ||
            !(chain.root == e.lineage.root) || chain.remaining > MaxChainEvents)
        {
            return reject(Status::Invalid);
        }
        if ((e.outcome == Outcome::Critical && (e.extra & 3) != 2) ||
            (e.outcome == Outcome::Hit && (e.extra & 3) != 1) ||
            (e.outcome == Outcome::Absorbed && !(e.extra & 0x400)) ||
            (!e.spell && !e.autoAttack))
        {
            return reject(Status::Invalid);
        }
        if (b.selector == Selector::AbsorbUsed && e.shieldOrigin != Origin::Native)
        {
            return reject(e.shieldOrigin == Origin::Untrusted ? Status::Invalid : Status::Recursive);
        }
        if (e.origin == Origin::Native && (e.lineage.parents.size || e.lineage.terminal ||
            !(e.lineage.root == Root{e.actor, e.id})))
        {
            return reject(Status::Recursive);
        }
        if (e.origin != Origin::Native && (e.origin != Origin::ValueTransfer || !NamedEdge(b, e, c.aura)))
        {
            return reject(Status::Recursive);
        }
        if ((b.spell == 705087 || b.spell == 705397 || b.spell == 707479) && !NamedEdge(b, e, c.aura))
        {
            return reject(Status::Recursive);
        }
        if (e.lineage.parents.size >= MaxDepth || e.lineage.terminal ||
            e.spell == b.spell || (e.spell == b.child && b.spell != 504750))
        {
            return reject(Status::Recursive);
        }
        for (std::size_t i = 0; i < e.lineage.parents.size; ++i)
        {
            if (!Valid(e.lineage.parents.values[i].caster) || !e.lineage.parents.values[i].application ||
                (e.lineage.parents.values[i].spell == b.spell && b.spell != 504750))
            {
                return reject(Status::Recursive);
            }
        }
        if (e.healthCost || e.selfDamage || (e.damage && e.actor == e.victim) ||
            (e.outcome != Outcome::Hit && e.outcome != Outcome::Critical &&
                !(b.selector == Selector::AbsorbUsed && e.outcome == Outcome::Absorbed)) ||
            !Matches(b.selector, e))
        {
            return reject(Status::NoEffect);
        }
        if (e.autoAttack && (b.spell == 570064 || b.spell == 681311 || b.spell == 801778 || b.spell == 524869))
        {
            return reject(Status::NoEffect);
        }
        bool incoming = Incoming(b.selector), pet = Pet(b.selector);
        auto owner = c.resolve(c.data, c.aura.caster, c.aura.caster);
        auto holder = c.resolve(c.data, c.aura.caster, c.aura.holder);
        auto actor = c.resolve(c.data, e.victim, e.actor);
        auto victim = c.resolve(c.data, e.actor, e.victim);
        if (!Live(owner, c.aura.caster, c.aura.caster) || !owner.player ||
            !Live(holder, c.aura.holder, c.aura.caster) ||
            actor.identity != e.actor || victim.identity != e.victim || !actor.inWorld || !victim.inWorld ||
            !SameFrame(e.actor, c.aura.caster) || !SameFrame(e.victim, c.aura.caster) ||
            victim.relation != e.relation || actor.relation != e.relation ||
            ((e.damage || b.selector == Selector::AbsorbUsed) ? e.relation != Relation::Hostile : e.relation != Relation::Friendly))
        {
            return reject(Status::Target);
        }
        if (incoming)
        {
            if (e.victim != c.aura.holder || (b.selector == Selector::AbsorbUsed && e.shieldCaster != c.aura.caster))
            {
                return reject(Status::Unauthorized);
            }
        }
        else if (pet)
        {
            if (!OwnedPet(actor, c.aura.caster, b.cohort) || e.controller != c.aura.caster ||
                e.originalOwner != c.aura.caster ||
                (e.originalCaster != e.actor && e.originalCaster != c.aura.caster))
            {
                return reject(Status::Unauthorized);
            }
        }
        else if (e.actor != c.aura.holder || e.originalCaster != e.actor || !actor.player)
        {
            return reject(Status::Unauthorized);
        }
        if ((c.requirementsMet & b.requirements) != b.requirements)
        {
            return reject(Status::Dependency);
        }
        if (e.spell && !NativeReady(c, e.spell))
        {
            return reject(Status::NativeMismatch);
        }
        if (b.sources.size && std::find(b.sources.values.begin(), b.sources.values.begin() + b.sources.size,
            e.spell) == b.sources.values.begin() + b.sources.size && !NamedEdge(b, e, c.aura))
        {
            return reject(Status::NoEffect);
        }
        if ((b.spell == 301011 && e.victim != c.aura.holder) ||
            (b.spell == 704884 && e.victim == c.aura.holder) ||
            (b.spell == 301273 && (!e.ownerMaxHealth || uint64_t(e.ownerHealth) * 2 <= e.ownerMaxHealth)) ||
            (b.spell == 707623 && (!e.ownerMaxHealth || uint64_t(e.ownerHealth) * 100 >= uint64_t(e.ownerMaxHealth) * 75)) ||
            (b.spell == 804223 && (!e.victimMaxHealth || uint64_t(e.victimHealth) * 100 >= uint64_t(e.victimMaxHealth) * 35)) ||
            (b.spell == 704777 && !e.frontArc) || (b.spell == 705306 && e.attackerBoss))
        {
            return reject(Status::NoEffect);
        }
        auto native = FindNative(b.spell);
        auto source = FindNative(e.spell);
        auto proc = c.stockProc;
        // Selected policy overrides replace the native mask, never external table restrictions.
        if (!proc.flags)
        {
            proc.flags = b.overrideProcFlags || !native->procFlags ? EventMask(e, incoming) : native->procFlags;
        }
        uint32_t flags = incoming ? e.takenFlags : e.doneFlags;
        uint32_t extra = e.extra;
        if ((b.overrideProcFlags || !native->procFlags) && !c.stockProc.extra)
        {
            // Explicit heal/absorb selectors override stock's no-periodic-heal default.
            proc.extra = b.selector == Selector::AbsorbUsed ? 0x400 : 3;
        }
        if (!(flags & EventMask(e, incoming)) ||
            !StockProcMatches(proc, flags, extra, e.school, source ? source->family : 0,
                source ? source->familyMask : std::array<uint32_t, 3>{}))
        {
            return reject(Status::NoEffect);
        }
        bool onceCast = b.aggregateCast && !e.periodic && !e.autoAttack && !incoming && !pet;
        if (onceCast && (!e.castId || !e.aggregateComplete))
        {
            return reject(Status::Invalid);
        }
        bool castSeen = false;
        auto remembered = Remember(p.m_next, e, onceCast, castSeen);
        if (remembered != Status::Ready)
        {
            return reject(remembered);
        }
        bool debit = !castSeen || e.periodic || !e.castId;
        if (!state.initialized)
        {
            p.m_next.initialized = true;
            p.m_next.aura = c.aura;
            p.m_next.charges = b.spell == 801827 && (c.requirementsMet & RockadierTen) ? 10 : b.charges;
        }
        if (b.charges && debit && !p.m_next.charges)
        {
            return reject(Status::Charges);
        }
        uint64_t ready = state.readyMs;
        AttackerCooldown* attackerCooldown = nullptr;
        if (b.spell == 704777)
        {
            ready = 0;
            for (std::size_t i = 0; i < p.m_next.attackers.size; ++i)
            {
                if (p.m_next.attackers.values[i].actor == e.actor)
                {
                    attackerCooldown = &p.m_next.attackers.values[i];
                    ready = attackerCooldown->readyMs;
                }
            }
            if (!attackerCooldown)
            {
                for (std::size_t i = 0; i < p.m_next.attackers.size; ++i)
                {
                    if (p.m_next.attackers.values[i].readyMs <= c.nowMs)
                    {
                        attackerCooldown = &p.m_next.attackers.values[i];
                        *attackerCooldown = {e.actor, 0};
                        break;
                    }
                }
                if (!attackerCooldown)
                {
                    if (!p.m_next.attackers.Push({e.actor, 0}))
                    {
                        return reject(Status::Limit);
                    }
                    attackerCooldown = &p.m_next.attackers.values[p.m_next.attackers.size - 1];
                }
            }
        }
        if (debit && c.nowMs < ready)
        {
            return reject(Status::Cooldown);
        }
        if (c.roll >= 100 || c.roll >= b.chance)
        {
            return reject(Status::NoEffect);
        }
        if (UINT64_MAX - c.nowMs < b.icdMs)
        {
            return reject(Status::Limit);
        }
        uint64_t input = b.selector == Selector::AbsorbUsed ? e.absorbed :
            b.selector == Selector::Overheal ? e.overheal : e.damage ? e.damage : e.healing;
        for (auto const& part : Bindings)
        {
            if (part.spell != b.spell || part.selector == Selector::None)
            {
                continue;
            }
            if (!part.defined || part.childContractGap || !NativeReady(c, part.child) ||
                (c.requirementsMet & part.requirements) != part.requirements)
            {
                return reject(Status::Dependency);
            }
            auto child = FindNative(part.child);
            auto facts = c.native(c.data, part.child);
            uint64_t numerator = uint64_t(std::max(0, c.basePercent[part.slot].value_or(part.percent)));
            uint32_t denominator = 100;
            switch (part.scaling)
            {
                case Scaling::ConsumedStacks:
                    if (!e.consumedStacks || e.consumedStacks > (b.spell == 560142 ? 15u : 5u))
                    {
                        return reject(Status::Invalid);
                    }
                    numerator *= e.consumedStacks;
                    break;
                case Scaling::AuraStacks:
                    if (!e.auraStacks || e.auraStacks > 10)
                    {
                        return reject(Status::Invalid);
                    }
                    numerator *= e.auraStacks;
                    break;
                case Scaling::NearbyEnemies: numerator *= std::min(10u, e.nearbyEnemies); break;
                case Scaling::Intellect:
                    numerator = numerator * 8 + uint64_t(e.intellect);
                    denominator = 800;
                    p.m_manaDebit = uint32_t(85 + uint64_t(e.intellect) * 2 / 5);
                    if (p.m_manaDebit > e.availableMana)
                    {
                        return reject(Status::Dependency);
                    }
                    break;
                case Scaling::None: numerator = 0; break;
                case Scaling::Percent: break;
            }
            int32_t amount = part.kind == Kind::Marker ? 0 : Percent(input, numerator, denominator);
            if (!amount && part.kind != Kind::Marker)
            {
                continue;
            }
            Bounded<Identity, MaxRecipients> targets;
            Kind outputKind = part.kind == Kind::Store || b.spell == 301273 ?
                (e.healing ? Kind::Heal : Kind::Damage) : part.kind;
            bool helpful = outputKind != Kind::Damage;
            switch (part.recipient)
            {
                case Recipient::Owner: targets.Push(c.aura.caster); break;
                case Recipient::Holder: targets.Push(c.aura.holder); break;
                case Recipient::Victim: case Recipient::EventTarget: targets.Push(e.victim); break;
                case Recipient::Attacker: targets.Push(e.actor); break;
                case Recipient::ChildTarget: return reject(Status::Dependency);
                default:
                    if (!c.recipients)
                    {
                        return reject(Status::Dependency);
                    }
                    targets = c.recipients(c.data, part, e);
                    break;
            }
            if (!targets.size || targets.size > MaxRecipients)
            {
                return reject(Status::Target);
            }
            bool area = part.recipient == Recipient::LowestFriendly || part.recipient == Recipient::NearbyFriendly ||
                part.recipient == Recipient::NearbyHostile || part.recipient == Recipient::NearbyMixed ||
                part.recipient == Recipient::SameRelation || part.recipient == Recipient::NearestHostile ||
                part.recipient == Recipient::GroundHostile;
            if (area && !facts.radius[0] && b.spell != 707560)
            {
                return reject(Status::Dependency);
            }
            // Native caster-area targets stay on the holder; splashes are centered on the event target.
            Identity anchor = (child->targetsA[0] == 22 || child->targetsA[0] == 30 ||
                b.spell == 600327 || b.spell == 525307 || b.spell == 706926 || b.spell == 706464) ?
                c.aura.holder : e.victim;
            if (part.recipient == Recipient::GroundHostile &&
                (!e.ground.valid || !SameFrame(e.ground.frame, c.aura.caster) || !c.groundDistance ||
                !std::all_of(e.ground.coordinates.begin(), e.ground.coordinates.end(),
                    [](double v) { return std::isfinite(v) && std::abs(v) <= 100000; })))
            {
                return reject(Status::Dependency);
            }
            Bounded<UnitFacts, MaxRecipients> resolved;
            for (std::size_t i = 0; i < targets.size; ++i)
            {
                auto const& target = targets.values[i];
                auto resolution = c.resolve(c.data, c.aura.caster, target);
                if (area)
                {
                    auto areaFacts = c.resolve(c.data, anchor, target);
                    if (areaFacts.identity != resolution.identity)
                    {
                        return reject(Status::Target);
                    }
                    resolution.distance = part.recipient == Recipient::GroundHostile ?
                        c.groundDistance(c.data, e.ground, target) : areaFacts.distance;
                }
                bool duplicate = false;
                for (std::size_t j = 0; j < i; ++j)
                {
                    duplicate |= target == targets.values[j];
                }
                if (duplicate || !Live(resolution, target, c.aura.caster) ||
                    resolution.relation != (helpful ? Relation::Friendly : Relation::Hostile) ||
                    (area && !part.includeOriginal && target == e.victim) ||
                    (part.recipient == Recipient::Ancestor && !OwnedPet(resolution, c.aura.caster, Cohort::Ancestor)) ||
                    (part.splitBudget && target != c.aura.caster && !OwnedPet(resolution, c.aura.caster, Cohort::Undead)))
                {
                    return reject(Status::Target);
                }
                if (area && resolution.distance > (b.spell == 707560 ? 20000u : facts.radius[0]))
                {
                    return reject(Status::Target);
                }
                resolved.Push(resolution);
            }
            std::sort(resolved.values.begin(), resolved.values.begin() + resolved.size,
                [&](UnitFacts const& a, UnitFacts const& other)
                {
                    if (part.recipient == Recipient::LowestFriendly && a.maxHealth && other.maxHealth)
                    {
                        uint64_t left = uint64_t(a.health) * other.maxHealth;
                        uint64_t right = uint64_t(other.health) * a.maxHealth;
                        if (left != right) { return left < right; }
                    }
                    return std::tie(a.distance, a.identity.guid) < std::tie(other.distance, other.identity.guid);
                });
            std::size_t count = part.splitBudget ? resolved.size : std::min(resolved.size, std::size_t(part.targetCap));
            for (std::size_t i = 0; i < count; ++i)
            {
                Intent intent;
                intent.source = c.aura;
                intent.source.slot = part.slot;
                intent.actor = pet ? e.actor : c.aura.holder;
                intent.originalCaster = c.aura.caster;
                intent.target = resolved.values[i].identity;
                intent.child = part.child;
                intent.slot = part.slot;
                intent.valueSlots = part.valueSlots;
                intent.companionSlots = part.companionSlots;
                intent.kind = outputKind;
                intent.schedule = part.schedule;
                intent.amount = part.splitBudget ? amount / int32_t(count) + (i < uint32_t(amount) % count ? 1 : 0) : amount;
                intent.followupHealPercent = part.followupHealPercent;
                intent.allowCritical = part.allowCritical;
                intent.lineage = e.lineage;
                intent.lineage.parents.Push(c.aura);
                intent.lineage.terminal = !OutgoingEdge(b.spell);
                intent.anchor = area ? anchor : intent.target;
                if (part.recipient == Recipient::GroundHostile) { intent.ground = e.ground; }
                if (part.schedule != Schedule::Immediate || part.replaceContribution || part.kind == Kind::Shield)
                {
                    if (!facts.durationMs || facts.durationMs > combat::MaxAuraDurationMs ||
                        UINT64_MAX - c.nowMs < facts.durationMs)
                    {
                        return reject(Status::Dependency);
                    }
                    intent.expiresMs = c.nowMs + facts.durationMs;
                }
                if (part.schedule == Schedule::Total || part.schedule == Schedule::PerTick)
                {
                    intent.ticks = facts.ticks[0];
                    intent.periodMs = child->periods[0];
                    if (!intent.periodMs || !intent.ticks || intent.ticks > MaxTicks ||
                        intent.ticks != facts.durationMs / intent.periodMs)
                    {
                        return reject(Status::Dependency);
                    }
                }
                if (part.accumulateShield)
                {
                    intent.amount = int32_t(std::min<uint64_t>(INT32_MAX,
                        std::min<uint64_t>(owner.maxHealth, uint64_t(amount) + c.shieldRemaining)));
                }
                if (b.spell == 704903 && count == 1)
                {
                    intent.amount = Percent(uint64_t(intent.amount), 150);
                }
                if (part.replaceContribution)
                {
                    Contribution* previous = nullptr;
                    for (std::size_t j = 0; j < p.m_next.contributions.size; ++j)
                    {
                        auto& contribution = p.m_next.contributions.values[j];
                        if (contribution.slot == part.slot)
                        {
                            previous = &contribution;
                        }
                    }
                    Contribution replacement{part.slot, part.child, intent.target, amount, intent.expiresMs};
                    if (previous)
                    {
                        intent.previousContribution = previous->amount;
                        *previous = replacement;
                    }
                    else if (!p.m_next.contributions.Push(replacement))
                    {
                        return reject(Status::Limit);
                    }
                }
                if (!p.m_intents.Push(intent))
                {
                    return reject(Status::Limit);
                }
                p.m_budgetDebit += intent.ticks * (intent.followupHealPercent ? 2 : 1);
                // Pierced's immediate and total bleed share the original snapshot.
                if (b.spell == 705033)
                {
                    p.m_intents.values[p.m_intents.size - 1].schedule = Schedule::Total;
                    p.m_intents.values[p.m_intents.size - 1].amount = Percent(input, 35);
                    intent.kind = Kind::Damage;
                    intent.schedule = Schedule::Immediate;
                    intent.ticks = 1;
                    intent.expiresMs = 0;
                    if (!p.m_intents.Push(intent)) { return reject(Status::Limit); }
                    ++p.m_budgetDebit;
                }
            }
        }
        if (!p.m_intents.size)
        {
            return reject(Status::NoEffect);
        }
        if (p.m_budgetDebit > chain.remaining)
        {
            return reject(Status::Limit);
        }
        if (debit)
        {
            if (b.charges) { --p.m_next.charges; }
            if (attackerCooldown) { attackerCooldown->readyMs = c.nowMs + b.icdMs; }
            else { p.m_next.readyMs = c.nowMs + b.icdMs; }
        }
        ++p.m_next.revision;
        p.m_next.clockMs = c.nowMs;
        p.m_suppressSlots = b.suppressSlots;
        p.m_status = Status::Ready;
        return p;
    }

    Status Commit(Context const& c, State& state, ChainBudget& chain, Plan const& plan, bool effectsPrepared)
    {
        if (plan.m_status != Status::Ready) { return plan.m_status; }
        if (!effectsPrepared) { return Status::Cancelled; }
        if (state.revision != plan.m_revision || chain.revision != plan.m_chainRevision || c.nowMs != plan.m_nowMs)
        {
            return Status::Stale;
        }
        auto current = Prepare(c, state, plan.m_event, chain);
        if (current.Result() != Status::Ready || current.m_intents.size != plan.m_intents.size ||
            current.m_manaDebit != plan.m_manaDebit || current.m_suppressSlots != plan.m_suppressSlots ||
            current.m_budgetDebit != plan.m_budgetDebit || current.m_next.charges != plan.m_next.charges)
        {
            return Status::Stale;
        }
        for (std::size_t i = 0; i < current.m_intents.size; ++i)
        {
            if (!SameIntent(current.m_intents.values[i], plan.m_intents.values[i]))
            {
                return Status::Stale;
            }
        }
        state = current.m_next;
        chain.remaining -= current.m_budgetDebit;
        ++chain.revision;
        return Status::Ready;
    }

    Bounded<Contribution, 3> RemoveContributions(State& state, uint64_t nowMs, bool invalidate)
    {
        Bounded<Contribution, 3> removed;
        if (state.contributions.size > 3 || nowMs < state.clockMs || state.revision == UINT64_MAX)
        {
            return removed;
        }
        Bounded<Contribution, 3> kept;
        for (std::size_t i = 0; i < state.contributions.size; ++i)
        {
            auto const& contribution = state.contributions.values[i];
            if (invalidate || contribution.expiresMs <= nowMs) { removed.Push(contribution); }
            else { kept.Push(contribution); }
        }
        if (removed.size || invalidate)
        {
            state.contributions = kept;
            ++state.revision;
        }
        if (invalidate)
        {
            state.active = false;
            state.histories = {};
            state.attackers = {};
            state.charges = 0;
            state.readyMs = 0;
        }
        state.clockMs = nowMs;
        return removed;
    }

    void ForgetActor(State& state, Identity const& retiredLife)
    {
        if (!Valid(retiredLife) || state.histories.size > MaxActors ||
            state.attackers.size > MaxRecipients || state.revision == UINT64_MAX)
        {
            return;
        }
        auto oldSize = state.histories.size + state.attackers.size;
        auto historiesEnd = std::remove_if(state.histories.values.begin(),
            state.histories.values.begin() + state.histories.size,
            [&](History const& h) { return h.actor == retiredLife; });
        state.histories.size = std::size_t(historiesEnd - state.histories.values.begin());
        auto attackersEnd = std::remove_if(state.attackers.values.begin(),
            state.attackers.values.begin() + state.attackers.size,
            [&](AttackerCooldown const& a) { return a.actor == retiredLife; });
        state.attackers.size = std::size_t(attackersEnd - state.attackers.values.begin());
        if (state.histories.size + state.attackers.size != oldSize) { ++state.revision; }
    }
}
