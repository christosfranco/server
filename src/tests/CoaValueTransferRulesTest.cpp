// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "CoaValueTransferRules.h"
#include <map>
#include <set>
#include <limits>

using namespace coa::transfer;

namespace
{
    struct Fixture
    {
        Identity owner{100, 7, 1, 1}, enemy{200, 7, 1, 1}, ally{300, 7, 1, 1};
        Identity pet{400, 7, 1, 1};
        Context context;
        State state;
        ChainBudget chain;
        std::map<uint64_t, UnitFacts> units;
        std::set<uint32_t> missing, mismatched;
        Bounded<Identity, MaxRecipients> targets;
        bool authorized = true, nativeDependencies = true;
        uint32_t duration = 5000, radius = 20000;
        Binding const* binding = nullptr;

        explicit Fixture(uint32_t spell, uint32_t slot = 0)
        {
            binding = FindBinding(spell, slot);
            context.aura = {spell, slot, owner, owner, 1};
            context.playerClass = binding ? binding->playerClass : 0;
            context.nowMs = 100;
            context.requirementsMet = NativeChecks;
            context.roll = 0;
            context.data = this;
            Add(owner, true);
            Add(enemy, false);
            Add(ally, true);
            Add(pet, true);
            units[pet.guid].player = false;
            units[pet.guid].creature = 1; // Synthetic fixture, never a production creature binding.
            units[pet.guid].cohort = Cohort::Undead;
            units[pet.guid].controller = units[pet.guid].originalOwner = owner;
            context.authorized = [](void const* data, AuraKey const& key, uint32_t cls)
            {
                auto const& f = *static_cast<Fixture const*>(data);
                return f.authorized && key.caster == f.owner && cls == f.binding->playerClass;
            };
            context.resolve = [](void const* data, Identity const& anchor, Identity const& target)
            {
                auto const& f = *static_cast<Fixture const*>(data);
                auto it = f.units.find(target.guid);
                if (it == f.units.end()) { return UnitFacts{}; }
                auto result = it->second;
                auto anchorIt = f.units.find(anchor.guid);
                if (anchorIt == f.units.end()) { return UnitFacts{}; }
                result.relation = (anchorIt->second.relation == it->second.relation) ?
                    Relation::Friendly : Relation::Hostile;
                return result;
            };
            context.native = [](void const* data, uint32_t spellId)
            {
                auto const& f = *static_cast<Fixture const*>(data);
                auto row = FindNative(spellId);
                NativeFacts facts;
                if (!row || f.missing.count(spellId)) { return facts; }
                facts.digest = f.mismatched.count(spellId) ? "not-the-native-row" : row->digest;
                facts.dependenciesVerified = f.nativeDependencies;
                facts.durationMs = row->periods[0] ? row->periods[0] * 5 : f.duration;
                facts.radius.fill(f.radius);
                facts.ticks.fill(5);
                return facts;
            };
            context.recipients = [](void const* data, Binding const&, Event const&)
            {
                return static_cast<Fixture const*>(data)->targets;
            };
        }
        void Add(Identity id, bool friendly)
        {
            UnitFacts f;
            f.identity = id;
            f.relation = friendly ? Relation::Friendly : Relation::Hostile;
            f.alive = f.inWorld = f.inRange = f.phase = f.los = f.player = true;
            f.health = 500;
            f.maxHealth = 1000;
            units[id.guid] = f;
        }
        Event Damage(uint64_t id = 1)
        {
            Event e;
            e.actor = e.originalCaster = owner;
            e.victim = enemy;
            e.id = e.castId = id;
            e.trustedIds = true;
            e.origin = Origin::Native;
            e.outcome = Outcome::Hit;
            e.relation = Relation::Hostile;
            e.spell = binding && binding->sources.size ? binding->sources.values[0] : 807263;
            e.school = 1;
            e.damage = 1000;
            e.ownerHealth = e.victimHealth = 500;
            e.ownerMaxHealth = e.victimMaxHealth = 1000;
            e.aggregateComplete = true;
            Flags(e);
            return e;
        }
        void Flags(Event& e)
        {
            e.doneFlags = e.periodic ? 0x40000 : (e.healing || e.overheal) ? 0x4000 :
                e.autoAttack ? (e.ranged ? 0x40 : 4) : e.melee ? 0x10 : e.ranged ? 0x100 : 0x10000;
            e.takenFlags = e.doneFlags << 1;
            if (e.damage || e.absorbed) { e.takenFlags |= 0x100000; }
            e.extra = e.outcome == Outcome::Critical ? 2 : e.outcome == Outcome::Absorbed ? 0x400 : 1;
            if (e.periodic && (e.healing || e.overheal)) { e.extra |= 0x40000; }
            e.lineage.root = {e.actor, e.id};
            chain.root = e.lineage.root;
        }
        Event Heal(uint64_t id = 1)
        {
            auto e = Damage(id);
            e.damage = 0;
            e.healing = 1000;
            e.victim = ally;
            e.relation = Relation::Friendly;
            Flags(e);
            return e;
        }
        Plan Apply(Event const& e)
        {
            auto p = Prepare(context, state, e, chain);
            CHECK(p.Result() == Status::Ready);
            CHECK(Commit(context, state, chain, p, true) == Status::Ready);
            return p;
        }
    };
}

