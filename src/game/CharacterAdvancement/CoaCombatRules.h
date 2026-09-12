// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COA_COMBAT_RULES_H
#define MANGOS_COA_COMBAT_RULES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

// Documented Ascender policy, not a decoder for vendor combat semantics.
namespace coa::combat
{
    constexpr std::size_t MaxAuras = 64;
    constexpr std::size_t MaxPending = 32;
    constexpr uint32_t MaxDelayMs = 60000;
    constexpr uint32_t MaxAuraDurationMs = 86400000;
    constexpr uint32_t MaxRuleDepth = 4;

    template<class T, std::size_t N> struct Bounded
    {
        std::array<T, N> values{};
        std::size_t size = 0;
        bool Push(T const& value)
        {
            if (size >= N) { return false; }
            values[size++] = value;
            return true;
        }
    };

    struct Identity
    {
        uint64_t guid = 0;
        uint32_t instance = 0;
        uint64_t generation = 0;
        uint32_t map = 0;
        uint64_t frame = 0;
        bool deck = false;
    };
    bool operator==(Identity const& a, Identity const& b);
    bool operator!=(Identity const& a, Identity const& b);

    enum class DestinationMode { Recipients, PersistentArea };
    struct Destination
    {
        Identity owner; // Captured map/frame/lifetime, not world-coordinate conversion.
        uint32_t phase = 0;
        float x = 0, y = 0, z = 0;
        DestinationMode mode = DestinationMode::Recipients;
        Bounded<Identity, 20> recipients;
    };
    using Location = std::variant<std::monostate, Destination>;

    enum class Target { Self, Enemy, Friendly };
    enum class Status
    {
        Ready, Unsupported, Invalid, Unauthorized, Insufficient, Limit,
        Stale, Failed, Recursive
    };
    enum class Operation
    {
        Delta, Cap, Duration, SummonDuration, Delay, DelayResource,
        SolarMarkers, Fragments, ConvertSoul, Unsupported, DurationAtDestination, DelayAtDestination
    };
    struct Binding
    {
        uint32_t spell, slot, effect, playerClass;
        Operation operation;
        uint32_t child, resource;
        int32_t amount;
        uint32_t suppressSlots;
    };
    Binding const* FindBinding(uint32_t spell, uint32_t slot, uint32_t effect);

    struct Resource
    {
        uint32_t spell, playerClass, cap, knownMarker;
        bool nonRefresh;
    };
    Resource const* FindResource(uint32_t spell);
    bool HasBindings(uint32_t spell);
    bool PolicyResourceEdge(uint32_t source, uint32_t resource);
    bool ControlledAura(uint32_t spell);

    struct NativeSpell
    {
        bool exists = false;
        // -1 permanent, 0 no duration, positive milliseconds, other values invalid.
        int32_t durationMs = 0;
    };
    // Tooltip-only resource generators the evidence pipeline cannot see: the
    // cast carries no 175/178/183 effect (class_resources.lua CLASSES), so no
    // Binding row can name them. Same numbers the Lua asserted.
    enum class GeneratorGate { None, Known, SelfAura, KnownOrSelfAura };
    struct Generator
    {
        uint32_t spell, playerClass, resource;
        int32_t amount;
        GeneratorGate gate;
        uint32_t gateSpell; // Known/self-aura precondition, 0 when gate is None.
    };
    Generator const* FindGenerator(uint32_t spell);
    struct Resolution
    {
        bool exists = false, alive = false, inWorld = false;
        bool inRange = false, friendly = false;
        Identity identity;
    };
    struct Context
    {
        Identity actor;
        uint32_t playerClass = 0;
        uint64_t nowMs = 0;
        bool alive = false, inWorld = false, inCombat = false;
        int32_t criticalStrikeRating = 0;
        uint32_t activeVow = 0;
        uint32_t weaponMask = 0; // bit0 main hand, bit1 off hand; usable equipped weapons only
        void const* data = nullptr;
        bool (*known)(void const*, uint32_t spell) = nullptr;
        // Self-aura presence for conditional generators (Sunwalker's Grace
        // 680313 gating Gavel; Thirst 500107 read as known OR applied, the
        // class_resources.lua Bloodmage [C]). Policy state tracks resources
        // and markers only, so this reads the live holder like the Lua did.
        bool (*selfAura)(void const*, uint32_t spell) = nullptr;
        // Committed catalog authorization, including an explicit source/currency edge.
        // currency=0 asks about the source spell itself. Never grant all children.
        bool (*authorized)(void const*, uint32_t spell, uint32_t currency) = nullptr;
        NativeSpell (*nativeSpell)(void const*, uint32_t spell) = nullptr;
        // Resolve identity and CURRENT relation/range/phase/LOS in the actor's frame.
        Resolution (*resolve)(void const*, Identity const&) = nullptr;
        bool (*destinationValid)(void const*, Destination const&) = nullptr;
    };

