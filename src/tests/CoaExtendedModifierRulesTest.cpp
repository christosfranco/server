// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "CoaExtendedModifierRules.h"
#include "StatSystem.h"

#include <cmath>
#include <limits>
#include <set>

using namespace coa::modifier;

namespace
{
    NativeModifier Native(uint32_t aura, int32_t amount, uint32_t spell = 700001)
    {
        NativeModifier n;
        n.spell = spell;
        n.effect = 6;
        n.aura = aura;
        n.family = 33;
        n.inputDigest[0] = 66;
        n.amount.basePoints = amount - 1;
        n.amount.dieSides = 1;
        return n;
    }
    SourceKey Key(NativeModifier const& n)
    {
        return {{1, 1}, {1, 1}, 1, n.spell, n.slot};
    }
    SourceContext Context()
    {
        SourceContext c;
        c.amount.level = 60;
        c.stacks = 1;
        c.authorize = [](void const*, SourceKey const&, NativeModifier const& n)
        {
            return n.inputDigest[0] == 66;
        };
        return c;
    }
    Query For(Field field)
    {
        Query q;
        q.field = field;
        q.actor = {1, 1};
        q.target = {2, 1};
        q.family = 33;
        q.spell = 800001;
        q.familyFlags = {0, 0, 0x80000000u};
        q.schoolMask = 4;
        q.damage = true;
        return q;
    }
    void Add(Ledger& ledger, NativeModifier const& n, SourceContext const& c = Context())
    {
        auto r = Evaluate(n, Key(n), c);
        REQUIRE(r.Result() == Status::Ready);
        REQUIRE(ledger.Replace(r) == Status::Ready);
    }
}

TEST(CoaModifier_NativeAmountSignedScalingAndNoUninitializedCurve)
{
    NativeAmount n;
    n.basePoints = 9;
    n.dieSides = 1;
    n.baseLevel = 10;
    n.spellLevel = 10;
    n.maxLevel = 40;
    n.perLevel = 1.5;
    n.perCombo = -2;
    AmountContext c;
    CHECK(CalculateAmount(n, c).status == Status::Invalid);
    c.level = 60;
    c.comboPoints = 3;
    CHECK_EQ(CalculateAmount(n, c).value, 49.0);
    n.needsNativeScalar = true;
    CHECK(CalculateAmount(n, c).status == Status::MissingContext);
    for (double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        c.nativeScalar = bad;
        CHECK(CalculateAmount(n, c).status == Status::Invalid);
    }
    c.nativeScalar = 2;
    CHECK_EQ(CalculateAmount(n, c).value, 98.0);
    n.basePoints = -61;
    CHECK_EQ(CalculateAmount(n, c).value, -42.0);
    n.dieSides = -3;
    CHECK(CalculateAmount(n, c).status == Status::MissingContext);
    c.dieRoll = -3;
    CHECK_EQ(CalculateAmount(n, c).value, -50.0);
    c.dieRoll = -4;
    CHECK(CalculateAmount(n, c).status == Status::Invalid);
    c.level = 61;
    CHECK(CalculateAmount(n, c).status == Status::Invalid);
}

TEST(CoaModifier_AllHitIsNotCritAndKeepsNativeBaseMiss)
{
    Ledger ledger;
    auto n = Native(333, 6);
    n.miscA = 32;
    n.effectMask = {0x4000000, 0, 0};
    Add(ledger, n);
    auto q = For(Field::HitChance);
    q.family = 19;
    for (Attack attack : {Attack::Melee, Attack::Offhand, Attack::Ranged, Attack::Spell})
    {
        q.attack = attack;
        CHECK_EQ(ledger.Calculate(q, 90).value, 96.0);
        CHECK_EQ(ledger.Calculate(q, 99).value, 100.0);
    }
    CHECK_EQ(ledger.Calculate(For(Field::CritChance), 12).value, 12.0);
    CHECK_EQ(MissChance(5, 3, Attack::Melee).value, 2.0);
    CHECK_EQ(MissChance(24, 6, Attack::Offhand).value, 18.0);
    CHECK_EQ(MissChance(17, 16, Attack::Spell).value, 1.0);
    CHECK_EQ(MissChance(17, 17, Attack::Spell).value, 0.0);
    CHECK_EQ(MissChance(17, 100, Attack::Spell).value, 0.0);
    CHECK_EQ(MissChance(5, -200, Attack::Ranged).value, 100.0);
    CHECK(MissChance(-1, 0, Attack::Spell).status == Status::Invalid);
}

TEST(CoaModifier_WitchblightChargeIsExplicitNotPermanentMinus100)
{
    Ledger ledger;
    auto n = Native(333, -100, 681218);
    Add(ledger, n);
    auto q = For(Field::HitChance);
    auto result = ledger.Calculate(q, 100);
    CHECK_EQ(result.value, 0.0);
    REQUIRE(result.forcedMissSource.has_value());
    CHECK(result.flags & ForcedMiss);
    q.damage = false;
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    REQUIRE(ledger.Remove(*result.forcedMissSource));
    CHECK(!ledger.Remove(*result.forcedMissSource));
    q.damage = true;
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
}