TEST(CoaTransfer_full_population_and_independent_golden_coefficients)
{
    std::size_t defined = 0, gaps = 0, supplemental = 0;
    std::set<uint32_t> rows, definedRows, events;
    for (std::size_t i = 0; i < BindingCount(); ++i)
    {
        auto b = BindingAt(i);
        REQUIRE(b);
        REQUIRE(FindNative(b->spell));
        CHECK(FindBinding(b->spell, b->slot) == b);
        CHECK_EQ(FindNative(b->spell)->auras[b->slot], 354u);
        CHECK_EQ(FindNative(b->spell)->children[b->slot], b->child);
        events.insert(uint32_t(b->selector));
        if (b->supplemental) { ++supplemental; continue; }
        rows.insert(b->spell);
        if (b->defined) { ++defined; definedRows.insert(b->spell); }
        else { ++gaps; CHECK(!b->gap.empty()); }
    }
    CHECK_EQ(rows.size(), 128u);
    CHECK_EQ(definedRows.size(), 108u);
    CHECK_EQ(defined, 113u);
    CHECK_EQ(gaps, 20u);
    CHECK_EQ(supplemental, 1u);
    CHECK_EQ(events.size(), 28u);
    struct Golden { uint32_t spell, slot, child; int32_t percent; };
    for (auto g : {Golden{500061, 1, 500534, 30}, {681088, 1, 681067, 20},
        {707479, 0, 707595, 200}, {300499, 0, 783054, 30}, {707657, 0, 561231, 30},
        {707403, 2, 707469, 30}, {803973, 2, 807651, 30}, {806944, 0, 806946, 50},
        {806111, 0, 806112, 50}, {806290, 0, 560355, 75}, {800722, 2, 802598, 0}})
    {
        auto b = FindBinding(g.spell, g.slot);
        REQUIRE(b);
        CHECK_EQ(b->child, g.child);
        CHECK_EQ(b->percent, g.percent);
    }
    CHECK(!FindBinding(300755, 0));
    CHECK(!FindBinding(707391, 3));
    CHECK(!BindingAt(BindingCount()));
    CHECK(FindBinding(92086, 1)->childContractGap);
    CHECK_EQ(FindBinding(707479, 0)->sources.values[0], 503864u);
}

TEST(CoaTransfer_saturating_wide_percent_and_signed_native31_values)
{
    CHECK_EQ(Percent(UINT32_MAX, 200), INT32_MAX);
    CHECK_EQ(Percent(uint64_t(UINT32_MAX) * 20, 200), INT32_MAX);
    CHECK_EQ(Percent(UINT64_MAX, UINT64_MAX), INT32_MAX);
    CHECK_EQ(Percent(99, 30), 29);
    CHECK_EQ(Percent(UINT64_MAX, 0), 0);
    CHECK_EQ(Percent(1, 1, 0), 0);
    CHECK_EQ(*NativeSignedValue(-31.25, true), -32);
    CHECK_EQ(*NativeSignedValue(31.75, true), 31);
    CHECK(!NativeSignedValue(-1, false));
    CHECK(!NativeSignedValue(std::numeric_limits<double>::infinity(), true));
    CHECK(!NativeSignedValue(std::numeric_limits<double>::quiet_NaN(), true));
    CHECK(!NativeSignedValue(std::numeric_limits<double>::max() * 31, true));
    CHECK_EQ(*NativeSignedValue(-1e100, true), INT32_MIN);
    CHECK_EQ(*NativeSignedValue(1e100, true), INT32_MAX);
}

TEST(CoaTransfer_stock_event_flags_all_24_bits_and_96bit_family)
{
    for (uint32_t bit = 0; bit < 24; ++bit)
    {
        ProcData p;
        p.flags = 1u << bit;
        CHECK(StockProcMatches(p, 1u << bit, 1, 1, 0, {}));
        CHECK(!StockProcMatches(p, 1u << ((bit + 1) % 24), 1, 1, 0, {}));
    }
    ProcData p;
    p.flags = 0x40000;
    CHECK(!StockProcMatches(p, p.flags, 0x40001, 1, 0, {}));
    p.extra = 1;
    CHECK(StockProcMatches(p, p.flags, 0x40001, 1, 0, {}));
    p.family = 38;
    p.mask = {0, 0, 0x80000000};
    p.school = 8;
    CHECK(StockProcMatches(p, p.flags, 1, 8, 38, {0, 0, 0x80000000}));
    CHECK(!StockProcMatches(p, p.flags, 1, 8, 37, {0, 0, 0x80000000}));
    CHECK(!StockProcMatches(p, p.flags, 1, 4, 38, {0, 0, 0x80000000}));
    CHECK(!StockProcMatches(p, p.flags, 1, 8, 38, {0x80000000, 0, 0}));
}

