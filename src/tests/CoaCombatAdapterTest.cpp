// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "CoaCombatRules.h"
#include "CoaCatalog.h"
#include "SharedDefines.h"
#include "DBCEnums.h"
#include "SpellAuraDefines.h"
#include "ObjectGuid.h"
#include "WorldPacket.h"
#include "Utilities/Errors.h"
#include <atomic>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include "CoaCombatGuidPacket.inc"

// Native boundary fixtures, NOT another implementation of Evaluate or the
// adapter transaction. The included method bodies come from production files.
namespace
{
    using namespace coa::combat;
    constexpr int UNIT_MOD_STAT_INTELLECT = 3, TOTAL_VALUE = 2;
    struct SpellEntry
    {
        uint32 ID = 0, Effect[3]{6, 0, 0}, EffectAura[3]{4, 0, 0};
        int32 amounts[3]{0, 0, 0}, PowerType = 0, duration = -1;
        uint32 cost = 0;
        int32 CalculateSimpleValue(SpellEffectIndex slot) const { return amounts[slot]; }
    };
    struct SpellStore
    {
        std::map<uint32, SpellEntry> rows;
        SpellEntry const* LookupEntry(uint32 id) const
        {
            auto it = rows.find(id);
            return it == rows.end() ? nullptr : &it->second;
        }
    } sSpellStore;
    bool IsAuraApplyEffect(SpellEntry const* info, SpellEffectIndex slot) { return info->Effect[slot] == 6; }
    struct Logger { template<class... Args> void outError(char const*, Args...) {} } sLog;
    struct RuntimeAura
    {
        struct Modifier { int32 m_amount = 0; } modifier;
        Modifier* GetModifier() { return &modifier; }
    };
    class Unit;
    class Player;
    class SpellAuraHolder
    {
    public:
        uint32 id = 0;
        ObjectGuid caster, item;
        uint32 m_procCharges = 0, m_stackAmount = 0;
        int32 duration = -1, maximum = -1;
        uint64 expires = 0;
        uint8 slot = 0, flags = AFLAG_NOT_CASTER, level = 60;
        bool controlled = false;
        std::array<std::unique_ptr<RuntimeAura>, 3> auras;
        uint32 GetId() const { return id; }
        ObjectGuid GetCasterGuid() const { return caster; }
        ObjectGuid GetCastItemGuid() const { return item; }
        uint32 GetStackAmount() const { return m_stackAmount; }
        uint32 GetAuraCharges() const { return m_procCharges; }
        int32 GetAuraDuration() const { return duration; }
        int32 GetAuraMaxDuration() const { return maximum; }
        uint8 GetAuraSlot() const { return slot; }
        uint8 GetAuraFlags() const { return flags; }
        uint8 GetAuraLevel() const { return level; }
        bool IsCoaControlled() const { return controlled; }
        uint64 GetCoaExpiry() const { return expires; }
        void SetCoaControlled(uint64) { controlled = true; }
        void AddAura(RuntimeAura* a, SpellEffectIndex i) { auras[i].reset(a); flags |= uint8(1u << i); }
        void SetLoadedState(ObjectGuid c, ObjectGuid it, uint32 stacks, uint32 charges, int32 max, int32 remaining)
        {
            caster = c; item = it; m_stackAmount = stacks; m_procCharges = charges;
            maximum = max; duration = remaining;
            expires = remaining > 0 ? uint64(remaining) : 0;
            if (duration > 0) { flags |= AFLAG_DURATION; }
        }
        void BuildUpdatePacket(WorldPacket& data) const;
    };
    class Unit
    {
    public:
        uint64 m_combatEpoch = 0;
        Identity identity{100, 7, 0};
        bool alive = true, inWorld = true, immune = false;
        std::map<std::pair<uint32, uint64>, SpellAuraHolder*> holders;
        std::function<void()> onActivate;
        Unit() { AdvanceCombatEpoch(); identity.generation = m_combatEpoch; }
        ~Unit() { for (auto& h : holders) { delete h.second; } }
        void AdvanceCombatEpoch();
        uint64 GetCombatEpoch() const { return m_combatEpoch; }
        uint32 GetGUIDLow() const { return uint32(identity.guid); }
        ObjectGuid GetObjectGuid() const { return ObjectGuid(identity.guid); }
        bool IsInWorld() const { return inWorld; }
        bool IsAlive() const { return alive; }
        bool IsImmuneToSpell(SpellEntry const*, bool) { return immune; }
        bool IsImmuneToSpellEffect(SpellEntry const*, SpellEffectIndex, bool) { return immune; }
        auto const& GetSpellAuraHolderMap() const { return holders; }
        size_t GetVisibleAurasCount() const { return holders.size(); }
        SpellAuraHolder* GetSpellAuraHolder(uint32 id, ObjectGuid caster) const
        {
            auto it = holders.find({id, caster.GetRawValue()});
            return it == holders.end() ? nullptr : it->second;
        }
        void StageCoaAura(SpellAuraHolder* before, SpellAuraHolder* after)
        {
            if (before) { holders.erase({before->id, before->caster.GetRawValue()}); }
            if (after) { holders[{after->id, after->caster.GetRawValue()}] = after; }
        }
        void ActivateCoaAura(SpellAuraHolder* before, SpellAuraHolder*)
        {
            if (onActivate) { onActivate(); }
            delete before;
        }
    };
    class CoaCombatTransaction;
    class Player : public Unit
    {
    public:
        State m_coaCombat;
        coa::Build m_coaBuild;
        std::set<uint32> learned, m_coaCombatSpells;
        bool m_coaReady = true, m_coaFailed = false, m_coaCombatPublishing = false;
        bool m_coaCombatInvalidating = false, m_coaCombatDraining = false;
        Bounded<Event, 32> m_coaCombatDeferred;
        uint64 m_coaEventSequence = 0;
        int32 m_coaIntellect = 0, rating = 0, statApplied = 0;
        uint32 playerClass = 27;
        uint32 activeVow = 0, weaponMask = 0;
        struct Clock { uint64 now = 0; uint64 CalculateTime(uint64 delta) const { return now + delta; } } m_Events;
        Unit* other = nullptr;
        uint32 nativePreflights = 0, packets = 0;
        SpellCastResult childResult = SPELL_CAST_OK;
        bool HasSpell(uint32 id) const { return learned.count(id); }
        bool IsCoaManaged() const { return true; }
        bool KnowsCoaCombatSpell(uint32) const;
        bool IsCoaCombatAuthorized(uint32, uint32 = 0) const;
        void SyncCoaCombatAuras();
        void UpdateCoaCombatRules();
        Context BuildCoaCombatContext() const;
        bool OnCoaCombatEvent(Event e);
        void HandleStatModifier(int, int, float amount, bool) { statApplied += int32(amount); }
        uint32 GetPower(Powers) const { return 1000; }
        uint32 GetHealth() const { return 1000; }
        void SendAurasForTarget(Unit* target)
        {
            WorldPacket packet;
            for (auto const& h : target->holders) { h.second->BuildUpdatePacket(packet); }
            ++packets;
        }
    };
    SpellAuraHolder* CreateSpellAuraHolder(SpellEntry const* info, Unit*, Player* owner)
    {
        auto* h = new SpellAuraHolder;
        h->id = info->ID;
        h->caster = owner->GetObjectGuid();
        return h;
    }
    RuntimeAura* CreateAura(SpellEntry const* info, SpellEffectIndex slot, void*, SpellAuraHolder*, Unit*, Player*)
    {
        return new RuntimeAura{{info->amounts[slot]}};
    }
    class Spell
    {
    public:
        Player* owner;
        SpellEntry const* m_spellInfo;
        Spell(Player* p, SpellEntry const* info, bool, ObjectGuid) : owner(p), m_spellInfo(info) {}
        SpellCastResult PrepareCoaLinkedCast(CastIntent const&)
        {
            ++owner->nativePreflights;
            return owner->childResult;
        }
        uint32 GetPowerCost() const { return m_spellInfo->cost; }
    };
    namespace CoaCombatIntegration
    {
        Identity Identify(Unit const& u) { auto id = u.identity; id.generation = u.GetCombatEpoch(); return id; }
        Unit* Resolve(Player& p, Identity const& id)
        {
            Unit* unit = id.guid == p.identity.guid ? &p : p.other;
            return unit && unit->IsInWorld() && Identify(*unit) == id ? unit : nullptr;
        }
        SpellCastResult CastResult(Status s) { return s == Status::Ready ? SPELL_CAST_OK : SPELL_FAILED_SPELL_UNAVAILABLE; }
        bool IsCounterOnlyComponent(uint32 spell, uint32 slot, uint32 aura, int32 value)
        {
            return spell == 500706 && slot == 1 && aura == 317 && value == 0;
        }
    }
    class CoaCombatTransaction
    {
    public:
        struct HolderChange
        {
            Unit* target; Identity targetLife; uint32 spell;
            SpellAuraHolder* previous; std::unique_ptr<SpellAuraHolder> replacement;
        };
        Player& m_owner;
        std::vector<Event> m_events;
        Context m_context;
        Plan m_plan;
        std::vector<Plan> m_plans;
        std::array<uint64, MAX_POWERS + 1> m_powerCosts{};
        std::vector<HolderChange> m_holders;
        std::vector<std::unique_ptr<Spell>> m_casts;
        bool m_ready = false;
        CoaCombatTransaction(Player& p, Event e) : m_owner(p), m_events{e} {}
        void ReservePower(int32, uint32);
        SpellCastResult Prepare();
        bool Publish();
        void ReleaseCasts() { m_casts.clear(); }
    };
    #include "CoaCombatMethodBodies.inc"