TEST(CoaModifier_AttackPowerMeleeAndRangedAreIndependent)
{
    Ledger ledger;
    auto n = Native(344, 150);
    Add(ledger, n);
    auto penalty = Native(344, -15, 578318);
    Add(ledger, penalty);
    CHECK_EQ(ledger.Calculate(For(Field::MeleeAttackPower), 1000).value, 1135.0);
    CHECK_EQ(ledger.Calculate(For(Field::RangedAttackPower), 2000).value, 2150.0);
    CHECK_EQ(ledger.Calculate(For(Field::DamageSpellPower), 300).value, 300.0);
    REQUIRE(ledger.Remove(Key(n)));
    CHECK_EQ(ledger.Calculate(For(Field::MeleeAttackPower), 1000).value, 985.0);
    CHECK_EQ(ledger.Calculate(For(Field::RangedAttackPower), 2000).value, 2000.0);
}

TEST(CoaModifier_SpellPowerSchoolsAndDerivedReplacementNoFeedback)
{
    Ledger ledger;
    auto n = Native(345, 20);
    n.miscA = 6;
    Add(ledger, n);
    auto q = For(Field::DamageSpellPower);
    CHECK_EQ(ledger.Calculate(q, 100).value, 120.0);
    q.field = Field::HealingSpellPower;
    CHECK_EQ(ledger.Calculate(q, 100).value, 120.0);
    q.schoolMask = 32;
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    n = Native(345, 0, 301353);
    n.miscA = 127;
    auto c = Context();
    CHECK(Evaluate(n, Key(n), c).Result() == Status::MissingContext);
    c.armorPenetrationRating = 240;
    Add(ledger, n, c);
    CHECK_EQ(ledger.Calculate(q, 100).value, 340.0);
    c.armorPenetrationRating = 300;
    for (int i = 0; i < 30; ++i) { Add(ledger, n, c); }
    CHECK_EQ(ledger.Calculate(q, 100).value, 400.0);
    REQUIRE(ledger.Remove(Key(n)));
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
}

TEST(CoaModifier_PartyBuffStrongestPerStatisticAndRank)
{
    Ledger ledger;
    auto low = Native(344, 65, 705070);
    low.rankRoot = 705070; low.rank = 1; low.group = Group::PartyPower;
    auto high = Native(344, 220, 567234);
    high.rankRoot = 705070; high.rank = 2; high.group = Group::PartyPower;
    Add(ledger, low);
    Add(ledger, high);
    auto sp = Native(345, 95, 704540);
    sp.group = Group::PartyPower;
    Add(ledger, sp);
    auto personal = Native(344, 30, 707000);
    Add(ledger, personal);
    CHECK_EQ(ledger.Calculate(For(Field::MeleeAttackPower), 0).value, 250.0);
    CHECK_EQ(ledger.Calculate(For(Field::HealingSpellPower), 0).value, 95.0);
    auto other = Native(344, 190, 806434);
    other.group = Group::PartyPower;
    auto foreign = Key(other); foreign.caster = {3, 1};
    REQUIRE(ledger.Replace(Evaluate(other, foreign, Context())) == Status::Ready);
    CHECK_EQ(ledger.Calculate(For(Field::MeleeAttackPower), 0).value, 250.0);
    REQUIRE(ledger.Remove(Key(high)));
    CHECK_EQ(ledger.Calculate(For(Field::MeleeAttackPower), 0).value, 220.0);
    REQUIRE(ledger.Remove(foreign));
    CHECK_EQ(ledger.Calculate(For(Field::MeleeAttackPower), 0).value, 95.0);
}

TEST(CoaModifier_NativeStacksAndRefreshRemovalNeverDrift)
{
    Ledger ledger;
    auto n = Native(344, -23);
    n.stackCap = 5;
    auto c = Context(); c.stacks = 3;
    Add(ledger, n, c);
    for (int i = 0; i < 100; ++i)
    {
        Add(ledger, n, c);
        CHECK_EQ(ledger.Size(), std::size_t(1));
        CHECK_EQ(ledger.Calculate(For(Field::MeleeAttackPower), 1000).value, 931.0);
    }
    auto next = Key(n); next.application = 2;
    c.stacks = 4;
    REQUIRE(ledger.Replace(Evaluate(n, next, c)) == Status::Ready);
    CHECK_EQ(ledger.Size(), std::size_t(1));
    CHECK(!ledger.Remove(Key(n)));
    CHECK(ledger.Replace(Evaluate(n, Key(n), c)) == Status::Stale);
    CHECK_EQ(ledger.Calculate(For(Field::MeleeAttackPower), 1000).value, 908.0);
    REQUIRE(ledger.Remove(next));
    CHECK_EQ(ledger.Calculate(For(Field::MeleeAttackPower), 1000).value, 1000.0);
    c.stacks = 0;
    CHECK(Evaluate(n, next, c).Result() == Status::Invalid);
    c.stacks = 6;
    CHECK(Evaluate(n, next, c).Result() == Status::Invalid);
}