TEST(CoaTransfer_exact_zero_mask_not_every_passive_and_nonzero_restrictions)
{
    Fixture f(504446);
    auto e = f.Damage();
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::Ready);
    f.context.aura.spell = 300755;
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::Unsupported);
    Fixture stock(600327);
    auto autoHit = stock.Damage();
    autoHit.autoAttack = autoHit.melee = true;
    stock.Flags(autoHit);
    stock.targets.Push(stock.ally);
    CHECK(Prepare(stock.context, stock.state, autoHit, stock.chain).Result() == Status::Ready);
    autoHit.doneFlags = 0x10000;
    CHECK(Prepare(stock.context, stock.state, autoHit, stock.chain).Result() == Status::NoEffect);
    Fixture override(560535, 1);
    auto crit = override.Damage();
    crit.periodic = true;
    crit.outcome = Outcome::Critical;
    override.Flags(crit);
    CHECK(Prepare(override.context, override.state, crit, override.chain).Result() == Status::Ready);
    override.context.stockProc.flags = 4;
    CHECK(Prepare(override.context, override.state, crit, override.chain).Result() == Status::NoEffect);
}

TEST(CoaTransfer_authorization_wrong_class_holder_caster_and_native_pin)
{
    Fixture f(504446);
    auto e = f.Damage();
    ++f.context.playerClass;
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::Unauthorized);
    --f.context.playerClass;
    f.context.aura.caster = f.ally;
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::Unauthorized);
    f.context.aura.caster = f.owner;
    e.originalCaster = f.ally;
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::Unauthorized);
    e.originalCaster = f.owner;
    f.mismatched.insert(f.context.aura.spell);
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::NativeMismatch);
    f.mismatched.clear();
    f.Apply(e);
    ++f.context.aura.application;
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::Stale);
}

TEST(CoaTransfer_effective_zero_overheal_absorb_miss_immunity_no_value)
{
    Fixture f(504446);
    auto e = f.Damage();
    e.damage = 0;
    e.absorbed = 1000;
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::NoEffect);
    e.damage = 1000;
    for (auto outcome : {Outcome::Miss, Outcome::Immune, Outcome::Evade, Outcome::Failed})
    {
        e.outcome = outcome;
        CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::NoEffect);
    }
    e.outcome = Outcome::Hit;
    e.healthCost = true;
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::NoEffect);
    Fixture heal(704275);
    auto h = heal.Heal();
    h.outcome = Outcome::Critical;
    h.healing = 0;
    h.overheal = 1000;
    heal.Flags(h);
    CHECK(Prepare(heal.context, heal.state, h, heal.chain).Result() == Status::NoEffect);
    Fixture over(707657);
    auto oh = over.Heal();
    oh.healing = 100000;
    oh.overheal = 101;
    over.Flags(oh);
    auto p = over.Apply(oh);
    REQUIRE(p.Intents().size == 1);
    CHECK_EQ(p.Intents().values[0].amount, 30);
}

TEST(CoaTransfer_prepare_commit_cancel_missing_child_and_revalidation)
{
    Fixture f(570064);
    auto e = f.Damage();
    f.missing.insert(f.binding->child);
    auto failed = Prepare(f.context, f.state, e, f.chain);
    CHECK(failed.Result() == Status::Dependency);
    CHECK_EQ(failed.Intents().size, 0u);
    CHECK_EQ(failed.SuppressSlots(), 0u);
    CHECK_EQ(f.state.revision, 0u);
    f.missing.clear();
    auto p = Prepare(f.context, f.state, e, f.chain);
    REQUIRE(p.Result() == Status::Ready);
    CHECK(!f.state.initialized);
    CHECK_EQ(f.chain.remaining, MaxChainEvents);
    CHECK(Commit(f.context, f.state, f.chain, p, false) == Status::Cancelled);
    CHECK(!f.state.initialized);
    f.units[f.owner.guid].los = false;
    CHECK(Commit(f.context, f.state, f.chain, p, true) == Status::Stale);
    CHECK(!f.state.initialized);
    f.units[f.owner.guid].los = true;
    CHECK(Commit(f.context, f.state, f.chain, p, true) == Status::Ready);
    CHECK_EQ(f.state.charges, 0u);
    CHECK(Commit(f.context, f.state, f.chain, p, true) == Status::Stale);
    auto next = f.Damage(2);
    CHECK(Prepare(f.context, f.state, next, f.chain).Result() == Status::Charges);
}

TEST(CoaTransfer_icd_begins_at_commit_not_at_planning)
{
    Fixture f(504446);
    auto e = f.Damage();
    auto p = Prepare(f.context, f.state, e, f.chain);
    REQUIRE(p.Result() == Status::Ready);
    CHECK_EQ(f.state.readyMs, 0u);
    CHECK(Commit(f.context, f.state, f.chain, p, false) == Status::Cancelled);
    f.Apply(e);
    CHECK_EQ(f.state.readyMs, 600u);
    auto next = f.Damage(2);
    CHECK(Prepare(f.context, f.state, next, f.chain).Result() == Status::Cooldown);
    f.context.nowMs = 600;
    f.Apply(next);
    CHECK_EQ(f.state.readyMs, 1100u);
}

