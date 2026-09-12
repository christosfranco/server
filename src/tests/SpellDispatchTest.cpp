// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestHarness.h"
#include "SpellDispatch.h"
#include "DBCEnums.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>

namespace
{
    struct FakeSpell
    {
        unsigned calls = 0;
        unsigned lowCalls = 0;
        unsigned nullCalls = 0;
        unsigned calculations = 0;

        void Effect(SpellEffectIndex)
        {
            ++calls;
        }
        void LowEffect(SpellEffectIndex)
        {
            ++lowCalls;
        }
        void EffectNULL(SpellEffectIndex)
        {
            ++nullCalls;
        }

        void Dispatch(uint32_t nativeType, SpellEffectIndex index)
        {
            if (!SpellDispatch::IsValidIndex(nativeType, handlers))
            {
                return;
            }
            ++calculations;
            (this->*handlers[nativeType])(index);
        }

        using Handler = void (FakeSpell::*)(SpellEffectIndex);
        Handler handlers[TOTAL_SPELL_EFFECTS];

        FakeSpell()
        {
            std::fill(std::begin(handlers), std::end(handlers), &FakeSpell::Effect);
            handlers[0] = &FakeSpell::EffectNULL;
            handlers[1] = &FakeSpell::LowEffect;
        }
    };

    struct FakeProc
    {
        enum class Result
        {
            Ok,
            Failed,
            CantTrigger
        };
        unsigned calls = 0;
        unsigned nullCalls = 0;
        unsigned cooldowns = 0;
        unsigned charges = 2;
        bool inUse = true;
        bool rejected = false;

        Result Proc()
        {
            ++calls;
            ++cooldowns;
            return Result::Ok;
        }
        Result HandleNULLProc()
        {
            ++nullCalls;
            return Result::Ok;
        }
        Result CantTrigger()
        {
            return Result::CantTrigger;
        }
        Result Failed()
        {
            return Result::Failed;
        }

        using Handler = Result (FakeProc::*)();
        Handler handlers[TOTAL_AURAS];

        FakeProc()
        {
            std::fill(std::begin(handlers), std::end(handlers), &FakeProc::Proc);
            handlers[0] = &FakeProc::HandleNULLProc;
            handlers[SPELL_AURA_MOD_POWER_REGEN] = &FakeProc::CantTrigger;
            handlers[SPELL_AURA_DUMMY] = &FakeProc::Failed;
        }

        void Dispatch(uint32_t const (&nativeTypes)[MAX_EFFECT_INDEX],
            unsigned presentMask = 7, unsigned eligibleMask = 7)
        {
            std::optional<uint32_t> auraTypes[MAX_EFFECT_INDEX];
            for (unsigned i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                if (presentMask & (1u << i))
                {
                    auraTypes[i] = nativeTypes[i];
                }
            }
            if (SpellDispatch::FirstInvalidIndex(auraTypes, handlers) < MAX_EFFECT_INDEX)
            {
                rejected = true;
                inUse = false;
                return;
            }
            bool procSuccess = true;
            bool anyAuraProc = false;
            for (unsigned i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                if (!auraTypes[i] || !(eligibleMask & (1u << i)))
                {
                    continue;
                }
                switch ((this->*handlers[*auraTypes[i]])())
                {
                    case Result::CantTrigger:
                        continue;
                    case Result::Failed:
                        procSuccess = false;
                        break;
                    case Result::Ok:
                        break;
                }
                anyAuraProc = true;
            }
            if (charges && procSuccess && anyAuraProc)
            {
                --charges;
            }
            inUse = false;
        }
    };
}