    struct Payload
    {
        std::array<std::optional<int32_t>, 3> basePoints{};
        std::array<uint32_t, 3> familyMask{}, effectMask{};
        uint32_t procMask = 0;
        int32_t nativeFlags = 0; // Retained metadata, NOT stock trigger flags.
    };
    struct Effect
    {
        uint32_t slot = 0, type = 0, child = 0;
        int32_t calculatedValue = 0;
        Identity target;
        Target targetPolicy = Target::Self;
        Payload payload;
        Location location;
    };
    enum class EventKind
    {
        Cast, CapModifier, ResourceGain, ResourceSpend, DirectSpell,
        Tick, Refresh, RemoveAura, Invalidate, SetResource
    };
    struct Event
    {
        EventKind kind = EventKind::Cast;
        uint64_t sequence = 0, castId = 0;
        uint32_t spell = 0;
        // RemoveAura: originalCaster is the removed HOLDER's caster; source is
        // the dispeller/action source (or holder caster for natural removal).
        Identity source, originalCaster, target;
        Target targetPolicy = Target::Self;
        Bounded<Effect, 3> effects;
        uint32_t resource = 0;
        int32_t amount = 0;
        bool apply = true;
        bool successful = true, direct = false, critical = false;
        bool damage = false, healing = false, triggered = false, resolvedHit = false;
        uint32_t nativeEventMask = 0;
        uint32_t effectiveAmount = 0;
        uint32_t ruleDepth = 0;
        std::array<uint32_t, MaxRuleDepth> ancestors{};
    };
    enum class IntentKind { Cast, DurationCast, SummonDuration, ResourceGain, Fulfillment };
    struct CastIntent
    {
        IntentKind kind = IntentKind::Cast;
        uint32_t parent = 0, spell = 0, slot = 0;
        Identity source, originalCaster, target;
        Target targetPolicy = Target::Self;
        uint64_t dueMs = 0, sequence = 0;
        uint32_t durationMs = 0, resource = 0;
        int32_t amount = 0;
        uint32_t ruleDepth = 1;
        std::array<uint32_t, MaxRuleDepth> ancestors{};
        Payload payload;
        Location location;
    };
    struct Aura
    {
        uint32_t spell = 0;
        Identity target, caster;
        int32_t stacks = 0, charges = 0, capBonus = 0;
        uint64_t expiresMs = 0;
    };
    struct AuraDelta { Aura before, after; };
    struct DerivedStat
    {
        uint32_t sourceSpell = 900755;
        int32_t before = 0, replacement = 0;
    };
    struct CastHistory { uint64_t newest = 0, seen = 0; };
    struct State
    {
        Identity actor;
        uint64_t revision = 0, lastEvent = 0, nextOrder = 1, clockMs = 0;
        Bounded<Aura, MaxAuras> auras;
        Bounded<CastIntent, MaxPending> pending;
        CastHistory heatCasts, dawnCasts;
        uint64_t dawnReadyMs = 0;
        uint64_t intellectAtMs = 0;
        uint32_t decayRemainderMs = 0;
        int32_t intellect = 0;
        bool inCombat = false;
    };
    int32_t Count(State const& state, uint32_t spell, Identity const& target);

    class Plan
    {
    public:
        Status Result() const { return m_status; }
        Bounded<AuraDelta, MaxAuras * 2> const& Deltas() const { return m_deltas; }
        Bounded<CastIntent, MaxPending> const& Casts() const { return m_casts; }
        DerivedStat const& Intellect() const { return m_intellect; }
        uint32_t SuppressSlots() const { return m_suppressSlots; }
        State const& Preview() const { return m_next; }
    private:
        friend Plan Evaluate(Context const&, State const&, Event const&);
        friend Status Commit(Context const&, Plan const&, bool, State&);
        Status m_status = Status::Invalid;
        uint64_t m_revision = 0, m_nowMs = 0;
        uint32_t m_playerClass = 0, m_suppressSlots = 0;
        Event m_event;
        State m_next;
        Bounded<AuraDelta, MaxAuras * 2> m_deltas;
        Bounded<CastIntent, MaxPending> m_casts;
        DerivedStat m_intellect;
    };

    // Evaluate at cast admission AND at finish, before any irreversible stock effects.
    // Commit is synchronous, same clock/generation, after all required sinks preflight.
    Plan Evaluate(Context const& context, State const& state, Event const& event);
    Status Commit(Context const& context, Plan const& plan, bool effectsReady, State& state);
}
#endif