TEST(CoaTransfer_paired_slots_one_roll_one_debit_and_original_snapshot)
{
    Fixture f(707391);
    auto e = f.Damage();
    e.melee = true;
    f.Flags(e);
    auto p = f.Apply(e);
    REQUIRE(p.Intents().size == 2);
    CHECK_EQ(p.Intents().values[0].amount, 300);
    CHECK_EQ(p.Intents().values[1].amount, 300);
    CHECK_EQ(p.SuppressSlots(), 7u);
    CHECK_EQ(f.state.revision, 1u);
    auto next = f.Damage(2);
    next.melee = true;
    f.Flags(next);
    f.context.roll = 20;
    CHECK(Prepare(f.context, f.state, next, f.chain).Result() == Status::NoEffect);
    Fixture overload(707403, 1);
    overload.context.requirementsMet |= CompanionTransaction;
    auto hit = overload.Damage();
    overload.missing.insert(707469);
    CHECK(Prepare(overload.context, overload.state, hit, overload.chain).Intents().size == 0);
    overload.missing.clear();
    auto pair = overload.Apply(hit);
    REQUIRE(pair.Intents().size == 2);
    CHECK_EQ(pair.Intents().values[1].amount, 300);
    Fixture pierce(705033);
    auto crit = pierce.Damage();
    crit.damage = 103;
    crit.outcome = Outcome::Critical;
    pierce.Flags(crit);
    auto parts = pierce.Apply(crit);
    REQUIRE(parts.Intents().size == 2);
    CHECK_EQ(parts.Intents().values[0].amount, 36);
    CHECK_EQ(parts.Intents().values[1].amount, 15);
    uint32_t total = 0;
    for (uint32_t i = 0; i < parts.Intents().values[0].ticks; ++i)
    {
        total += TickValue(parts.Intents().values[0], i);
    }
    CHECK_EQ(total, 36u);
}

TEST(CoaTransfer_incoming_healing_foreign_healer_retains_original_passive_owner)
{
    Fixture f(500962);
    f.context.aura.holder = f.ally;
    auto e = f.Heal();
    e.actor = e.originalCaster = f.ally;
    e.victim = f.ally;
    f.Flags(e);
    auto p = f.Apply(e);
    REQUIRE(p.Intents().size == 1);
    CHECK(p.Intents().values[0].kind == Kind::Shield);
    CHECK(p.Intents().values[0].target == f.ally);
    CHECK(p.Intents().values[0].originalCaster == f.owner);
    CHECK_EQ(p.Intents().values[0].amount, 200);
}

TEST(CoaTransfer_ancestor_pet_binding_not_nearby_player_and_pet_claims)
{
    Fixture f(500061, 1);
    auto e = f.Heal();
    f.context.requirementsMet |= WorldBinding;
    f.targets.Push(f.ally);
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::Target);
    f.targets.values[0] = f.pet;
    f.units[f.pet.guid].cohort = Cohort::Ancestor;
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::Target);
    f.units[f.pet.guid].worldBindingVerified = f.units[f.pet.guid].aiReady = true;
    auto p = f.Apply(e);
    REQUIRE(p.Intents().size == 1);
    CHECK(p.Intents().values[0].target == f.pet);
    CHECK_EQ(p.Intents().values[0].amount, 300);
    Fixture pet(705817);
    pet.context.requirementsMet |= WorldBinding;
    auto hit = pet.Damage();
    hit.actor = hit.originalCaster = pet.pet;
    hit.controller = hit.originalOwner = pet.owner;
    pet.Flags(hit);
    CHECK(Prepare(pet.context, pet.state, hit, pet.chain).Result() == Status::Unauthorized);
    pet.units[pet.pet.guid].aiReady = pet.units[pet.pet.guid].worldBindingVerified = true;
    pet.units[pet.pet.guid].controller = pet.ally;
    CHECK(Prepare(pet.context, pet.state, hit, pet.chain).Result() == Status::Unauthorized);
    pet.units[pet.pet.guid].controller = pet.owner;
    auto ownerHeal = pet.Apply(hit);
    CHECK(ownerHeal.Intents().values[0].target == pet.owner);
    CHECK_EQ(ownerHeal.Intents().values[0].amount, 150);
}

TEST(CoaTransfer_split_undead_single_budget_not_full_copy_per_pet)
{
    Fixture f(680388);
    f.context.requirementsMet |= WorldBinding;
    f.units[f.pet.guid].aiReady = f.units[f.pet.guid].worldBindingVerified = true;
    f.targets.Push(f.owner);
    f.targets.Push(f.pet);
    auto e = f.Damage();
    e.damage = 101;
    auto p = f.Apply(e);
    REQUIRE(p.Intents().size == 2);
    CHECK_EQ(p.Intents().values[0].amount + p.Intents().values[1].amount, 30);
}