TEST(CoaModifier_AbsorbSummedSnapshotAndEffectiveConsumption)
{
    Ledger ledger;
    auto n = Native(317, 20, 582308); n.miscA = 127;
    Add(ledger, n);
    auto second = Native(317, 25, 302072); second.miscA = 127;
    second.effectMask = {0, 0, 0x80000000u};
    Add(ledger, second);
    auto q = For(Field::AbsorbCapacity); q.damageShield = true;
    CHECK_EQ(ledger.Calculate(q, 100).value, 145.0);
    q.family = 32;
    CHECK_EQ(ledger.Calculate(q, 100).value, 120.0);
    q.family = 33;
    double snapshot = ledger.Calculate(q, 100).value;
    ledger.Clear();
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    auto used = ConsumeShield(snapshot, 70);
    CHECK(used.status == Status::Ready);
    CHECK_EQ(used.consumed, 70u);
    CHECK_EQ(used.remaining, 75u);
    CHECK_EQ(used.damage, 0u);
    used = ConsumeShield(used.remaining, 90);
    CHECK_EQ(used.consumed, 75u);
    CHECK_EQ(used.remaining, 0u);
    CHECK_EQ(used.damage, 15u);
    CHECK_EQ(ConsumeShield(0, 100).consumed, 0u);
    CHECK(ConsumeShield(0.5, 10).status == Status::Invalid);
}

TEST(CoaModifier_AbsorbZeroInsanityBaseOnlyAndPetScope)
{
    Ledger ledger;
    auto zero = Native(317, 0, 500706); zero.slot = 1; zero.stackCap = 100;
    zero.miscA = 127; zero.effectMask = {0, 0, 0x80000000u};
    auto c = Context(); c.stacks = 100;
    auto replacement = Evaluate(zero, Key(zero), c);
    REQUIRE(replacement.Result() == Status::Ready);
    CHECK_EQ(replacement.Contributions()[0].amount, 0.0);
    REQUIRE(ledger.Replace(replacement) == Status::Ready);
    auto q = For(Field::AbsorbCapacity); q.damageShield = true;
    CHECK_EQ(ledger.Calculate(q, 123).value, 123.0);
    CHECK_EQ(ledger.Calculate(q, 123).matched, std::size_t(1));
    auto base = Native(317, 100, 681088); base.slot = 2;
    base.effectMask = {0, 0, 0x80000000u};
    replacement = Evaluate(base, Key(base), Context());
    REQUIRE(replacement.Result() == Status::Ready);
    CHECK_EQ(replacement.SuppressSlots(), 1u);
    REQUIRE(ledger.Replace(replacement) == Status::Ready);
    CHECK_EQ(ledger.Calculate(q, 123).value, 123.0);
    q.field = Field::AbsorbBase;
    CHECK_EQ(ledger.Calculate(q, 123).value, 246.0);
    q.damageShield = false;
    CHECK_EQ(ledger.Calculate(q, 123).value, 123.0);
    auto pet = Native(317, 10, 500167); pet.effect = 190; pet.slot = 1;
    auto key = Key(pet); key.holder = {3, 1};
    c = Context();
    CHECK(Evaluate(pet, key, c).Result() == Status::MissingContext);
    c.petOwner = key.caster;
    REQUIRE(ledger.Replace(Evaluate(pet, key, c)) == Status::Ready);
    q = For(Field::AbsorbCapacity); q.damageShield = true;
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    q.actor = key.holder;
    CHECK_EQ(ledger.Calculate(q, 100).value, 110.0);
}

TEST(CoaModifier_PetHealingPowerIsPercentNotFlatSpellPower)
{
    Ledger ledger;
    auto n = Native(345, 0, 808008); n.effect = 190; n.stackCap = 3; n.miscA = 127;
    auto key = Key(n); key.holder = {3, 1};
    auto c = Context(); c.petOwner = key.caster; c.stacks = 3;
    CHECK(Evaluate(n, key, c).Result() == Status::MissingContext);
    c.ownerHealingPower = 200;
    REQUIRE(ledger.Replace(Evaluate(n, key, c)) == Status::Ready);
    auto q = For(Field::DamageDone); q.actor = key.holder;
    CHECK_EQ(ledger.Calculate(q, 100).value, 160.0);
    q.field = Field::HealingDone;
    CHECK_EQ(ledger.Calculate(q, 100).value, 160.0);
    q.field = Field::DamageSpellPower;
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
}

TEST(CoaModifier_HealingReceivedAndHeraldAreNotTheSameDirection)
{
    Ledger ledger;
    auto n = Native(319, -20, 705019); n.miscA = 127;
    auto key = Key(n); key.holder = {2, 1};
    REQUIRE(ledger.Replace(Evaluate(n, key, Context())) == Status::Ready);
    auto q = For(Field::HealingTaken); q.damage = false; q.healing = true;
    CHECK_EQ(ledger.Calculate(q, 100).value, 80.0);
    q.actor = {3, 1};
    CHECK_EQ(ledger.Calculate(q, 100).value, 80.0);
    q.target = {4, 1};
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    auto herald = Native(319, 15, 520326); herald.miscA = 127;
    Add(ledger, herald);
    q = For(Field::DamageDone);
    CHECK_EQ(ledger.Calculate(q, 100).value, 115.0);
    q.field = Field::HealingDone;
    CHECK_EQ(ledger.Calculate(q, 100).value, 115.0);
    q.field = Field::HealingTaken; q.target = {1, 1};
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
}