TEST(SpellDispatch_ConstexprBoundsKeepTableContractSeparate)
{
    constexpr FakeSpell::Handler handlers[] = {nullptr, &FakeSpell::EffectNULL};
    static_assert(SpellDispatch::IsValidIndex(0, handlers), "zero is in range");
    static_assert(SpellDispatch::IsValidIndex(1, handlers), "deferred handler is in range");
    static_assert(!SpellDispatch::IsValidIndex(2, handlers), "ceiling is exclusive");
    constexpr std::optional<uint32_t> types[] = {0, 1, 2};
    static_assert(SpellDispatch::FirstInvalidIndex(types, handlers) == 2, "last slot checked");
    constexpr std::optional<uint32_t> inactive[] = {std::nullopt, std::nullopt, std::nullopt};
    static_assert(SpellDispatch::FirstInvalidIndex(inactive, handlers) == 3, "absent is not invalid");
    constexpr std::optional<uint32_t> oversized[] = {0, std::nullopt, UINT32_MAX};
    static_assert(SpellDispatch::FirstInvalidIndex(oversized, handlers) == 2, "active full-width bound");
    CHECK(SpellDispatch::IsValidIndex(0, handlers)); // Not permission to invoke nullptr.
}

TEST(SpellDispatch_AllExistingEffectIndicesKeepTheirHandlers)
{
    static_assert(TOTAL_SPELL_EFFECTS == 165, "review table changes, do not expand for native IDs");
    static_assert(MAX_EFFECT_INDEX == 3, "the spell record has three effect slots");
    for (uint32_t type = 0; type < TOTAL_SPELL_EFFECTS; ++type)
    {
        FakeSpell spell;
        CHECK(spell.handlers[type] != nullptr);
        for (unsigned i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            spell.Dispatch(type, SpellEffectIndex(i));
        }
        CHECK_EQ(spell.calculations, 3u);
        CHECK_EQ(spell.nullCalls, type == 0 ? 3u : 0u);
        CHECK_EQ(spell.lowCalls, type == 1 ? 3u : 0u);
        CHECK_EQ(spell.calls, type > 1 ? 3u : 0u);
    }
}

TEST(SpellDispatch_UnsupportedEffectsNeverAliasOrCalculateDamage)
{
    FakeSpell spell;
    for (uint32_t type : {uint32_t(TOTAL_SPELL_EFFECTS), 175u, 183u, 255u,
        256u, 257u, std::numeric_limits<uint32_t>::max()})
    {
        CHECK(!SpellDispatch::IsValidIndex(type, spell.handlers));
        spell.Dispatch(type, EFFECT_INDEX_2);
    }
    CHECK_EQ(spell.calculations, 0u);
    CHECK_EQ(spell.calls, 0u);
    CHECK_EQ(spell.lowCalls, 0u);
    CHECK_EQ(spell.nullCalls, 0u);
}

TEST(SpellDispatch_AllExistingProcIndicesIncludingAbove255RemainValid)
{
    static_assert(TOTAL_AURAS == 317, "317 is invalid, not a new handler");
    for (uint32_t type = 0; type < TOTAL_AURAS; ++type)
    {
        FakeProc proc;
        CHECK(proc.handlers[type] != nullptr);
        uint32_t const types[MAX_EFFECT_INDEX] = {type, type, type};
        proc.Dispatch(types, 1);
        CHECK(!proc.rejected);
        CHECK(!proc.inUse);
        CHECK_EQ(proc.nullCalls, type == 0 ? 1u : 0u);
        bool const handled = type != 0 && type != SPELL_AURA_MOD_POWER_REGEN && type != SPELL_AURA_DUMMY;
        CHECK_EQ(proc.calls, handled ? 1u : 0u);
        CHECK_EQ(proc.cooldowns, handled ? 1u : 0u);
        CHECK_EQ(proc.charges, type == SPELL_AURA_MOD_POWER_REGEN || type == SPELL_AURA_DUMMY ? 2u : 1u);
    }
}