TEST(CoaTransfer_absorbed_to_ap_replaces_and_expires_only_own_source)
{
    Fixture f(681088, 1);
    auto e = f.Damage();
    e.actor = e.originalCaster = f.enemy;
    e.victim = f.owner;
    e.shieldCaster = f.owner;
    e.shieldOrigin = Origin::Native;
    e.damage = 0;
    e.absorbed = 101;
    e.outcome = Outcome::Absorbed;
    f.Flags(e);
    auto first = f.Apply(e);
    CHECK_EQ(first.Intents().values[0].amount, 20);
    auto derivedShield = e;
    derivedShield.shieldOrigin = Origin::ValueTransfer;
    CHECK(Prepare(f.context, State{}, derivedShield, f.chain).Result() == Status::Recursive);
    e.id = e.castId = 2;
    e.absorbed = 50;
    f.Flags(e);
    auto replacement = f.Apply(e);
    CHECK_EQ(replacement.Intents().values[0].previousContribution, 20);
    CHECK_EQ(replacement.Intents().values[0].amount, 10);
    CHECK_EQ(f.state.contributions.size, 1u);
    auto early = RemoveContributions(f.state, 200, false);
    CHECK_EQ(early.size, 0u);
    auto expired = RemoveContributions(f.state, 5100, false);
    REQUIRE(expired.size == 1);
    CHECK_EQ(expired.values[0].amount, 10);
    CHECK_EQ(f.state.contributions.size, 0u);
    CHECK_EQ(RemoveContributions(f.state, 5200, false).size, 0u);
    Fixture other(681088, 1);
    auto foreign = e;
    foreign.shieldCaster = other.ally;
    other.Flags(foreign);
    CHECK(Prepare(other.context, other.state, foreign, other.chain).Result() == Status::Unauthorized);
}

TEST(CoaTransfer_named_synergy_chain_and_200_percent_terminal_heal)
{
    Fixture blades(680276);
    blades.context.requirementsMet |= CompanionTransaction;
    auto e = blades.Damage();
    e.autoAttack = e.melee = true;
    blades.Flags(e);
    auto first = blades.Apply(e);
    Fixture pilfer(705087, 1);
    auto child = pilfer.Damage(2);
    child.spell = 520571;
    child.damage = 400;
    child.origin = Origin::ValueTransfer;
    child.lineage = first.Intents().values[0].lineage;
    pilfer.chain = blades.chain;
    auto heal = pilfer.Apply(child);
    CHECK_EQ(heal.Intents().values[0].amount, 200);
    CHECK(heal.Intents().values[0].lineage.terminal);
    CHECK_EQ(heal.Intents().values[0].lineage.parents.size, 2u);
    child.lineage.parents.values[0].caster = pilfer.ally;
    CHECK(Prepare(pilfer.context, State{}, child, pilfer.chain).Result() == Status::Recursive);
    Fixture life(707479);
    auto echo = life.Damage();
    echo.origin = Origin::ValueTransfer;
    echo.lineage.parents.Push({504750, 0, life.owner, life.owner, 1});
    echo.damage = UINT32_MAX;
    auto large = life.Apply(echo);
    REQUIRE(large.Intents().size == 1);
    CHECK_EQ(large.Intents().values[0].amount, INT32_MAX);
    CHECK(large.Intents().values[0].target == life.owner);
    echo.victim = life.ally;
    CHECK(Prepare(life.context, State{}, echo, life.chain).Result() == Status::Target);
}

TEST(CoaTransfer_cross_actor_bounce_and_shared_root_budget_are_finite)
{
    Fixture f(704275);
    auto e = f.Heal();
    e.outcome = Outcome::Critical;
    f.Flags(e);
    auto first = f.Apply(e);
    Fixture bounce(500962);
    bounce.owner = {600, 7, 1, 1};
    bounce.Add(bounce.owner, true);
    bounce.context.aura.caster = bounce.owner;
    bounce.context.aura.holder = bounce.ally;
    auto derived = bounce.Heal(2);
    derived.actor = derived.originalCaster = f.owner;
    derived.origin = Origin::ValueTransfer;
    derived.lineage = first.Intents().values[0].lineage;
    bounce.chain = f.chain;
    CHECK(bounce.context.aura.caster != first.Intents().values[0].originalCaster);
    CHECK(Prepare(bounce.context, bounce.state, derived, bounce.chain).Result() == Status::Recursive);
    Fixture many(504446);
    auto native = many.Damage();
    for (uint32_t i = 0; i < MaxChainEvents; ++i)
    {
        many.state = {};
        many.context.aura.application = i + 1;
        many.Apply(native);
    }
    many.state = {};
    CHECK_EQ(many.chain.remaining, 0u);
    CHECK(Prepare(many.context, many.state, native, many.chain).Result() == Status::Limit);
    native.lineage.parents.size = MaxDepth + 1;
    CHECK(Prepare(many.context, many.state, native, many.chain).Result() == Status::Invalid);
}