    Context Player::BuildCoaCombatContext() const
    {
        Context c;
        c.actor = CoaCombatIntegration::Identify(*this); c.playerClass = playerClass;
        c.alive = alive; c.inWorld = inWorld; c.nowMs = m_Events.now; c.data = this;
        c.criticalStrikeRating = rating;
        c.activeVow = activeVow; c.weaponMask = weaponMask;
        c.known = [](void const* p, uint32 id) { return static_cast<Player const*>(p)->KnowsCoaCombatSpell(id); };
        c.authorized = [](void const* p, uint32 id, uint32 r) { return static_cast<Player const*>(p)->IsCoaCombatAuthorized(id, r); };
        c.nativeSpell = [](void const*, uint32 id)
        {
            auto* info = sSpellStore.LookupEntry(id);
            return NativeSpell{info != nullptr, info ? info->duration : 0};
        };
        c.resolve = [](void const* p, Identity const& id)
        {
            auto* u = CoaCombatIntegration::Resolve(*const_cast<Player*>(static_cast<Player const*>(p)), id);
            return u ? Resolution{true, u->alive, u->inWorld, true, false, CoaCombatIntegration::Identify(*u)} : Resolution{};
        };
        c.destinationValid = [](void const* p, Destination const& d)
        {
            return d.owner == CoaCombatIntegration::Identify(*static_cast<Player const*>(p)) && d.phase == 1;
        };
        return c;
    }
    Event Input(Player& p, uint32 spell, EventKind kind = EventKind::Cast)
    {
        Event e;
        e.spell = spell; e.kind = kind;
        e.source = e.originalCaster = e.target = CoaCombatIntegration::Identify(p);
        return e;
    }
    void Definitions()
    {
        sSpellStore.rows.clear();
        for (uint32 id : {500149u, 802938u, 802939u, 804586u, 704396u, 807440u, 807441u,
            807389u, 807533u, 900755u, 500706u, 803061u, 805077u, 500363u, 803031u, 680448u, 506823u, 506824u,
            806068u, 680472u})
        {
            sSpellStore.rows[id].ID = id;
        }
        sSpellStore.rows[807440].EffectAura[0] = 42;
        sSpellStore.rows[807440].duration = 60000;
        sSpellStore.rows[803061].duration = 10000;
    }
    void Seed(Player& p, uint32 id, uint32 stacks, uint64 caster = 0)
    {
        auto* h = new SpellAuraHolder;
        h->id = id; h->caster = ObjectGuid(caster ? caster : p.identity.guid); h->m_stackAmount = stacks;
        p.holders[{id, h->caster.GetRawValue()}] = h;
    }
}

