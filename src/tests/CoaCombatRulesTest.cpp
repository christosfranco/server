// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "CoaCombatRules.h"
#include <limits>
#include <map>
#include <set>

using namespace coa::combat;

namespace
{
    struct Fixture
    {
        State state;
        Context context;
        Identity enemy{200, 7, 1}, friendly{300, 7, 1};
        std::set<uint32_t> known{300755, 804584, 807693, 1001};
        std::set<uint32_t> missing;
        std::set<uint32_t> selfAuras;
        std::set<std::pair<uint32_t, uint32_t>> edges;
        std::map<uint32_t, int32_t> durations{{807440, 60000}, {803061, 10000}, {680441, 20000}, {804301, 10000}};
        bool targetExists = true, targetAlive = true, targetRange = true;
        bool destinationAllowed = true;
        uint64_t targetGeneration = 1;

        explicit Fixture(uint32_t playerClass)
        {
            context.actor = {100, 7, 1};
            context.playerClass = playerClass;
            context.alive = context.inWorld = true;
            context.data = this;
            state.actor = context.actor;
            context.known = [](void const* data, uint32_t spell)
            {
                return static_cast<Fixture const*>(data)->known.count(spell) != 0;
            };
            context.selfAura = [](void const* data, uint32_t spell)
            {
                return static_cast<Fixture const*>(data)->selfAuras.count(spell) != 0;
            };
            context.authorized = [](void const* data, uint32_t spell, uint32_t currency)
            {
                return static_cast<Fixture const*>(data)->edges.count({spell, currency}) != 0;
            };
            context.nativeSpell = [](void const* data, uint32_t spell)
            {
                auto const& f = *static_cast<Fixture const*>(data);
                auto it = f.durations.find(spell);
                return NativeSpell{!f.missing.count(spell), it == f.durations.end() ? -1 : it->second};
            };
            context.resolve = [](void const* data, Identity const& target)
            {
                auto const& f = *static_cast<Fixture const*>(data);
                auto identity = target;
                identity.generation = f.targetGeneration;
                return Resolution{f.targetExists, f.targetAlive, true, f.targetRange,
                    target.guid == f.friendly.guid, identity};
            };
            context.destinationValid = [](void const* data, Destination const&)
            {
                return static_cast<Fixture const*>(data)->destinationAllowed;
            };
            Approve(500728, {500706});
            Approve(300755, {807389, 807533});
            Approve(1001, {500149, 807389, 807533, 500706, 805077, 500363, 804670});
        }
        // Fixture callbacks model an explicit committed authorization edge, not learning children.
        void Approve(uint32_t spell, std::initializer_list<uint32_t> currencies = {})
        {
            edges.insert({spell, 0});
            for (auto currency : currencies) { edges.insert({spell, currency}); }
        }
        Event Input(EventKind kind, uint32_t spell = 0)
        {
            Event e;
            e.kind = kind;
            e.sequence = state.lastEvent + 1;
            e.spell = spell;
            e.source = e.originalCaster = e.target = context.actor;
            return e;
        }
        Effect Slot(uint32_t slot, uint32_t type, uint32_t child, int32_t value = 1, bool hostile = false)
        {
            Effect e;
            e.slot = slot;
            e.type = type;
            e.child = child;
            e.calculatedValue = value;
            e.target = hostile ? enemy : context.actor;
            e.targetPolicy = hostile ? Target::Enemy : Target::Self;
            return e;
        }
        Event Cast(uint32_t spell, Effect effect)
        {
            auto e = Input(EventKind::Cast, spell);
            e.effects.Push(effect);
            return e;
        }
        Event Gain(uint32_t resource, int32_t amount, bool spend = false)
        {
            auto e = Input(spend ? EventKind::ResourceSpend : EventKind::ResourceGain, 1001);
            e.resource = resource;
            e.amount = amount;
            return e;
        }
        Event Crit(uint64_t cast)
        {
            auto e = Input(EventKind::DirectSpell, 1001);
            e.target = enemy;
            e.targetPolicy = Target::Enemy;
            e.direct = e.damage = e.critical = true;
            e.effectiveAmount = 10;
            e.castId = cast;
            return e;
        }
        Plan Apply(Event const& e, bool ready = true)
        {
            auto p = Evaluate(context, state, e);
            CHECK(p.Result() == Status::Ready);
            CHECK(Commit(context, p, ready, state) == (ready ? Status::Ready : Status::Failed));
            return p;
        }
        void Set(uint32_t spell, int32_t stacks, Identity target = {})
        {
            if (!target.guid) { target = context.actor; }
            for (std::size_t i = 0; i < state.auras.size; ++i)
            {
                if (state.auras.values[i].spell == spell && state.auras.values[i].target == target)
                {
                    state.auras.values[i].stacks = stacks;
                    return;
                }
            }
            CHECK(state.auras.Push({spell, target, context.actor, stacks}));
        }
        int32_t Value(uint32_t spell) { return Count(state, spell, context.actor); }
        Aura const* AuraFor(uint32_t spell)
        {
            for (std::size_t i = 0; i < state.auras.size; ++i)
            {
                if (state.auras.values[i].spell == spell) { return &state.auras.values[i]; }
            }
            return nullptr;
        }
    };
}

TEST(CoaCombat_solar_exact_amounts_thresholds_and_atomic_dawn)
{
    Fixture f(27);
    f.Approve(800764, {500149});
    f.Approve(804098, {500149});
    f.Approve(804584, {500149});
    auto invocation = f.Cast(800764, f.Slot(2, 175, 500149));
    invocation.effects.Push(f.Slot(1, 183, 704396, 100));
    auto first = f.Apply(invocation);
    CHECK_EQ(f.Value(500149), 10);
    CHECK_EQ(f.Value(802938), 1);
    CHECK_EQ(f.Value(802939), 1);
    CHECK_EQ(f.Value(704396), 0);
    CHECK_EQ(first.Preview().pending.size, 0u);
    CHECK_EQ(first.SuppressSlots(), 6u);
    auto dawn = f.Input(EventKind::Cast, 804584);
    auto rejected = Evaluate(f.context, f.state, dawn);
    CHECK(rejected.Result() == Status::Insufficient);
    CHECK_EQ(rejected.Deltas().size, 0u);
    f.Apply(f.Cast(804098, f.Slot(1, 175, 500149)));
    CHECK_EQ(f.Value(500149), 11);
    f.Apply(f.Gain(500149, 100));
    CHECK_EQ(f.Value(500149), 20);
    CHECK_EQ(f.Value(804586), 1);
    CHECK_EQ(f.Value(704396), 1);
    auto before = f.state.revision;
    auto p = Evaluate(f.context, f.state, f.Input(EventKind::Cast, 804584));
    REQUIRE(p.Result() == Status::Ready);
    CHECK_EQ(f.Value(500149), 20);
    CHECK_EQ(Count(p.Preview(), 500149, f.context.actor), 0);
    CHECK(Commit(f.context, p, false, f.state) == Status::Failed);
    CHECK_EQ(f.state.revision, before);
    CHECK_EQ(f.Value(807440), 0);
    CHECK(Commit(f.context, p, true, f.state) == Status::Ready);
    CHECK_EQ(f.Value(500149), 0);
    CHECK_EQ(f.Value(704396), 0);
    REQUIRE(f.AuraFor(807440));
    CHECK_EQ(f.AuraFor(807440)->charges, 10);
    CHECK(Commit(f.context, p, true, f.state) == Status::Stale);
    f.Apply(f.Gain(500149, 10));
    CHECK_EQ(f.Value(500149), 0);
}