TEST(CoaTransfer_per_actor_fifo_zero_ids_and_once_cast_versus_each_hit)
{
    Fixture once(802756);
    auto e = once.Damage();
    e.trustedIds = false;
    CHECK(Prepare(once.context, once.state, e, once.chain).Result() == Status::Invalid);
    e.trustedIds = true;
    e.castId = 0;
    CHECK(Prepare(once.context, once.state, e, once.chain).Result() == Status::Invalid);
    e.castId = 5;
    once.Apply(e);
    e.id = 2;
    once.Flags(e);
    CHECK(Prepare(once.context, once.state, e, once.chain).Result() == Status::Duplicate);
    Fixture each(707391);
    auto hit = each.Damage();
    hit.melee = true;
    each.Flags(hit);
    each.Apply(hit);
    hit.id = 2;
    each.Flags(hit);
    each.Apply(hit); // Same cast, separately committed hit, paired outputs still share a roll.
    CHECK_EQ(each.state.revision, 2u);
    CHECK(Prepare(each.context, each.state, hit, each.chain).Result() == Status::Duplicate);
    Fixture fifo(802756);
    for (uint64_t i = 1; i <= HistorySize + 1; ++i)
    {
        fifo.chain.remaining = MaxChainEvents;
        fifo.Apply(fifo.Damage(i));
    }
    auto old = fifo.Damage(1);
    CHECK(Prepare(fifo.context, fifo.state, old, fifo.chain).Result() == Status::Duplicate);
    CHECK_EQ(fifo.state.histories.values[0].size, HistorySize);
    CHECK_EQ(fifo.state.histories.values[0].eventFloor, 1u);
}

TEST(CoaTransfer_lifecycle_frame_generation_unlearn_cancel_pending)
{
    Fixture f(804891);
    auto e = f.Damage();
    e.actor = e.originalCaster = f.enemy;
    e.victim = f.owner;
    f.Flags(e);
    auto p = Prepare(f.context, f.state, e, f.chain);
    REQUIRE(p.Result() == Status::Ready);
    ++f.units[f.owner.guid].identity.generation;
    CHECK(Commit(f.context, f.state, f.chain, p, true) == Status::Stale);
    --f.units[f.owner.guid].identity.generation;
    f.Apply(e);
    CHECK_EQ(RemoveContributions(f.state, 100, true).size, 1u);
    CHECK(!f.state.active);
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::Stale);
    Fixture deck(504446);
    auto d = deck.Damage();
    d.victim.frame = 44;
    CHECK(Prepare(deck.context, deck.state, d, deck.chain).Result() == Status::Target);
}

TEST(CoaTransfer_shield_accumulates_with_cap_no_raw_child_stat_scaling)
{
    Fixture f(704889);
    auto e = f.Damage();
    e.damage = 900;
    f.context.shieldRemaining = 700;
    auto p = f.Apply(e);
    REQUIRE(p.Intents().size == 1);
    CHECK_EQ(p.Intents().values[0].amount, 1000);
    CHECK(p.Intents().values[0].suppressSourceBonuses);
    CHECK(!p.Intents().values[0].allowCritical);
    Fixture stacked(560748, 2);
    auto s = stacked.Damage();
    s.consumedStacks = 5;
    auto scaled = stacked.Apply(s);
    CHECK_EQ(scaled.Intents().values[0].amount, 100);
    Fixture modified(560748, 2);
    modified.context.basePercent[2] = 3;
    modified.Flags(s);
    auto withModifier = modified.Apply(s);
    CHECK_EQ(withModifier.Intents().values[0].amount, 150);
}

TEST(CoaTransfer_defined_source_gaps_stay_blocked_no_partial_source_claim)
{
    for (std::size_t i = 0; i < BindingCount(); ++i)
    {
        auto b = BindingAt(i);
        if (b->defined || b->slot != b->budgetSlot) { continue; }
        Fixture f(b->spell, b->slot);
        auto p = Prepare(f.context, f.state, f.Damage(), f.chain);
        CHECK(p.Result() == Status::SourceGap);
        CHECK_EQ(p.Intents().size, 0u);
    }
}

TEST(CoaTransfer_direct_includes_autos_but_ability_only_and_holder_scope_do_not)
{
    Fixture direct(802756);
    auto e = direct.Damage();
    e.spell = 0;
    e.autoAttack = e.melee = true;
    direct.Flags(e);
    CHECK(Prepare(direct.context, direct.state, e, direct.chain).Result() == Status::Ready);
    Fixture ability(681311, 1);
    ability.context.requirementsMet |= CompanionTransaction;
    ability.Flags(e);
    CHECK(Prepare(ability.context, ability.state, e, ability.chain).Result() == Status::NoEffect);
    Fixture selfOnly(504446);
    selfOnly.context.aura.holder = selfOnly.ally;
    auto foreignHolder = selfOnly.Damage();
    foreignHolder.actor = foreignHolder.originalCaster = selfOnly.ally;
    selfOnly.Flags(foreignHolder);
    CHECK(Prepare(selfOnly.context, selfOnly.state, foreignHolder, selfOnly.chain).Result() == Status::Unauthorized);
}

TEST(CoaTransfer_event_flags_cannot_disagree_with_critical_outcome)
{
    Fixture f(704275);
    auto e = f.Heal();
    e.outcome = Outcome::Critical;
    f.Flags(e);
    e.extra = 1;
    CHECK(Prepare(f.context, f.state, e, f.chain).Result() == Status::Invalid);
}