TEST(CoaCombatAdapter_actual_preparation_failure_and_all_maps_before_callbacks)
{
    Definitions();
    Player p;
    p.learned.insert(804584); p.m_coaCombatSpells.insert(800764);
    auto e = Input(p, 800764);
    Effect slot; slot.slot = 2; slot.type = 175; slot.child = 500149; slot.target = e.target;
    e.effects.Push(slot);
    sSpellStore.rows.erase(802939);
    CoaCombatTransaction fail(p, e);
    CHECK(fail.Prepare() != SPELL_CAST_OK);
    CHECK(!fail.Publish());
    CHECK(p.holders.empty()); CHECK_EQ(p.packets, 0u);
    Definitions();
    unsigned callbacks = 0;
    p.onActivate = [&]
    {
        ++callbacks;
        CHECK(p.m_coaCombatPublishing);
        REQUIRE(p.GetSpellAuraHolder(500149, p.GetObjectGuid()));
        CHECK_EQ(p.GetSpellAuraHolder(500149, p.GetObjectGuid())->GetStackAmount(), 10u);
        CHECK(p.GetSpellAuraHolder(802938, p.GetObjectGuid()));
        CHECK(p.GetSpellAuraHolder(802939, p.GetObjectGuid()));
    };
    CoaCombatTransaction good(p, e);
    REQUIRE(good.Prepare() == SPELL_CAST_OK);
    CHECK(p.holders.empty());
    CHECK(good.Publish()); CHECK_EQ(callbacks, 3u);
    CHECK(!good.Publish());
}