TEST(CoaCombat_dawn_direct_owned_events_charges_cooldown_and_failed_child)
{
    Fixture f(27);
    f.Approve(804584, {500149});
    f.Set(500149, 20);
    f.Apply(f.Input(EventKind::Cast, 804584));
    for (int variant = 0; variant < 4; ++variant)
    {
        auto e = f.Crit(1);
        if (variant == 0) { e.direct = false; }
        if (variant == 1) { e.triggered = true; }
        if (variant == 2) { e.effectiveAmount = 0; }
        if (variant == 3) { e.target = f.context.actor; e.targetPolicy = Target::Self; }
        f.Apply(e);
        CHECK_EQ(f.AuraFor(807440)->charges, 10);
    }
    auto hit = f.Crit(1);
    hit.critical = false;
    f.Apply(hit); // Native text says abilities, not only critical strikes. Wrong Vow still costs.
    CHECK_EQ(f.AuraFor(807440)->charges, 9);
    f.context.nowMs = 999;
    f.Apply(f.Crit(2));
    CHECK_EQ(f.AuraFor(807440)->charges, 9);
    f.context.nowMs = 1000;
    auto heal = f.Crit(3);
    heal.damage = false;
    heal.healing = true;
    heal.target = f.friendly;
    heal.targetPolicy = Target::Friendly;
    CHECK_EQ(f.Apply(heal).Casts().size, 0u);
    CHECK_EQ(f.AuraFor(807440)->charges, 8);
    f.context.nowMs = 2000;
    f.context.activeVow = 807749; f.context.weaponMask = 3;
    f.known.insert(807749); f.Approve(807749);
    auto bonus = f.Crit(4); bonus.nativeEventMask = 0x10;
    f.missing.insert(506824);
    auto rejected = Evaluate(f.context, f.state, bonus);
    CHECK(rejected.Result() == Status::Failed);
    CHECK_EQ(rejected.Deltas().size, 0u);
    CHECK_EQ(f.AuraFor(807440)->charges, 8);
    f.missing.clear();
    auto p = f.Apply(bonus);
    REQUIRE(p.Casts().size == 2);
    CHECK(p.Casts().values[0].kind == IntentKind::Fulfillment);
    CHECK(p.Casts().values[0].target == f.enemy);
    CHECK_EQ(p.Casts().values[0].spell, 506824u);
    CHECK_EQ(p.Casts().values[1].spell, 506823u);
    CHECK_EQ(p.Casts().values[0].payload.basePoints[0].value(), 100);
    CHECK_EQ(f.AuraFor(807440)->charges, 7);
    f.context.nowMs = 3000;
    bonus.sequence = f.state.lastEvent + 1;
    f.Apply(bonus);
    CHECK_EQ(f.AuraFor(807440)->charges, 7); // One charge per cast across multiple targets.
    for (uint64_t cast = 5; cast < 12; ++cast)
    {
        f.context.nowMs += 1000;
        f.Apply(f.Crit(cast));
    }
    CHECK_EQ(f.Value(807440), 0);
    f.Apply(f.Gain(500149, 1));
    CHECK_EQ(f.Value(500149), 1);
}

TEST(CoaCombat_heat_embers_remainder_cap_and_direct_crit_no_icd)
{
    Fixture f(24);
    f.Set(807389, 99);
    f.Apply(f.Crit(1));
    CHECK_EQ(f.Value(807389), 19);
    CHECK_EQ(f.Value(807533), 1);
    f.context.nowMs = 1;
    f.Apply(f.Crit(2));
    CHECK_EQ(f.Value(807389), 39); // 5s maintenance period is NOT a Heat ICD.
    f.Apply(f.Crit(2));
    CHECK_EQ(f.Value(807389), 39);
    f.Set(807389, 99);
    f.Set(807533, 4);
    f.Apply(f.Crit(3));
    CHECK_EQ(f.Value(807389), 19);
    CHECK_EQ(f.Value(807533), 5);
    f.Set(807389, 99);
    f.Apply(f.Crit(4));
    CHECK_EQ(f.Value(807389), 19);
    CHECK_EQ(f.Value(807533), 5);
    f.Apply(f.Gain(807389, 10000));
    CHECK_EQ(f.Value(807389), 19);
    CHECK_EQ(f.Value(807533), 5);
    auto over = Evaluate(f.context, f.state, f.Gain(807389, std::numeric_limits<int32_t>::max()));
    CHECK(over.Result() == Status::Invalid);
    CHECK_EQ(over.Deltas().size, 0u);
}

TEST(CoaCombat_heat_filters_marker_ownership_school_agnostic_and_no_recursion)
{
    Fixture f(24);
    for (int variant = 0; variant < 6; ++variant)
    {
        auto e = f.Crit(uint64_t(variant + 1));
        if (variant == 0) { e.critical = false; }
        if (variant == 1) { e.direct = false; }
        if (variant == 2) { e.triggered = true; }
        if (variant == 3) { e.damage = false; e.healing = true; }
        if (variant == 4) { e.effectiveAmount = 0; }
        if (variant == 5) { f.known.erase(300755); }
        f.Apply(e);
        CHECK_EQ(f.Value(807389), 0);
    }
    f.known.insert(300755);
    auto pet = f.Crit(7);
    pet.source = f.enemy;
    CHECK(Evaluate(f.context, f.state, pet).Result() == Status::Unauthorized);
    f.known.erase(1001);
    f.Apply(f.Crit(8));
    CHECK_EQ(f.Value(807389), 0);
    f.known.insert(1001);
    f.Apply(f.Crit(9)); // No event school/family restriction is invented.
    CHECK_EQ(f.Value(807389), 20);
    f.known.erase(300755);
    f.Apply(f.Input(EventKind::Refresh));
    CHECK_EQ(f.Value(807389), 0);
    f.context.playerClass = 23;
    f.Approve(807391, {807389});
    CHECK(Evaluate(f.context, f.state, f.Cast(807391, f.Slot(0, 175, 807389))).Result() == Status::Unauthorized);
}

TEST(CoaCombat_out_of_order_cast_completion_and_bounded_replay_window)
{
    Fixture f(24);
    f.Apply(f.Crit(2));
    f.Apply(f.Crit(1));
    CHECK_EQ(f.Value(807389), 40);
    f.Apply(f.Crit(2));
    f.Apply(f.Crit(1));
    CHECK_EQ(f.Value(807389), 40);
    f.Apply(f.Crit(100));
    f.Apply(f.Crit(36)); // Outside the 64-cast window is stale, not a new proc.
    CHECK_EQ(f.Value(807389), 60);
    f.Apply(f.Crit(37));
    CHECK_EQ(f.Value(807389), 80);
}