TEST(CoaTransfer_all_defined_descriptor_operation_paths_with_synthetic_sinks)
{
    uint32_t preparedParents = 0;
    for (std::size_t index = 0; index < BindingCount(); ++index)
    {
        auto b = BindingAt(index);
        if (!b->defined || b->slot != b->budgetSlot || b->childContractGap || b->selector == Selector::None)
        {
            continue;
        }
        Fixture f(b->spell, b->slot);
        f.context.requirementsMet = b->requirements;
        auto e = f.Damage();
        bool incoming = false, pet = false;
        switch (b->selector)
        {
            case Selector::CriticalDamage: case Selector::DirectCriticalDamage:
            case Selector::PhysicalNatureCritical: e.outcome = Outcome::Critical; break;
            case Selector::Heal: case Selector::HealReceived: case Selector::CriticalHeal:
            case Selector::PeriodicCriticalHeal: case Selector::Overheal:
                e = f.Heal();
                if (b->selector == Selector::CriticalHeal || b->selector == Selector::PeriodicCriticalHeal)
                {
                    e.outcome = Outcome::Critical;
                }
                e.periodic = b->selector == Selector::PeriodicCriticalHeal;
                if (b->selector == Selector::Overheal) { e.overheal = 500; }
                incoming = b->selector == Selector::HealReceived;
                break;
            case Selector::DamageTaken: case Selector::DirectDamageTaken:
            case Selector::PhysicalDamageTaken: case Selector::MagicDamageTaken:
                incoming = true;
                if (b->selector == Selector::MagicDamageTaken) { e.school = 4; }
                break;
            case Selector::MeleeDamage: case Selector::MeleeDamageTaken:
                e.melee = true;
                incoming = b->selector == Selector::MeleeDamageTaken;
                break;
            case Selector::AutoCritical: case Selector::PetAutoCritical:
                e.outcome = Outcome::Critical;
                [[fallthrough]];
            case Selector::AutoDamage: case Selector::MeleeAuto: case Selector::RangedAuto:
                e.autoAttack = true;
                e.melee = b->selector != Selector::RangedAuto;
                e.ranged = b->selector == Selector::RangedAuto;
                pet = b->selector == Selector::PetAutoCritical;
                break;
            case Selector::PeriodicDamage: e.periodic = true; break;
            case Selector::PetDamage: case Selector::PetDamageOrHeal: pet = true; break;
            case Selector::AbsorbUsed:
                e.damage = 0;
                e.absorbed = 100;
                e.outcome = Outcome::Absorbed;
                e.shieldCaster = f.owner;
                e.shieldOrigin = Origin::Native;
                incoming = true;
                break;
            default: break;
        }
        if (incoming)
        {
            e.actor = e.originalCaster = e.healing ? f.ally : f.enemy;
            e.victim = f.owner;
        }
        if (pet)
        {
            e.actor = e.originalCaster = f.pet;
            e.controller = e.originalOwner = f.owner;
        }
        f.units[f.pet.guid].cohort = b->cohort == Cohort::OwnedSummon ? Cohort::Undead : b->cohort;
        f.units[f.pet.guid].aiReady = f.units[f.pet.guid].worldBindingVerified = true;
        e.consumedStacks = 1;
        e.auraStacks = 1;
        e.nearbyEnemies = 1;
        e.frontArc = true;
        if (b->spell == 301011) { e.victim = f.owner; }
        if (b->spell == 301273) { e.ownerHealth = 750; }
        if (b->spell == 804223) { e.victimHealth = 300; }
        f.Flags(e);
        if (b->spell == 705087 || b->spell == 705397 || b->spell == 707479)
        {
            e.origin = Origin::ValueTransfer;
            e.lineage.parents.Push({b->spell == 705087 ? 680276u : b->spell == 705397 ? 803997u : 504750u,
                0, f.owner, f.owner, 1});
        }
        e.ground = {f.owner, {1, 2, 3}, true};
        f.context.groundDistance = [](void const*, Point const&, Identity const&) { return 1000u; };
        f.Add({201, 7, 1, 1}, false);
        f.context.recipients = [](void const* data, Binding const& part, Event const& event)
        {
            auto const& fixture = *static_cast<Fixture const*>(data);
            Bounded<Identity, MaxRecipients> targets;
            if (part.recipient == Recipient::Ancestor) { targets.Push(fixture.pet); }
            else if (part.splitBudget) { targets.Push(fixture.owner); targets.Push(fixture.pet); }
            else if (part.kind == Kind::Damage || (part.kind == Kind::Store && event.damage))
            {
                targets.Push(part.includeOriginal ? fixture.enemy : Identity{201, 7, 1, 1});
            }
            else { targets.Push(event.victim == fixture.ally ? fixture.owner : fixture.ally); }
            return targets;
        };
        auto p = Prepare(f.context, f.state, e, f.chain);
        if (p.Result() != Status::Ready)
        {
            CHECK_EQ(b->spell, 0u); // Failing row ID is printed by the native harness.
            continue;
        }
        CHECK(p.Intents().size > 0);
        CHECK(Commit(f.context, f.state, f.chain, p, true) == Status::Ready);
        ++preparedParents;
    }
    CHECK_EQ(preparedParents, 107u);
}

