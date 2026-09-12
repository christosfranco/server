// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_COA_STANCE_RULES_H
#define MANGOS_COA_STANCE_RULES_H

#include <cstddef>
#include <cstdint>

namespace coa
{
    // Exclusive self-buff sets (class_resources.lua STANCE_SETS, same ids).
    // Ascension encodes the swap-cooldown half in Category/CategoryRecoveryTime
    // (enforced by the core from the DBC); the remove-the-others half lived in
    // the Lua and lives here now. On a successful cast of a member every OTHER
    // member leaves the caster; recasting the active member toggles it off.
    // A refused swap (category/personal cooldown) never reaches the cast, so
    // it removes nothing. Player-scoped only.
    struct StanceSet
    {
        char const* name;
        uint32_t members[7];
        std::size_t size;
    };
    inline StanceSet const* StanceSetForSpell(uint32_t spell)
    {
        static constexpr StanceSet Sets[] = {
            {"devotions", {801681, 801682, 801684, 801685}, 4},
            {"vows", {803489, 803491, 803494, 803719, 807435, 807547, 807749}, 7},
            {"quivers", {500103, 800260, 800261, 800262, 801069}, 5},
            {"formations", {800317, 803130, 803417}, 3},
            {"poses", {802282, 802293}, 2},
            {"tonics", {802276, 802277, 802278, 802279}, 4},
            {"undead", {500982, 500983, 500985}, 3},
            {"augments", {704444, 706195, 806614}, 3},
            {"standards", {500260, 500263}, 2},
            {"aspects", {800510, 801123, 801128, 802203}, 4},
            {"aeons", {806290, 806291, 806292, 806293}, 4},
        };
        for (auto const& set : Sets)
        {
            for (std::size_t i = 0; i < set.size; ++i)
            {
                if (set.members[i] == spell) { return &set; }
            }
        }
        return nullptr;
    }
}
#endif