TEST(CoaCombat_crit_rating_absolute_replacement_maintenance_unequip_respec)
{
    Fixture f(24);
    f.context.criticalStrikeRating = 123;
    auto p = f.Apply(f.Input(EventKind::Refresh));
    CHECK_EQ(p.Intellect().replacement, 123);
    CHECK_EQ(p.Intellect().before, 0);
    f.context.nowMs = 4999;
    f.context.criticalStrikeRating = 200;
    f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.state.intellect, 123);
    f.context.nowMs = 5000;
    p = f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(p.Intellect().before, 123);
    CHECK_EQ(p.Intellect().replacement, 200);
    f.context.nowMs = 10000;
    f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.state.intellect, 200);
    f.context.criticalStrikeRating = 7;
    f.Apply(f.Input(EventKind::Refresh));
    CHECK_EQ(f.state.intellect, 7);
    f.known.erase(300755);
    p = f.Apply(f.Input(EventKind::Refresh));
    CHECK_EQ(p.Intellect().before, 7);
    CHECK_EQ(p.Intellect().replacement, 0);
}

TEST(CoaCombat_insanity_decay_partition_transition_and_no_double_tick)
{
    Fixture f(25);
    f.Set(500706, 20);
    f.context.nowMs = 9999;
    f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.Value(500706), 20);
    CHECK_EQ(f.state.decayRemainderMs, 9999u);
    f.context.nowMs = 10000;
    f.Apply(f.Cast(500728, f.Slot(0, 175, 500706, 3)));
    CHECK_EQ(f.Value(500706), 18);
    f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.Value(500706), 18);
    f.context.nowMs = 19000;
    f.Apply(f.Input(EventKind::Tick));
    f.context.nowMs = 19500;
    f.context.inCombat = true;
    f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.state.decayRemainderMs, 0u);
    f.context.nowMs = 29500;
    f.Apply(f.Cast(500728, f.Slot(0, 175, 500706)));
    CHECK_EQ(f.Value(500706), 18);
    f.context.inCombat = false;
    f.Apply(f.Input(EventKind::Tick));
    f.context.nowMs = 39500;
    f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.Value(500706), 16);
    Fixture whole(25), split(25);
    whole.Set(500706, 50);
    split.Set(500706, 50);
    whole.context.nowMs = 35000;
    whole.Apply(whole.Input(EventKind::Tick));
    for (int i = 0; i < 7; ++i)
    {
        split.context.nowMs += 5000;
        split.Apply(split.Input(EventKind::Tick));
    }
    CHECK_EQ(whole.Value(500706), 44);
    CHECK_EQ(split.Value(500706), 44);
    CHECK_EQ(split.state.decayRemainderMs, 5000u);
    Fixture fresh(25);
    fresh.context.nowMs = 30000;
    fresh.Apply(fresh.Gain(500706, 10));
    CHECK_EQ(fresh.Value(500706), 10); // Elapsed decay cannot eat a new gain.
}

TEST(CoaCombat_total_madness_threshold_no_spend_end_clear_and_herald)
{
    Fixture f(25);
    f.context.inCombat = true;
    f.Set(500706, 99);
    f.Apply(f.Gain(500706, 1));
    CHECK_EQ(f.Value(500706), 100);
    CHECK_EQ(f.Value(803061), 1);
    REQUIRE(f.AuraFor(803061));
    CHECK_EQ(f.AuraFor(803061)->expiresMs, 10000u);
    f.Apply(f.Gain(500706, 40, true));
    CHECK_EQ(f.Value(500706), 100);
    f.context.nowMs = 10000;
    f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.Value(500706), 0);
    CHECK_EQ(f.Value(803061), 0);
    f.known.insert(805120);
    f.Apply(f.Gain(500706, 100));
    CHECK_EQ(f.Value(803061), 0);
    f.known.erase(805120);
    f.Apply(f.Input(EventKind::Refresh));
    CHECK_EQ(f.Value(803061), 1);
    auto remove = f.Input(EventKind::RemoveAura);
    remove.resource = 803061;
    f.Apply(remove);
    CHECK_EQ(f.Value(500706), 0);
    CHECK_EQ(f.Value(803061), 0);
}

TEST(CoaCombat_reaper_five_for_one_overflow_and_separate_spending)
{
    Fixture f(30);
    f.Apply(f.Gain(805077, 4));
    CHECK_EQ(f.Value(805077), 4);
    CHECK_EQ(f.Value(500363), 0);
    f.Apply(f.Gain(805077, 1));
    CHECK_EQ(f.Value(805077), 0);
    CHECK_EQ(f.Value(500363), 1);
    f.Apply(f.Gain(805077, 12));
    CHECK_EQ(f.Value(805077), 2);
    CHECK_EQ(f.Value(500363), 3);
    CHECK_EQ(f.Value(803031), 1);
    f.Apply(f.Gain(805077, 5));
    CHECK_EQ(f.Value(805077), 2);
    CHECK_EQ(f.Value(500363), 3);
    f.Apply(f.Gain(500363, 1, true));
    CHECK_EQ(f.Value(500363), 2);
    CHECK_EQ(f.Value(805077), 2);
    CHECK_EQ(f.Value(803031), 0);
    f.Approve(500363, {500363, 805077});
    auto conversion = f.Cast(500363, f.Slot(2, 175, 805077));
    CHECK(Evaluate(f.context, f.state, conversion).Result() == Status::Insufficient);
    f.Set(805077, 5);
    auto p = f.Apply(conversion);
    CHECK_EQ(f.Value(805077), 0);
    CHECK_EQ(f.Value(500363), 3);
    CHECK_EQ(p.SuppressSlots(), 7u);
    f.Approve(805077, {805077, 500363});
    f.Set(805077, 5);
    f.Apply(f.Cast(805077, f.Slot(1, 183, 805078, 100, true)));
    CHECK_EQ(f.Value(805077), 0);
    CHECK_EQ(f.state.pending.size, 0u);
}

TEST(CoaCombat_explicit_175_exceptions_electrocute_and_pair_suppression)
{
    Fixture storm(16);
    for (uint32_t spell : {501421u, 501432u, 801844u})
    {
        storm.Approve(spell, {803102});
        auto p = storm.Apply(storm.Cast(spell, storm.Slot(2, 175, 804084, 0, true)));
        CHECK_EQ(p.Casts().size, 0u);
    }
    CHECK_EQ(storm.Value(803102), 60);
    Fixture fel(14);
    fel.Approve(801024, {800058});
    fel.Apply(fel.Cast(801024, fel.Slot(0, 175, 800058, -1)));
    CHECK_EQ(fel.Value(800058), 2);
    fel.Approve(805239, {800058});
    auto p = fel.Apply(fel.Cast(805239, fel.Slot(1, 175, 800058)));
    CHECK_EQ(fel.Value(800058), 6);
    CHECK_EQ(p.SuppressSlots(), 6u);
    fel.Approve(807425, {800058});
    p = fel.Apply(fel.Cast(807425, fel.Slot(0, 183, 800058, 2)));
    CHECK_EQ(fel.Value(800058), 4);
    CHECK_EQ(fel.state.pending.size, 0u);
    Fixture scrap(28);
    scrap.Approve(707474, {801816});
    scrap.Set(801816, 4);
    auto spend = scrap.Cast(707474, scrap.Slot(0, 175, 801816));
    CHECK(Evaluate(scrap.context, scrap.state, spend).Result() == Status::Insufficient);
    scrap.Set(801816, 8);
    scrap.Apply(spend);
    CHECK_EQ(scrap.Value(801816), 3);
    Fixture ranger(21);
    ranger.Approve(520791, {804329});
    p = ranger.Apply(ranger.Cast(520791, ranger.Slot(1, 175, 804329)));
    CHECK_EQ(ranger.Value(804329), 5);
    CHECK_EQ(p.SuppressSlots(), 3u);
}