TEST(CoaCombatAdapter_actual_aura_import_cas_external_mutation_and_foreign_caster)
{
    Definitions();
    Player p; p.learned.insert(804584); p.m_coaCombatSpells.insert(804584);
    Seed(p, 500149, 20); Seed(p, 500149, 7, 999);
    p.SyncCoaCombatAuras();
    CHECK_EQ(Count(p.m_coaCombat, 500149, CoaCombatIntegration::Identify(p)), 20);
    auto e = Input(p, 804584);
    CoaCombatTransaction tx(p, e);
    REQUIRE(tx.Prepare() == SPELL_CAST_OK);
    p.GetSpellAuraHolder(500149, p.GetObjectGuid())->m_stackAmount = 19;
    CHECK(!tx.Publish());
    CHECK_EQ(p.GetSpellAuraHolder(500149, ObjectGuid(uint64(999)))->m_stackAmount, 7u);
    CHECK(!p.GetSpellAuraHolder(807440, p.GetObjectGuid()));
}

TEST(CoaCombatAdapter_real_event_gate_defers_reentrance_and_bounds_drain)
{
    Definitions();
    Player p;
    p.m_coaCombatPublishing = true;
    Event e; e.kind = EventKind::Refresh;
    for (unsigned i = 0; i < 32; ++i) { CHECK(p.OnCoaCombatEvent(e)); }
    CHECK(!p.OnCoaCombatEvent(e));
    CHECK_EQ(p.m_coaCombatDeferred.size, 32u);
    CHECK_EQ(p.m_coaCombat.revision, 0u);
    CHECK_EQ(p.packets, 0u);
    p.m_coaCombatPublishing = false;
    CHECK(p.OnCoaCombatEvent(e));
    CHECK_EQ(p.m_coaCombatDeferred.size, 0u);
    CHECK_EQ(p.m_coaCombat.lastEvent, 33u);
}

TEST(CoaCombatAdapter_actual_dawn_charges_native_wire_and_failed_child_no_debit)
{
    Definitions();
    Player p; p.learned.insert(804584); p.m_coaCombatSpells.insert(804584);
    Seed(p, 500149, 20);
    REQUIRE(p.OnCoaCombatEvent(Input(p, 804584)));
    auto* dawn = p.GetSpellAuraHolder(807440, p.GetObjectGuid());
    REQUIRE(dawn);
    CHECK_EQ(dawn->m_procCharges, 10u);
    CHECK_EQ(dawn->m_stackAmount, 1u);
    WorldPacket packet;
    dawn->BuildUpdatePacket(packet);
    // Native packet: slot, spell ID, flags, level, charge*stack byte, duration.
    CHECK_EQ(packet.read<uint32>(1), 807440u);
    CHECK_EQ(packet.read<uint8>(7), 10u);
    CHECK_EQ(packet.read<uint32>(8), 60000u);
    CHECK_EQ(packet.read<uint32>(12), 60000u);
    p.learned.insert(1001); p.m_coaCombatSpells.insert(1001);
    p.learned.insert(807749); p.m_coaCombatSpells.insert(807749);
    p.activeVow = 807749; p.weaponMask = 1;
    Unit target; target.identity.guid = 200; p.other = &target;
    auto e = Input(p, 1001, EventKind::DirectSpell);
    e.target = CoaCombatIntegration::Identify(target); e.targetPolicy = Target::Enemy;
    e.castId = 1; e.direct = e.damage = true; e.nativeEventMask = 0x10; e.effectiveAmount = 3;
    p.childResult = SPELL_FAILED_IMMUNE;
    CoaCombatTransaction tx(p, e);
    CHECK(tx.Prepare() == SPELL_FAILED_IMMUNE);
    CHECK(!tx.Publish());
    CHECK_EQ(dawn->m_procCharges, 10u);
    CHECK_EQ(p.nativePreflights, 1u);
}