TEST(SpellDispatch_InvalidProcHolderHasNoPartialCallsChargesOrCooldown)
{
    for (uint32_t type : {uint32_t(TOTAL_AURAS), 354u, 512u, 513u,
        std::numeric_limits<uint32_t>::max()})
    {
        for (unsigned i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            FakeProc proc;
            uint32_t types[MAX_EFFECT_INDEX] = {42, 256, 316};
            types[i] = type;
            proc.Dispatch(types);
            CHECK(proc.rejected);
            CHECK(!proc.inUse);
            CHECK_EQ(proc.calls, 0u);
            CHECK_EQ(proc.nullCalls, 0u);
            CHECK_EQ(proc.charges, 2u);
            CHECK_EQ(proc.cooldowns, 0u);
        }
    }
}

TEST(SpellDispatch_InactiveProcMetadataDoesNotRejectSupportedAura)
{
    for (uint32_t staleType : {uint32_t(TOTAL_AURAS), 354u,
        std::numeric_limits<uint32_t>::max()})
    {
        for (unsigned i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            FakeProc proc;
            uint32_t types[MAX_EFFECT_INDEX] = {staleType, staleType, staleType};
            types[i] = 42;
            proc.Dispatch(types, 1u << i);
            CHECK(!proc.rejected);
            CHECK_EQ(proc.calls, 1u);
            CHECK_EQ(proc.nullCalls, 0u);
            CHECK_EQ(proc.charges, 1u);
            CHECK_EQ(proc.cooldowns, 1u);
            CHECK(!proc.inUse);
        }
    }
}

TEST(SpellDispatch_AllInactiveProcSlotsAreNoops)
{
    FakeProc proc;
    uint32_t const types[MAX_EFFECT_INDEX] = {TOTAL_AURAS, 354,
        std::numeric_limits<uint32_t>::max()};
    proc.Dispatch(types, 0);
    CHECK(!proc.rejected);
    CHECK_EQ(proc.calls, 0u);
    CHECK_EQ(proc.nullCalls, 0u);
    CHECK_EQ(proc.charges, 2u);
    CHECK_EQ(proc.cooldowns, 0u);
    CHECK(!proc.inUse);
}

TEST(SpellDispatch_ActiveInvalidProcCannotBeHiddenByEventFiltering)
{
    FakeProc proc;
    uint32_t const types[MAX_EFFECT_INDEX] = {42, 0, TOTAL_AURAS};
    proc.Dispatch(types, 5, 1); // Slot2 exists, but this event would filter it out.
    CHECK(proc.rejected);
    CHECK_EQ(proc.calls, 0u);
    CHECK_EQ(proc.charges, 2u);
    CHECK_EQ(proc.cooldowns, 0u);
    CHECK(!proc.inUse);
}

TEST(SpellDispatch_ValidProcResultAndDeferredChargePolicyIsUnchanged)
{
    FakeProc deferred;
    uint32_t const zeros[MAX_EFFECT_INDEX] = {0, 0, 0};
    deferred.Dispatch(zeros);
    CHECK_EQ(deferred.nullCalls, 3u);
    CHECK_EQ(deferred.charges, 1u); // One charge per holder, not per effect.
    CHECK_EQ(deferred.cooldowns, 0u);

    FakeProc noProc;
    uint32_t const cant[MAX_EFFECT_INDEX] = {SPELL_AURA_MOD_POWER_REGEN,
        SPELL_AURA_MOD_POWER_REGEN, SPELL_AURA_MOD_POWER_REGEN};
    noProc.Dispatch(cant);
    CHECK_EQ(noProc.charges, 2u);
    CHECK_EQ(noProc.cooldowns, 0u);

    FakeProc failed;
    uint32_t const mixed[MAX_EFFECT_INDEX] = {42, SPELL_AURA_DUMMY, 0};
    failed.Dispatch(mixed);
    CHECK_EQ(failed.charges, 2u);
    CHECK_EQ(failed.calls, 1u); // Existing valid-handler failure is not atomic rollback.
    CHECK_EQ(failed.cooldowns, 1u);
    CHECK_EQ(failed.nullCalls, 1u);
}