TEST(CoaModifier_ConditionalThresholdsPreEventSchoolsAndNamedSelection)
{
    Ledger ledger;
    auto n = Native(303, 10, 300325); n.miscA = 13;
    CHECK(Evaluate(n, Key(n), Context()).Result() == Status::MissingContext);
    n.selectedSpells = {800001};
    Add(ledger, n);
    auto q = For(Field::DamageDone);
    CHECK(ledger.Calculate(q, 100).status == Status::MissingContext);
    q.targetState = TargetState{35, 100};
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    q.targetState->health = 34;
    CHECK_EQ(ledger.Calculate(q, 100).value, 110.0);
    q.spell = 800002;
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    ledger.Clear();
    n = Native(303, 25, 520034); n.miscA = 23; n.selectedSpells = {800002};
    Add(ledger, n);
    q.targetState->health = 75;
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    q.targetState->health = 76;
    CHECK_EQ(ledger.Calculate(q, 100).value, 125.0);
    q.targetState->health = 50; q.targetState->ownTranquilCircle = true;
    CHECK_EQ(ledger.Calculate(q, 100).value, 125.0);
    ledger.Clear();
    n = Native(303, 20); n.miscA = 28; n.miscB = 4;
    Add(ledger, n);
    q.targetState->health = 80;
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    q.targetState->health = 81;
    CHECK_EQ(ledger.Calculate(q, 100).value, 120.0);
    q.schoolMask = 32;
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
}

TEST(CoaModifier_BleedPoisonAndConditionalHealing)
{
    Ledger ledger;
    auto n = Native(303, 10, 705050); n.miscA = 18;
    Add(ledger, n);
    auto q = For(Field::DamageDone);
    q.targetState = TargetState{50, 100};
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    q.targetState->poisoned = true;
    CHECK_EQ(ledger.Calculate(q, 100).value, 110.0);
    ledger.Clear();
    n.spell = 705733;
    Add(ledger, n);
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    q.targetState->bleeding = true;
    CHECK_EQ(ledger.Calculate(q, 100).value, 110.0);
    n = Native(360, 25, 560561); n.miscA = 13;
    Add(ledger, n);
    q.field = Field::HealingDone; q.targetState->health = 30;
    CHECK_EQ(ledger.Calculate(q, 100).value, 125.0);
}

TEST(CoaModifier_CasterSpecificCritArmorAndPeriodicReception)
{
    for (uint32_t aura : {308u, 330u, 338u, 214u, 351u})
    {
        Ledger ledger;
        auto n = Native(aura, 20);
        n.miscA = aura == 308 || aura == 338 ? 0 : 4;
        n.miscB = aura == 214 ? 1 : 0;
        auto key = Key(n); key.holder = {2, 1};
        REQUIRE(ledger.Replace(Evaluate(n, key, Context())) == Status::Ready);
        Field field = aura == 338 ? Field::Armor : aura == 214 ? Field::DamageTaken :
            aura == 351 ? Field::HealingTaken : Field::CritChance;
        auto q = For(field); q.periodic = true;
        CHECK_EQ(ledger.Calculate(q, 50).value, aura == 338 ? 40.0 : aura == 214 || aura == 351 ? 60.0 : 70.0);
        q.actor = {3, 1};
        CHECK_EQ(ledger.Calculate(q, 50).value, 50.0);
        q.actor = {1, 2};
        CHECK_EQ(ledger.Calculate(q, 50).value, 50.0);
        q.actor = {1, 1}; q.schoolMask = 32;
        if (aura == 330 || aura == 214 || aura == 351) { CHECK_EQ(ledger.Calculate(q, 50).value, 50.0); }
        q.schoolMask = 4; q.periodic = false;
        if (aura == 214 || aura == 351) { CHECK_EQ(ledger.Calculate(q, 50).value, 50.0); }
        ledger.Clear();
        CHECK_EQ(ledger.Calculate(q, 50).value, 50.0);
    }
}

TEST(CoaModifier_StatAndPoolInputsExcludeDerivedFeedback)
{
    Ledger ledger;
    auto n = Native(327, 100, 92132); n.miscA = 3; n.miscB = 0;
    auto c = Context();
    CHECK(Evaluate(n, Key(n), c).Result() == Status::MissingContext);
    c.stats[0] = 200;
    Add(ledger, n, c);
    CHECK_EQ(ledger.Calculate(For(Field::Intellect), 100).value, 300.0);
    n = Native(328, 150, 300351); n.miscA = 0; n.miscB = 3;
    c.stats[3] = 40;
    CHECK(Evaluate(n, Key(n), c).Result() == Status::MissingContext);
    c.manaProvisioned = true;
    Add(ledger, n, c);
    CHECK_EQ(ledger.Calculate(For(Field::MaxMana), 1000).value, 1060.0);
    ledger.Clear();
    n = Native(328, 200, 560080); n.miscA = 0; n.miscB = 3;
    CHECK(Evaluate(n, Key(n), c).Result() == Status::MissingContext);
    c.nativeManaFromIntellect = StatSystem::CalculateManaBonusFromIntellect(40);
    Add(ledger, n, c);
    CHECK_EQ(ledger.Calculate(For(Field::MaxMana), 1000).value, 1640.0);
    n.miscA = -2; n.slot = 1; c.healthProvisioned = true;
    Add(ledger, n, c);
    CHECK_EQ(ledger.Calculate(For(Field::MaxHealth), 1000).value, 1080.0);
    c.healthProvisioned = false;
    REQUIRE(ledger.Replace(Evaluate(n, Key(n), c)) == Status::Inactive);
    CHECK_EQ(ledger.Calculate(For(Field::MaxHealth), 1000).value, 1000.0);
}

