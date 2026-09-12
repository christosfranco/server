// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COA_VALUE_TRANSFER_RULES_H
#define MANGOS_COA_VALUE_TRANSFER_RULES_H

#include "CoaCombatRules.h"
#include <string_view>

namespace coa::transfer
{
    using combat::Identity;
    using combat::Bounded;
    constexpr std::size_t MaxRecipients = 20, MaxIntents = 60;
    constexpr std::size_t MaxActors = 8, HistorySize = 32, MaxDepth = 4;
    constexpr uint32_t MaxChainEvents = 64, MaxTicks = 120;
    constexpr uint64_t MaxInput = uint64_t(UINT32_MAX) * MaxRecipients;

    enum class Status
    {
        Ready, NoEffect, Unsupported, SourceGap, NativeMismatch, Unauthorized,
        Invalid, Target, Dependency, Cooldown, Charges, Duplicate, Limit,
        Recursive, Stale, Cancelled
    };
    enum class Selector
    {
        None, Damage, DirectDamage, CriticalDamage, DirectCriticalDamage,
        Heal, CriticalHeal, PeriodicCriticalHeal, Overheal, DamageOrHeal,
        DamageTaken, DirectDamageTaken, MagicDamageTaken, PhysicalDamageTaken,
        PhysicalDamage, PhysicalNatureCritical, MeleeDamage, MeleeDamageTaken,
        AutoDamage, AutoCritical, MeleeAuto, RangedAuto, PeriodicDamage,
        HealReceived, AbsorbUsed, PetDamage, PetDamageOrHeal, PetAutoCritical
    };
    enum class Recipient
    {
        Owner, Holder, Victim, Attacker, EventTarget, Ancestor, SplitUndead,
        LowestFriendly, NearbyFriendly, NearbyHostile, NearestHostile,
        NearbyMixed, SameRelation, GroundHostile, ChildTarget
    };
    enum class Kind { Damage, Heal, Shield, AttackPower, Mana, Marker, Store };
    enum class Scaling { Percent, ConsumedStacks, AuraStacks, NearbyEnemies, Intellect, None };
    enum class Schedule { Immediate, Total, PerTick, Expiry };
    enum class Relation { Unknown, Friendly, Hostile };
    enum class Cohort { None, OwnedSummon, Undead, Ancestor, Ziggi };
    enum class Origin { Untrusted, Native, ValueTransfer, Resource };
    enum class Outcome { Failed, Hit, Critical, Absorbed, Miss, Immune, Evade };

    // These facts require exact source/native-condition checks in the adapter.
    enum Requirement : uint32_t
    {
        NativeChecks = 1, InnerDemon = 2, AdvantageSpent = 4,
        OwnBlackBlood = 8, OwnBlaze = 16, OwnThreads = 32,
        OwnLethargy = 64, OwnFluxArc = 128, AccursedForm = 256,
        FireEngraving = 512, BoonOfBear = 1024, DemonfireSix = 2048,
        RageCost = 4096, EngravingChildren = 8192, CompanionTransaction = 16384,
        WorldBinding = 32768, InitialStrike = 65536, RockadierTen = 131072
    };
    struct NativeRecord
    {
        uint32_t spell = 0;
        std::string_view digest;
        uint32_t family = 0, school = 0, procFlags = 0;
        std::array<uint32_t, 3> familyMask{}, effects{}, auras{}, children{};
        std::array<uint32_t, 3> targetsA{}, targetsB{}, periods{}, radii{};
        uint32_t durationIndex = 0, rangeIndex = 0, targetCap = 0;
        bool targetMatches = false;
    };
    struct Binding
    {
        uint32_t spell = 0, slot = 0, budgetSlot = 0, playerClass = 0, child = 0;
        bool defined = false, supplemental = false;
        Selector selector = Selector::None;
        Recipient recipient = Recipient::ChildTarget;
        Kind kind = Kind::Damage;
        Scaling scaling = Scaling::Percent;
        Schedule schedule = Schedule::Immediate;
        int32_t percent = 0;
        uint32_t chance = 0, charges = 0, icdMs = 0, requirements = NativeChecks;
        uint32_t valueSlots = 0, companionSlots = 0, suppressSlots = 0;
        uint32_t targetCap = 1, followupHealPercent = 0;
        bool aggregateCast = false, includeOriginal = false, splitBudget = false;
        bool replaceContribution = false, accumulateShield = false;
        bool overrideProcFlags = false, childContractGap = false, allowCritical = false;
        bool holderMayDiffer = false;
        Cohort cohort = Cohort::None;
        Bounded<uint32_t, 64> sources;
        std::string_view gap;
    };
    Binding const* FindBinding(uint32_t spell, uint32_t slot);
    Binding const* BindingAt(std::size_t index);
    std::size_t BindingCount();
    NativeRecord const* FindNative(uint32_t spell);