TEST(CoaCombat_rejected_cast_has_no_deltas_child_or_partial_commit)
{
    Fixture f(16);
    f.Approve(567555, {803102});
    f.Set(803102, 19);
    auto e = f.Cast(567555, f.Slot(2, 175, 803102));
    auto p = Evaluate(f.context, f.state, e);
    CHECK(p.Result() == Status::Insufficient);
    CHECK_EQ(p.Deltas().size, 0u);
    CHECK_EQ(p.Casts().size, 0u);
    CHECK(Commit(f.context, p, true, f.state) == Status::Insufficient);
    CHECK_EQ(f.Value(803102), 19);
    f.Set(803102, 20);
    e.successful = false;
    CHECK(Evaluate(f.context, f.state, e).Result() == Status::Failed);
    e.successful = true;
    p = Evaluate(f.context, f.state, e);
    REQUIRE(p.Result() == Status::Ready);
    CHECK(Commit(f.context, p, false, f.state) == Status::Failed);
    CHECK_EQ(f.Value(803102), 20);
    CHECK(Commit(f.context, p, true, f.state) == Status::Ready);
    CHECK_EQ(f.Value(803102), 0);
    CHECK(Commit(f.context, p, true, f.state) == Status::Stale);
    CHECK(Evaluate(f.context, f.state, e).Result() == Status::Invalid);
}

TEST(CoaCombat_delay_preserves_identity_payload_clock_and_equal_time_order)
{
    Fixture f(30);
    f.Approve(500483);
    auto e = f.Cast(500483, f.Slot(2, 183, 500481, 50));
    e.effects.Push(f.Slot(1, 183, 500481, 50));
    e.effects.values[0].payload.basePoints = {7, -9, 0};
    e.effects.values[0].payload.effectMask = {1, 0x80000000u, 3};
    e.effects.values[0].payload.familyMask = {8, 9, 10};
    e.effects.values[0].payload.procMask = 64;
    e.effects.values[0].payload.nativeFlags = 524287;
    auto p = f.Apply(e);
    CHECK_EQ(p.Casts().size, 0u);
    CHECK_EQ(f.state.pending.size, 2u);
    f.context.nowMs = 49;
    p = f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(p.Casts().size, 0u);
    f.context.nowMs = 50;
    p = f.Apply(f.Input(EventKind::Tick));
    REQUIRE(p.Casts().size == 2);
    auto const& a = p.Casts().values[0];
    auto const& b = p.Casts().values[1];
    CHECK_EQ(a.slot, 1u);
    CHECK_EQ(b.slot, 2u);
    CHECK(a.sequence < b.sequence);
    CHECK(b.source == f.context.actor && b.originalCaster == f.context.actor);
    CHECK(b.target == f.context.actor);
    CHECK(b.payload.basePoints == e.effects.values[0].payload.basePoints);
    CHECK(b.payload.effectMask == e.effects.values[0].payload.effectMask);
    CHECK(b.payload.familyMask == e.effects.values[0].payload.familyMask);
    CHECK_EQ(b.payload.procMask, 64u);
    CHECK_EQ(b.payload.nativeFlags, 524287);
    CHECK_EQ(f.state.pending.size, 0u);
    p = f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(p.Casts().size, 0u);
}

TEST(CoaCombat_delay_fire_cancels_dead_missing_range_unlearn_generation_logout)
{
    for (int variant = 0; variant < 8; ++variant)
    {
        Fixture f(31);
        f.Approve(560155);
        f.Apply(f.Cast(560155, f.Slot(0, 183, 680448, 500, true)));
        if (variant == 0) { f.targetExists = false; }
        if (variant == 1) { f.targetAlive = false; }
        if (variant == 2) { f.targetRange = false; }
        if (variant == 3) { f.edges.erase({560155, 0}); }
        if (variant == 4) { ++f.context.actor.generation; }
        if (variant == 5) { f.context.inWorld = false; }
        if (variant == 6) { f.context.alive = false; }
        if (variant == 7) { ++f.targetGeneration; }
        f.context.nowMs = 700; // Late delivery is not permission to resurrect an old job.
        auto p = f.Apply(f.Input(EventKind::Tick));
        CHECK_EQ(p.Casts().size, 0u);
        CHECK_EQ(f.state.pending.size, 0u);
    }
    Fixture f(31);
    f.Approve(560155);
    f.Apply(f.Cast(560155, f.Slot(0, 183, 680448, 500, true)));
    f.targetRange = false;
    f.context.nowMs = 250;
    f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.state.pending.size, 1u); // Range matters at fire, not during travel.
    f.targetRange = true;
    f.context.nowMs = 500;
    CHECK_EQ(f.Apply(f.Input(EventKind::Tick)).Casts().size, 1u);
}

TEST(CoaCombat_delay_limits_bad_targets_and_recursive_chain_guard)
{
    Fixture f(31);
    f.Approve(560155);
    for (int32_t delay : {0, -1, 60001, std::numeric_limits<int32_t>::max()})
    {
        CHECK(Evaluate(f.context, f.state, f.Cast(560155, f.Slot(0, 183, 680448, delay, true))).Result() == Status::Invalid);
    }
    auto wrong = f.Cast(560155, f.Slot(0, 183, 680448, 100, true));
    wrong.effects.values[0].target.instance++;
    CHECK(Evaluate(f.context, f.state, wrong).Result() == Status::Invalid);
    for (unsigned i = 0; i < 32; ++i)
    {
        f.Apply(f.Cast(560155, f.Slot(0, 183, 680448, 60000, true)));
    }
    auto p = Evaluate(f.context, f.state, f.Cast(560155, f.Slot(0, 183, 680448, 100, true)));
    CHECK(p.Result() == Status::Limit);
    CHECK_EQ(p.Deltas().size, 0u);
    f.Apply(f.Input(EventKind::Invalidate));
    auto recurse = f.Cast(560155, f.Slot(0, 183, 680448, 100, true));
    recurse.ruleDepth = 1;
    recurse.ancestors[0] = 560155;
    CHECK(Evaluate(f.context, f.state, recurse).Result() == Status::Recursive);
    recurse.ancestors[0] = 680448;
    CHECK(Evaluate(f.context, f.state, recurse).Result() == Status::Recursive);
    recurse.ruleDepth = std::numeric_limits<uint32_t>::max();
    CHECK(Evaluate(f.context, f.state, recurse).Result() == Status::Recursive);
}

