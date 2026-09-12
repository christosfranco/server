// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COMBO_RULES_H
#define MANGOS_COMBO_RULES_H

#include <algorithm>
#include <cstdint>

namespace ComboRules
{
    inline uint8_t AddPoints(uint8_t current, int32_t delta, bool sameTarget)
    {
        return uint8_t(std::clamp<int64_t>((sameTarget ? int64_t(current) : 0) + delta, 0, 5));
    }

    inline bool CanFinish(bool needsCombo, bool triggered, bool ignoreTargetState,
        uint8_t points, bool matchingTarget)
    {
        return !needsCombo || triggered || ignoreTargetState || (points > 0 && matchingTarget);
    }
}
#endif