TEST(CoaCombatAdapter_actual_scheduler_target_incarnation_and_original_identity)
{
    Definitions();
    Player p; p.playerClass = 31; p.m_coaCombatSpells.insert(560155);
    Unit target; target.identity.guid = 200; p.other = &target;
    auto e = Input(p, 560155);
    Effect slot; slot.type = 183; slot.child = 680448; slot.calculatedValue = 500;
    slot.target = CoaCombatIntegration::Identify(target); slot.targetPolicy = Target::Enemy;
    e.effects.Push(slot);
    REQUIRE(p.OnCoaCombatEvent(e)); CHECK_EQ(p.m_coaCombat.pending.size, 1u);
    auto old = target.GetCombatEpoch();
    target.AdvanceCombatEpoch(); // Same GUID, different incarnation, real epoch allocator.
    CHECK(old != target.GetCombatEpoch());
    p.m_Events.now = 500;
    p.UpdateCoaCombatRules();
    CHECK_EQ(p.m_coaCombat.pending.size, 0u);
    CHECK_EQ(p.nativePreflights, 0u);
    e.source.guid = 999;
    CHECK(!p.OnCoaCombatEvent(e));
}

TEST(CoaCombatAdapter_removed_holder_and_owner_epoch_invalidate_preparation)
{
    Definitions();
    Player p; p.learned.insert(804584); p.m_coaCombatSpells.insert(804584);
    Seed(p, 500149, 20);
    CoaCombatTransaction tx(p, Input(p, 804584));
    REQUIRE(tx.Prepare() == SPELL_CAST_OK);
    auto* old = p.GetSpellAuraHolder(500149, p.GetObjectGuid());
    p.holders.erase({500149, p.identity.guid});
    delete old;
    CHECK(!tx.Publish()); // Must not dereference the old prepared holder pointer.
    Seed(p, 500149, 20);
    CoaCombatTransaction later(p, Input(p, 804584));
    REQUIRE(later.Prepare() == SPELL_CAST_OK);
    p.AdvanceCombatEpoch();
    CHECK(!later.Publish());
    CHECK(!p.GetSpellAuraHolder(807440, p.GetObjectGuid()));
}

TEST(CoaCombatAdapter_native_fire_immunity_cancels_instead_of_retrying_late)
{
    Definitions();
    Player p; p.playerClass = 31; p.m_coaCombatSpells.insert(560155);
    Unit target; target.identity.guid = 200; p.other = &target;
    auto e = Input(p, 560155);
    Effect slot; slot.type = 183; slot.child = 680448; slot.calculatedValue = 500;
    slot.target = CoaCombatIntegration::Identify(target); slot.targetPolicy = Target::Enemy;
    e.effects.Push(slot);
    REQUIRE(p.OnCoaCombatEvent(e));
    p.childResult = SPELL_FAILED_IMMUNE;
    p.m_Events.now = 500;
    p.UpdateCoaCombatRules();
    CHECK_EQ(p.m_coaCombat.pending.size, 0u);
    CHECK_EQ(p.nativePreflights, 1u);
    p.childResult = SPELL_CAST_OK;
    p.m_Events.now = 600;
    p.UpdateCoaCombatRules();
    CHECK_EQ(p.nativePreflights, 1u);
}

TEST(CoaCombatAdapter_positive_unsupported_aura_is_not_a_visual_counter)
{
    Definitions();
    Player p; p.playerClass = 25; p.m_coaCombatSpells.insert(520773);
    auto& info = sSpellStore.rows[500706];
    info.Effect[1] = 6; info.EffectAura[1] = 317; info.amounts[1] = 10;
    auto e = Input(p, 520773);
    Effect slot; slot.slot = 1; slot.type = 175; slot.child = 500706; slot.target = e.target;
    e.effects.Push(slot);
    CoaCombatTransaction positive(p, e);
    CHECK(positive.Prepare() == SPELL_FAILED_IMMUNE);
    CHECK(!positive.Publish()); CHECK(p.holders.empty());
    info.amounts[1] = 0;
    CoaCombatTransaction zero(p, e);
    REQUIRE(zero.Prepare() == SPELL_CAST_OK);
    CHECK(zero.Publish());
    auto* insanity = p.GetSpellAuraHolder(500706, p.GetObjectGuid());
    REQUIRE(insanity);
    CHECK(!insanity->auras[1]); // No fabricated server Dummy aura for317.
    CHECK_EQ(insanity->m_stackAmount, 3u);
}