TEST(CoaCombat_duration_cast_paired_override_summon_and_numeric_four_seconds)
{
    Fixture earth(32);
    earth.Approve(560233);
    auto e = earth.Cast(560233, earth.Slot(1, 178, 520417, 8000, true));
    CHECK(Evaluate(earth.context, earth.state, e).Result() == Status::Invalid);
    e.effects.values[0].payload.basePoints = {-30, -30, -30};
    auto p = earth.Apply(e);
    REQUIRE(p.Casts().size == 1);
    CHECK(p.Casts().values[0].kind == IntentKind::DurationCast);
    CHECK_EQ(p.Casts().values[0].durationMs, 8000u);
    CHECK_EQ(p.Casts().values[0].payload.basePoints[0].value(), -30);
    CHECK_EQ(p.SuppressSlots(), 3u);
    Fixture summon(16);
    summon.Approve(573451);
    p = summon.Apply(summon.Cast(573451, summon.Slot(0, 178, 573438, 20000, true)));
    REQUIRE(p.Casts().size == 1);
    CHECK(p.Casts().values[0].kind == IntentKind::SummonDuration);
    CHECK_EQ(p.Casts().values[0].durationMs, 20000u);
    Fixture solar(27);
    solar.Approve(712448);
    p = solar.Apply(solar.Cast(712448, solar.Slot(0, 178, 804751, 4000)));
    CHECK_EQ(p.Casts().values[0].durationMs, 4000u);
    Fixture fire(24);
    fire.Approve(538442);
    p = fire.Apply(fire.Cast(538442, fire.Slot(0, 178, 800103, 5000)));
    CHECK_EQ(p.SuppressSlots(), 3u); // Paired177 is coalesced, not run twice.
}

TEST(CoaCombat_delay_resource_reward_does_not_consume_fragments)
{
    Fixture f(30);
    f.Approve(680338, {500363});
    f.Set(805077, 2);
    f.Apply(f.Cast(680338, f.Slot(0, 183, 500363, 150)));
    CHECK_EQ(f.Value(500363), 0);
    f.context.nowMs = 150;
    auto p = f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.Value(500363), 1);
    CHECK_EQ(f.Value(805077), 2);
    CHECK_EQ(p.Casts().size, 0u);
}

TEST(CoaCombat_delayed_resource_requires_edge_before_cast_and_cancels_lost_edge)
{
    Fixture f(30);
    f.Approve(680338); // Knowing a parent is not an arbitrary resource grant edge.
    auto e = f.Cast(680338, f.Slot(0, 183, 500363, 150));
    CHECK(Evaluate(f.context, f.state, e).Result() == Status::Unauthorized);
    f.Approve(680338, {500363});
    f.Apply(e);
    f.edges.erase({680338, 500363});
    f.context.nowMs = 150;
    auto p = f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.state.pending.size, 0u);
    CHECK_EQ(f.Value(500363), 0);
    CHECK_EQ(p.Casts().size, 0u);
}

TEST(CoaCombat_cap_modifier_is_not_delta_idempotent_and_unlearn_reverses)
{
    Fixture f(25);
    f.Approve(807693, {804670, 804711});
    auto e = f.Cast(807693, f.Slot(0, 175, 804670));
    e.effects.values[0].target = f.friendly;
    e.effects.values[0].targetPolicy = Target::Friendly;
    CHECK(Evaluate(f.context, f.state, e).Result() == Status::Invalid);
    e.kind = EventKind::CapModifier;
    f.Apply(e);
    CHECK_EQ(Count(f.state, 804670, f.friendly), 0);
    REQUIRE(f.AuraFor(804670));
    CHECK_EQ(f.AuraFor(804670)->capBonus, 1);
    e.sequence = f.state.lastEvent + 1;
    f.Apply(e);
    CHECK_EQ(f.AuraFor(804670)->capBonus, 1);
    auto gain = f.Gain(804670, 20);
    gain.target = f.friendly;
    gain.targetPolicy = Target::Friendly;
    f.Apply(gain);
    CHECK_EQ(Count(f.state, 804670, f.friendly), 6);
    f.known.erase(807693);
    f.Apply(f.Input(EventKind::Refresh));
    CHECK_EQ(Count(f.state, 804670, f.friendly), 5);
    CHECK_EQ(f.AuraFor(804670)->capBonus, 0);
}

TEST(CoaCombat_target_stacks_zero_modes_and_nonrefresh_duration)
{
    Fixture necro(23);
    necro.Approve(570132, {570131});
    auto p = necro.Apply(necro.Cast(570132, necro.Slot(0, 175, 570131, 0, true)));
    CHECK_EQ(Count(necro.state, 570131, necro.enemy), 1);
    CHECK_EQ(p.Casts().size, 0u);
    Fixture earth(31);
    earth.Approve(681072, {680441});
    earth.Apply(earth.Cast(681072, earth.Slot(0, 175, 680441, 2)));
    REQUIRE(earth.AuraFor(680441));
    CHECK_EQ(earth.AuraFor(680441)->expiresMs, 20000u);
    earth.context.nowMs = 19000;
    earth.Apply(earth.Cast(681072, earth.Slot(0, 175, 680441, 2)));
    CHECK_EQ(earth.Value(680441), 4);
    CHECK_EQ(earth.AuraFor(680441)->expiresMs, 20000u);
    earth.context.nowMs = 20000;
    earth.Apply(earth.Input(EventKind::Tick));
    CHECK_EQ(earth.Value(680441), 0);
    earth.Apply(earth.Cast(681072, earth.Slot(0, 175, 680441, 2)));
    CHECK_EQ(earth.AuraFor(680441)->expiresMs, 40000u);
    Fixture fire(24);
    fire.Approve(572381, {804301});
    p = fire.Apply(fire.Cast(572381, fire.Slot(0, 175, 804301, 5)));
    CHECK_EQ(fire.Value(804301), 5);
    CHECK_EQ(p.SuppressSlots(), 3u);
    fire.context.nowMs = 9000;
    fire.Apply(fire.Cast(572381, fire.Slot(0, 175, 804301, 5)));
    CHECK_EQ(fire.AuraFor(804301)->expiresMs, 10000u);
}

TEST(CoaCombat_unknown_bindings_missing_child_foreign_aura_and_malformed_state)
{
    Fixture f(27);
    f.Approve(804098, {500149});
    auto e = f.Cast(804098, f.Slot(1, 175, 500149));
    f.missing.insert(500149);
    CHECK(Evaluate(f.context, f.state, e).Result() == Status::Failed);
    f.missing.clear();
    f.known.erase(804584);
    CHECK(Evaluate(f.context, f.state, e).Result() == Status::Unauthorized);
    f.known.insert(804584);
    e.effects.values[0].child = 123;
    CHECK(Evaluate(f.context, f.state, e).Result() == Status::Invalid);
    e.effects.values[0].type = 431;
    CHECK(Evaluate(f.context, f.state, e).Result() == Status::Unsupported);
    CHECK(!FindBinding(999999, 0, 175));
    REQUIRE(FindBinding(806039, 1, 178));
    CHECK(FindBinding(806039, 1, 178)->operation == Operation::DurationAtDestination);
    f.Set(500149, 5);
    f.state.auras.values[0].caster = f.enemy;
    CHECK(Evaluate(f.context, f.state, f.Input(EventKind::Tick)).Result() == Status::Invalid);
    f.state.auras.values[0].caster = f.context.actor;
    f.state.auras.size = MaxAuras + 1;
    CHECK(Evaluate(f.context, f.state, f.Input(EventKind::Tick)).Result() == Status::Invalid);
    f.state.auras.size = 1;
    f.state.pending.size = MaxPending + 1;
    CHECK(Evaluate(f.context, f.state, f.Input(EventKind::Tick)).Result() == Status::Invalid);
    f.state.pending.size = 0;
    e.effects.size = 4;
    CHECK(Evaluate(f.context, f.state, e).Result() == Status::Invalid);
}