TEST(CoaModifier_FlatAndPercentSpellmodsUseOriginalBaseAndAll96Bits)
{
    for (int op : {0, 1, 10, 11, 14, 34})
    {
        Ledger ledger;
        auto flat = Native(107, -3, 700010); flat.miscA = op;
        flat.effectMask = {0, 0, 0x80000000u};
        auto percent = Native(108, -20, 700011); percent.miscA = op;
        percent.effectMask = flat.effectMask;
        Add(ledger, flat); Add(ledger, percent);
        auto q = For(SpellModField(op, flat.spell));
        CHECK_EQ(ledger.Calculate(q, 100).value, 77.0);
        q.family = 32;
        CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
        q.family = 33; q.familyFlags = {0x80000000u, 0, 0};
        CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
        q.familyFlags = {0, 0, 0x80000000u};
        REQUIRE(ledger.Remove(Key(percent)));
        CHECK_EQ(ledger.Calculate(q, 100).value, 97.0);
        REQUIRE(ledger.Remove(Key(flat)));
        CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    }
    Ledger ledger;
    auto n = Native(108, -150); n.miscA = 34; n.effectMask = {0, 0, 0x80000000u};
    Add(ledger, n);
    CHECK_EQ(ledger.Calculate(For(Field::TargetCount), 5).value, 1.0);
}

TEST(CoaModifier_AllExtendedSpellmodOperationClassificationsLiteral)
{
    CHECK(SpellModField(32, 706671) == Field::AttackPowerCoefficient);
    CHECK(SpellModField(34, 680797) == Field::TargetCount);
    CHECK(SpellModField(37, 504003) == Field::CriticalHealingBonus);
    CHECK(SpellModField(37, 504694) == Field::CriticalDamageBonus);
    CHECK(SpellModField(38, 807047) == Field::DamageDone);
    CHECK(SpellModField(40, 300685) == Field::SpellPowerCoefficient);
    CHECK(SpellModField(41, 300284) == Field::SpellPowerCoefficient);
    CHECK(SpellModField(42, 705061) == Field::RangedAttackPowerCoefficient);
    CHECK(SpellModField(45, 705061) == Field::RangedAttackPowerCoefficient);
    for (int op : {31, 33, 35, 36, 39, 43, 44, 46, -1, 20007})
    {
        CHECK(SpellModField(op, 700001) == Field::Count);
    }
}

TEST(CoaModifier_CoefficientsBeforeDamageNoNewTermFromZero)
{
    for (int op : {32, 40, 41, 42, 45})
    {
        Ledger ledger;
        auto flat = Native(107, 25, 706671); flat.miscA = op;
        flat.effectMask = {0, 0, 0x80000000u};
        auto pct = Native(108, 50, 704988); pct.miscA = op; pct.effectMask = flat.effectMask;
        Add(ledger, flat); Add(ledger, pct);
        auto q = For(SpellModField(op, flat.spell));
        q.periodic = op == 40 || op == 45;
        CHECK(ledger.Calculate(q, 0.5).status == Status::MissingContext);
        q.hasNativeCoefficient = false;
        CHECK_EQ(ledger.Calculate(q, 0.5).value, 0.5);
        q.hasNativeCoefficient = true;
        CHECK_EQ(ledger.Calculate(q, 0.5).value, 1.0);
        CHECK_EQ(ledger.Calculate(q, 0).value, 0.0);
        if (op != 32)
        {
            q.periodic = !q.periodic;
            CHECK_EQ(ledger.Calculate(q, 0.5).value, 0.5);
        }
    }
}

TEST(CoaModifier_CritHealingExceptionAndNonPlayerOpponents)
{
    Ledger ledger;
    auto n = Native(108, 50, 504003); n.miscA = 37; n.effectMask = {0, 0, 0x80000000u};
    Add(ledger, n);
    auto q = For(Field::CriticalHealingBonus); q.healing = true; q.damage = false;
    CHECK_EQ(ledger.Calculate(q, 50).value, 50.0);
    q.critical = true;
    CHECK_EQ(ledger.Calculate(q, 50).value, 75.0);
    n.spell = 504694;
    Add(ledger, n);
    q.field = Field::CriticalDamageBonus;
    CHECK_EQ(ledger.Calculate(q, 50).value, 50.0);
    q.healing = false; q.damage = true;
    CHECK_EQ(ledger.Calculate(q, 50).value, 75.0);
    n.spell = 807047; n.miscA = 38;
    Add(ledger, n);
    q.field = Field::DamageDone;
    CHECK(ledger.Calculate(q, 100).status == Status::MissingContext);
    q.playerOpponent = false;
    CHECK_EQ(ledger.Calculate(q, 100).value, 150.0);
    q.playerOpponent = true; // Includes player-owned pets, not just player unit type.
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
}

