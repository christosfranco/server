// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COA_EXTENDED_MODIFIER_RULES_H
#define MANGOS_COA_EXTENDED_MODIFIER_RULES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Owner-selected Ascender rules. No native loader, grants, Unit or database access.
namespace coa::modifier
{
    constexpr std::size_t MaxSources = 256;
    constexpr std::size_t MaxSelectedSpells = 128;
    constexpr double ValueBudget = 1000000000.0;
    using Mask = std::array<uint32_t, 3>;
    using Digest = std::array<uint8_t, 32>;

    enum class Status { Ready, Inactive, Unsupported, Invalid, Unauthorized, MissingContext, Limit, Stale };
    enum class Attack { Melee, Offhand, Ranged, Spell };
    enum class Field
    {
        HitChance, MeleeAttackPower, RangedAttackPower, DamageSpellPower,
        HealingSpellPower, AbsorbCapacity, AbsorbBase, DamageDone, DamageTaken, HealingDone,
        HealingTaken, CritChance, Armor, Strength, Agility, Stamina, Intellect,
        Spirit, MaxMana, MaxHealth, MinimumRange, CastTime, Cooldown, Duration,
        Cost, TargetCount, AttackPowerCoefficient, SpellPowerCoefficient,
        RangedAttackPowerCoefficient, CriticalHealingBonus, CriticalDamageBonus,
        CastWhileMoving, CastDuringChannel, ChannelDefense, PeriodicHaste,
        AreaTargetBoundary, Metadata, Count
    };
    enum class Arithmetic { Flat, Percent, SpellPercent, AbsorbPercent, Flag };
    enum class Scope { Holder, Target, CasterTarget, Area };
    enum class Phase { Any, Direct, Periodic };
    enum class Group { Independent, PartyPower, PartyHit };
    enum NodeFlags : uint32_t
    {
        NoFlags = 0, MovingCast = 1, PreserveChannel = 2, ChannelAvoidance = 4,
        HastedPeriodic = 8, NoMinimumRange = 16, BlockTarget = 32, ForcedMiss = 64
    };

    struct Entity
    {
        uint64_t guid = 0, generation = 0;
    };
    bool operator==(Entity const& a, Entity const& b);
    struct SourceKey
    {
        Entity caster, holder;
        uint64_t application = 0;
        uint32_t spell = 0, slot = 0;
    };
    bool operator==(SourceKey const& a, SourceKey const& b);

    struct NativeAmount
    {
        int32_t basePoints = 0, dieSides = 0;
        double perLevel = 0, perCombo = 0;
        uint32_t baseLevel = 0, spellLevel = 0, maxLevel = 0;
        bool needsNativeScalar = false;
    };
    struct AmountContext
    {
        uint32_t level = 0, comboPoints = 0;
        std::optional<int32_t> dieRoll;
        // Supplied by the verified native scaling path, never a guessed class curve.
        std::optional<double> nativeScalar;
    };
    struct ValueResult
    {
        Status status = Status::Invalid;
        double value = 0;
    };
    ValueResult CalculateAmount(NativeAmount const& native, AmountContext const& context);