TEST(CoaCombat_commit_rechecks_unlearn_target_and_clock_without_side_effects)
{
    Fixture f(31);
    f.Approve(574160);
    auto e = f.Cast(574160, f.Slot(1, 178, 573070, 10000, true));
    auto p = Evaluate(f.context, f.state, e);
    REQUIRE(p.Result() == Status::Ready);
    f.edges.erase({574160, 0});
    CHECK(Commit(f.context, p, true, f.state) == Status::Unauthorized);
    f.Approve(574160);
    f.targetExists = false;
    CHECK(Commit(f.context, p, true, f.state) == Status::Invalid);
    f.targetExists = true;
    ++f.context.nowMs;
    CHECK(Commit(f.context, p, true, f.state) == Status::Stale);
    CHECK_EQ(f.state.revision, 0u);
}

TEST(CoaCombat_expired_target_slots_reused_and_finite_duration_required)
{
    Fixture f(23);
    f.Approve(570132, {570131});
    for (uint64_t i = 0; i < MaxAuras; ++i)
    {
        CHECK(f.state.auras.Push({570131, {1000 + i, 7, 1}, f.context.actor, 1, 0, 0, 10}));
    }
    CHECK(Evaluate(f.context, f.state, f.Cast(570132, f.Slot(0, 175, 570131, 1, true))).Result() == Status::Limit);
    f.context.nowMs = 10;
    auto p = f.Apply(f.Cast(570132, f.Slot(0, 175, 570131, 1, true)));
    CHECK_EQ(f.state.auras.size, 1u);
    CHECK_EQ(p.Deltas().size, 65u);
    Fixture earth(31);
    earth.Approve(681072, {680441});
    earth.durations[680441] = 0;
    CHECK(Evaluate(earth.context, earth.state, earth.Cast(681072, earth.Slot(0, 175, 680441, 2))).Result() == Status::Invalid);
    Fixture solar(27);
    solar.Approve(712448);
    CHECK(Evaluate(solar.context, solar.state, solar.Cast(712448, solar.Slot(0, 178, 804751, -1))).Result() == Status::Invalid);
}

TEST(CoaCombat_lifecycle_clears_resources_markers_jobs_stats_not_an_automatic_grant)
{
    Fixture f(27);
    f.Approve(804584, {500149});
    f.Set(500149, 20);
    f.Apply(f.Input(EventKind::Refresh));
    CHECK_EQ(f.Value(704396), 1);
    f.known.erase(804584);
    auto p = f.Apply(f.Input(EventKind::Refresh));
    CHECK_EQ(f.Value(500149), 0);
    CHECK_EQ(f.Value(704396), 0);
    CHECK(p.Deltas().size > 0);
    f.known.insert(804584);
    f.Set(500149, 20);
    f.Apply(f.Input(EventKind::Cast, 804584));
    f.context.alive = false;
    f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.Value(807440), 0);
    CHECK_EQ(f.state.auras.size, 0u);
    f.context.alive = true;
    ++f.context.actor.generation;
    f.Apply(f.Input(EventKind::Refresh));
    CHECK_EQ(f.Value(500149), 0);
    CHECK_EQ(f.Value(807440), 0);
}

TEST(CoaCombat_review_remove_matches_holder_caster_not_dispeller)
{
    Fixture f(23);
    f.Approve(570132, {570131});
    f.Apply(f.Cast(570132, f.Slot(0, 175, 570131, 1, true)));
    auto remove = f.Input(EventKind::RemoveAura);
    remove.resource = 570131; remove.target = f.enemy;
    remove.source = remove.originalCaster = f.friendly;
    auto foreign = Evaluate(f.context, f.state, remove);
    CHECK(foreign.Result() == Status::Unauthorized);
    CHECK_EQ(foreign.Deltas().size, 0u);
    CHECK_EQ(Count(foreign.Preview(), 570131, f.enemy), 1);
    remove.originalCaster = f.context.actor;
    f.Apply(remove); // A foreign dispeller may remove THIS actor's actual holder.
    CHECK_EQ(Count(f.state, 570131, f.enemy), 0);
}

TEST(CoaCombat_review_dead_cap_only_identity_retires_and_reuses_all_slots)
{
    Fixture f(25);
    f.Approve(807693, {804670});
    for (uint64_t id = 1000; id < 1000 + MaxAuras; ++id)
    {
        auto e = f.Cast(807693, f.Slot(0, 175, 804670, 1, true));
        e.kind = EventKind::CapModifier;
        e.effects.values[0].target.guid = id;
        f.Apply(e);
    }
    f.targetAlive = false;
    f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.state.auras.size, 0u);
    f.targetAlive = true;
    auto next = f.Cast(807693, f.Slot(0, 175, 804670, 1, true));
    next.kind = EventKind::CapModifier;
    f.Apply(next);
    CHECK_EQ(f.state.auras.size, 1u);
}

TEST(CoaCombat_review_combat_edge_settles_whole_ticks_then_discards_fraction)
{
    for (uint64_t time : {10000u, 10500u, 30500u})
    {
        Fixture whole(25), split(25);
        whole.Set(500706, 20); split.Set(500706, 20);
        whole.context.nowMs = split.context.nowMs = time;
        whole.context.inCombat = true;
        whole.Apply(whole.Input(EventKind::Tick));
        split.Apply(split.Input(EventKind::Tick));
        split.context.inCombat = true;
        split.Apply(split.Input(EventKind::Tick));
        CHECK_EQ(whole.Value(500706), 20 - int32_t(time / 10000) * 2);
        CHECK_EQ(whole.Value(500706), split.Value(500706));
        CHECK_EQ(whole.state.decayRemainderMs, 0u);
        whole.context.nowMs += 30000;
        whole.context.inCombat = false;
        whole.Apply(whole.Input(EventKind::Tick));
        CHECK_EQ(whole.Value(500706), split.Value(500706));
    }
}

TEST(CoaCombat_review_partial_paired_overrides_reject_before_suppression)
{
    Fixture f(32); f.Approve(560233);
    for (unsigned mask = 0; mask < 7; ++mask)
    {
        auto e = f.Cast(560233, f.Slot(1, 178, 520417, 8000, true));
        for (unsigned slot = 0; slot < 3; ++slot)
        {
            if (mask & (1u << slot)) { e.effects.values[0].payload.basePoints[slot] = -30; }
        }
        auto p = Evaluate(f.context, f.state, e);
        CHECK(p.Result() == Status::Invalid);
        CHECK_EQ(p.Casts().size, 0u); CHECK_EQ(p.SuppressSlots(), 0u);
    }
}