TEST(CoaModifier_NumericNegativeSuppressorsRecomputeWithoutDivision)
{
    Ledger ledger;
    auto n = Native(319, -100);
    auto q = For(Field::HealingTaken); q.target = {1, 1};
    Add(ledger, n);
    auto positive = Native(319, 50, 700002); Add(ledger, positive);
    CHECK_EQ(ledger.Calculate(q, 100).value, 0.0);
    REQUIRE(ledger.Remove(Key(n)));
    CHECK_EQ(ledger.Calculate(q, 100).value, 150.0);
    n.amount.basePoints = -26;
    Add(ledger, n);
    CHECK_EQ(ledger.Calculate(q, 100).value, 112.5);
    REQUIRE(ledger.Remove(Key(positive)));
    CHECK_EQ(ledger.Calculate(q, 100).value, 75.0);
    REQUIRE(ledger.Remove(Key(n)));
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
}

TEST(CoaModifier_SchoolCritNotHitAndBadDerivedStatsDenied)
{
    Ledger ledger;
    auto n = Native(350, 8, 680514); n.miscA = 32;
    Add(ledger, n);
    auto q = For(Field::CritChance);
    CHECK_EQ(ledger.Calculate(q, 10).value, 10.0);
    q.schoolMask = 32;
    CHECK_EQ(ledger.Calculate(q, 10).value, 18.0);
    q.field = Field::HitChance;
    CHECK_EQ(ledger.Calculate(q, 10).value, 10.0);
    n = Native(345, 0, 301353);
    auto c = Context();
    for (double invalid : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        c.armorPenetrationRating = invalid;
        CHECK(Evaluate(n, Key(n), c).Result() == Status::Invalid);
    }
}

TEST(CoaModifier_SelectedMovementChannelHasteAndRangeFlags)
{
    for (uint32_t aura : {312u, 313u, 316u, 353u})
    {
        Ledger ledger;
        auto n = Native(aura, 0); n.effectMask = {0, 0, 0x80000000u};
        Add(ledger, n);
        auto q = For(aura == 312 ? Field::MinimumRange : aura == 313 ? Field::CastWhileMoving :
            aura == 316 ? Field::PeriodicHaste : Field::CastDuringChannel);
        q.periodic = true; q.channeling = true; q.instant = true;
        auto result = ledger.Calculate(q, 5);
        CHECK(result.status == Status::Ready);
        CHECK_EQ(result.flags, aura == 312 ? uint32_t(NoMinimumRange) : aura == 313 ? uint32_t(MovingCast) :
            aura == 316 ? uint32_t(HastedPeriodic) : uint32_t(PreserveChannel));
        q.family = 32;
        CHECK_EQ(ledger.Calculate(q, 5).flags, 0u);
        q.family = 33; q.instant = false;
        if (aura == 353) { CHECK_EQ(ledger.Calculate(q, 5).flags, 0u); }
    }
    CHECK_EQ(HastedTickRemaining(3000, 50, 0.5).value, 1000.0);
    CHECK_EQ(HastedTickRemaining(3000, -25, 0.25).value, 3000.0);
    CHECK(HastedTickRemaining(3000, -100, 0).status == Status::Invalid);
    CHECK(HastedTickRemaining(3000, 0, 1).status == Status::Invalid);
}

TEST(CoaModifier_ChannelDefenseAreaBoundaryAndInstantManaCost)
{
    Ledger ledger;
    auto n = Native(348, 1, 800206); Add(ledger, n);
    auto q = For(Field::ChannelDefense);
    CHECK_EQ(ledger.Calculate(q, 0).flags, 0u);
    q.channeling = true;
    CHECK(ledger.Calculate(q, 0).flags & ChannelAvoidance);
    n = Native(336, 0, 805756);
    auto c = Context();
    CHECK(Evaluate(n, Key(n), c).Result() == Status::MissingContext);
    c.area = 42; Add(ledger, n, c);
    q = For(Field::AreaTargetBoundary);
    CHECK(ledger.Calculate(q, 0).status == Status::MissingContext);
    q.areaState = AreaState{42, false, true, true, true};
    CHECK(ledger.Calculate(q, 0).flags & BlockTarget);
    q.areaState->actorInside = true;
    CHECK_EQ(ledger.Calculate(q, 0).flags, 0u);
    q.areaState->actorInside = false; q.areaState->targeted = false;
    CHECK_EQ(ledger.Calculate(q, 0).flags, 0u);
    n = Native(357, 10, 574309); Add(ledger, n);
    q = For(Field::Cost); q.instant = true; q.manaCost = true;
    CHECK_EQ(ledger.Calculate(q, 100).value, 90.0);
    q.manaCost = false;
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
    q.manaCost = true; q.instant = false;
    CHECK_EQ(ledger.Calculate(q, 100).value, 100.0);
}

