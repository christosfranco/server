/**
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#ifndef MANGOS_STATSYSTEM_H
#define MANGOS_STATSYSTEM_H

#include "SharedDefines.h"

#include <algorithm>
#include <cmath>
#include <limits>

enum BaseModGroup : uint32
{
    CRIT_PERCENTAGE,
    RANGED_CRIT_PERCENTAGE,
    OFFHAND_CRIT_PERCENTAGE,
    SHIELD_BLOCK_VALUE,
    BASEMOD_END
};

enum BaseModType : uint32
{
    FLAT_MOD,
    PCT_MOD
};

#define MOD_END (PCT_MOD+1)

namespace StatSystem
{
    class PercentModifier
    {
    public:
        bool Set(float& value, float replacement)
        {
            if (!std::isfinite(replacement))
            {
                return false;
            }
            // Explicit replacement discards the old stack, including suppressors.
            *this = {};
            value = replacement;
            return true;
        }

        bool Apply(float& value, float amount, bool apply)
        {
            if (!std::isfinite(amount) || !std::isfinite(value))
            {
                return false;
            }
            if (amount == 0.0f)
            {
                return true;
            }

            uint32& count = amount <= -100.0f ? m_suppressors : m_factors;
            if ((apply && count == std::numeric_limits<uint32>::max()) ||
                (!apply && count == 0))
            {
                return false;
            }

            if (!m_suppressors && !m_factors)
            {
                m_base = value;
                m_mantissa = 1.0;
                m_exponent = 0;
            }
            if (apply)
            {
                ++count;
            }
            else
            {
                --count;
            }

            if (amount > -100.0f)
            {
                if (!m_factors)
                {
                    // The last removal restores the exact baseline, not a
                    // rounded series of multiplications and divisions.
                    m_mantissa = 1.0;
                    m_exponent = 0;
                }
                else
                {
                    int exponent;
                    double factor = std::frexp((100.0 + double(amount)) / 100.0, &exponent);
                    m_exponent += apply ? exponent : -exponent;
                    m_mantissa = std::frexp(apply ? m_mantissa * factor : m_mantissa / factor,
                        &exponent);
                    m_exponent += exponent;
                }
            }

            if (m_suppressors || m_base == 0.0f)
            {
                value = 0.0f;
            }
            else
            {
                // Keep the unsaturated product so even extreme stacks can be
                // removed again. Only the published float is clamped.
                int exponent;
                double mantissa = std::frexp(std::abs(double(m_base)) * m_mantissa, &exponent);
                int64 totalExponent = m_exponent + exponent;
                if (totalExponent > std::numeric_limits<float>::max_exponent)
                {
                    value = std::numeric_limits<float>::max();
                }
                else if (totalExponent < std::numeric_limits<float>::min_exponent -
                    std::numeric_limits<float>::digits)
                {
                    value = 0.0f;
                }
                else
                {
                    value = float(std::min(std::ldexp(mantissa, int(totalExponent)),
                        double(std::numeric_limits<float>::max())));
                }
                value = std::copysign(value, m_base);
            }
            return true;
        }

    private:
        double m_mantissa = 1.0;
        int64 m_exponent = 0;
        float m_base = 1.0f;
        uint32 m_suppressors = 0;
        uint32 m_factors = 0;
    };

    inline bool ApplyFlatModifier(float& value, float amount, bool apply)
    {
        float result = value + (apply ? amount : -amount);
        if (!std::isfinite(amount) || !std::isfinite(result))
        {
            return false;
        }
        value = result;
        return true;
    }

    inline bool ApplyBaseModifier(float (&values)[BASEMOD_END][MOD_END],
        PercentModifier (&percentages)[BASEMOD_END], BaseModGroup group,
        BaseModType type, float amount, bool apply)
    {
        if (group >= BASEMOD_END || type >= MOD_END)
        {
            return false;
        }
        return type == PCT_MOD ? percentages[group].Apply(values[group][type], amount, apply)
            : ApplyFlatModifier(values[group][type], amount, apply);
    }

    inline bool SetBaseModifier(float (&values)[BASEMOD_END][MOD_END],
        PercentModifier (&percentages)[BASEMOD_END], BaseModGroup group,
        BaseModType type, float value)
    {
        if (group >= BASEMOD_END || type >= MOD_END || !std::isfinite(value))
        {
            return false;
        }
        if (type == PCT_MOD)
        {
            // Crit slots store an additive baseline, not a shield multiplier.
            if (group == SHIELD_BLOCK_VALUE && value < 0.0f)
            {
                return false;
            }
            return percentages[group].Set(values[group][type], value);
        }
        values[group][type] = value;
        return true;
    }

    inline uint32 CalculateShieldBlockValue(float flat, float strength, float percent)
    {
        float value = (flat + strength * 0.5f - 10.0f) * percent;
        if (!std::isfinite(value) || value <= 0.0f)
        {
            return 0;
        }
        return uint32(std::min(double(value), double(std::numeric_limits<uint32>::max())));
    }

    template<class SpellMap, class KnownSpell, class SpellLookup>
    bool HasParrySource(SpellMap const& spells, KnownSpell known, SpellLookup lookup)
    {
        for (auto const& spell : spells)
        {
            if (!known(spell.first))
            {
                continue;
            }
            auto info = lookup(spell.first);
            if (info)
            {
                for (uint32 effect : info->Effect)
                {
                    if (effect == SPELL_EFFECT_PARRY)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    constexpr bool CanModifyPower(int32 power, Powers displayPower, bool player)
    {
        // Keep player modifiers even while a pool is unavailable or capped at
        // zero; applying/removing them must not depend on the current form.
        return power >= 0 && power < MAX_POWERS &&
            (player || power == int32(displayPower));
    }

    constexpr float CalculateManaBonusFromIntellect(float intellect)
    {
        float baseIntellect = std::min(intellect, 20.0f);
        return baseIntellect + (intellect - baseIntellect) * 15.0f;
    }

    inline uint32 CalculateMaxPower(uint32 createPower, float bonusPower,
        float baseValue, float basePct, float totalValue, float totalPct)
    {
        // Modifiers alter a provisioned pool; they do not provision one.
        if (!createPower)
        {
            return 0;
        }

        float value = (baseValue + createPower) * basePct;
        value += totalValue + bonusPower;
        value *= totalPct;
        if (!std::isfinite(value) || value <= 0.0f)
        {
            return 0;
        }

        return uint32(std::min(double(value),
            double(std::numeric_limits<uint32>::max())));
    }

    /**
     * @brief Applies the stock avoidance diminishing-return curve when calibrated.
     *
     * A non-positive cap or coefficient means no class curve is known. Preserve
     * the finite contribution unchanged rather than inventing a cap.
     */
    constexpr float CalculateDiminishingContribution(
        float diminishing, float cap, float coefficient)
    {
        if (cap <= 0.0f || coefficient <= 0.0f)
        {
            return diminishing;
        }

        return diminishing * cap / (diminishing + cap * coefficient);
    }

    inline float CalculateParryChance(uint32 classId, bool canParry,
        float nondiminishing, float diminishing, float coefficient)
    {
        constexpr float parryCap[MAX_STOCK_CLASS] =
        {
            47.003525f,   // Warrior
            47.003525f,   // Paladin
            145.560408f,  // Hunter
            145.560408f,  // Rogue
            0.0f,        // Priest
            47.003525f,   // Death Knight
            145.560408f,  // Shaman
            0.0f,        // Mage
            0.0f,        // Warlock
            0.0f,        // Unused stock class
            0.0f         // Druid
        };
        if (!canParry || !classId || classId >= MAX_CLASSES)
        {
            return 0.0f;
        }

        float cap = classId <= MAX_STOCK_CLASS ? parryCap[classId - 1] : 0.0f;
        // Stock zeroes are intentional exclusions, not missing calibration.
        if (classId <= MAX_STOCK_CLASS && cap == 0.0f)
        {
            return 0.0f;
        }

        float value = nondiminishing +
            CalculateDiminishingContribution(diminishing, cap, coefficient);
        return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
    }

    struct ManaRegen
    {
        float normal;
        float interrupted;
    };

    inline ManaRegen CalculateManaRegen(uint32 maxMana, float spiritRegen,
        float flatRegen, int32 interruptPercent)
    {
        if (!maxMana)
        {
            return {0.0f, 0.0f};
        }

        interruptPercent = std::min(interruptPercent, 100);
        return {std::max(0.0f, flatRegen + spiritRegen),
            std::max(0.0f, flatRegen + spiritRegen * interruptPercent / 100.0f)};
    }
}

#endif