    struct AuraKey
    {
        uint32_t spell = 0, slot = 0;
        Identity caster, holder;
        uint64_t application = 0;
    };
    bool operator==(AuraKey const&, AuraKey const&);
    struct Root { Identity actor; uint64_t event = 0; };
    bool operator==(Root const&, Root const&);
    struct Lineage
    {
        Root root;
        Bounded<AuraKey, MaxDepth> parents;
        bool terminal = false;
    };
    // One shared budget for ALL branches of a root, never copied per recipient.
    struct ChainBudget { Root root; uint64_t revision = 0; uint32_t remaining = MaxChainEvents; };
    struct Point
    {
        Identity frame;
        std::array<double, 3> coordinates{};
        bool valid = false;
    };
    struct Event
    {
        Identity actor, originalCaster, controller, originalOwner, victim, shieldCaster;
        uint64_t id = 0, castId = 0;
        bool trustedIds = false, periodic = false, autoAttack = false;
        bool melee = false, ranged = false, aggregateComplete = false;
        bool healthCost = false, selfDamage = false, frontArc = false, attackerBoss = false;
        Origin origin = Origin::Untrusted;
        Origin shieldOrigin = Origin::Untrusted;
        Outcome outcome = Outcome::Failed;
        Relation relation = Relation::Unknown;
        uint32_t spell = 0, school = 0, doneFlags = 0, takenFlags = 0, extra = 0;
        uint64_t damage = 0, healing = 0, overheal = 0, absorbed = 0;
        uint32_t ownerHealth = 0, ownerMaxHealth = 0, victimHealth = 0, victimMaxHealth = 0;
        uint32_t consumedStacks = 0, auraStacks = 0, nearbyEnemies = 0;
        uint32_t intellect = 0, availableMana = 0;
        Lineage lineage;
        Point ground;
    };
    struct UnitFacts
    {
        Identity identity, controller, originalOwner;
        Relation relation = Relation::Unknown; // Relative to queried anchor.
        Cohort cohort = Cohort::None;
        uint32_t creature = 0, health = 0, maxHealth = 0, distance = 0;
        bool alive = false, inWorld = false, inRange = false, phase = false, los = false;
        bool player = false, worldBindingVerified = false, aiReady = false;
    };
    struct NativeFacts
    {
        std::string_view digest;
        bool dependenciesVerified = false;
        uint32_t durationMs = 0;
        std::array<uint32_t, 3> radius{}, ticks{};
    };
    struct ProcData
    {
        uint32_t flags = 0, extra = 0, school = 0, family = 0;
        std::array<uint32_t, 3> mask{};
    };
    bool StockProcMatches(ProcData const&, uint32_t flags, uint32_t extra,
                          uint32_t school, uint32_t family,
                          std::array<uint32_t, 3> const& mask);
    struct Context
    {
        AuraKey aura;
        uint32_t playerClass = 0, requirementsMet = 0;
        uint64_t nowMs = 0;
        // Stable roll supplied once per aura/root event, including retries.
        uint32_t roll = 100;
        // Native modifier result on the authored base percentage, BEFORE this
        // rule's consumed-stack/INT multiplier. Never a child flat basepoint.
        std::array<std::optional<int32_t>, 3> basePercent{};
        ProcData stockProc;
        void const* data = nullptr;
        bool (*authorized)(void const*, AuraKey const&, uint32_t playerClass) = nullptr;
        UnitFacts (*resolve)(void const*, Identity const& anchor, Identity const& target) = nullptr;
        NativeFacts (*native)(void const*, uint32_t spell) = nullptr;
        // Complete bounded selection, with native radius and exact row/slot policy.
        Bounded<Identity, MaxRecipients> (*recipients)(void const*, Binding const&, Event const&) = nullptr;
        // Frame-local distance in thousandths of a yard; needed only for fixed-ground output.
        uint32_t (*groundDistance)(void const*, Point const&, Identity const&) = nullptr;
        // Current own-source shield capacity, not the original or total absorb pool.
        uint32_t shieldRemaining = 0;
    };
    struct Seen { uint64_t event = 0, cast = 0; };
    struct History
    {
        Identity actor;
        std::array<Seen, HistorySize> fifo{};
        uint64_t eventFloor = 0, castFloor = 0;
        std::size_t size = 0, next = 0;
    };
    struct Contribution
    {
        uint32_t slot = 0, child = 0;
        Identity target;
        int32_t amount = 0;
        uint64_t expiresMs = 0;
    };
    struct AttackerCooldown { Identity actor; uint64_t readyMs = 0; };
    struct State
    {
        AuraKey aura;
        uint64_t revision = 0, clockMs = 0, readyMs = 0;
        uint32_t charges = 0;
        bool initialized = false, active = true;
        Bounded<History, MaxActors> histories;
        Bounded<Contribution, 3> contributions;
        Bounded<AttackerCooldown, MaxRecipients> attackers;
    };
    struct Intent
    {
        AuraKey source;
        Identity actor, originalCaster, target;
        uint32_t child = 0, slot = 0, valueSlots = 0, companionSlots = 0;
        Kind kind = Kind::Damage;
        Schedule schedule = Schedule::Immediate;
        int32_t amount = 0, previousContribution = 0;
        uint32_t ticks = 1, periodMs = 0, followupHealPercent = 0;
        uint64_t expiresMs = 0;
        bool allowCritical = false, suppressSourceBonuses = true;
        Lineage lineage;
        Identity anchor;
        Point ground;
    };
    // Saturating integral percentage arithmetic; no uint32 intermediates.
    int32_t Percent(uint64_t input, uint64_t numerator, uint32_t denominator = 100);
    std::optional<int32_t> NativeSignedValue(double value, bool signedAllowed);
    int32_t TickValue(Intent const&, uint32_t tick);