TEST(CoaCombatAdapter_native_draconic_entry_4039_is_spell_300755_not_spell_4039)
{
    auto path = std::getenv("ASCENDER_COA_TSV");
    auto pin = std::getenv("ASCENDER_COA_SHA256");
    if (!path || !pin)
    {
        std::printf("  SKIP native CoA artifact: set explicit ASCENDER_COA_TSV/ASCENDER_COA_SHA256\n");
        return;
    }
    auto catalog = coa::Catalog::Load(path, pin);
    CHECK_EQ(catalog->EntrySpell(4039, 1, 60).spell, 300755u);
    Player selected; selected.playerClass = 24;
    selected.m_coaBuild = catalog->Authorize(24, 60, 39, {});
    selected.learned.insert(300755);
    CHECK(selected.KnowsCoaCombatSpell(300755));
    CHECK_EQ(FindResource(807389)->knownMarker, 300755u);
    selected.m_coaBuild = catalog->Authorize(24, 60, 0, {});
    CHECK(!selected.KnowsCoaCombatSpell(300755)); // Even a leftover broad grant is insufficient.
    CHECK(!selected.KnowsCoaCombatSpell(4039));
}

TEST(CoaCombatAdapter_review_callback_ownership_and_same_event_queue_reclamation)
{
    Definitions();
    Player p; p.playerClass = 31; p.m_coaCombatSpells.insert(560155); p.m_coaCombatSpells.insert(573215);
    Unit target; target.identity.guid = 200; p.other = &target;
    auto e = Input(p, 560155);
    Effect slot; slot.type = 183; slot.child = 680448; slot.calculatedValue = 500;
    slot.target = CoaCombatIntegration::Identify(target); slot.targetPolicy = Target::Enemy;
    e.effects.Push(slot);
    for (size_t i = 0; i < MaxPending; ++i) { REQUIRE(p.OnCoaCombatEvent(e)); }
    target.alive = false; p.m_Events.now = 100;
    auto replacement = Input(p, 573215);
    Effect gain; gain.type = 175; gain.child = 806068; gain.target = replacement.target;
    Effect delay; delay.slot = 1; delay.type = 183; delay.child = 680472;
    delay.calculatedValue = 100; delay.target = replacement.target;
    replacement.effects.Push(gain); replacement.effects.Push(delay);
    REQUIRE(p.OnCoaCombatEvent(replacement));
    CHECK_EQ(p.m_coaCombat.pending.size, 1u);
    auto* actual = p.GetSpellAuraHolder(806068, p.GetObjectGuid());
    REQUIRE(actual); CHECK_EQ(actual->m_stackAmount, 2u);
    auto remove = Input(p, 0, EventKind::RemoveAura);
    remove.resource = 806068;
    remove.source = remove.originalCaster = CoaCombatIntegration::Identify(target);
    CHECK(!p.OnCoaCombatEvent(remove));
    CHECK(p.GetSpellAuraHolder(806068, p.GetObjectGuid()) == actual);
    remove.originalCaster = CoaCombatIntegration::Identify(p);
    CHECK(p.OnCoaCombatEvent(remove));
    CHECK(!p.GetSpellAuraHolder(806068, p.GetObjectGuid()));
}

TEST(CoaCombatAdapter_destination_job_reaches_native_preflight_once)
{
    Definitions(); sSpellStore.rows[803745].ID = 803745;
    Player p; p.playerClass = 32; p.m_coaCombatSpells.insert(802631);
    auto e = Input(p, 802631);
    Effect slot; slot.type = 183; slot.child = 803745; slot.calculatedValue = 1000;
    Destination d; d.owner = e.source; d.phase = 1; d.mode = DestinationMode::PersistentArea;
    d.x = 4; d.y = 3; slot.location = d;
    e.effects.Push(slot);
    REQUIRE(p.OnCoaCombatEvent(e));
    CHECK_EQ(p.nativePreflights, 0u); CHECK_EQ(p.m_coaCombat.pending.size, 1u);
    p.m_Events.now = 1000; p.UpdateCoaCombatRules();
    CHECK_EQ(p.nativePreflights, 1u); CHECK_EQ(p.m_coaCombat.pending.size, 0u);
    p.UpdateCoaCombatRules(); CHECK_EQ(p.nativePreflights, 1u);
}