TEST(CoaCombat_review_cast_reaps_cancelled_queue_before_capacity_without_early_fire)
{
    Fixture f(31); f.Approve(560155); f.Approve(573215, {806068});
    for (size_t i = 0; i < MaxPending; ++i)
    {
        f.Apply(f.Cast(560155, f.Slot(0, 183, 680448, 500, true)));
    }
    f.targetAlive = false; f.context.nowMs = 100;
    auto e = f.Cast(573215, f.Slot(1, 183, 680472, 100));
    e.effects.Push(f.Slot(0, 175, 806068));
    auto p = f.Apply(e);
    CHECK_EQ(p.Casts().size, 0u); CHECK_EQ(f.state.pending.size, 1u);
    CHECK_EQ(f.Value(806068), 2);
    f.context.nowMs = 200;
    CHECK_EQ(f.Apply(f.Input(EventKind::Tick)).Casts().size, 1u);
    CHECK_EQ(f.Apply(f.Input(EventKind::Tick)).Casts().size, 0u);
}

TEST(CoaCombat_vow_requires_owned_active_native_melee_event_and_real_weapons)
{
    for (unsigned weapons = 0; weapons <= 3; ++weapons)
    {
        Fixture f(27); f.Approve(804584, {500149}); f.Approve(807749);
        f.known.insert(807749); f.context.activeVow = 807749; f.context.weaponMask = weapons;
        f.Set(500149, 20); f.Apply(f.Input(EventKind::Cast, 804584));
        auto wrong = f.Crit(1); wrong.nativeEventMask = 0x100;
        CHECK_EQ(f.Apply(wrong).Casts().size, 0u);
        CHECK_EQ(f.AuraFor(807440)->charges, 9);
        f.context.nowMs = 1000;
        auto melee = f.Crit(2); melee.nativeEventMask = 0x10; melee.critical = false;
        auto p = f.Apply(melee);
        CHECK_EQ(p.Casts().size, (weapons & 1) + ((weapons >> 1) & 1));
        CHECK_EQ(f.AuraFor(807440)->charges, 8);
        for (size_t i = 0; i < p.Casts().size; ++i)
        {
            auto const& hit = p.Casts().values[i];
            CHECK(hit.source == f.context.actor && hit.originalCaster == f.context.actor && hit.target == f.enemy);
            CHECK_EQ(hit.payload.basePoints[0].value(), 100);
            CHECK_EQ(hit.parent, 807749u); CHECK_EQ(hit.ruleDepth, 2u);
        }
        f.context.nowMs = 2000; f.known.erase(807749);
        melee.sequence = f.state.lastEvent + 1; melee.castId = 3;
        CHECK_EQ(f.Apply(melee).Casts().size, 0u);
        CHECK_EQ(f.AuraFor(807440)->charges, 7);
    }
}

TEST(CoaCombat_completed_killing_hit_spends_charge_without_attacking_dead_target)
{
    Fixture f(27); f.Approve(804584, {500149}); f.Approve(807749); f.known.insert(807749);
    f.context.activeVow = 807749; f.context.weaponMask = 3;
    f.Set(500149, 20); f.Apply(f.Input(EventKind::Cast, 804584));
    auto e = f.Crit(1); e.nativeEventMask = 0x10; e.resolvedHit = true;
    f.targetAlive = false;
    CHECK_EQ(f.Apply(e).Casts().size, 0u);
    CHECK_EQ(f.AuraFor(807440)->charges, 9);
    Fixture fire(24);
    auto crit = fire.Crit(1); crit.resolvedHit = true; fire.targetAlive = false;
    fire.Apply(crit);
    CHECK_EQ(fire.Value(807389), 20);
}

TEST(CoaCombat_destination_bindings_fixed_frames_delays_and_duration)
{
    struct Row { uint32_t spell, slot, effect, cls, child, time; bool persistent; };
    for (auto row : {Row{806039,1,178,25,520418,8000,false}, {520186,1,183,22,520205,6000,false},
        {560248,1,183,29,520663,500,false}, {802631,0,183,32,803745,1000,true},
        {806415,0,183,31,807859,500,true}, {806415,2,183,31,807120,500,true}})
    {
        Fixture f(row.cls); f.Approve(row.spell);
        auto slot = f.Slot(row.slot, row.effect, row.child, int32_t(row.time));
        auto bad = Evaluate(f.context, f.state, f.Cast(row.spell, slot));
        CHECK(bad.Result() == Status::Invalid);
        Destination d; d.owner = f.context.actor; d.phase = 1; d.x = 3; d.y = 4; d.z = 5;
        d.mode = row.persistent ? DestinationMode::PersistentArea : DestinationMode::Recipients;
        if (!row.persistent) { d.recipients.Push(f.enemy); }
        slot.target = {}; slot.location = d;
        slot.payload.nativeFlags = row.spell == 806415 ? 5242870 : 0;
        auto p = f.Apply(f.Cast(row.spell, slot));
        if (row.effect == 183)
        {
            CHECK_EQ(p.Casts().size, 0u); CHECK_EQ(f.state.pending.size, 1u);
            f.context.nowMs = row.time;
            p = f.Apply(f.Input(EventKind::Tick));
        }
        REQUIRE(p.Casts().size == 1);
        auto const& result = std::get<Destination>(p.Casts().values[0].location);
        CHECK(result.owner == f.context.actor); CHECK_EQ(result.x, 3); CHECK_EQ(result.phase, 1u);
        CHECK_EQ(result.recipients.size, row.persistent ? 0u : 1u);
        if (row.effect == 178) { CHECK_EQ(p.Casts().values[0].durationMs, 8000u); }
    }
}

TEST(CoaCombat_destination_refuses_nan_cross_frame_duplicate_recipients_and_stale_jobs)
{
    Fixture f(31); f.Approve(806415);
    Destination d; d.owner = f.context.actor; d.phase = 1; d.mode = DestinationMode::PersistentArea;
    auto slot = f.Slot(0, 183, 807859, 500); slot.target = {};
    d.x = std::numeric_limits<float>::quiet_NaN(); slot.location = d;
    CHECK(Evaluate(f.context, f.state, f.Cast(806415, slot)).Result() == Status::Invalid);
    d.x = 0; ++d.owner.frame; slot.location = d;
    CHECK(Evaluate(f.context, f.state, f.Cast(806415, slot)).Result() == Status::Invalid);
    d.owner = f.context.actor; slot.location = d;
    f.Apply(f.Cast(806415, slot));
    f.destinationAllowed = false; f.context.nowMs = 100;
    CHECK_EQ(f.Apply(f.Input(EventKind::Tick)).Casts().size, 0u);
    CHECK_EQ(f.state.pending.size, 0u);
}