    class Plan
    {
    public:
        Status Result() const { return m_status; }
        Bounded<Intent, MaxIntents> const& Intents() const { return m_intents; }
        State const& Preview() const { return m_next; }
        uint32_t SuppressSlots() const { return m_suppressSlots; }
        uint32_t ManaDebit() const { return m_manaDebit; }
    private:
        friend Plan Prepare(Context const&, State const&, Event const&, ChainBudget const&);
        friend Status Commit(Context const&, State&, ChainBudget&, Plan const&, bool);
        Status m_status = Status::Invalid;
        uint64_t m_revision = 0, m_chainRevision = 0, m_nowMs = 0;
        uint32_t m_suppressSlots = 0, m_manaDebit = 0, m_budgetDebit = 0;
        State m_next;
        Event m_event;
        Bounded<Intent, MaxIntents> m_intents;
    };
    Plan Prepare(Context const&, State const&, Event const&, ChainBudget const&);
    // effectsPrepared means result-bearing, all-target preparation, NOT void CastSpell.
    Status Commit(Context const&, State&, ChainBudget&, Plan const&, bool effectsPrepared);
    // Expiry/unlearn returns inverse source replacements; never adds AP each tick.
    Bounded<Contribution, 3> RemoveContributions(State&, uint64_t nowMs, bool invalidate);
    // Call only after this exact actor life becomes unresolvable, not on overflow.
    void ForgetActor(State&, Identity const& retiredLife);
}
#endif