TEST(CoaModifier_UnsupportedAndMalformedFailClosedWithoutChangingLedger)
{
    Ledger ledger;
    auto n = Native(344, 25); Add(ledger, n);
    auto c = Context(); c.authorize = nullptr;
    CHECK(ledger.Replace(Evaluate(n, Key(n), c)) == Status::Unauthorized);
    c = Context(); n.inputDigest[0] = 67;
    CHECK(ledger.Replace(Evaluate(n, Key(n), c)) == Status::Unauthorized);
    n.inputDigest = {};
    CHECK(Evaluate(n, Key(n), c).Result() == Status::Invalid);
    n = Native(344, 25); n.amountLimit = 24;
    CHECK(ledger.Replace(Evaluate(n, Key(n), c)) == Status::Limit);
    n.amountLimit = ValueBudget; n.amount.perLevel = std::numeric_limits<double>::infinity();
    CHECK(Evaluate(n, Key(n), c).Result() == Status::Invalid);
    n = Native(344, 25); n.effect = 3;
    CHECK(Evaluate(n, Key(n), c).Result() == Status::Unsupported);
    for (uint32_t aura : {0u, 164u, 347u, 354u, 999u})
    {
        n = Native(aura, 1);
        CHECK(Evaluate(n, Key(n), c).Result() == Status::Unsupported);
    }
    n = Native(303, 10); n.miscA = 999;
    CHECK(Evaluate(n, Key(n), c).Result() == Status::Unsupported);
    CHECK_EQ(ledger.Calculate(For(Field::MeleeAttackPower), 100).value, 125.0);
    CHECK(ledger.Calculate(For(Field::MeleeAttackPower), ValueBudget).status == Status::Limit);
    CHECK(ledger.Calculate(For(Field::MeleeAttackPower), std::numeric_limits<double>::quiet_NaN()).status == Status::Invalid);
    CHECK(ledger.Calculate(For(Field(-1)), 0).status == Status::Invalid);
    CHECK(MissChance(5, 0, Attack(-1)).status == Status::Invalid);
    n = Native(340, 0); Add(ledger, n);
    CHECK_EQ(ledger.Calculate(For(Field::Metadata), 0).value, 0.0);
    n.amount.basePoints = 0;
    CHECK(Evaluate(n, Key(n), c).Result() == Status::Invalid);
}

TEST(CoaModifier_EquipmentMissingDoesNotMeanUnrestrictedAndBudgetIsBounded)
{
    Ledger ledger;
    auto n = Native(344, 10); n.equippedItemClass = 2;
    auto c = Context();
    CHECK(Evaluate(n, Key(n), c).Result() == Status::MissingContext);
    c.equipmentEligible = true; Add(ledger, n, c);
    c.equipmentEligible = false;
    auto refresh = Key(n); refresh.application = 2;
    CHECK(ledger.Replace(Evaluate(n, refresh, c)) == Status::Inactive);
    CHECK_EQ(ledger.Size(), std::size_t(0));
    n = Native(344, 1);
    for (std::size_t i = 0; i < MaxSources; ++i)
    {
        n.spell = uint32_t(700000 + i);
        Add(ledger, n);
    }
    n.spell = 800000;
    CHECK(ledger.Replace(Evaluate(n, Key(n), Context())) == Status::Limit);
    CHECK_EQ(ledger.Size(), MaxSources);
}

#ifdef COA_MODIFIER_CATALOG_FIXTURE
#include COA_MODIFIER_CATALOG_FIXTURE
TEST(CoaModifier_PrivateCatalogEveryDefinedDescriptorAdmitted)
{
    auto bindings = ModifierCatalogFixture();
    REQUIRE(bindings.size() == 412);
    std::set<uint32_t> auras, modRecords;
    std::size_t mods = 0, absorbs = 0;
    for (auto const& n : bindings)
    {
        auto key = Key(n);
        auto c = Context();
        c.authorize = [](void const*, SourceKey const&, NativeModifier const&) { return true; };
        c.amount.nativeScalar = 1;
        c.amount.dieRoll = 1;
        c.equipmentEligible = true;
        c.manaProvisioned = true; c.healthProvisioned = true;
        c.armorPenetrationRating = 100; c.ownerHealingPower = 200;
        c.nativeManaFromIntellect = 1220;
        for (auto& stat : c.stats) { stat = 100; }
        c.area = 42;
        if (n.effect == 190) { key.holder = {3, 1}; c.petOwner = key.caster; }
        auto result = Evaluate(n, key, c);
        if (result.Result() != Status::Ready)
        {
            testing::ReportFailure(__FILE__, __LINE__, "descriptor rejected " + std::to_string(n.spell) + "/" + std::to_string(n.slot));
        }
        if (n.aura == 107 || n.aura == 108) { ++mods; modRecords.insert(n.spell); }
        else { auras.insert(n.aura); }
        if (n.aura == 317) { ++absorbs; }
    }
    CHECK_EQ(auras.size(), std::size_t(23));
    CHECK_EQ(mods, std::size_t(33));
    CHECK_EQ(modRecords.size(), std::size_t(30));
    CHECK_EQ(absorbs, std::size_t(15));
}