TEST(CoaCombat_lua_port_sun_granters_amounts_and_gavel_guard)
{
    // class_resources.lua Sun Cleric granters: 804097 +2 (Vow periodic
    // cover), Flash 500144 +1, Spears of Light 800232 +2, Gavel 800611 +1
    // only while Sunwalker's Grace 680313 is on the player.
    Fixture f(27);
    f.Approve(804097, {500149});
    f.Approve(500144, {500149});
    f.Approve(800232, {500149});
    f.Approve(800611, {500149});
    f.Apply(f.Input(EventKind::Cast, 804097));
    CHECK_EQ(f.Value(500149), 2);
    f.Apply(f.Input(EventKind::Cast, 500144));
    CHECK_EQ(f.Value(500149), 3);
    f.Apply(f.Input(EventKind::Cast, 800232));
    CHECK_EQ(f.Value(500149), 5);
    CHECK_EQ(f.Value(802938), 1);
    CHECK(Evaluate(f.context, f.state, f.Input(EventKind::Cast, 800611)).Result() == Status::Unauthorized);
    CHECK_EQ(f.Value(500149), 5);
    f.selfAuras.insert(680313);
    f.Apply(f.Input(EventKind::Cast, 800611));
    CHECK_EQ(f.Value(500149), 6);
    // Flash ranks carry the same +1.
    f.Approve(502358, {500149});
    f.Apply(f.Input(EventKind::Cast, 502358));
    CHECK_EQ(f.Value(500149), 7);
    // Wrong class is refused, not granted.
    f.context.playerClass = 16;
    CHECK(Evaluate(f.context, f.state, f.Input(EventKind::Cast, 500144)).Result() == Status::Unauthorized);
}

TEST(CoaCombat_lua_port_sun_gains_stop_under_dawn)
{
    // generate_sun: positive deltas are dropped while Dawn 807440 is up.
    Fixture f(27);
    f.Approve(804584, {500149});
    f.Approve(804097, {500149});
    f.Set(500149, 20);
    f.Apply(f.Input(EventKind::Cast, 804584));
    REQUIRE(f.AuraFor(807440));
    f.Apply(f.Input(EventKind::Cast, 804097));
    CHECK_EQ(f.Value(500149), 0);
}

TEST(CoaCombat_lua_port_shock_needs_storm_talent_and_thirst_needs_thirst)
{
    // Stormbringer Shock 804020 "Generates 20 Static" only when 500040 is
    // known; Bloodmage Bloodmoon Blast 500125 "+1 Thirst" only when 500107
    // is known or applied.
    Fixture f(16);
    f.Approve(804020, {803102});
    CHECK(Evaluate(f.context, f.state, f.Input(EventKind::Cast, 804020)).Result() == Status::Unauthorized);
    CHECK_EQ(f.Value(803102), 0);
    f.known.insert(500040);
    f.Apply(f.Input(EventKind::Cast, 804020));
    CHECK_EQ(f.Value(803102), 20);
    Fixture b(20);
    b.Approve(500125, {706613});
    CHECK(Evaluate(b.context, b.state, b.Input(EventKind::Cast, 500125)).Result() == Status::Unauthorized);
    b.known.insert(500107);
    b.Apply(b.Input(EventKind::Cast, 500125));
    CHECK_EQ(b.Value(706613), 1);
    Fixture c(20);
    c.Approve(500125, {706613});
    c.selfAuras.insert(500107);
    c.Apply(c.Input(EventKind::Cast, 500125));
    CHECK_EQ(c.Value(706613), 1);
}

TEST(CoaCombat_lua_port_flare_knight_ranger_cultist_reaper_amounts)
{
    // Pyromancer Flare Bolt 800790 +20 Heat; Knight Infernal Strike 801016
    // +2 Demonfire; Ranger Quick Shot 500074 +1 Advantage; Cultist Blade of
    // the Empire 500720 +20 Insanity; Reaper Reap 500357 +1 Soul Fragment.
    Fixture f(24);
    f.Approve(800790, {807389});
    f.Apply(f.Input(EventKind::Cast, 800790));
    CHECK_EQ(f.Value(807389), 20);
    Fixture k(17);
    k.Approve(801016, {500906});
    k.Apply(k.Input(EventKind::Cast, 801016));
    CHECK_EQ(k.Value(500906), 2);
    Fixture r(21);
    r.Approve(500074, {804329});
    r.Apply(r.Input(EventKind::Cast, 500074));
    CHECK_EQ(r.Value(804329), 1);
    Fixture u(25);
    u.Approve(500720, {500706});
    u.Apply(u.Input(EventKind::Cast, 500720));
    CHECK_EQ(u.Value(500706), 20);
    Fixture p(30);
    p.Approve(500357, {805077});
    p.Apply(p.Input(EventKind::Cast, 500357));
    CHECK_EQ(p.Value(805077), 1);
}

TEST(CoaCombat_lua_port_felsworn_demon_within_consumes_at_cap)
{
    // 800222 known + 6 Felfury -> orb consumed to 0, Inner Demon 804216
    // applied for 30,000 ms. Below cap, unknown requires, or buff already
    // up: no-op, and a further capping under the buff is left alone. The
    // transform is deferred one event so the capping grant publishes the
    // visible 6 first (the gate journals the wire max).
    Fixture f(14);
    f.Approve(1001, {800058});
    f.Approve(800058, {800058}); // At-cap consume: self-edge, like PolicyResourceEdge.
    f.Apply(f.Gain(800058, 5));
    CHECK_EQ(f.Value(800058), 5);
    CHECK(!f.AuraFor(804216));
    f.Apply(f.Gain(800058, 1));
    CHECK_EQ(f.Value(800058), 6); // Requires 800222 unknown: orb sits at cap.
    CHECK(!f.AuraFor(804216));
    f.known.insert(800222);
    f.Apply(f.Input(EventKind::Tick)); // Cap was already reached: transform.
    CHECK_EQ(f.Value(800058), 0);
    REQUIRE(f.AuraFor(804216));
    CHECK_EQ(f.AuraFor(804216)->stacks, 1);
    CHECK_EQ(f.AuraFor(804216)->expiresMs, f.context.nowMs + 30000);
    f.Apply(f.Gain(800058, 6)); // Cap reached this event: published first...
    CHECK_EQ(f.Value(800058), 6);
    CHECK_EQ(f.AuraFor(804216)->stacks, 1);
    f.Apply(f.Input(EventKind::Tick)); // ...then left alone: buff already up.
    CHECK_EQ(f.Value(800058), 6);
    CHECK_EQ(f.AuraFor(804216)->stacks, 1);
}

TEST(CoaCombat_lua_port_inner_demon_rearms_after_expiry)
{
    Fixture f(14);
    f.Approve(1001, {800058});
    f.Approve(800058, {800058}); // At-cap consume: self-edge, like PolicyResourceEdge.
    f.known.insert(800222);
    f.Apply(f.Gain(800058, 6));
    CHECK_EQ(f.Value(800058), 6); // Published first...
    f.Apply(f.Input(EventKind::Tick)); // ...consumed on the next event.
    CHECK_EQ(f.Value(800058), 0);
    REQUIRE(f.AuraFor(804216));
    f.context.nowMs += 30000;
    f.Apply(f.Input(EventKind::Tick));
    CHECK(!f.AuraFor(804216));
    f.Apply(f.Gain(800058, 6));
    CHECK_EQ(f.Value(800058), 6);
    f.Apply(f.Input(EventKind::Tick));
    CHECK_EQ(f.Value(800058), 0);
    REQUIRE(f.AuraFor(804216));
}
