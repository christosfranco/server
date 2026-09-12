// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_SPELL_DISPATCH_H
#define MANGOS_SPELL_DISPATCH_H

#include <cstddef>
#include <cstdint>
#include <optional>

namespace SpellDispatch
{
    // Bounds only: in-range NULL/deferred handlers retain their table semantics.
    template<typename Handler, std::size_t Count>
    constexpr bool IsValidIndex(uint32_t nativeType, Handler const (&)[Count])
    {
        return nativeType < Count;
    }

    // Absence comes from instantiated Aura objects, not unused native columns.
    // Validate all present types before a sibling handler can have side effects.
    template<std::size_t Effects, typename Handler, std::size_t Count>
    constexpr std::size_t FirstInvalidIndex(std::optional<uint32_t> const (&nativeTypes)[Effects],
        Handler const (&handlers)[Count])
    {
        for (std::size_t i = 0; i < Effects; ++i)
        {
            if (nativeTypes[i] && !IsValidIndex(*nativeTypes[i], handlers))
            {
                return i;
            }
        }
        return Effects;
    }
}

#endif