TEST(CoaModifier_PrivateThirtyThreeSpellmodsLiteralValuesAndRemoval)
{
    struct Expected
    {
        uint32_t spell, slot;
        int32_t op;
        Field field;
        double base, value;
    };
    // Independently reviewed policy cases, not expectations computed by the compiler.
    Expected const cases[] =
    {
        {706671, 1, 32, Field::AttackPowerCoefficient, 1, 1.25},
        {704988, 1, 32, Field::AttackPowerCoefficient, 1, 1.25},
        {680797, 0, 34, Field::TargetCount, 4, 5},
        {704794, 0, 34, Field::TargetCount, 4, 5},
        {705068, 2, 34, Field::TargetCount, 4, 6},
        {705859, 1, 34, Field::TargetCount, 4, 1},
        {706477, 0, 34, Field::TargetCount, 4, 9},
        {706956, 1, 34, Field::TargetCount, 4, 9},
        {707894, 0, 34, Field::TargetCount, 4, 6},
        {800220, 0, 34, Field::TargetCount, 3, 20},
        {806081, 2, 34, Field::TargetCount, 4, 5},
        {504003, 0, 37, Field::CriticalHealingBonus, 100, 115},
        {504694, 2, 37, Field::CriticalDamageBonus, 100, 110},
        {807047, 1, 38, Field::DamageDone, 100, 115},
        {300685, 1, 40, Field::SpellPowerCoefficient, 1, 1.3},
        {570149, 0, 40, Field::SpellPowerCoefficient, 1, 1.15},
        {704175, 1, 40, Field::SpellPowerCoefficient, 1, 1},
        {807544, 1, 40, Field::SpellPowerCoefficient, 1, 1.2},
        {300284, 0, 41, Field::SpellPowerCoefficient, 1, 1.25},
        {300685, 0, 41, Field::SpellPowerCoefficient, 1, 1.2},
        {300995, 0, 41, Field::SpellPowerCoefficient, 1, 1.2},
        {560194, 1, 41, Field::SpellPowerCoefficient, 1, 1.15},
        {561349, 1, 41, Field::SpellPowerCoefficient, 1, 1.3},
        {680862, 0, 41, Field::SpellPowerCoefficient, 1, 1.25},
        {705667, 0, 41, Field::SpellPowerCoefficient, 1, 1.15},
        {707317, 0, 41, Field::SpellPowerCoefficient, 1, 1.25},
        {707793, 0, 41, Field::SpellPowerCoefficient, 1, 1.3},
        {804557, 1, 41, Field::SpellPowerCoefficient, 1, 1.25},
        {705061, 0, 42, Field::RangedAttackPowerCoefficient, 1, 1.15},
        {707884, 0, 42, Field::RangedAttackPowerCoefficient, 1, 1.3},
        {582855, 1, 45, Field::RangedAttackPowerCoefficient, 1, 1},
        {705061, 1, 45, Field::RangedAttackPowerCoefficient, 1, 1.15},
        {707884, 1, 45, Field::RangedAttackPowerCoefficient, 1, 1.3}
    };
    CHECK_EQ(std::size(cases), std::size_t(33));
    auto bindings = ModifierCatalogFixture();
    for (auto const& expected : cases)
    {
        auto found = std::find_if(bindings.begin(), bindings.end(), [&](NativeModifier const& n)
        {
            return n.spell == expected.spell && n.slot == expected.slot && (n.aura == 107 || n.aura == 108);
        });
        REQUIRE(found != bindings.end());
        CHECK_EQ(found->miscA, expected.op);
        auto c = Context();
        c.authorize = [](void const*, SourceKey const&, NativeModifier const&) { return true; };
        c.amount.nativeScalar = 1;
        c.amount.dieRoll = 1;
        c.equipmentEligible = true;
        Ledger ledger;
        REQUIRE(ledger.Replace(Evaluate(*found, Key(*found), c)) == Status::Ready);
        auto q = For(expected.field);
        q.family = found->family;
        q.familyFlags = found->effectMask;
        q.periodic = expected.op == 40 || expected.op == 45;
        q.critical = true;
        q.healing = expected.field == Field::CriticalHealingBonus;
        q.damage = !q.healing;
        q.hasNativeCoefficient = true;
        q.playerOpponent = false;
        double cap = expected.field == Field::TargetCount ? 20 : ValueBudget;
        auto result = ledger.Calculate(q, expected.base, cap);
        REQUIRE(result.status == Status::Ready);
        CHECK(std::abs(result.value - expected.value) < 0.000001);
        CHECK_EQ(result.matched, std::size_t(1));
        ++q.family;
        CHECK_EQ(ledger.Calculate(q, expected.base, cap).value, expected.base);
        --q.family;
        REQUIRE(ledger.Remove(Key(*found)));
        CHECK_EQ(ledger.Calculate(q, expected.base, cap).value, expected.base);
    }
}
#endif
