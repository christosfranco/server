// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_PLAYER_SAVE_PREFLIGHT_H
#define MANGOS_PLAYER_SAVE_PREFLIGHT_H
#include <algorithm>

namespace PlayerPersistence
{
    // Item/slot lookups are read-only. No save callback can consume dirty state
    // until the whole queue passes. Account for state/queue changes made by the
    // writer: buyback normalization and changed enchantment durations.
    template<class Iterator, class Queue, class Enchantments, class State, class Lookup, class Save>
    bool WithInventoryPreflight(Iterator buybackBegin, Iterator buybackEnd,
        Queue const& queue, Enchantments const& enchantments, State removed, Lookup itemAtPosition, Save save)
    {
        for (auto* item : queue)
        {
            if (item && (item->GetState() != removed ||
                std::find(buybackBegin, buybackEnd, item) != buybackEnd) && itemAtPosition(item) != item)
            {
                return false;
            }
        }
        for (auto const& enchantment : enchantments)
        {
            auto* item = enchantment.item;
            if (!item || (item->GetEnchantmentDuration(enchantment.slot) != enchantment.leftduration && itemAtPosition(item) != item))
            {
                return false;
            }
        }
        return save();
    }
}
#endif
