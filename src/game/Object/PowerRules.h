// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_POWER_RULES_H
#define MANGOS_POWER_RULES_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace PowerRules
{
    template<std::size_t Count>
    inline void ResetRegeneration(double (&remainders)[Count], uint32_t& timer, uint32_t interval)
    {
        for (double& remainder : remainders)
        {
            remainder = 0;
        }
        timer = interval;
    }

    inline double FocusRatePerSecond(double rate, double multiplier)
    {
        if (!std::isfinite(rate) || !std::isfinite(multiplier) || rate < 0 || multiplier < 0)
        {
            return 0;
        }
        // Retain the reviewed Ranger baseline: six focus per second.
        return 6.0 * rate * multiplier;
    }

    inline uint32_t ApplyDelta(uint32_t current, uint32_t maximum, int64_t delta)
    {
        if (delta <= -int64_t(current))
        {
            return 0;
        }
        if (delta >= int64_t(maximum) - current)
        {
            return maximum;
        }
        return uint32_t(int64_t(current) + delta);
    }

    inline uint32_t ApplyGain(uint32_t current, uint32_t maximum, double gain)
    {
        current = std::min(current, maximum);
        if (!std::isfinite(gain) || gain <= 0)
        {
            return current;
        }
        if (gain >= double(maximum - current))
        {
            return maximum;
        }
        return current + uint32_t(gain);
    }

    inline void ClampRemainder(uint32_t current, uint32_t maximum, double& remainder)
    {
        if (!maximum || (current >= maximum && remainder > 0) ||
            (!current && remainder < 0))
        {
            remainder = 0;
        }
    }

    // A signed rate supports both regeneration and out-of-combat decay. Carry
    // belongs to the pool, not the displayed power or the current regen rate.
    inline uint32_t Regenerate(uint32_t current, uint32_t maximum,
        double ratePerSecond, uint32_t milliseconds, double& remainder)
    {
        current = std::min(current, maximum);
        ClampRemainder(current, maximum, remainder);
        if (!std::isfinite(ratePerSecond))
        {
            return current;
        }
        double amount = ratePerSecond * (double(milliseconds) / 1000.0) + remainder;
        // Only snap sub-billionth-unit roundoff at nonzero integer boundaries;
        // tiny rates must still accumulate rather than rounding to zero per tick.
        double rounded = std::round(amount);
        if (rounded != 0 && std::abs(amount - rounded) < 1e-9)
        {
            amount = rounded;
        }
        if (amount >= double(maximum - current))
        {
            remainder = 0;
            return maximum;
        }
        if (amount <= -double(current))
        {
            remainder = 0;
            return 0;
        }
        double whole = std::trunc(amount);
        remainder = amount - whole;
        return ApplyDelta(current, maximum, int64_t(whole));
    }
}
#endif