TEST(CoaTransfer_frame_local_ground_snapshot_and_lowest_health_selection)
{
    Fixture ground(570173);
    auto event = ground.Damage();
    ground.targets.Push({201, 7, 1, 1});
    ground.Add({201, 7, 1, 1}, false);
    CHECK(Prepare(ground.context, ground.state, event, ground.chain).Result() == Status::Dependency);
    event.ground = {ground.owner, {1, 2, 3}, true};
    ground.context.groundDistance = [](void const*, Point const&, Identity const&) { return 1000u; };
    auto p = ground.Apply(event);
    CHECK(p.Intents().values[0].ground.coordinates == event.ground.coordinates);
    CHECK(p.Intents().values[0].schedule == Schedule::PerTick);
    Fixture lowest(301011);
    auto heal = lowest.Heal();
    heal.victim = lowest.owner;
    lowest.Flags(heal);
    for (uint64_t id = 301; id <= 306; ++id)
    {
        Identity target{id, 7, 1, 1};
        lowest.Add(target, true);
        lowest.units[id].health = uint32_t(307 - id) * 10;
        lowest.targets.Push(target);
    }
    auto selected = lowest.Apply(heal);
    REQUIRE(selected.Intents().size == 5);
    CHECK_EQ(selected.Intents().values[0].target.guid, 306u);
    CHECK_EQ(selected.Intents().values[4].target.guid, 302u);
}

TEST(CoaTransfer_overlapping_parent_states_and_once_cast_charge_for_multiple_victims)
{
    Fixture f(801778);
    auto first = f.Damage();
    f.Apply(first);
    CHECK_EQ(f.state.charges, 0u);
    auto second = first;
    second.id = 2;
    second.victim = {201, 7, 1, 1};
    f.Add(second.victim, false);
    f.Flags(second);
    f.Apply(second);
    CHECK_EQ(f.state.charges, 0u);
    CHECK_EQ(f.state.revision, 2u);
    Fixture a(680581), b(704889);
    auto e = a.Damage();
    b.chain = a.chain;
    auto heal = a.Apply(e);
    b.chain = a.chain;
    auto shield = b.Apply(e);
    CHECK_EQ(heal.Intents().values[0].amount, 100);
    CHECK_EQ(shield.Intents().values[0].amount, 1000);
    CHECK_EQ(a.state.revision, 1u);
    CHECK_EQ(b.state.revision, 1u);
}

TEST(CoaTransfer_per_attacker_icd_and_retired_life_history_cleanup)
{
    Fixture f(704777);
    auto hit = f.Damage();
    hit.actor = hit.originalCaster = f.enemy;
    hit.victim = f.owner;
    hit.melee = true;
    hit.frontArc = true;
    f.Flags(hit);
    f.Apply(hit);
    hit.id = hit.castId = 2;
    f.Flags(hit);
    CHECK(Prepare(f.context, f.state, hit, f.chain).Result() == Status::Cooldown);
    hit.actor = hit.originalCaster = {201, 7, 1, 1};
    f.Add(hit.actor, false);
    f.Flags(hit);
    f.Apply(hit);
    CHECK_EQ(f.state.attackers.size, 2u);
    auto revision = f.state.revision;
    ForgetActor(f.state, {200, 7, 2, 1});
    CHECK_EQ(f.state.revision, revision);
    f.units.erase(f.enemy.guid);
    ForgetActor(f.state, f.enemy);
    CHECK_EQ(f.state.histories.size, 1u);
    CHECK_EQ(f.state.attackers.size, 1u);
    hit.actor = hit.originalCaster = f.enemy;
    f.Flags(hit);
    CHECK(Prepare(f.context, f.state, hit, f.chain).Result() == Status::Target);
}

TEST(CoaTransfer_mixed_damage_heal_and_expiry_store_keep_event_kind)
{
    Fixture nightmare(301273);
    nightmare.context.requirementsMet |= RageCost | CompanionTransaction;
    auto heal = nightmare.Heal();
    heal.ownerHealth = 750;
    auto p = nightmare.Apply(heal);
    CHECK(p.Intents().values[0].kind == Kind::Heal);
    CHECK(p.Intents().values[0].target == nightmare.ally);
    CHECK_EQ(p.Intents().values[0].amount, 100);
    Fixture store(520388);
    auto h = store.Heal();
    store.targets.Push(store.owner);
    auto stored = store.Apply(h);
    CHECK(stored.Intents().values[0].kind == Kind::Heal);
    CHECK(stored.Intents().values[0].schedule == Schedule::Expiry);
    CHECK(stored.Intents().values[0].allowCritical);
    CHECK_EQ(store.state.charges, 4u);
    CHECK_EQ(store.state.readyMs, 3100u);
    CHECK(stored.Intents().values[0].expiresMs > store.context.nowMs);
}