    struct NativeModifier
    {
        uint32_t spell = 0, slot = 0, effect = 0, aura = 0;
        int32_t miscA = 0, miscB = 0;
        uint32_t family = 0;
        Mask familyFlags{}, effectMask{};
        Digest inputDigest{}; // Entire consumed three-slot native row, not just this slot.
        NativeAmount amount;
        uint32_t stackCap = 0, rankRoot = 0, rank = 0;
        int32_t equippedItemClass = -1;
        uint32_t equippedSubclassMask = 0, equippedInventoryMask = 0;
        // Only the two authored named-spell exceptions use this private exact-ID set.
        std::vector<uint32_t> selectedSpells;
        // Private compiler resolves native ranks; these are NOT inferred from names at runtime.
        Group group = Group::Independent;
        double amountLimit = ValueBudget;
    };
    struct SourceContext
    {
        void const* data = nullptr;
        // Must check frozen binding membership, whole-row pin, committed native source,
        // rank/group/selection metadata, caster/holder generations and pet cohort.
        bool (*authorize)(void const*, SourceKey const&, NativeModifier const&) = nullptr;
        AmountContext amount;
        uint32_t stacks = 0;
        std::optional<bool> equipmentEligible;
        std::array<std::optional<double>, 5> stats{};
        std::optional<double> armorPenetrationRating, ownerHealingPower;
        // Sample with ALL policy-derived feedback excluded. Use the existing curve.
        std::optional<double> nativeManaFromIntellect;
        std::optional<bool> manaProvisioned, healthProvisioned;
        Entity petOwner;
        uint64_t area = 0;
    };
    struct Contribution
    {
        Field field = Field::Metadata;
        Arithmetic arithmetic = Arithmetic::Flat;
        double amount = 0;
        Scope scope = Scope::Holder;
        Phase phase = Phase::Any;
        uint32_t schoolMask = 0, condition = 0, flags = 0;
        bool matchMask = true, poisonAlternative = false, ownCircleAlternative = false;
        bool nonPlayerOnly = false, instantOnly = false, criticalOnly = false;
        bool healingOnly = false, damageOnly = false;
    };
    class Replacement
    {
    public:
        Status Result() const { return m_status; }
        SourceKey const& Source() const { return m_source; }
        NativeModifier const& Binding() const { return m_binding; }
        std::vector<Contribution> const& Contributions() const { return m_terms; }
        uint32_t SuppressSlots() const { return m_suppressSlots; }
    private:
        friend Replacement Evaluate(NativeModifier const&, SourceKey const&, SourceContext const&);
        friend class Ledger;
        Status m_status = Status::Invalid;
        SourceKey m_source;
        NativeModifier m_binding;
        std::vector<Contribution> m_terms;
        uint32_t m_suppressSlots = 0;
        uint64_t m_area = 0;
    };
    Replacement Evaluate(NativeModifier const& native, SourceKey const& source,
        SourceContext const& context);
    bool SupportsAura(uint32_t aura);
    Field SpellModField(int32_t operation, uint32_t sourceSpell);
    bool MatchesFamily(uint32_t family, Mask const& mask,
        uint32_t candidateFamily, Mask const& candidateMask);

    struct TargetState
    {
        uint32_t health = 0, maxHealth = 0;
        bool bleeding = false, poisoned = false, ownTranquilCircle = false;
    };
    struct AreaState
    {
        uint64_t area = 0;
        bool actorInside = false, targetInside = false;
        bool actorHostileToOwner = false, targeted = false;
    };
    struct Query
    {
        Field field = Field::Metadata;
        Entity actor, target;
        uint32_t spell = 0, family = 0, schoolMask = 0;
        Mask familyFlags{};
        Attack attack = Attack::Spell;
        bool periodic = false, critical = false, healing = false, damage = false;
        bool instant = false, channeling = false, manaCost = false;
        bool damageShield = false;
        std::optional<bool> playerOpponent;
        // A coefficient of zero, or no corresponding native stat term, stays zero.
        std::optional<bool> hasNativeCoefficient;
        std::optional<TargetState> targetState;
        std::optional<AreaState> areaState;
    };
    struct Result
    {
        Status status = Status::Invalid;
        double value = 0, flat = 0, additivePercent = 0, multiplier = 1;
        uint32_t flags = 0;
        std::size_t matched = 0;
        // Preflight and consume only after an otherwise-successful damaging attack.
        std::optional<SourceKey> forcedMissSource = std::nullopt;
    };
    class Ledger
    {
    public:
        Status Replace(Replacement const& replacement);
        bool Remove(SourceKey const& source);
        void Clear() { m_sources.clear(); }
        std::size_t Size() const { return m_sources.size(); }
        // Absolute recalculation from the original baseline, never apply an inverse delta.
        Result Calculate(Query const& query, double base, double cap = ValueBudget) const;
    private:
        std::vector<Replacement> m_sources;
    };

    ValueResult MissChance(double baseMiss, double hitBonus, Attack attack);
    // Creation and consumption are separate: changing modifiers cannot refill a shield.
    struct AbsorbResult
    {
        Status status = Status::Invalid;
        uint32_t consumed = 0, remaining = 0, damage = 0;
    };
    AbsorbResult ConsumeShield(double capacity, double incomingDamage);
    // Scheduler keeps expiry fixed and carries fractional progress through a haste change.
    ValueResult HastedTickRemaining(double baseIntervalMs, double hastePercent,
        double completedFraction);
}
#endif
